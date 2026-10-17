#include "kc/roster.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace kc {

std::string RosterHandleText(const Handle& h) {
    return std::to_string(h.type) + ":" + std::to_string(h.container) + ":" + std::to_string(h.containerSerial) + ":" + std::to_string(h.index) + ":" +
           std::to_string(h.serial);
}

bool ParseRosterHandle(const std::string& text, Handle& out) {
    unsigned a = 0, b = 0, c = 0, d = 0, e = 0;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%u:%u:%u:%u:%u%c", &a, &b, &c, &d, &e, &tail) != 5) return false;
    out = Handle{};
    out.type = a;
    out.container = b;
    out.containerSerial = c;
    out.index = d;
    out.serial = e;
    return out.valid();
}

const RosterChar* RosterPlayer::LatestOwn() const {
    for (auto it = chars.rbegin(); it != chars.rend(); ++it)
        if (it->kind != RosterKind::Member) return &*it;
    return nullptr;
}

void Roster::Parse(const std::string& text) {
    players_.clear();
    dirty_ = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        unsigned long long id = 0;
        std::string tok;
        if (!(ls >> id) || !id || !(ls >> tok)) continue;
        RosterKind kind = RosterKind::Own;
        if (tok == "squad" || tok == "editor") {
            kind = tok == "squad" ? RosterKind::Member : RosterKind::Editor;
            if (!(ls >> tok)) continue;
        }
        Handle h;
        if (!ParseRosterHandle(tok, h)) continue;
        std::string name;
        std::getline(ls, name);
        const size_t start = name.find_first_not_of(' ');
        name = start == std::string::npos ? std::string() : name.substr(start);
        RosterPlayer& p = players_["steam:" + std::to_string(id)];
        p.steamId = id;
        // a handle written twice for one player (0.3.4 wrote a found character again): the later wins
        p.chars.erase(std::remove_if(p.chars.begin(), p.chars.end(), [&](const RosterChar& c) { return c.handle == h; }), p.chars.end());
        p.chars.push_back({h, name.substr(0, 64), kind});
        Trim(p);
    }
    dirty_ = false;
}

std::string Roster::Serialize() const {
    std::string out;
    for (const auto& [key, p] : players_) {
        if (!p.steamId) continue;
        for (const auto& c : p.chars) {
            out += std::to_string(p.steamId);
            if (c.kind == RosterKind::Member) out += " squad";
            else if (c.kind == RosterKind::Editor) out += " editor";
            out += " " + RosterHandleText(c.handle) + " " + c.name + "\n";
        }
    }
    return out;
}

void Roster::Trim(RosterPlayer& p) {
    auto count = [&](RosterKind k) { return size_t(std::count_if(p.chars.begin(), p.chars.end(), [&](const RosterChar& c) { return c.kind == k; })); };
    while (count(RosterKind::Own) > kMaxOwnHistory) {   // the oldest own characters go first
        auto it = std::find_if(p.chars.begin(), p.chars.end(), [](const RosterChar& c) { return c.kind == RosterKind::Own; });
        p.chars.erase(it);
        dirty_ = true;
    }
    while (p.chars.size() > kMaxPerPlayer) {
        auto it = std::find_if(p.chars.begin(), p.chars.end(), [](const RosterChar& c) { return c.kind == RosterKind::Member; });
        p.chars.erase(it != p.chars.end() ? it : p.chars.begin());
        dirty_ = true;
    }
}

bool Roster::Note(const std::string& key, uint64_t steamId, const std::string& playerName, const Handle& h, const std::string& name, RosterKind kind) {
    if (key.empty() || !h.valid()) return false;
    bool changed = false;
    // another player's Member record of it: the character changed hands
    for (auto& [k, o] : players_) {
        if (k == key) continue;
        const size_t before = o.chars.size();
        o.chars.erase(std::remove_if(o.chars.begin(), o.chars.end(), [&](const RosterChar& c) { return c.handle == h && c.kind == RosterKind::Member; }),
                      o.chars.end());
        changed |= o.chars.size() != before;
    }
    RosterPlayer& p = players_[key];
    if (steamId && p.steamId != steamId) { p.steamId = steamId; changed = true; }
    if (!playerName.empty()) p.playerName = playerName;
    auto it = std::find_if(p.chars.begin(), p.chars.end(), [&](const RosterChar& c) { return c.handle == h; });
    if (it == p.chars.end()) {
        p.chars.push_back({h, name.substr(0, 64), kind});
        Trim(p);
        dirty_ = true;
        return true;
    }
    if (!name.empty() && it->name != name.substr(0, 64)) { it->name = name.substr(0, 64); changed = true; }
    RosterKind next = it->kind;
    if (kind == RosterKind::Own) next = RosterKind::Own;
    else if (kind == RosterKind::Editor && it->kind == RosterKind::Member) next = RosterKind::Editor;
    if (next != it->kind) {
        it->kind = next;
        changed = true;
    }
    // the newest own character after every other own one (LatestOwn); members may follow it
    if (kind != RosterKind::Member && std::any_of(it + 1, p.chars.end(), [](const RosterChar& c) { return c.kind != RosterKind::Member; })) {
        RosterChar c = *it;
        p.chars.erase(it);
        p.chars.push_back(c);
        changed = true;
    }
    if (changed) {
        Trim(p);
        dirty_ = true;
    }
    return changed;
}

bool Roster::Rehandle(const Handle& from, const Handle& to, const std::string& name) {
    if (from == to || !to.valid()) return false;
    bool changed = false;
    for (auto& [k, p] : players_) {
        auto it = std::find_if(p.chars.begin(), p.chars.end(), [&](const RosterChar& c) { return c.handle == from; });
        if (it == p.chars.end()) continue;
        // the old handle stays recorded just before the new one: an older save of this world (the host's
        // quick load, 16/10 21:50) has the character under it
        auto dup = std::find_if(p.chars.begin(), p.chars.end(), [&](const RosterChar& c) { return c.handle == to; });
        if (dup != p.chars.end()) {   // already recorded under the new handle: the stronger kind
            if (it->kind != RosterKind::Member && dup->kind == RosterKind::Member) { dup->kind = it->kind; changed = true; }
            continue;
        }
        RosterChar now = *it;
        now.handle = to;
        if (!name.empty()) now.name = name.substr(0, 64);
        p.chars.insert(it + 1, now);
        Trim(p);
        changed = true;
    }
    dirty_ |= changed;
    return changed;
}

bool Roster::ForgetMember(const std::string& key, const Handle& h) {
    auto pit = players_.find(key);
    if (pit == players_.end()) return false;
    auto& v = pit->second.chars;
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const RosterChar& c) { return c.handle == h && c.kind == RosterKind::Member; }), v.end());
    const bool changed = v.size() != before;
    dirty_ |= changed;
    return changed;
}

bool Roster::EditorDone(const std::string& key, const Handle& h) {
    RosterPlayer* p = Find(key);
    if (!p) return false;
    for (auto& c : p->chars)
        if (c.handle == h && c.kind == RosterKind::Editor) {
            c.kind = RosterKind::Own;
            dirty_ = true;
            return true;
        }
    return false;
}

const RosterPlayer* Roster::Find(const std::string& key) const {
    auto it = players_.find(key);
    return it == players_.end() ? nullptr : &it->second;
}

RosterPlayer* Roster::Find(const std::string& key) {
    auto it = players_.find(key);
    return it == players_.end() ? nullptr : &it->second;
}

std::string Roster::ReservedFor(const Handle& h, RosterKind* kind) const {
    for (const auto& [k, p] : players_)
        for (const auto& c : p.chars)
            if (c.handle == h && c.kind != RosterKind::Member) {
                if (kind) *kind = c.kind;
                return k;
            }
    return {};
}

std::string Roster::AnyRecordOf(const Handle& h) const {
    for (const auto& [k, p] : players_)
        for (const auto& c : p.chars)
            if (c.handle == h) return k;
    return {};
}

} // namespace kc
