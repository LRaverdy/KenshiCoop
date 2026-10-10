// Lot E: buildings. Placing buildings and furniture in build mode, buying and dismantling them,
// construction progress and removal, for every player.
//
// The host's game is the only one that builds. A client's build mode builds nothing: the placement
// (what the game's factory would get) goes to the host, which builds it, gives it a netId and sends
// the same placement to everyone (the one who asked included), whose game builds the same thing at
// the same place. Construction progress and dismantling run on the host (its workers) and are
// imposed on clients; a building the host's game destroys is removed everywhere. Buying a building
// is done by the host and replayed by every client (the game's own purchase: ownership, doors).
#include <algorithm>
#include <cmath>

#include "kc/session.h"

namespace kc {

namespace {
constexpr double kBuildStateInterval = 1.0;    // changed construction states go out this often
constexpr double kBuildFullInterval = 10.0;    // and every followed building this often
constexpr double kSiteScanInterval = 3.0;      // host: looks for construction sites near the players
constexpr float kSiteRadius = 1000.0f;         // around each player character
constexpr double kForgetFinishedSite = 30.0;   // host: a finished site not placed in the session
constexpr size_t kMaxPendingPlaces = 32;

bool SameState(float a, uint8_t fa, float b, uint8_t fb) { return fa == fb && std::fabs(a - b) < 0.01f; }
} // namespace

void Session::ResetBuildings() {
    buildings_.clear();
    pendingPlaces_.clear();
    pendingBuildActions_.clear();
    hostPlaces_.clear();
    hostActions_.clear();
    hostStates_.clear();
    hostRemoves_.clear();
    buildSyncedPlayers_.clear();
    nextBuildStates_ = nextSiteScan_ = nextBuildFull_ = 0;
}

size_t Session::buildingsResolved() const {
    size_t n = 0;
    for (const auto& [id, b] : buildings_) n += b.resolved ? 1 : 0;
    return n;
}

uint32_t Session::TrackBuilding(const Handle& h, const std::string& sid, const Vec3& pos, bool placed) {
    for (auto& [id, b] : buildings_)
        if (b.handle == h) {
            b.placed |= placed;
            return id;
        }
    BuildingRec b;
    b.handle = h;
    b.sid = sid;
    b.pos = pos;
    b.resolved = true;
    b.placed = placed;
    b.seenBuilding = clock_();
    const uint32_t id = nextNetId_++;
    buildings_[id] = std::move(b);
    world_.TrackBuilding(h);
    return id;
}

void Session::SendBuildStates(double now, bool full, PeerId onlyTo) {
    BuildStateMsg m;
    auto flush = [&] {
        if (m.entries.empty()) return;
        Writer w;
        Encode(w, m);
        if (onlyTo != kNoPeer) SendReliable(onlyTo, w);
        else BroadcastReliable(w, true);
        m.entries.clear();
    };
    for (auto& [id, b] : buildings_) {
        float progress = 0;
        uint8_t flags = 0;
        if (!world_.ReadBuildState(b.handle, progress, flags)) continue;   // its zone is not loaded now
        if (!(flags & kSiteComplete) || (flags & kSiteDismantling)) b.seenBuilding = now;
        if (!full && SameState(progress, flags, b.progress, b.flags)) continue;
        if (onlyTo == kNoPeer) {
            b.progress = progress;
            b.flags = flags;
            b.sentAt = now;
        }
        m.entries.push_back({id, b.sid, b.pos, progress, flags});
        if (m.entries.size() >= 200) flush();
    }
    flush();
}

// ============================== host ==============================

void Session::HostBuildingPacket(RemotePlayer& from, Msg type, Reader& r) {
    if (!from.inGame) return;
    if (type == Msg::BuildPlace) {
        BuildPlace m;
        if (Decode(r, m) && m.netId == 0 && pendingPlaces_.size() < kMaxPendingPlaces) pendingPlaces_.emplace_back(from.id, std::move(m));
    } else if (type == Msg::BuildAction) {
        BuildAction m;
        if (Decode(r, m) && pendingBuildActions_.size() < kMaxPendingPlaces) pendingBuildActions_.emplace_back(from.id, std::move(m));
    }
}

void Session::HostBuildings(double now) {
    auto nameOf = [&](uint8_t pid) {
        auto pl = players_.find(pid);
        return pl != players_.end() ? pl->second.name : "player " + std::to_string(pid);
    };
    auto tell = [&](uint8_t pid, const std::string& text) {
        auto pl = players_.find(pid);
        if (pl == players_.end()) return;
        Chat c;
        c.from = 0;   // a notice, not a player's line
        c.text = text;
        Writer w;
        Encode(w, c);
        SendReliable(pl->second.peer, w);
    };
    auto announce = [&](BuildPlace m, const Handle& created, const std::string& who) {
        std::string sid;
        Vec3 pos;
        if (!world_.BuildingIdentity(created, sid, pos)) { sid = m.sid; pos = m.worldPos; }
        m.netId = TrackBuilding(created, sid, pos, true);
        m.worldPos = pos;
        Writer w;
        Encode(w, m);
        BroadcastReliable(w, true);
        char b[160];
        snprintf(b, sizeof(b), " (netId %u, at %.0f,%.0f,%.0f)", unsigned(m.netId), double(pos.x), double(pos.y), double(pos.z));
        log_(who + " places " + world_.TemplateName(m.sid) + b);
    };
    // The host player's own placements: built already; every client builds them too.
    world_.TakeLocalPlacements(scratchPlacements_);
    for (const auto& lp : scratchPlacements_)
        if (lp.created.valid()) announce(lp.place, lp.created, "the host");
    // Clients' placements: built here first, then everywhere.
    for (auto& [pid, p] : pendingPlaces_) {
        Handle created;
        Vec3 wpos;
        // the checks build mode makes before it accepts a spot: a placement it would have refused
        // (debug placements, a modified client) is not built anywhere
        std::string why, whyFr;
        if (!world_.CheckPlacement(p, why, whyFr)) {
            char at[96];
            snprintf(at, sizeof(at), " at %.1f,%.1f,%.1f", double(p.pos.x), double(p.pos.y), double(p.pos.z));
            log_("[" + nameOf(pid) + "] placement of " + world_.TemplateName(p.sid) + at + " refused: invalid spot (" + why + ")");
            tell(pid, "Impossible de construire " + world_.TemplateName(p.sid) + " ici : " + whyFr);
            continue;
        }
        if (!world_.ExecutePlacement(p, created, wpos) || !created.valid()) {
            log_("[" + nameOf(pid) + "] placement of " + world_.TemplateName(p.sid) + " could not be built on the host");
            tell(pid, "Impossible de construire " + world_.TemplateName(p.sid) + " ici (refusé par la partie de l'hôte).");
            continue;
        }
        announce(p, created, "[" + nameOf(pid) + "]");
    }
    pendingPlaces_.clear();
    // Buying and dismantling asked by clients: the host's game does it; a purchase is replayed by
    // every client (ownership, doors), a dismantling comes back through the construction state.
    for (auto& [pid, a] : pendingBuildActions_) {
        std::string refused;
        const char* what = a.kind == BuildActionKind::Buy ? "buys" : "dismantles";
        if (!world_.ExecuteBuildAction(a, refused)) {
            log_("[" + nameOf(pid) + "] " + what + " " + world_.TemplateName(a.sid) + ": refused (" + (refused.empty() ? "not found" : refused) + ")");
            tell(pid, refused.empty() ? "Ce bâtiment est introuvable chez l'hôte." : refused);
            continue;
        }
        log_("[" + nameOf(pid) + "] " + what + " " + world_.TemplateName(a.sid));
        if (a.kind == BuildActionKind::Buy) {
            Writer w;
            Encode(w, a);
            BroadcastReliable(w, true);
        }
    }
    pendingBuildActions_.clear();
    // The host player's own purchases: replayed by every client too.
    world_.TakeLocalBuildActions(scratchBuildActions_);
    for (const auto& a : scratchBuildActions_) {
        if (a.kind != BuildActionKind::Buy) continue;
        log_("the host buys " + world_.TemplateName(a.sid));
        Writer w;
        Encode(w, a);
        BroadcastReliable(w, true);
    }
    // Construction sites near the players (placed before the session, or by NPC workers).
    if (now >= nextSiteScan_) {
        nextSiteScan_ = now + kSiteScanInterval;
        std::vector<Vec3> centers;
        for (auto& [id, e] : entities_) {
            EntityState st;
            if (e.squad && world_.Read(e.handle, st)) centers.push_back(st.pos);
        }
        std::vector<Handle> sites;
        if (!centers.empty()) world_.ConstructionSitesNear(centers, kSiteRadius, sites);
        for (const Handle& h : sites) {
            std::string sid;
            Vec3 pos;
            if (world_.BuildingIdentity(h, sid, pos)) TrackBuilding(h, sid, pos, false);
        }
        for (auto it = buildings_.begin(); it != buildings_.end();)
            it = (!it->second.placed && now - it->second.seenBuilding > kForgetFinishedSite) ? buildings_.erase(it) : std::next(it);
    }
    // Buildings the host's game destroyed (dismantled, wrecked): gone everywhere.
    world_.TakeBuildingRemovals(scratchRemoved_);
    for (const Handle& h : scratchRemoved_) {
        for (auto it = buildings_.begin(); it != buildings_.end(); ++it) {
            if (it->second.handle != h) continue;
            BuildRemove m{it->first, it->second.sid, it->second.pos};
            Writer w;
            Encode(w, m);
            BroadcastReliable(w, true);
            log_("building removed: " + world_.TemplateName(it->second.sid) + " (netId " + std::to_string(it->first) + ")");
            buildings_.erase(it);
            break;
        }
    }
    // Construction states: what changed every second, everything every 10 s, and everything at
    // once to a player who just arrived.
    if (now >= nextBuildStates_) {
        nextBuildStates_ = now + kBuildStateInterval;
        const bool full = now >= nextBuildFull_;
        if (full) nextBuildFull_ = now + kBuildFullInterval;
        SendBuildStates(now, full, kNoPeer);
    }
    for (auto& [pid, p] : players_) {
        if (!p.inGame || buildSyncedPlayers_.count(pid)) continue;
        buildSyncedPlayers_.insert(pid);
        SendBuildStates(now, true, p.peer);
    }
    for (auto it = buildSyncedPlayers_.begin(); it != buildSyncedPlayers_.end();)
        it = players_.count(*it) ? std::next(it) : buildSyncedPlayers_.erase(it);
}

// ============================== client ==============================

void Session::ClientBuildingPacket(Msg type, Reader& r) {
    switch (type) {
    case Msg::BuildPlace: {
        BuildPlace m;
        if (Decode(r, m) && m.netId && hostPlaces_.size() < 256) hostPlaces_.push_back(std::move(m));
        break;
    }
    case Msg::BuildState: {
        BuildStateMsg m;
        if (Decode(r, m)) for (auto& e : m.entries) if (hostStates_.size() < 4096) hostStates_.push_back(std::move(e));
        break;
    }
    case Msg::BuildRemove: {
        BuildRemove m;
        if (Decode(r, m) && hostRemoves_.size() < 1024) hostRemoves_.push_back(std::move(m));
        break;
    }
    case Msg::BuildAction: {
        BuildAction m;
        if (Decode(r, m) && hostActions_.size() < 64) hostActions_.push_back(std::move(m));
        break;
    }
    default: break;
    }
}

void Session::ClientBuildings(double now) {
    // Our build mode built nothing: the host builds it (and tells everyone).
    world_.TakeLocalPlacements(scratchPlacements_);
    for (const auto& lp : scratchPlacements_) {
        Writer w;
        Encode(w, lp.place);
        SendReliable(net_.serverPeer(), w);
        log_("building placement asked of the host: " + world_.TemplateName(lp.place.sid));
    }
    world_.TakeLocalBuildActions(scratchBuildActions_);
    for (const auto& a : scratchBuildActions_) {
        Writer w;
        Encode(w, a);
        SendReliable(net_.serverPeer(), w);
        log_(std::string(a.kind == BuildActionKind::Buy ? "purchase" : "dismantling") + " of " + world_.TemplateName(a.sid) + " asked of the host");
    }
    // The host's new buildings: built here the same way (unless we already have it: a save).
    for (const auto& p : hostPlaces_) {
        BuildingRec& b = buildings_[p.netId];
        if (b.resolved) continue;
        b.sid = p.sid;
        b.pos = p.worldPos;
        Handle h;
        Vec3 wpos;
        if (world_.FindBuilding(p.sid, p.worldPos, h)) {
            b.handle = h;
            b.resolved = true;
        } else if (world_.ExecutePlacement(p, h, wpos) && h.valid()) {
            b.handle = h;
            b.resolved = true;
            log_("built the host's " + world_.TemplateName(p.sid) + " (netId " + std::to_string(p.netId) + ")");
        } else {
            log_("could not build the host's " + world_.TemplateName(p.sid) + " here (netId " + std::to_string(p.netId) + ")");
        }
    }
    hostPlaces_.clear();
    // Purchases: the game's own purchase here too.
    for (const auto& a : hostActions_) {
        std::string why;
        const bool ok = world_.ExecuteBuildAction(a, why);
        log_(std::string("replayed the purchase of ") + world_.TemplateName(a.sid) + (ok ? "" : ": FAILED (" + why + ")"));
    }
    hostActions_.clear();
    // Construction states.
    for (const auto& e : hostStates_) {
        BuildingRec& b = buildings_[e.netId];
        if (b.sid.empty()) {
            b.sid = e.sid;
            b.pos = e.pos;
        }
        b.haveWant = true;
        b.wantProgress = e.progress;
        b.wantFlags = e.flags;
    }
    hostStates_.clear();
    for (auto& [id, b] : buildings_) {
        if (!b.resolved) {
            if (now < b.tryAt || b.sid.empty()) continue;
            b.tryAt = now + 2.0;
            Handle h;
            if (!world_.FindBuilding(b.sid, b.pos, h)) continue;
            b.handle = h;
            b.resolved = true;
            b.progress = -1;
            b.flags = 0xFF;
        }
        if (!b.haveWant || SameState(b.progress, b.flags, b.wantProgress, b.wantFlags)) continue;
        world_.ApplyBuildState(b.handle, b.wantProgress, b.wantFlags);
        b.progress = b.wantProgress;
        b.flags = b.wantFlags;
    }
    // Removed on the host: removed here.
    for (const auto& m : hostRemoves_) {
        Handle h;
        auto it = buildings_.find(m.netId);
        if (it != buildings_.end() && it->second.resolved) h = it->second.handle;
        else world_.FindBuilding(m.sid, m.pos, h);
        if (h.valid() && world_.RemoveBuilding(h)) log_("removed " + world_.TemplateName(m.sid) + " as on the host");
        if (it != buildings_.end()) buildings_.erase(it);
    }
    hostRemoves_.clear();
}

} // namespace kc
