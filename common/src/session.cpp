#include "kc/session.h"

#include <algorithm>
#include <cmath>

namespace kc {

namespace {

constexpr size_t kMaxChatLines = 50;
constexpr double kBufferSeconds = 1.0;       // client keeps this much interpolation history
constexpr double kInterestInterval = 0.5;
constexpr double kPingInterval = 1.0;
constexpr double kConnectTimeout = 10.0;
constexpr double kPresenceInterval = 1.0;
constexpr double kSpawnGrace = 1.5;   // zones stream in at slightly different times on each side
constexpr double kVitalsRefresh = 2.0;
constexpr double kVitalsReapply = 0.25;      // client re-imposes host vitals this often
constexpr float kInterestHysteresis = 1.25f; // NPCs are dropped only beyond radius * this

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
float QuatDot(const Quat& a, const Quat& b) { return std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z); }
Quat Nlerp(Quat a, const Quat& b, float t) {
    // take the short way round
    if (a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z < 0) { a.w = -a.w; a.x = -a.x; a.y = -a.y; a.z = -a.z; }
    Quat q{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (len > 1e-6f) { q.w /= len; q.x /= len; q.y /= len; q.z /= len; } else q = b;
    return q;
}

bool StateChanged(const EntityState& a, const EntityState& b) {
    return Dist(a.pos, b.pos) > 0.02f || Dist(a.dest, b.dest) > 0.25f || a.flags != b.flags || QuatDot(a.rot, b.rot) < 0.99995f ||
           a.combatTarget != b.combatTarget || a.gait != b.gait || std::fabs(a.pace - b.pace) > 0.1f;
}
uint64_t InventoryHash(const std::vector<ItemState>& items) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& i : items) {
        h = Fnv1a64(i.templateSid.data(), i.templateSid.size(), h);
        h = Fnv1a64(i.materialSid.data(), i.materialSid.size(), h);
        h = Fnv1a64(i.manufacturerSid.data(), i.manufacturerSid.size(), h);
        h = Fnv1a64(i.section.data(), i.section.size(), h);
        const int32_t v[5] = {i.quantity, i.x, i.y, i.equipped ? 1 : 0, i.level};
        h = Fnv1a64(v, sizeof(v), h);
    }
    return h;
}

bool VitalsChanged(const EntityVitals& a, const EntityVitals& b) {
    return std::fabs(a.blood - b.blood) > 0.05f || std::fabs(a.koTimer - b.koTimer) > 0.5f || a.flags != b.flags || a.parts != b.parts;
}

} // namespace

Session::Session(IWorld& world, SessionConfig cfg, ClockFn clock, LogFn log)
    : world_(world), cfg_(std::move(cfg)), clock_(std::move(clock)), log_(std::move(log)) {
    Net::GlobalInit();
    cb_.onConnect = [this](PeerId p) {
        if (net_.isServer()) {
            pendingPeers_[p] = clock_();
        } else if (state_ == SessionState::Connecting) {
            Hello h;
            h.gameBuild = world_.GameBuild();
            h.modsHash = world_.ModsHash();
            h.name = cfg_.name;
            Writer w;
            Encode(w, h);
            SendReliable(net_.serverPeer(), w);
            state_ = SessionState::Handshake;
            log_("connected, handshaking");
        }
    };
    cb_.onDisconnect = [this](PeerId p) { OnDisconnect(p); };
    cb_.onPacket = [this](PeerId p, uint8_t c, const uint8_t* d, size_t n) { OnPacket(p, c, d, n); };
}

Session::~Session() {
    Leave();
    Net::GlobalShutdown();
}

bool Session::Host(std::string* err) {
    Leave();
    if (!world_.Ready()) {
        if (err) *err = "load a save before hosting";
        return false;
    }
    if (!net_.Listen(cfg_.port, kMaxPlayers, err)) return false;
    state_ = SessionState::Hosting;
    localId_ = hostId_;
    lastError_.clear();
    lastLive_ = clock_();
    world_.SetRole(false, true);
    controllableDirty_ = true;
    nextInterest_ = 0;
    log_("hosting on UDP port " + std::to_string(cfg_.port));
    return true;
}

bool Session::Join(const std::string& address, uint16_t port, std::string* err) {
    Leave();
    if (!net_.Connect(address, port, err)) return false;
    state_ = SessionState::Connecting;
    lastError_.clear();
    connectStarted_ = clock_();
    offsetValid_ = false;
    log_("connecting to " + address + ":" + std::to_string(port));
    return true;
}

void Session::Leave() {
    const bool wasActive = state_ != SessionState::Idle && state_ != SessionState::Failed;
    const bool roleWasSet = state_ == SessionState::Hosting || state_ == SessionState::Connected;
    net_.Close();
    entities_.clear();
    byHandle_.clear();
    players_.clear();
    sync_.clear();
    pendingPeers_.clear();
    pendingCommands_.clear();
    despawnQueue_.clear();
    owners_.clear();
    byIdentity_.clear();
    exportFiles_.clear();
    exportFiles_.shrink_to_fit();
    exporting_ = exportReady_ = false;
    dlFiles_.clear();
    dlFiles_.shrink_to_fit();
    dlComplete_ = importStarted_ = false;
    haveTime_ = false;
    missingSquad_ = 0;
    localId_ = 0;
    if (holding_) {
        holding_ = false;
        world_.HoldForJoin(false);
    }
    if (roleWasSet) {
        world_.SetRole(false, false);
        world_.SetControllable({});
    }
    if (wasActive) log_("session closed");
    if (state_ != SessionState::Failed) state_ = SessionState::Idle;
}

void Session::Fail(const std::string& why) {
    log_("session failed: " + why);
    Leave();
    lastError_ = why;
    state_ = SessionState::Failed;
}

void Session::Tick(bool worldLive) {
    if (state_ == SessionState::Idle || state_ == SessionState::Failed) return;
    const double now = clock_();
    net_.Poll(cb_);
    if (state_ == SessionState::Idle || state_ == SessionState::Failed) return;  // poll may have ended it
    const bool live = worldLive && world_.Ready();
    if (isHost()) HostTick(now, live);
    else ClientTick(now, live);
    net_.Flush();
}

// ============================== host ==============================

void Session::HostTick(double now, bool live) {
    if (live) lastLive_ = now;
    else if (now - lastLive_ > cfg_.worldLostTimeout) { Fail("world unloaded"); return; }

    for (auto it = pendingPeers_.begin(); it != pendingPeers_.end();) {
        if (now - it->second > cfg_.handshakeTimeout) { net_.Kick(it->first); it = pendingPeers_.erase(it); }
        else ++it;
    }

    HostJoinFlow(now, live);
    if (state_ != SessionState::Hosting || !live) return;

    if (now >= nextInterest_) {
        nextInterest_ = now + kInterestInterval;
        UpdateInterest();
    }
    if (controllableDirty_) PushControllable();

    for (auto& [from, c] : pendingCommands_) ApplyCommand(from, c);
    pendingCommands_.clear();
    for (auto& [from, op] : pendingInvOps_) HostInvOp(from, op);
    if (!pendingInvOps_.empty()) nextInventory_ = 0;   // show the result right away
    pendingInvOps_.clear();
    // The host's own orders execute natively in its world; nothing to intercept.
    scratchOrders_.clear();
    world_.TakeLocalOrders(scratchOrders_);

    const TimeState t = world_.GetTime();
    if (now >= nextTimeState_ || t.speed != lastTime_.speed || t.paused != lastTime_.paused) {
        lastTime_ = t;
        nextTimeState_ = now + cfg_.timeStateInterval;
        Writer w;
        Encode(w, t);
        BroadcastReliable(w, true);
    }

    bool anyInGame = false;
    for (auto& [pid, p] : players_) anyInGame |= p.inGame;
    if (anyInGame && now >= nextInventory_) {
        nextInventory_ = now + 0.5;
        SendInventories(now, false, kNoPeer);
    }
    // Weather: sent the tick any region's weather changes (before the effects that weather places),
    // and in full every 10 s.
    if (anyInGame && now >= nextWeather_) {
        nextWeather_ = now;
        std::vector<RegionWeather> w;
        world_.ReadWeather(w);
        bool changed = w.size() != lastWeather_.size() || now >= weatherForceAt_;
        for (size_t i = 0; !changed && i < w.size(); ++i) changed = !w[i].sameKind(lastWeather_[i]);
        if (changed && !w.empty()) {
            weatherForceAt_ = now + 10.0;
            lastWeather_ = w;
            WeatherMsg m;
            m.regions = std::move(w);
            Writer out(4096);
            Encode(out, m);
            BroadcastReliable(out, true);
        }
    }
    // Weather effects: every one the host's game places goes out as soon as it appears; the
    // complete live set every 5 s heals anything missed (and serves newcomers).
    {
        EffectsMsg m;
        const bool full = anyInGame && now >= effectsFullAt_;
        world_.ReadEffects(m, full);   // drained even with nobody to send them to
        if (full) effectsFullAt_ = now + 5.0;
        if (anyInGame && !m.empty()) {
            Writer out(1024);
            Encode(out, m);
            BroadcastReliable(out, true);
        }
    }
    if (anyInGame) {
        if (now >= nextSnapshot_) {
            nextSnapshot_ = std::max(nextSnapshot_ + 1.0 / cfg_.snapshotRate, now - 0.5 / cfg_.snapshotRate);
            SendSnapshots(now);
        }
        if (now >= nextVitals_) {
            nextVitals_ = std::max(nextVitals_ + 1.0 / cfg_.vitalsRate, now - 0.5 / cfg_.vitalsRate);
            SendVitals(now);
        }
    }

    if (now >= nextPing_) {
        nextPing_ = now + kPingInterval;
        for (auto& [pid, p] : players_) p.rttMs = net_.stats(p.peer).rttMs;
    }
}

void Session::HostJoinFlow(double now, bool live) {
    std::vector<uint8_t> joining;
    for (auto& [pid, p] : players_) if (!p.inGame && !sync_[pid].kicked) joining.push_back(pid);

    if (joining.empty()) {
        if (holding_ && live) { world_.HoldForJoin(false); holding_ = false; }
        if (exportReady_ || exporting_) {  // the next joiner gets a fresh save
            exportFiles_.clear();
            exportFiles_.shrink_to_fit();
            exportReady_ = exporting_ = false;
        }
        return;
    }
    // Hold the world still: what is saved must be exactly what the joiner will see.
    if (live && !holding_) { world_.HoldForJoin(true); holding_ = true; }

    for (uint8_t id : joining) {
        if (now - sync_[id].joinedAt > cfg_.loadTimeout) Kick(players_[id], RejectReason::Timeout);
    }

    bool needWorld = false;
    for (uint8_t id : joining) needWorld |= !sync_[id].worldSent;
    if (needWorld && !exportReady_) {
        if (!exporting_ && live && holding_) {
            // Each newcomer's own character goes into the world before it is saved for them.
            if (cfg_.characterPerPlayer) {
                for (uint8_t id : joining) {
                    PlayerSync& s = sync_[id];
                    if (s.ownChecked || s.worldSent) continue;
                    s.ownChecked = true;
                    if (world_.EnsurePlayerCharacter(players_[id].name, s.own)) Assign(s.own, id);
                    else log_("no character of their own for " + players_[id].name);
                }
            }
            std::string err;
            if (world_.BeginWorldExport(&err)) {
                exporting_ = true;
                exportStarted_ = now;
                log_("saving the world for joining players");
            } else {
                log_("cannot save the world for joining players: " + err);
                for (auto& [pid, p] : players_) if (!p.inGame) Kick(p, RejectReason::HostSaveFailed);
                return;
            }
        }
        if (exporting_) {
            std::string err;
            const ExportStatus st = world_.PollWorldExport(exportFiles_, &err);
            if (st == ExportStatus::Done && live) {
                exporting_ = false;
                exportReady_ = true;
                exportHash_ = world_.Fingerprint();
                uint64_t bytes = 0;
                for (auto& f : exportFiles_) bytes += f.data.size();
                log_("world saved: " + std::to_string(exportFiles_.size()) + " files, " + std::to_string(bytes / 1024) + " KB");
            } else if (st == ExportStatus::Failed || now - exportStarted_ > cfg_.exportTimeout) {
                exporting_ = false;
                log_("saving the world failed: " + (err.empty() ? std::string("timeout") : err));
                for (auto& [pid, p] : players_) if (!p.inGame) Kick(p, RejectReason::HostSaveFailed);
                return;
            }
        }
    }
    if (exportReady_) {
        for (auto& [pid, p] : players_) {
            if (p.inGame || sync_[pid].worldSent) continue;
            StreamWorld(p);
            sync_[pid].worldSent = true;
        }
    }
    if (!live) return;
    for (auto& [pid, p] : players_) {
        PlayerSync& s = sync_[pid];
        if (p.inGame || !s.readyPending) continue;
        s.readyPending = false;
        if (s.readyHash != world_.Fingerprint()) { Kick(p, RejectReason::WorldMismatch); continue; }
        FinishJoin(p);
    }
}

void Session::StreamWorld(const RemotePlayer& p) {
    WorldBegin b;
    b.fileCount = uint32_t(exportFiles_.size());
    for (auto& f : exportFiles_) b.totalBytes += f.data.size();
    Writer w;
    Encode(w, b);
    SendReliable(p.peer, w);
    for (uint32_t i = 0; i < exportFiles_.size(); ++i) {
        const WorldFile& f = exportFiles_[i];
        uint64_t off = 0;
        do {
            WorldChunk c;
            c.file = i;
            c.offset = off;
            if (off == 0) { c.path = f.path; c.fileSize = f.data.size(); }
            const size_t n = size_t(std::min<uint64_t>(kWorldChunkSize, f.data.size() - off));
            c.data.assign(f.data.begin() + ptrdiff_t(off), f.data.begin() + ptrdiff_t(off + n));
            Writer cw(n + 64);
            Encode(cw, c);
            SendReliable(p.peer, cw);
            off += n;
        } while (off < f.data.size());
    }
    Writer e;
    Encode(e, WorldEnd{exportHash_});
    SendReliable(p.peer, e);
    log_("world sent to " + p.name);
}

void Session::FinishJoin(RemotePlayer& p) {
    p.inGame = true;
    sync_[p.id].sent.clear();
    for (auto& [nid, e] : entities_) SendBind(e, p.peer);
    Writer t;
    Encode(t, world_.GetTime());
    SendReliable(p.peer, t);
    SendInventories(clock_(), true, p.peer);
    weatherForceAt_ = 0;   // the newcomer gets the full weather on the next weather tick
    nextWeather_ = 0;
    effectsFullAt_ = clock_() + 1.0;   // and every weather effect once that weather is in place
    AddChat("* " + p.name + " is in the world");
}

bool Session::Configure(const std::string& name, uint16_t port) {
    if ((state_ != SessionState::Idle && state_ != SessionState::Failed) || !ValidName(name) || port == 0) return false;
    cfg_.name = name;
    cfg_.port = port;
    return true;
}

bool Session::KickPlayer(uint8_t playerId) {
    auto it = players_.find(playerId);
    if (!isHost() || it == players_.end()) return false;
    Kick(it->second, RejectReason::Kicked);
    return true;
}

void Session::Kick(RemotePlayer& p, RejectReason why) {
    PlayerSync& s = sync_[p.id];
    if (s.kicked) return;   // already on its way out
    s.kicked = true;
    Writer w;
    Encode(w, Reject{why});
    SendReliable(p.peer, w);
    net_.Kick(p.peer);
    log_("removed " + p.name + ": " + ToString(why));
}

void Session::UpdateInterest() {
    for (auto& [id, e] : entities_) e.keep = false;

    auto ensure = [this](const Handle& h, bool squad) -> Entity& {
        auto it = byHandle_.find(h);
        if (it != byHandle_.end()) {
            Entity& e = entities_[it->second];
            e.keep = true;
            return e;
        }
        // The same character under a new handle (it died, or changed squad): it keeps its netId,
        // and everyone is told which handle it replaces.
        const uint64_t id = world_.Identity(h);
        if (auto known = id ? byIdentity_.find(id) : byIdentity_.end(); known != byIdentity_.end() && entities_.count(known->second)) {
            Entity& e = entities_[known->second];
            const Handle previous = e.handle;
            byHandle_.erase(previous);
            e.handle = h;
            byHandle_[h] = e.netId;
            for (auto& o : owners_) if (o.first == previous) o.first = h;
            e.keep = true;
            for (auto& [pid, p] : players_) if (p.inGame) SendBind(e, p.peer, previous);
            if (e.squad) controllableDirty_ = true;
            return e;
        }
        Entity e;
        e.netId = nextNetId_++;
        e.handle = h;
        e.identity = id;
        if (id) byIdentity_[id] = e.netId;
        e.squad = squad;
        e.owner = squad ? hostId_ : 0;
        e.hasSpawn = world_.ReadSpawnInfo(h, e.spawn);
        if (squad)
            for (const auto& [oh, pid] : owners_) if (oh == h) e.owner = pid;
        e.keep = true;
        byHandle_[h] = e.netId;
        Entity& ref = entities_[e.netId] = std::move(e);
        for (auto& [pid, p] : players_) if (p.inGame) SendBind(ref, p.peer);
        if (squad) controllableDirty_ = true;
        return ref;
    };

    // The squad is always replicated, and its members are the centres of interest.
    scratchHandles_.clear();
    world_.PlayerCharacters(scratchHandles_);
    std::vector<Vec3> centers;
    for (const Handle& h : scratchHandles_) {
        if (!h.valid()) continue;
        ensure(h, true);
        EntityState st;
        if (world_.Read(h, st)) centers.push_back(st.pos);
    }

    if (!centers.empty()) {
        scratchHandles_.clear();
        world_.NearbyCharacters(centers, cfg_.interestRadius, scratchHandles_);
        for (const Handle& h : scratchHandles_) if (h.valid()) ensure(h, false);
        // hysteresis: an NPC slightly outside the radius stays bound, to avoid bind/unbind flapping
        const float outer = cfg_.interestRadius * kInterestHysteresis;
        for (auto& [id, e] : entities_) {
            if (e.keep || e.squad || cfg_.interestRadius <= 0) continue;
            EntityState st;
            if (!world_.Read(e.handle, st)) continue;
            for (const Vec3& c : centers) if (Dist(c, st.pos) <= outer) { e.keep = true; break; }
        }
    }

    for (auto it = entities_.begin(); it != entities_.end();) {
        if (it->second.keep) { ++it; continue; }
        Writer w;
        Encode(w, Unbind{it->first});
        BroadcastReliable(w, true);
        if (it->second.squad) controllableDirty_ = true;
        for (auto& [pid, s] : sync_) s.sent.erase(it->first);
        byHandle_.erase(it->second.handle);
        if (auto bi = byIdentity_.find(it->second.identity); bi != byIdentity_.end() && bi->second == it->first) byIdentity_.erase(bi);
        it = entities_.erase(it);
    }
}

void Session::SendSnapshots(double now) {
    stateCache_.clear();
    for (auto& [id, e] : entities_) {
        EntityState st;
        if (!world_.Read(e.handle, st)) continue;
        st.netId = id;
        Handle target;
        if (world_.ReadCombat(e.handle, target)) {
            auto t = byHandle_.find(target);
            st.combatTarget = t != byHandle_.end() ? t->second : 0;
        }
        stateCache_[id] = st;
    }
    for (auto& [pid, p] : players_) {
        if (!p.inGame) continue;
        auto& sent = sync_[pid].sent;
        Snapshot s;
        s.tick = ++tick_;
        s.hostTime = now;
        for (const auto& [id, st] : stateCache_) {
            Sent& last = sent[id];
            const bool moving = (st.flags & kFlagMoving) != 0;
            if (!moving && !StateChanged(st, last.state) && now - last.at < cfg_.refreshInterval) continue;
            last.state = st;
            last.at = now;
            s.entities.push_back(st);
        }
        if (s.entities.empty()) continue;
        for (const auto& pkt : EncodeSnapshot(s)) net_.Send(p.peer, kChanSnapshot, pkt.data(), pkt.size(), false);
    }
}

void Session::SendVitals(double now) {
    vitalsCache_.clear();
    for (auto& [id, e] : entities_) {
        EntityVitals v;
        if (world_.ReadVitals(e.handle, v)) { v.netId = id; vitalsCache_[id] = std::move(v); }
    }
    for (auto& [pid, p] : players_) {
        if (!p.inGame) continue;
        auto& sent = sync_[pid].sent;
        VitalsMsg m;
        m.tick = tick_;
        for (const auto& [id, v] : vitalsCache_) {
            Sent& last = sent[id];
            if (!VitalsChanged(v, last.vitals) && now - last.vitalsAt < kVitalsRefresh) continue;
            last.vitals = v;
            last.vitalsAt = now;
            m.entities.push_back(v);
        }
        if (m.entities.empty()) continue;
        for (const auto& pkt : EncodeVitals(m)) net_.Send(p.peer, kChanVitals, pkt.data(), pkt.size(), false);
    }
}

void Session::SendBind(const Entity& e, PeerId to, const Handle& previous) {
    Bind b;
    b.netId = e.netId;
    b.handle = e.handle;
    b.previous = previous;
    b.owner = e.owner;
    b.squad = e.squad;
    b.hasSpawn = e.hasSpawn;
    b.spawn = e.spawn;
    Writer w;
    Encode(w, b);
    SendReliable(to, w);
}

void Session::Assign(const Handle& h, uint8_t playerId) {
    if (!isHost()) return;
    if (playerId != hostId_ && !players_.count(playerId)) return;
    auto it = std::find_if(owners_.begin(), owners_.end(), [&](auto& o) { return o.first == h; });
    if (it != owners_.end()) it->second = playerId; else owners_.emplace_back(h, playerId);
    if (Entity* e = entityByHandle(h); e && e->squad) {
        e->owner = playerId;
        for (auto& [pid, p] : players_) if (p.inGame) SendBind(*e, p.peer);  // Bind doubles as an ownership update
    }
    controllableDirty_ = true;
}

void Session::PushControllable() {
    controllableDirty_ = false;
    std::vector<Handle> mine;
    for (auto& [id, e] : entities_) if (e.squad && e.owner == localId_) mine.push_back(e.handle);
    world_.SetControllable(mine);
}

void Session::HostPacket(PeerId peer, Msg type, Reader& r) {
    RemotePlayer* pl = playerByPeer(peer);
    if (type == Msg::Hello) {
        if (pl) return;  // duplicate hello
        Hello h;
        auto reject = [&](RejectReason why) {
            Writer w;
            Encode(w, Reject{why});
            SendReliable(peer, w);
            net_.Kick(peer);
            pendingPeers_.erase(peer);
            log_(std::string("rejected a player: ") + ToString(why));
        };
        if (!Decode(r, h) || h.protocol != kProtocolVersion) return reject(RejectReason::BadProtocol);
        if (h.gameBuild != world_.GameBuild()) return reject(RejectReason::GameMismatch);
        if (h.modsHash != world_.ModsHash()) return reject(RejectReason::ModsMismatch);
        if (!ValidName(h.name)) return reject(RejectReason::BadName);
        uint8_t id = 0;
        for (uint8_t i = 2; i <= kMaxPlayers; ++i) if (!players_.count(i)) { id = i; break; }
        if (!id) return reject(RejectReason::Full);
        // Names identify each player's own character, so they are unique in a session: a second
        // "Player" becomes "Player 2".
        bool taken = h.name == cfg_.name;
        for (auto& [pid, p] : players_) taken |= p.name == h.name;
        if (taken) h.name = h.name.substr(0, kMaxNameLen - 4) + " " + std::to_string(id);
        pendingPeers_.erase(peer);

        Welcome wm;
        wm.yourId = id;
        wm.hostTime = clock_();
        wm.players.push_back({hostId_, cfg_.name});
        for (auto& [pid, p] : players_) wm.players.push_back({pid, p.name});
        Writer w;
        Encode(w, wm);
        SendReliable(peer, w);

        Writer j;
        Encode(j, PlayerInfo{id, h.name});
        BroadcastReliable(j, false);

        RemotePlayer np;
        np.id = id;
        np.name = h.name;
        np.peer = peer;
        np.inGame = false;
        players_[id] = np;
        PlayerSync s;
        s.joinedAt = clock_();
        sync_[id] = std::move(s);
        AddChat("* " + h.name + " is joining...");
        return;
    }
    if (!pl) return;  // everything else requires a completed handshake

    switch (type) {
    case Msg::Ready: {
        ReadyMsg m;
        PlayerSync& s = sync_[pl->id];
        if (!Decode(r, m) || pl->inGame || !s.worldSent) break;
        s.readyHash = m.worldHash;
        s.readyPending = true;   // verified on the next live tick
        break;
    }
    case Msg::Command: {
        Command c;
        if (pl->inGame && Decode(r, c) && pendingCommands_.size() < 1024) pendingCommands_.emplace_back(pl->id, c);
        break;
    }
    case Msg::InvOp: {
        InvOp op;
        if (pl->inGame && Decode(r, op) && pendingInvOps_.size() < 256) pendingInvOps_.emplace_back(pl->id, op);
        break;
    }
    case Msg::Chat: {
        Chat c;
        if (!Decode(r, c)) break;
        c.from = pl->id;
        c.text = SanitizeChat(c.text);
        if (c.text.empty()) break;
        Writer w;
        Encode(w, c);
        BroadcastReliable(w, false);
        AddChat(pl->name + ": " + c.text);
        break;
    }
    case Msg::Ping: {
        Ping p;
        if (!Decode(r, p)) break;
        Writer w;
        EncodePing(w, p, true);
        SendReliable(peer, w);
        break;
    }
    default: break;  // host ignores host-bound-only messages from clients
    }
}

void Session::SendInventories(double now, bool force, PeerId onlyTo) {
    (void)now;
    for (auto& [id, e] : entities_) {
        std::vector<ItemState> items;
        if (!world_.ReadInventory(e.handle, items)) continue;
        const uint64_t h = InventoryHash(items);
        if (!force && h == e.invHash) continue;
        InventoryMsg m;
        m.netId = id;
        m.items = std::move(items);
        Writer w(1024);
        Encode(w, m);
        if (onlyTo != kNoPeer) {
            SendReliable(onlyTo, w);
        } else {
            e.invHash = h;
            BroadcastReliable(w, true);
        }
    }
}

// A client player may move items out of its own characters and, like the game allows, out of
// NPCs that are knocked out or dead, into its own characters. Never out of a conscious NPC (it
// still uses its gear) nor out of another player's characters.
void Session::HostInvOp(uint8_t from, const InvOp& op) {
    auto src = entities_.find(op.fromNetId);
    if (src == entities_.end()) return;
    EntityState st;
    EntityVitals v;
    const bool srcDown = (world_.Read(src->second.handle, st) && (st.flags & (kFlagDown | kFlagDead)) != 0) ||
                         (world_.ReadVitals(src->second.handle, v) && (v.flags & (kVitUnconscious | kVitDead)) != 0);
    const bool srcOk = (src->second.squad && src->second.owner == from) || (!src->second.squad && srcDown);
    if (op.kind == InvOpKind::Drop) {
        if (src->second.owner == from) world_.ExecuteInvOp(src->second.handle, src->second.handle, op);
        src->second.invHash = 0;
        return;
    }
    auto dst = entities_.find(op.toNetId);
    if (dst == entities_.end() || !srcOk || !dst->second.squad || dst->second.owner != from) {
        log_("refused an inventory move from player " + std::to_string(from));
        src->second.invHash = 0;   // resend the true state so the client's prediction is undone
        if (dst != entities_.end()) dst->second.invHash = 0;
        return;
    }
    world_.ExecuteInvOp(src->second.handle, dst->second.handle, op);
    // Whatever happened, everyone (the requester first) gets the real state of both inventories.
    src->second.invHash = 0;
    dst->second.invHash = 0;
}

// Client: compare what our characters hold with what the host last told us; a difference is
// the local player moving things in the inventory UI. Turn it into item movements for the host.
void Session::ClientInventoryDiff(double now) {
    if (now < nextInvDiff_) return;
    nextInvDiff_ = now + 0.2;
    struct Delta { uint32_t netId; ItemState item; };
    std::vector<Delta> gone, added;
    std::vector<uint32_t> involved;
    for (auto& [id, e] : entities_) {
        if (!e.present || !e.haveInv || e.invDirty || now < e.invPendingUntil) continue;
        std::vector<ItemState> local;
        if (!world_.ReadInventory(e.handle, local) || local == e.inv) continue;
        // multiset difference (items keep their place unless moved)
        std::vector<bool> used(local.size(), false);
        for (const auto& h : e.inv) {
            bool found = false;
            for (size_t i = 0; i < local.size() && !found; ++i)
                if (!used[i] && local[i] == h) { used[i] = true; found = true; }
            if (!found) gone.push_back({id, h});
        }
        for (size_t i = 0; i < local.size(); ++i) if (!used[i]) added.push_back({id, local[i]});
        involved.push_back(id);
    }
    if (gone.empty() && added.empty()) return;
    // pair each disappeared stack with an appeared one of the same kind
    std::vector<bool> addUsed(added.size(), false);
    size_t sent = 0;
    for (const auto& g : gone) {
        for (size_t i = 0; i < added.size(); ++i) {
            if (addUsed[i] || !added[i].item.sameKind(g.item)) continue;
            addUsed[i] = true;
            InvOp op;
            op.kind = InvOpKind::Move;
            op.fromNetId = g.netId;
            op.toNetId = added[i].netId;
            op.item = g.item;
            op.item.quantity = std::min(g.item.quantity, added[i].item.quantity);
            op.toSection = added[i].item.section;
            op.toX = added[i].item.x;
            op.toY = added[i].item.y;
            Writer w;
            Encode(w, op);
            SendReliable(net_.serverPeer(), w);
            ++sent;
            break;
        }
    }
    for (uint32_t id : involved) {
        auto it = entities_.find(id);
        if (it == entities_.end()) continue;
        if (sent) it->second.invPendingUntil = now + 3.0;   // wait for the host's answer
        else it->second.invDirty = true;                     // nothing we can ask for: back to the host's state
    }
}

void Session::ApplyCommand(uint8_t from, const Command& c) {
    auto it = entities_.find(c.netId);
    if (it == entities_.end()) return;
    if (!it->second.squad || it->second.owner != from) {
        log_("ignored command for a character player " + std::to_string(from) + " does not own");
        return;
    }
    world_.Order(it->second.handle, c);
}

// ============================== client ==============================

void Session::ClientTick(double now, bool live) {
    switch (state_) {
    case SessionState::Connecting:
    case SessionState::Handshake:
        if (now - connectStarted_ > kConnectTimeout) Fail("no answer from host (wrong address, port closed or firewall?)");
        return;
    case SessionState::Downloading:
        if (now - connectStarted_ > cfg_.loadTimeout) { Fail("downloading the host's world timed out"); return; }
        if (dlComplete_ && !importStarted_) {
            std::string err;
            importGeneration_ = world_.WorldGeneration();
            if (!world_.BeginWorldImport(dlFiles_, &err)) { Fail("cannot load the host's world: " + err); return; }
            dlFiles_.clear();
            dlFiles_.shrink_to_fit();
            importStarted_ = true;
            loadStarted_ = now;
            loadStableSince_ = 0;
            sawOtherWorld_ = false;
            state_ = SessionState::Loading;
            log_("host world downloaded, loading it");
        }
        return;
    case SessionState::Loading:
        if (now - loadStarted_ > cfg_.loadTimeout) {
            Fail(sawOtherWorld_ ? "the loaded world differs from the host's" : "loading the host's world timed out");
            return;
        }
        // The game may run frames while a world is still half-built: only accept a world that
        // matches the host's for a full second.
        if (!live || world_.WorldGeneration() == importGeneration_) { loadStableSince_ = 0; return; }
        if (world_.Fingerprint() != dlHash_) { sawOtherWorld_ = true; loadStableSince_ = 0; return; }
        if (loadStableSince_ == 0) loadStableSince_ = now;
        if (now - loadStableSince_ >= 1.0) {
            const uint64_t fp = world_.Fingerprint();
            Writer w;
            Encode(w, ReadyMsg{fp});
            SendReliable(net_.serverPeer(), w);
            state_ = SessionState::Connected;
            lastLive_ = now;
            world_.SetRole(true, true);
            controllableDirty_ = true;
            AddChat("* in the host's world as player " + std::to_string(localId_));
        }
        return;
    default: break;
    }

    // Connected
    if (live) lastLive_ = now;
    else if (now - lastLive_ > cfg_.worldLostTimeout) { Fail("world unloaded"); return; }
    if (!live) return;

    if (haveTime_) world_.SetTime(hostTime_);
    for (const Handle& h : despawnQueue_) world_.Despawn(h);
    despawnQueue_.clear();
    if (controllableDirty_) PushControllable();

    // NPCs stream in and out of the local world as zones load: re-resolve them regularly.
    const bool fullCheck = now >= nextPresenceCheck_;
    if (fullCheck) nextPresenceCheck_ = now + kPresenceInterval;
    uint32_t missing = 0;
    for (auto& [id, e] : entities_) {
        if (fullCheck || !e.checked) {
            e.present = world_.Exists(e.handle);
            e.checked = true;
        }
        if (e.present) e.missingSince = -1;
        else if (e.missingSince < 0) e.missingSince = now;
        if (e.squad && !e.present) ++missing;
    }
    // Our own characters are never recreated (a missing one means a different save); another
    // player's can be: it may have been made for a player who joined after we did.
    auto spawnable = [&](const Entity& e) {
        return !e.present && (!e.squad || e.owner != localId_) && e.hasSpawn && !e.buf.empty() && now - e.missingSince >= kSpawnGrace;
    };
    // Characters the local game made on its own have no place in the host's world: they stand in
    // for missing host characters of the same kind, or go away.
    if (fullCheck && cfg_.interestRadius <= 0) {   // with a radius we do not know everything the host has
        std::vector<Handle> known;
        std::vector<MissingChar> lacking;
        known.reserve(entities_.size());
        for (auto& [id, e] : entities_) {
            known.push_back(e.handle);
            if (spawnable(e)) lacking.push_back({e.handle, e.spawn, e.buf.back().s.pos});
        }
        std::vector<Handle> adopted;
        world_.Reconcile(known, lacking, now, adopted);
        for (const Handle& h : adopted)
            if (Entity* e = entityByHandle(h)) {
                e->spawned = true;
                e->present = world_.Exists(h);
                if (e->present) e->missingSince = -1;
            }
    }
    // Still not in our world (the host spawned it after the save): create a stand-in where the
    // host has it. Only right after Reconcile, which may have found a local character instead.
    for (auto& [id, e] : entities_) {
        if (!fullCheck || !spawnable(e) || now < e.nextSpawnTry) continue;
        ++e.spawnAttempts;
        e.nextSpawnTry = now + (e.spawnAttempts < 3 ? 2.0 : 15.0);   // keep trying, slowly: things change
        if (world_.Spawn(e.handle, e.spawn, e.buf.back().s)) {
            e.spawned = true;
            e.present = world_.Exists(e.handle);
            if (e.present) e.missingSince = -1;
        }
    }
    if (missing != missingSquad_ && missing > missingSquad_)
        log_(std::to_string(missing) + " host squad members are missing in the local world");
    missingSquad_ = missing;

    // Local orders never execute locally: they become commands, the host runs them, and the
    // result comes back through snapshots.
    scratchOrders_.clear();
    world_.TakeLocalOrders(scratchOrders_);
    for (auto& [h, cmd] : scratchOrders_) {
        Entity* e = entityByHandle(h);
        if (!e || !e->squad || e->owner != localId_) continue;
        Command c = cmd;
        c.seq = ++cmdSeq_;
        c.netId = e->netId;
        Writer w;
        Encode(w, c);
        SendReliable(net_.serverPeer(), w);
    }

    if (offsetValid_) {
        const double renderTime = now + offset_ - cfg_.interpDelay;
        for (auto& [id, e] : entities_) {
            if (!e.present || e.buf.empty()) continue;
            world_.Apply(e.handle, Interpolate(e, renderTime), e.buf.back().s);
            // Melee: fight the same target as on the host (the swings are animated locally, the
            // outcome comes from the host's vitals). Re-imposed now and then in case it lapsed.
            const uint32_t want = e.buf.back().s.combatTarget;
            if (want != e.combatApplied || (want && now >= e.combatReapply)) {
                auto t = want ? entities_.find(want) : entities_.end();
                if (!want) world_.ApplyCombat(e.handle, false, Handle{});
                else if (t != entities_.end() && t->second.present) world_.ApplyCombat(e.handle, true, t->second.handle);
                e.combatApplied = want;
                e.combatReapply = now + 2.0;
            }
        }
    }
    ClientInventoryDiff(now);
    for (auto& [id, e] : entities_) {
        if (!e.present || !e.invDirty || now < e.invRetry) continue;
        if (world_.ApplyInventory(e.handle, e.inv)) { e.invDirty = false; e.invFailures = 0; continue; }
        // The game would not lay it out exactly like the host: retry a few times, then stop
        // (and never mistake that difference for a player action: see ClientInventoryDiff).
        e.invRetry = now + 2.0;
        if (++e.invFailures >= 3) { e.invDirty = false; e.haveInv = false; }
    }
    // Local simulation (bleeding, healing...) keeps nudging health: re-impose the host's values.
    const bool reapply = now >= nextVitalsApply_;
    if (reapply) nextVitalsApply_ = now + kVitalsReapply;
    for (auto& [id, e] : entities_) {
        if (!e.present || !e.haveVitals || !(reapply || e.vitalsDirty)) continue;
        world_.ApplyVitals(e.handle, e.vitals);
        e.vitalsDirty = false;
    }

    if (now >= nextPing_) {
        nextPing_ = now + kPingInterval;
        rttMs_ = net_.stats(net_.serverPeer()).rttMs;
    }
}

EntityState Session::Interpolate(const Entity& e, double rt) const {
    const auto& b = e.buf;
    if (rt <= b.front().t) return b.front().s;
    if (rt >= b.back().t) return b.back().s;  // no extrapolation: hold the newest known state
    for (size_t i = 1; i < b.size(); ++i) {
        if (b[i].t < rt) continue;
        const Sample& a = b[i - 1];
        const Sample& c = b[i];
        if (Dist(a.s.pos, c.s.pos) > cfg_.snapDistance) return c.s;  // teleport: don't sweep across
        const float t = float((rt - a.t) / std::max(1e-6, c.t - a.t));
        EntityState out = c.s;
        out.pos = Lerp(a.s.pos, c.s.pos, t);
        out.rot = Nlerp(a.s.rot, c.s.rot, t);
        out.flags = t < 0.5f ? a.s.flags : c.s.flags;
        return out;
    }
    return b.back().s;
}

void Session::ClientPacket(Msg type, Reader& r) {
    switch (type) {
    case Msg::Welcome: {
        Welcome m;
        if (!Decode(r, m) || state_ != SessionState::Handshake) break;
        localId_ = m.yourId;
        players_.clear();
        for (auto& p : m.players) if (p.id != localId_) players_[p.id] = RemotePlayer{p.id, p.name};
        offset_ = m.hostTime - clock_();
        offsetValid_ = true;
        state_ = SessionState::Downloading;
        dlInfo_ = {};
        dlFiles_.clear();
        dlBytes_ = 0;
        dlComplete_ = importStarted_ = false;
        AddChat("* joined as player " + std::to_string(localId_) + ", receiving the host's world...");
        break;
    }
    case Msg::Reject: {
        Reject m;
        Fail(Decode(r, m) ? std::string("rejected by host: ") + ToString(m.reason) : "rejected by host");
        break;
    }
    case Msg::WorldBegin: {
        WorldBegin m;
        if (state_ != SessionState::Downloading || !Decode(r, m)) { Fail("bad world transfer from host"); break; }
        dlInfo_ = m;
        dlFiles_.assign(m.fileCount, WorldFile{});
        dlBytes_ = 0;
        break;
    }
    case Msg::WorldChunk: {
        WorldChunk c;
        if (state_ != SessionState::Downloading || !Decode(r, c) || c.file >= dlFiles_.size()) { Fail("bad world transfer from host"); break; }
        WorldFile& f = dlFiles_[c.file];
        if (c.offset == 0) {
            if (!f.path.empty() || !f.data.empty()) { Fail("bad world transfer from host"); break; }
            f.path = c.path;
            f.data.reserve(size_t(c.fileSize));
        }
        if (f.path.empty() || c.offset != f.data.size() || dlBytes_ + c.data.size() > dlInfo_.totalBytes) {
            Fail("bad world transfer from host");
            break;
        }
        f.data.insert(f.data.end(), c.data.begin(), c.data.end());
        dlBytes_ += c.data.size();
        break;
    }
    case Msg::WorldEnd: {
        WorldEnd m;
        if (state_ != SessionState::Downloading || !Decode(r, m) || dlBytes_ != dlInfo_.totalBytes) { Fail("bad world transfer from host"); break; }
        for (auto& f : dlFiles_) if (f.path.empty()) { Fail("bad world transfer from host"); return; }
        dlHash_ = m.worldHash;
        dlComplete_ = true;
        break;
    }
    case Msg::PlayerJoined: {
        PlayerInfo m;
        if (!Decode(r, m) || m.id == localId_) break;
        players_[m.id] = RemotePlayer{m.id, m.name};
        AddChat("* " + m.name + " is joining...");
        break;
    }
    case Msg::PlayerLeft: {
        PlayerLeft m;
        if (!Decode(r, m)) break;
        auto it = players_.find(m.id);
        if (it != players_.end()) { AddChat("* " + it->second.name + " left"); players_.erase(it); }
        break;
    }
    case Msg::Chat: {
        Chat m;
        if (!Decode(r, m)) break;
        auto it = players_.find(m.from);
        const std::string who = m.from == localId_ ? cfg_.name : (it != players_.end() ? it->second.name : "?");
        AddChat(who + ": " + SanitizeChat(m.text));
        break;
    }
    case Msg::Bind: {
        Bind m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        Entity& e = entities_[m.netId];
        if (e.netId != 0 && e.handle != m.handle) {
            // the host's character got a new handle: ours stays the same character
            if (m.previous.valid() && m.previous == e.handle) world_.Rehandle(e.handle, m.handle);
            byHandle_.erase(e.handle);
            e.checked = false;
        }
        e.netId = m.netId;
        e.handle = m.handle;
        e.owner = m.owner;
        e.squad = m.squad;
        e.hasSpawn = m.hasSpawn;
        e.spawn = m.spawn;
        byHandle_[m.handle] = m.netId;
        if (e.squad) controllableDirty_ = true;
        break;
    }
    case Msg::Unbind: {
        Unbind m;
        if (!Decode(r, m)) break;
        auto it = entities_.find(m.netId);
        if (it == entities_.end()) break;
        if (it->second.squad) controllableDirty_ = true;
        if (it->second.spawned) despawnQueue_.push_back(it->second.handle);   // removed on the next live tick
        byHandle_.erase(it->second.handle);
        entities_.erase(it);
        break;
    }
    case Msg::Snapshot: {
        Snapshot s;
        if (state_ != SessionState::Connected || !Decode(r, s)) break;
        const double now = clock_();
        // Clock sync: the least-delayed packet gives the best estimate of host-local offset.
        const double sample = s.hostTime - now;
        if (!offsetValid_ || sample > offset_) { offset_ = sample; offsetValid_ = true; }
        else offset_ -= 0.001 / cfg_.snapshotRate;  // slow decay keeps tracking if the estimate overshot
        for (const EntityState& st : s.entities) {
            auto it = entities_.find(st.netId);
            if (it == entities_.end()) continue;  // snapshot raced ahead of its Bind
            auto& buf = it->second.buf;
            if (!buf.empty() && buf.back().t >= s.hostTime) continue;  // stale or duplicate
            // Unchanged entities are only refreshed now and then: after such a gap, assume the
            // entity stood still until just before this sample instead of sliding across the gap.
            const double step = 1.0 / cfg_.snapshotRate;
            if (!buf.empty() && s.hostTime - buf.back().t > 1.5 * step) buf.push_back({s.hostTime - step, buf.back().s});
            buf.push_back({s.hostTime, st});
            while (buf.size() > 2 && buf.front().t < s.hostTime - kBufferSeconds) buf.pop_front();
        }
        break;
    }
    case Msg::Vitals: {
        VitalsMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        for (auto& v : m.entities) {
            auto it = entities_.find(v.netId);
            if (it == entities_.end()) continue;
            it->second.vitals = std::move(v);
            it->second.haveVitals = true;
            it->second.vitalsDirty = true;
        }
        break;
    }
    case Msg::TimeState: {
        TimeState t;
        if (Decode(r, t)) { hostTime_ = t; haveTime_ = true; }
        break;
    }
    case Msg::Weather: {
        WeatherMsg m;
        if (state_ == SessionState::Connected && Decode(r, m)) world_.ApplyWeather(m.regions);
        break;
    }
    case Msg::Effects: {
        EffectsMsg m;
        if (state_ == SessionState::Connected && Decode(r, m)) world_.ApplyEffects(m);
        break;
    }
    case Msg::Inventory: {
        InventoryMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        auto it = entities_.find(m.netId);
        if (it == entities_.end()) break;
        it->second.inv = std::move(m.items);
        it->second.haveInv = true;
        it->second.invDirty = true;
        it->second.invRetry = 0;
        it->second.invPendingUntil = 0;   // the host answered: our prediction is settled either way
        it->second.invFailures = 0;
        break;
    }
    case Msg::Pong: break;
    default: break;
    }
}

// ============================== shared ==============================

void Session::OnPacket(PeerId peer, uint8_t chan, const uint8_t* data, size_t size) {
    Reader r(data, size);
    const auto type = PeekType(r);
    if (!type) return;
    // Each message type travels on exactly one channel.
    const uint8_t expected = *type == Msg::Snapshot ? kChanSnapshot : *type == Msg::Vitals ? kChanVitals : kChanReliable;
    if (chan != expected) return;
    if (net_.isServer()) HostPacket(peer, *type, r);
    else ClientPacket(*type, r);
}

void Session::OnDisconnect(PeerId peer) {
    if (!net_.isServer()) {
        if (state_ != SessionState::Failed) Fail(state_ == SessionState::Connected ? "lost connection to host" : "could not connect to host");
        return;
    }
    pendingPeers_.erase(peer);
    RemotePlayer* pl = playerByPeer(peer);
    if (!pl) return;
    const uint8_t id = pl->id;
    const std::string name = pl->name;
    players_.erase(id);
    sync_.erase(id);
    // the leaver's characters go back to the host so they are never left uncontrolled
    for (auto& o : owners_) if (o.second == id) o.second = hostId_;
    for (auto& [nid, e] : entities_) {
        if (e.owner != id) continue;
        e.owner = hostId_;
        for (auto& [pid, p] : players_) if (p.inGame) SendBind(e, p.peer);
    }
    controllableDirty_ = true;
    Writer w;
    Encode(w, PlayerLeft{id});
    BroadcastReliable(w, false);
    AddChat("* " + name + " left");
}

void Session::SendChat(const std::string& text) {
    const std::string clean = SanitizeChat(text);
    if (clean.empty()) return;
    Chat c;
    c.text = clean;
    Writer w;
    if (isHost()) {
        c.from = hostId_;
        Encode(w, c);
        BroadcastReliable(w, false);
        AddChat(cfg_.name + ": " + clean);
    } else if (state_ == SessionState::Connected) {
        Encode(w, c);
        SendReliable(net_.serverPeer(), w);
    }
}

void Session::SendReliable(PeerId to, const Writer& w) { net_.Send(to, kChanReliable, w.data(), w.size(), true); }

void Session::BroadcastReliable(const Writer& w, bool inGameOnly, PeerId except) {
    // Only peers that completed the handshake receive game traffic.
    for (auto& [pid, p] : players_)
        if (p.peer != except && (!inGameOnly || p.inGame)) net_.Send(p.peer, kChanReliable, w.data(), w.size(), true);
}

void Session::AddChat(const std::string& line) {
    chat_.push_back(line);
    while (chat_.size() > kMaxChatLines) chat_.pop_front();
    log_(line);
}

RemotePlayer* Session::playerByPeer(PeerId p) {
    for (auto& [id, pl] : players_) if (pl.peer == p) return &pl;
    return nullptr;
}

Session::Entity* Session::entityByHandle(const Handle& h) {
    auto it = byHandle_.find(h);
    if (it == byHandle_.end()) return nullptr;
    auto e = entities_.find(it->second);
    return e == entities_.end() ? nullptr : &e->second;
}

uint8_t Session::ownerOf(const Handle& h) const {
    auto it = byHandle_.find(h);
    if (it == byHandle_.end()) return 0;
    auto e = entities_.find(it->second);
    return e == entities_.end() ? 0 : e->second.owner;
}

size_t Session::npcCount() const {
    size_t n = 0;
    for (auto& [id, e] : entities_) if (!e.squad) ++n;
    return n;
}

uint32_t Session::missingNpcs() const {
    uint32_t n = 0;
    for (auto& [id, e] : entities_) if (!e.squad && e.checked && !e.present) ++n;
    return n;
}

uint32_t Session::spawnedNpcs() const {
    uint32_t n = 0;
    for (auto& [id, e] : entities_) if (e.spawned) ++n;
    return n;
}

uint32_t Session::pingMs() const {
    if (isHost()) return 0;
    return rttMs_;
}

double Session::downloadProgress() const {
    if (state_ != SessionState::Downloading || dlInfo_.totalBytes == 0) return 0;
    return double(dlBytes_) / double(dlInfo_.totalBytes);
}

void Session::ForEachEntity(const std::function<void(uint32_t, const Handle&, uint8_t, bool, bool)>& fn) const {
    for (const auto& [id, e] : entities_) fn(id, e.handle, e.owner, e.squad, isHost() ? true : e.present);
}

bool Session::TargetOf(const Handle& h, EntityState& latest, EntityState& rendered) const {
    auto it = byHandle_.find(h);
    if (it == byHandle_.end()) return false;
    auto e = entities_.find(it->second);
    if (e == entities_.end() || e->second.buf.empty()) return false;
    latest = e->second.buf.back().s;
    rendered = Interpolate(e->second, clock_() + offset_ - cfg_.interpDelay);
    return true;
}

size_t Session::joiningPlayers() const {
    size_t n = 0;
    for (auto& [id, p] : players_) if (!p.inGame) ++n;
    return n;
}

} // namespace kc
