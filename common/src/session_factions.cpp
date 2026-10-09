// Lot B: faction relations, bounties and crimes are the host's. The host sends them when they
// change (and in full every 15 s, and to a player who just arrived); clients impose them on their
// game, again every few seconds since the local game may drift (its own updates, expiring crimes).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "kc/session.h"

namespace kc {

namespace {

uint64_t HashBytes(const Writer& w) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = w.data();
    for (size_t i = 0; i < w.size(); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

std::string RelationText(const RelationState& s) {
    char b[96];
    snprintf(b, sizeof(b), "%.0f%s%s%s", double(s.relation), s.alliance ? " allied" : "", s.war ? " at war" : "", s.peace ? " peace" : "");
    return b;
}

} // namespace

void Session::ResetFactions() {
    nextFactions_ = nextFactionsFull_ = nextFactionsApply_ = 0;
    factionsHash_ = bountiesHash_ = 0;
    factionsServed_.clear();
    hostFactions_ = FactionsMsg{};
    hostBounties_ = BountiesMsg{};
    haveFactions_ = haveBounties_ = factionsDirty_ = false;
    factionsView_ = FactionsView{};
}

void Session::SendFactions(double now) {
    if (now < nextFactions_) return;
    nextFactions_ = now + 1.0;
    const bool full = now >= nextFactionsFull_;
    if (full) nextFactionsFull_ = now + 15.0;
    // players that have not had the full state yet (they just arrived)
    std::vector<PeerId> fresh;
    std::set<PeerId> live;
    for (auto& [pid, p] : players_) {
        if (!p.inGame) continue;
        live.insert(p.peer);
        if (!factionsServed_.count(p.peer)) fresh.push_back(p.peer);
    }
    for (auto it = factionsServed_.begin(); it != factionsServed_.end();) it = live.count(*it) ? std::next(it) : factionsServed_.erase(it);

    FactionsMsg fm;
    if (world_.ReadFactions(fm)) {
        Writer w(4096);
        Encode(w, fm);
        const uint64_t h = HashBytes(w);
        if (h != factionsHash_ || full) {
            // the log tells what changed (not the periodic refresh)
            if (factionsHash_ != 0 && h != factionsHash_) {
                for (const auto& f : fm.factions) {
                    auto old = std::find_if(hostFactions_.factions.begin(), hostFactions_.factions.end(),
                                            [&](const FactionRelationEntry& e) { return e.factionSid == f.factionSid; });
                    if (old == hostFactions_.factions.end()) continue;
                    if (f.hasOurs && old->hasOurs &&
                        (std::fabs(f.ours.relation - old->ours.relation) >= 1.0f || f.ours.alliance != old->ours.alliance || f.ours.war != old->ours.war))
                        log_("relations: " + world_.TemplateName(f.factionSid) + " now " + RelationText(f.ours) + " (was " + RelationText(old->ours) + ")");
                }
                if (fm.playerRank != hostFactions_.playerRank) log_("relations: player faction rank now " + std::to_string(fm.playerRank));
            }
            BroadcastReliable(w, true);
            factionsHash_ = h;
            hostFactions_ = std::move(fm);
            ++factionsView_.sent;
        } else {
            for (PeerId peer : fresh) SendReliable(peer, w);
        }
    }

    BountiesMsg bm;
    for (auto& [id, e] : entities_) {
        if (!e.squad || e.container) continue;
        CharBounties cb;
        if (!world_.ReadBounties(e.handle, cb)) continue;
        cb.netId = id;
        bm.chars.push_back(std::move(cb));
    }
    std::sort(bm.chars.begin(), bm.chars.end(), [](const CharBounties& a, const CharBounties& b) { return a.netId < b.netId; });
    Writer w(1024);
    Encode(w, bm);
    const uint64_t h = HashBytes(w);
    if (h != bountiesHash_ || full) {
        if (bountiesHash_ != 0 && h != bountiesHash_) {
            for (const auto& c : bm.chars) {
                auto old = std::find_if(hostBounties_.chars.begin(), hostBounties_.chars.end(), [&](const CharBounties& o) { return o.netId == c.netId; });
                auto ent = entities_.find(c.netId);
                const std::string who = ent != entities_.end() ? world_.CharacterNameOf(ent->second.handle) : "character " + std::to_string(c.netId);
                for (const auto& b : c.bounties) {
                    int32_t before = 0;
                    if (old != hostBounties_.chars.end())
                        for (const auto& ob : old->bounties) if (ob.factionSid == b.factionSid) before = ob.amount;
                    if (b.amount != before)
                        log_("bounty: " + who + " wanted by " + world_.TemplateName(b.factionSid) + " for " + std::to_string(b.amount) + " cats (was " +
                             std::to_string(before) + ")");
                }
                if (old != hostBounties_.chars.end() && old->crime != c.crime)
                    log_("crime: " + who + (c.crime ? " commits crime " + std::to_string(c.crime) + " against " + world_.TemplateName(c.crimeFactionSid)
                                                    : std::string(" no longer commits a crime")));
                if (old != hostBounties_.chars.end() && (old->prisonSentence > 0) != (c.prisonSentence > 0))
                    log_("prison: " + who + (c.prisonSentence > 0 ? " sentenced, " + std::to_string(int(c.prisonSentence)) + " h to serve" : std::string(" free")));
            }
        }
        BroadcastReliable(w, true);
        bountiesHash_ = h;
        hostBounties_ = std::move(bm);
    } else {
        for (PeerId peer : fresh) SendReliable(peer, w);
    }
    for (PeerId peer : fresh) factionsServed_.insert(peer);
}

bool Session::ClientFactionsPacket(Msg type, Reader& r) {
    if (type != Msg::Factions && type != Msg::Bounties) return false;
    if (state_ != SessionState::Connected) return true;
    if (type == Msg::Factions) {
        FactionsMsg m;
        if (!Decode(r, m)) return true;
        hostFactions_ = std::move(m);
        haveFactions_ = true;
        ++factionsView_.received;
    } else {
        BountiesMsg m;
        if (!Decode(r, m)) return true;
        hostBounties_ = std::move(m);
        haveBounties_ = true;
        ++factionsView_.bountiesReceived;
    }
    factionsDirty_ = true;
    return true;
}

void Session::ClientFactionsTick(double now) {
    if (!factionsDirty_ && now < nextFactionsApply_) return;
    const bool fresh = factionsDirty_;
    nextFactionsApply_ = now + 2.0;
    factionsDirty_ = false;
    size_t n = 0;
    if (haveFactions_) n += world_.ApplyFactions(hostFactions_);
    if (haveBounties_)
        for (const auto& c : hostBounties_.chars) {
            auto it = entities_.find(c.netId);
            if (it != entities_.end() && it->second.present) n += world_.ApplyBounties(it->second.handle, c);
        }
    if (!n) return;
    factionsView_.corrected += n;
    static double lastDriftLog = -1e9;
    if (fresh || now - lastDriftLog > 30.0) {
        if (!fresh) lastDriftLog = now;
        log_("factions: " + std::to_string(n) + " relation/bounty values set to the host's" + (fresh ? "" : " (the local game had changed them)"));
    }
}

} // namespace kc
