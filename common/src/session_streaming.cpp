// Per-player streaming (0.3.1): each client is sent the characters around its own characters, and
// recreates them at its own pace; a client that keeps missing NPCs gets its zone sent again.
//
// Before 0.3.1 every client got every character the host's game kept active, which is decided
// around the host's camera and the host's own squad: a player 900 m away received the crowd around
// the host (positions 20 times a second, animations 15 times a second), recreated it in land its own
// game had not loaded (the stand-ins vanished at once and were made again every few seconds: 9723
// "stand-in ... is gone here" lines for one player in the 10 October session), and the link to that
// player was full of characters nobody near them could see.
#include <algorithm>
#include <cmath>

#include "kc/session.h"
#include "kc/streaming.h"

namespace kc {

namespace {
constexpr float kStreamHysteresis = 1.25f;   // a character leaves a player's stream only beyond radius * this
constexpr double kDormantAfter = 4.0;        // client: no state from the host for this long: not sent to us any more
constexpr double kStreamReportEvery = 60.0;  // host: a line per player and per minute
uint32_t Bit(uint8_t id) { return id < 32 ? (1u << id) : 0u; }
} // namespace

bool Session::StreamsTo(const Entity& e, uint8_t playerId) const {
    if (cfg_.streamRadius <= 0 || playerId >= 32 || e.squad || e.container) return true;
    return (e.streamMask & Bit(playerId)) != 0;
}

bool Session::streamsTo(const Handle& h, uint8_t playerId) const {
    auto b = byHandle_.find(h);
    if (b == byHandle_.end()) return false;
    auto e = entities_.find(b->second);
    return e != entities_.end() && StreamsTo(e->second, playerId);
}

size_t Session::streamedTo(uint8_t playerId) const {
    auto s = sync_.find(playerId);
    return s == sync_.end() ? 0 : s->second.streamed;
}

// Host, every interest update (0.5 s): which player is sent which character. A player's centres are
// their own characters; a player without any (a spectator) sees what the host's squad sees. Squad
// members (every player's characters) and containers go to everyone, as before.
void Session::UpdateStreams() {
    std::map<uint8_t, std::vector<Vec3>> own;   // player -> their characters' positions
    std::vector<Vec3> squad;                    // every squad member
    std::unordered_map<uint32_t, Vec3> at;
    for (auto& [id, e] : entities_) {
        if (e.container) continue;
        EntityState st;
        if (!world_.Read(e.handle, st)) continue;
        at[id] = st.pos;
        if (e.squad) {
            own[e.owner].push_back(st.pos);
            squad.push_back(st.pos);
        }
    }
    std::vector<uint8_t> remote;
    for (const auto& [pid, p] : players_) if (p.inGame) remote.push_back(pid);
    const float r = cfg_.streamRadius;
    std::unordered_map<uint32_t, uint32_t> before;
    std::vector<Handle> nearRemote;   // near a client player's own characters (damage numbers)
    for (auto& [id, e] : entities_) {
        before[id] = e.streamMask;
        if (e.container || e.squad || r <= 0) {
            e.streamMask = ~0u;
        } else {
            uint32_t mask = 0;
            auto p = at.find(id);
            for (uint8_t pid : remote) {
                if (p == at.end()) break;
                auto o = own.find(pid);
                const std::vector<Vec3>& centres = o != own.end() && !o->second.empty() ? o->second : squad;
                const bool was = (e.streamMask & Bit(pid)) != 0;
                if (WithinAny(p->second, centres, was ? r * kStreamHysteresis : r)) mask |= Bit(pid);
            }
            e.streamMask = mask;
        }
        if (e.container) continue;
        auto p = at.find(id);
        if (p == at.end()) continue;
        for (uint8_t pid : remote) {
            auto o = own.find(pid);
            if (o != own.end() && WithinAny(p->second, o->second, r > 0 ? r : 3000.0f)) { nearRemote.push_back(e.handle); break; }
        }
    }
    // What a streamed character fights or carries goes with it (the last snapshot's view of them).
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& [id, st] : stateCache_) {
            auto e = entities_.find(id);
            if (e == entities_.end()) continue;
            for (uint32_t other : {st.combatTarget, st.carrying}) {
                if (!other) continue;
                auto o = entities_.find(other);
                if (o != entities_.end() && !o->second.container) o->second.streamMask |= e->second.streamMask;
            }
        }
    // A player who starts being sent a character gets all of its state at once (positions, health),
    // not only what changes from now on.
    for (uint8_t pid : remote) {
        PlayerSync& s = sync_[pid];
        s.streamed = 0;
        for (auto& [id, e] : entities_) {
            if (e.container) continue;
            const bool now = StreamsTo(e, pid);
            s.streamed += now ? 1 : 0;
            auto b = before.find(id);
            const bool was = b != before.end() && (cfg_.streamRadius <= 0 || e.squad || (b->second & Bit(pid)) != 0);
            if (now && !was) s.sent.erase(id);
        }
    }
    world_.SetRemoteInterest(nearRemote);
}

// Host: a line per player and per minute in the log: how many characters they are sent, how fast
// their link goes (measures the "dense town far from the host" case).
void Session::StreamReport(double now) {
    if (now < nextStreamReport_) return;
    const bool first = nextStreamReport_ == 0;
    nextStreamReport_ = now + kStreamReportEvery;
    size_t chars = 0;
    for (const auto& [id, e] : entities_) chars += e.container ? 0 : 1;
    for (auto& [pid, p] : players_) {
        if (!p.inGame) continue;
        PlayerSync& s = sync_[pid];
        const NetStats st = net_.stats(p.peer);
        if (!first && s.statsAt > 0 && now > s.statsAt) {
            const double kbs = double(st.queued - std::min(st.queued, s.bytesAt)) / 1024.0 / (now - s.statsAt);
            char b[220];
            snprintf(b, sizeof(b), "stream: %s is sent %zu of %zu characters (around their own), %.1f KB/s to them, ping %u ms, %.1f %% lost",
                     p.name.c_str(), s.streamed, chars, kbs, unsigned(st.rttMs), st.lossPermille / 10.0);
            log_(b);
        }
        s.statsAt = now;
        s.bytesAt = st.queued;
    }
}

// Host: a client's report (every 5 s). Missing NPCs or characters off for a while: that client's
// zone is sent again, by itself, without the host clicking and without pausing anyone.
void Session::ZoneHealthReport(RemotePlayer& pl, const ClientReport& m) {
    if (!cfg_.autoZoneResync || !pl.inGame) return;
    std::string why;
    switch (zoneHealth_.Report(pl.id, m.missingNpcs, m.farOff, clock_(), why)) {
    case ZoneHealth::Action::None: break;
    case ZoneHealth::Action::Resync: ZoneResync(pl.id, why); break;
    case ZoneHealth::Action::StillDiffers:
        log_("zone still differs for " + pl.name + " after " + std::to_string(zoneHealth_.tries(pl.id)) + " automatic zone resyncs (" + why +
             "): a full resync (Resynchroniser) may be needed");
        break;
    }
}

// Host: every character that player is sent, bound again (the client checks it and recreates it at
// once if missing), with all its state on the next ticks and its inventory now. Nobody is paused.
size_t Session::ZoneResync(uint8_t playerId, const std::string& why) {
    auto pl = players_.find(playerId);
    if (!isHost() || pl == players_.end() || !pl->second.inGame) return 0;
    PlayerSync& s = sync_[playerId];
    size_t n = 0;
    for (auto& [id, e] : entities_) {
        if (e.container || !StreamsTo(e, playerId)) continue;
        SendBind(e, pl->second.peer);
        s.sent.erase(id);
        std::vector<ItemState> items;
        if (world_.ReadInventory(e.handle, items)) {
            InventoryMsg m;
            m.netId = id;
            m.items = std::move(items);
            Writer w(1024);
            Encode(w, m);
            SendReliable(pl->second.peer, w);
        }
        ++n;
    }
    ++zoneResyncs_;
    log_("auto zone resync for " + pl->second.name + (why.empty() ? std::string() : " (" + why + ")") + ": " + std::to_string(n) +
         " characters around them sent again; nobody paused");
    return n;
}

// ---------------------------------------------------------------- client

void Session::UpdateMyCentres() {
    myCentres_.clear();
    for (const auto& [id, e] : entities_)
        if (e.squad && e.owner == localId_ && !e.buf.empty()) myCentres_.push_back(e.buf.back().s.pos);
}

bool Session::Dormant(const Entity& e, double now) const { return now - e.streamAt > kDormantAfter; }

// A host character we may recreate here: the host still sends it to us, and it is near one of our own
// characters (where our game has the land loaded). Without characters of our own: anywhere sent.
bool Session::InMyArea(const Entity& e, double now) const {
    if (e.buf.empty() || Dormant(e, now)) return false;
    if (cfg_.standInRadius <= 0 || myCentres_.empty()) return true;
    return WithinAny(e.buf.back().s.pos, myCentres_, cfg_.standInRadius);
}

} // namespace kc
