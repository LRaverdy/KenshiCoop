// Lot D: prisons. Captive characters (cages, shackles, slavery, prison sentences) as the host's game
// has them, imposed on every client.
//
// The host reads each followed character's captivity twice a second and sends what changed (a
// character set free once, with everything cleared), plus every captive every 5 s so a late joiner
// or a lost update catches up. Clients impose it: the same local cage (found by kind and place),
// the same flags; a caged character is then held where the cage holds it (no position fights).
// Everything that changes captivity (being caged by a hostile NPC, picking a lock, a player opening
// the cage) happens in the host's game: client orders already go there.
#include "kc/session.h"

namespace kc {

namespace {
constexpr double kCaptivesInterval = 0.5;
constexpr double kCaptivesFull = 5.0;
constexpr double kCaptivesReapply = 2.0;

std::string Describe(const CaptiveState& s) {
    std::string out;
    if (s.caged) out += " caged";
    if (s.chained) out += " chained";
    if (s.slaveState) out += " slave-state=" + std::to_string(s.slaveState);
    if (!s.slaveOf.empty()) out += " slave-of=" + s.slaveOf;
    if (s.escaped) out += " escaped";
    if (s.kidnapped) out += " kidnapped";
    if (s.sentence > 0) out += " sentence=" + std::to_string(int(s.sentence)) + "h";
    return out.empty() ? " free" : out;
}
} // namespace

size_t Session::captiveCount() const {
    size_t n = 0;
    if (isHost()) {
        for (const auto& [id, s] : captiveSent_) n += s.free() ? 0 : 1;
    } else {
        for (const auto& [id, s] : captives_) n += s.free() ? 0 : 1;
    }
    return n;
}

void Session::HostCaptives(double now) {
    if (now < nextCaptives_) return;
    nextCaptives_ = now + kCaptivesInterval;
    const bool full = now >= captivesFullAt_;
    if (full) captivesFullAt_ = now + kCaptivesFull;
    CaptivesMsg changed, all;
    for (auto& [id, e] : entities_) {
        if (e.container) continue;
        CaptiveState s;
        if (!world_.ReadCaptive(e.handle, s)) continue;
        s.netId = id;
        auto it = captiveSent_.find(id);
        const bool known = it != captiveSent_.end();
        if ((known && it->second != s) || (!known && !s.free())) {
            changed.chars.push_back(s);
            log_("captivity: " + world_.CharacterNameOf(e.handle) + (s.caged ? " in " + world_.TemplateName(s.cageSid) : std::string{}) +
                 " now" + Describe(s));
        }
        if (!s.free()) all.chars.push_back(s);
        if (s.free()) captiveSent_.erase(id);   // sent free once (above), then forgotten
        else captiveSent_[id] = s;
    }
    for (auto it = captiveSent_.begin(); it != captiveSent_.end();)
        it = entities_.count(it->first) ? std::next(it) : captiveSent_.erase(it);
    const CaptivesMsg& m = full ? all : changed;
    if (full) {   // the full set, plus those just freed
        for (const auto& c : changed.chars)
            if (c.free()) all.chars.push_back(c);
    }
    if (m.chars.empty()) return;
    for (size_t i = 0; i < m.chars.size(); i += kMaxCaptivesPerMsg) {
        CaptivesMsg part;
        part.chars.assign(m.chars.begin() + ptrdiff_t(i), m.chars.begin() + ptrdiff_t(std::min(m.chars.size(), i + kMaxCaptivesPerMsg)));
        Writer w;
        Encode(w, part);
        BroadcastReliable(w, true);
    }
}

void Session::OnCaptives(Reader& r) {
    CaptivesMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    for (auto& c : m.chars) {
        auto it = captives_.find(c.netId);
        if (it != captives_.end() && it->second == c) continue;
        auto e = entities_.find(c.netId);
        log_("captivity from the host: " + (e != entities_.end() ? world_.CharacterNameOf(e->second.handle) : std::string("?")) + Describe(c));
        captivesDirty_.insert(c.netId);
        captives_[c.netId] = std::move(c);
    }
}

void Session::ClientCaptives(double now) {
    const bool reapply = now >= captivesReapplyAt_;
    if (reapply) captivesReapplyAt_ = now + kCaptivesReapply;
    for (auto it = captives_.begin(); it != captives_.end();) {
        const uint32_t id = it->first;
        auto e = entities_.find(id);
        if (e == entities_.end()) { captivesDirty_.erase(id); it = captives_.erase(it); continue; }
        if (!e->second.present || !(reapply || captivesDirty_.count(id))) { ++it; continue; }
        world_.ApplyCaptive(e->second.handle, it->second);
        captivesDirty_.erase(id);
        // a freed character: imposed once, then there is nothing more to keep
        it = it->second.free() ? captives_.erase(it) : std::next(it);
    }
}

} // namespace kc
