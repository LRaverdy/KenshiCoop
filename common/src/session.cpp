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
           a.combatTarget != b.combatTarget || a.gait != b.gait || std::fabs(a.pace - b.pace) > 0.1f || a.carrying != b.carrying;
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
    return std::fabs(a.blood - b.blood) > 0.05f || std::fabs(a.koTimer - b.koTimer) > 0.5f || std::fabs(a.hunger - b.hunger) > 0.5f ||
           a.flags != b.flags || a.parts != b.parts;
}

bool StatsChanged(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > 0.0005f) return true;
    return false;
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
            h.steamId = cfg_.steamId;
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

bool Session::Join(const std::string& address, uint16_t port, std::string* err, uint32_t mtu) {
    Leave();
    if (!net_.Connect(address, port, err, mtu)) return false;
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
    haveMoney_ = false;
    moneySent_ = false;
    dialog_ = DialogView{};
    dialogOwner_.clear();
    lastSquads_ = SquadsMsg{};
    haveSquads_ = false;
    pendingLooks_.clear();
    editRequest_ = 0;
    editingSent_ = false;
    logOut_.clear();
    logDropped_ = 0;
    walkingToContainers_.clear();
    containerAsks_.clear();
    pendingWindow_ = 0;
    windowOpenedAt_ = -1;
    trades_.clear();
    ResetBuildings();
    trade_ = ClientTrade{};
    ResetFactions();
    ResetDoors();   // lot A
    floorSent_.clear(); floors_.clear(); nextFloors_ = nextFloorsFull_ = 0; floorPlayers_ = 0;   // fix G6
    captiveSent_.clear(); captives_.clear(); captivesDirty_.clear();   // lot D: prisons
    haveMoneyBase_ = false;
    unsentSpend_ = 0;
    ResetRanged();   // lot C
    dialogReplies_.clear();
    holdForEditor_ = false;
    pendingAnswers_.clear();
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
    HostInvOps();
    if (!pendingInvOps_.empty()) nextInventory_ = 0;   // show the result right away
    pendingInvOps_.clear();
    for (const auto& a : pendingAnswers_) world_.DialogAnswer(a.dialogId, a.index);
    pendingAnswers_.clear();
    HostContainers(now);
    HostTrades(now);
    HostDoors(now);   // lot A
    HostCaptives(now);   // lot D: prisons
    HostRanged(now);   // lot C
    HostBuildings(now);
    HostFloors(now);   // fix G6
    // A player's new looks: applied here, then shown to everyone else.
    for (auto& [from, m] : pendingLooks_) {
        auto it = entities_.find(m.netId);
        if (it == entities_.end() || !it->second.squad || it->second.owner != from) { log_("ignored looks for a character the player does not own"); continue; }
        world_.ApplyAppearance(it->second.handle, m);
        Writer w;
        Encode(w, m);
        for (auto& [pid, p] : players_) if (p.inGame && pid != from) SendReliable(p.peer, w);
        log_("new looks for " + m.name);
    }
    pendingLooks_.clear();
    SendEditedAppearances();
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
    if (anyInGame && now >= nextSquads_) {
        nextSquads_ = now + 0.5;
        SendSquads(now);
    }
    if (anyInGame) SendDialogs();
    else { world_.TakeDialogEvents(scratchDialogs_); scratchDialogs_.clear(); }
    if (anyInGame && now >= nextProgress_) {
        nextProgress_ = now + 1.0;
        SendProgress(now);
    }
    if (anyInGame) SendFactions(now);   // lot B
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
    // Animations: every start/stop goes out the tick it happened; each character's current action
    // and modes once a second (newcomers, anything missed).
    {
        std::vector<std::pair<Handle, AnimEvent>> evs;
        const bool state = anyInGame && now >= animStateAt_;
        if (state) animStateAt_ = now + 1.0;
        world_.TakeAnimEvents(evs, state);
        AnimMsg m;
        for (auto& [h, e] : evs) {
            auto it = byHandle_.find(h);
            if (it == byHandle_.end()) continue;
            e.netId = it->second;
            m.events.push_back(std::move(e));
        }
        if (anyInGame && !m.events.empty()) {
            Writer out(1024);
            Encode(out, m);
            BroadcastReliable(out, true);
        }
    }
    // Items dropped and picked up: the tick it happened.
    {
        GroundMsg g;
        world_.TakeGroundEvents(g.events);
        if (anyInGame && !g.events.empty()) {
            Writer out(512);
            Encode(out, g);
            BroadcastReliable(out, true);
        }
    }
    // What every nearby character is playing, 15 times a second (unreliable, like snapshots).
    if (anyInGame && now >= nextAnimFrame_) {
        nextAnimFrame_ = std::max(nextAnimFrame_ + 1.0 / 15.0, now - 0.5 / 15.0);
        AnimFrameMsg m;
        m.hostTime = now;
        for (auto& [id, e] : entities_) {
            AnimFrame f;
            if (!world_.ReadAnimFrame(e.handle, f)) continue;
            f.netId = id;
            m.chars.push_back(std::move(f));
        }
        if (!m.chars.empty()) {
            const auto pkts = EncodeAnimFrames(m);
            for (auto& [pid, p] : players_)
                if (p.inGame)
                    for (const auto& pkt : pkts) net_.Send(p.peer, kChanSnapshot, pkt.data(), pkt.size(), false);
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

    // A player in the character editor: everyone waits for them (10 minutes at most).
    bool editing = false;
    for (auto& [pid, p] : players_) editing |= p.inGame && p.editing && now - p.editingSince < 600.0;
    if (joining.empty() && editing) {
        if (live && !holding_) { world_.HoldForJoin(true); holding_ = true; }
        holdForEditor_ = true;
        return;
    }
    holdForEditor_ = false;
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
                    bool created = false;
                    if (world_.EnsurePlayerCharacter(players_[id].name, players_[id].steamId, s.own, created)) {
                        Assign(s.own, id);
                        s.ownCreated = created;
                    }
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
    // Someone arrived while that save was being made: it has no character of theirs. Save again
    // (the ones already receiving the first save keep it).
    if (exportReady_ && cfg_.characterPerPlayer && live && holding_) {
        bool lateJoiner = false;
        for (uint8_t id : joining) lateJoiner |= !sync_[id].worldSent && !sync_[id].ownChecked;
        if (lateJoiner) {
            log_("a player arrived while the world was being saved: saving it again with their character");
            exportFiles_.clear();
            exportFiles_.shrink_to_fit();
            exportReady_ = false;
            return;
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
    AddChat("* " + p.name + " est dans la partie", "* " + p.name + " is in the world");
    squadsAt_ = -1e9;   // the newcomer gets the squads now
    PlayerSync& s = sync_[p.id];
    if (s.ownCreated && s.own.valid()) {
        if (auto it = byHandle_.find(s.own); it != byHandle_.end()) {
            Writer w;
            Encode(w, EditCharacter{it->second});
            SendReliable(p.peer, w);
        }
        s.ownCreated = false;
    }
}

bool Session::Configure(const std::string& name, uint16_t port) {
    if ((state_ != SessionState::Idle && state_ != SessionState::Failed) || !ValidName(name) || port == 0) return false;
    cfg_.name = name;
    cfg_.port = port;
    return true;
}

// Floors: on a client the characters are placed where the host's are, so they never take the
// stairs themselves and keep the floor they had; the game shows the floor of the selected
// character. The host sends each character's floor group when it changes (and all of them now
// and then, for whoever joined since).
void Session::HostFloors(double now) {
    if (now < nextFloors_) return;
    nextFloors_ = now + 0.25;
    size_t inGame = 0;
    for (auto& [pid, p] : players_) inGame += p.inGame;
    const bool full = now >= nextFloorsFull_ || inGame > floorPlayers_;   // a newcomer gets everyone's at once
    floorPlayers_ = inGame;
    if (full) nextFloorsFull_ = now + 5.0;
    FloorsMsg m;
    for (auto& [id, e] : entities_) {
        uint8_t g = 0;
        if (e.container || !world_.ReadFloor(e.handle, g)) continue;
        auto it = floorSent_.find(id);
        const bool changed = it == floorSent_.end() || it->second != g;
        if (!changed && !(full && g != 9)) continue;   // 9: ground floor, what a character has by default
        floorSent_[id] = g;
        if (m.entries.size() < kMaxEntitiesPerMsg) m.entries.push_back({id, g});
    }
    for (auto it = floorSent_.begin(); it != floorSent_.end();) it = entities_.count(it->first) ? std::next(it) : floorSent_.erase(it);
    if (m.entries.empty()) return;
    Writer w;
    Encode(w, m);
    BroadcastReliable(w, true);
}

void Session::ExpectStall(uint8_t playerId, double seconds) {
    auto it = players_.find(playerId);
    if (!isHost() || it == players_.end()) return;
    net_.ExpectSilence(it->second.peer, seconds);
    Writer w;
    Encode(w, StallMsg{uint16_t(std::clamp(seconds, 1.0, 600.0))});
    SendReliable(it->second.peer, w);
    net_.Flush();   // before this game freezes too, or the client's
    log_(it->second.name + " may freeze while loading a zone: the connection waits up to 2 minutes");
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
        if (it->second.container) {   // only the players who have it open know it
            const bool walking = std::any_of(walkingToContainers_.begin(), walkingToContainers_.end(), [&](const PendingContainer& p) { return p.netId == it->first; });
            if (!it->second.openBy.empty() || walking) { ++it; continue; }
            byHandle_.erase(it->second.handle);
            for (auto& [pid, s] : sync_) s.sent.erase(it->first);
            it = entities_.erase(it);
            continue;
        }
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
        Handle carried;
        if (world_.ReadCarry(e.handle, carried)) {
            auto t = byHandle_.find(carried);
            st.carrying = t != byHandle_.end() ? t->second : 0;
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

// Lines said aloud go to everyone who knows the speaker; a conversation window goes to the owner of
// the character in it (the host's game shows it to nobody).
void Session::SendDialogs() {
    world_.TakeDialogEvents(scratchDialogs_);
    if (scratchDialogs_.empty()) return;
    DialogMsg says;
    std::unordered_map<uint8_t, DialogMsg> perPlayer;
    for (auto& d : scratchDialogs_) {
        auto sp = byHandle_.find(d.speaker);
        DialogEvent e;
        e.kind = d.kind;
        e.dialogId = d.dialogId;
        e.netId = sp != byHandle_.end() ? sp->second : 0;
        e.text = std::move(d.text);
        e.shout = d.shout;
        e.replies = std::move(d.replies);
        if (d.kind == DialogKind::Say) {
            if (e.netId) says.events.push_back(std::move(e));
            continue;
        }
        auto pc = byHandle_.find(d.pc);
        auto ent = pc != byHandle_.end() ? entities_.find(pc->second) : entities_.end();
        uint8_t owner = 0;
        if (ent != entities_.end() && ent->second.squad) owner = ent->second.owner;
        else if (auto o = dialogOwner_.find(d.dialogId); o != dialogOwner_.end()) owner = o->second;
        if (!owner || owner == hostId_) continue;
        if (d.kind == DialogKind::Close) dialogOwner_.erase(d.dialogId);
        else dialogOwner_[d.dialogId] = owner;
        const std::string who = players_.count(owner) ? players_[owner].name : "?";
        if (d.kind == DialogKind::Open) log_("[" + who + "] conversation with " + e.text);
        if (d.kind == DialogKind::Text) {
            std::string r;
            for (size_t i = 0; i < e.replies.size(); ++i) r += (i ? " | " : "") + std::to_string(i + 1) + ". " + e.replies[i];
            log_("[" + who + "] they say: \"" + e.text.substr(0, 160) + "\"" + (r.empty() ? "" : "  answers: " + r));
            dialogReplies_[d.dialogId] = e.replies;
        }
        if (d.kind == DialogKind::Close) { log_("[" + who + "] conversation over"); dialogReplies_.erase(d.dialogId); }
        perPlayer[owner].events.push_back(std::move(e));
    }
    scratchDialogs_.clear();
    for (auto& [pid, p] : players_) {
        if (!p.inGame) continue;
        if (!says.events.empty()) {
            Writer w;
            Encode(w, says);
            SendReliable(p.peer, w);
        }
        if (auto it = perPlayer.find(pid); it != perPlayer.end()) {
            Writer w;
            Encode(w, it->second);
            SendReliable(p.peer, w);
        }
    }
}

namespace {
bool SameSquads(const SquadsMsg& a, const SquadsMsg& b) {
    if (a.squads.size() != b.squads.size()) return false;
    for (size_t i = 0; i < a.squads.size(); ++i)
        if (a.squads[i].name != b.squads[i].name || a.squads[i].members != b.squads[i].members) return false;
    return true;
}
} // namespace

// Squads are sent when they change, and every 10 s (a player who just arrived gets them too).
void Session::SendSquads(double now) {
    std::vector<IWorld::WorldSquad> ws;
    world_.ReadSquads(ws);
    SquadsMsg m;
    for (const auto& s : ws) {
        SquadInfo info;
        info.name = s.name.substr(0, kMaxNameLen * 4);
        for (const auto& h : s.members)
            if (auto it = byHandle_.find(h); it != byHandle_.end()) info.members.push_back(it->second);
        if (!info.members.empty()) m.squads.push_back(std::move(info));
    }
    if (m.squads.empty()) return;
    const bool changed = !haveSquads_ || !SameSquads(m, lastSquads_);
    if (!changed && now - squadsAt_ < 10.0) return;
    lastSquads_ = m;
    haveSquads_ = true;
    squadsAt_ = now;
    Writer w;
    Encode(w, m);
    BroadcastReliable(w, true);
}

// Characters edited in the game's character editor here: the host shows its own edits to everyone,
// a client sends the edits of its own characters to the host.
void Session::SendEditedAppearances() {
    world_.TakeEditedCharacters(scratchEdited_);
    for (const auto& h : scratchEdited_) {
        Entity* e = entityByHandle(h);
        if (!e || !e->squad) continue;
        if (isClient() && e->owner != localId_) continue;
        AppearanceMsg m;
        if (!world_.ReadAppearance(h, m)) continue;
        m.netId = e->netId;
        Writer w;
        Encode(w, m);
        if (isHost()) BroadcastReliable(w, true);
        else SendReliable(net_.serverPeer(), w);
        log_("sent the new looks of " + m.name + " (" + std::to_string(m.fields.size()) + " values)");
    }
}

bool Session::EditOwnCharacter() {
    if (!isClient()) return false;
    for (auto& [id, e] : entities_)
        if (e.squad && e.owner == localId_ && e.present) return world_.OpenCharacterEditor(e.handle);
    return false;
}

size_t Session::RequestResync(uint8_t playerId) {
    if (!isHost()) return 0;
    size_t n = 0;
    for (auto& [pid, p] : players_) {
        if (!p.inGame || (playerId && pid != playerId)) continue;
        Writer w;
        EncodeResync(w);
        SendReliable(p.peer, w);
        log_("resync: " + p.name + " reloads the host's world");
        ++n;
    }
    return n;
}

// A player looks into a container: their character walks there (like a loot order), then the
// container gets a netId and its items go to that player; it closes when they close it.
void Session::HostContainers(double now) {
    constexpr float kReach = 80.0f;   // big buildings (mines, farms): one cannot stand on their centre
    for (auto& [from, ask] : containerAsks_) {
        auto pl = players_.find(from);
        auto looter = entities_.find(ask.looterNetId);
        if (pl == players_.end() || looter == entities_.end() || !looter->second.squad || looter->second.owner != from) continue;
        Handle h;
        if (!world_.FindContainer(ask.sid, ask.pos, h)) {
            log_("[" + pl->second.name + "] container " + world_.TemplateName(ask.sid) + " not found here");
            continue;
        }
        if (world_.ContainerLocked(h)) {   // lot A: a locked chest is opened by picking its lock first
            log_("[" + pl->second.name + "] container " + world_.TemplateName(ask.sid) + " is locked: not opened");
            Chat c;
            c.from = 0;
            c.text = "C'est verrouillé : il faut d'abord crocheter la serrure.";
            Writer w;
            Encode(w, c);
            SendReliable(pl->second.peer, w);
            continue;
        }
        uint32_t id = 0;
        if (auto b = byHandle_.find(h); b != byHandle_.end()) id = b->second;
        else {
            Entity e;
            e.netId = nextNetId_++;
            e.handle = h;
            e.container = true;
            e.containerPos = ask.pos;
            e.keep = true;
            id = e.netId;
            byHandle_[h] = id;
            entities_[id] = std::move(e);
        }
        log_("[" + pl->second.name + "] goes to look into " + world_.TemplateName(ask.sid));
        if (world_.DistanceTo(looter->second.handle, ask.pos) > kReach) {
            Command c;
            c.kind = CommandKind::MoveTo;
            c.pos = ask.pos;
            world_.Order(looter->second.handle, c);
        }
        walkingToContainers_.push_back({from, ask.looterNetId, id, now + 30.0});
    }
    containerAsks_.clear();
    for (auto it = walkingToContainers_.begin(); it != walkingToContainers_.end();) {
        auto pl = players_.find(it->player);
        auto looter = entities_.find(it->looter);
        auto cont = entities_.find(it->netId);
        if (pl == players_.end() || looter == entities_.end() || cont == entities_.end() || now > it->until) { it = walkingToContainers_.erase(it); continue; }
        if (world_.DistanceTo(looter->second.handle, cont->second.containerPos) > kReach) { ++it; continue; }
        cont->second.openBy.insert(it->player);
        cont->second.invHash = 0;   // its items go out now
        ContainerOpened m;
        m.netId = it->netId;
        m.looterNetId = it->looter;
        world_.ContainerKind(cont->second.handle, m.sid);
        m.pos = cont->second.containerPos;
        Writer w;
        Encode(w, m);
        SendReliable(pl->second.peer, w);
        log_("[" + pl->second.name + "] opens " + world_.TemplateName(m.sid));
        it = walkingToContainers_.erase(it);
    }
    // a looter that walked away: the window closes
    for (auto& [id, e] : entities_) {
        if (!e.container) continue;
        for (auto o = e.openBy.begin(); o != e.openBy.end();) {
            if (InTrade(*o, id)) { ++o; continue; }   // a shop counter: see HostTrades
            bool near = false;
            for (auto& [lid, le] : entities_)
                if (le.squad && le.owner == *o && world_.DistanceTo(le.handle, e.containerPos) <= kReach * 2) { near = true; break; }
            if (near) { ++o; continue; }
            if (auto pl = players_.find(*o); pl != players_.end()) {
                Writer w;
                Encode(w, ContainerClose{id, "trop loin"});
                SendReliable(pl->second.peer, w);
            }
            o = e.openBy.erase(o);
        }
    }
}

bool Session::InTrade(uint8_t player, uint32_t container) const {
    auto t = trades_.find(player);
    return t != trades_.end() && std::find(t->second.counters.begin(), t->second.counters.end(), container) != t->second.counters.end();
}

void Session::EndTrade(uint8_t player, const std::string& reason) {
    auto t = trades_.find(player);
    if (t == trades_.end()) return;
    auto pl = players_.find(player);
    bool told = false;
    for (uint32_t id : t->second.counters) {
        auto e = entities_.find(id);
        if (e == entities_.end() || !e->second.openBy.erase(player) || pl == players_.end()) continue;
        Writer w;
        Encode(w, ContainerClose{id, told ? std::string{} : reason});
        SendReliable(pl->second.peer, w);
        told = true;
    }
    trades_.erase(t);
}

// A merchant's trade window asked for by the host's game for another player's character (its "let's
// trade" in their conversation): it opens on that player's screen, with the shop's counters (the
// containers it sells from) sent like open containers. The trade lasts while the player keeps the
// window open and stays near the merchant.
void Session::HostTrades(double now) {
    world_.TakeTradeRequests(scratchTradeReqs_);
    for (const auto& req : scratchTradeReqs_) {
        Entity* looter = entityByHandle(req.looter);
        if (!looter || !looter->squad || looter->owner == hostId_) continue;
        auto pl = players_.find(looter->owner);
        if (pl == players_.end() || !pl->second.inGame) continue;
        const std::string who = pl->second.name;
        Entity* trader = entityByHandle(req.trader);
        const std::string merchant = world_.CharacterNameOf(req.trader);
        TradeOpen m;
        m.looterNetId = looter->netId;
        m.traderNetId = trader ? trader->netId : 0;
        std::vector<IWorld::ShopCounter> counters;
        if (!trader || !world_.ShopCounters(req.trader, counters) || counters.empty()) {
            log_("[" + who + "] cannot trade with " + merchant + ": " + (trader ? "no shop counters (a travelling merchant)" : "the merchant is not followed"));
            if (!trader) continue;
            m.note = merchant + " n'a pas d'étal : le commerce avec les marchands ambulants n'est pas encore géré en multijoueur.";
            Writer w;
            Encode(w, m);
            SendReliable(pl->second.peer, w);
            continue;
        }
        EndTrade(pl->first, {});   // one trade window at a time
        HostTrade t;
        t.trader = trader->netId;
        t.looter = looter->netId;
        t.since = now;
        world_.MoneyOf(req.trader, m.traderMoney);
        t.traderMoney = m.traderMoney;
        for (const auto& c : counters) {
            if (t.counters.size() >= kMaxTradeCounters) break;
            uint32_t id = 0;
            if (auto b = byHandle_.find(c.handle); b != byHandle_.end()) id = b->second;
            else {
                Entity e;
                e.netId = nextNetId_++;
                e.handle = c.handle;
                e.container = true;
                e.containerPos = c.pos;
                e.keep = true;
                id = e.netId;
                byHandle_[c.handle] = id;
                entities_[id] = std::move(e);
            }
            Entity& e = entities_[id];
            if (!e.container) continue;   // never a character
            e.openBy.insert(pl->first);
            e.invHash = 0;                // its items go out now
            t.counters.push_back(id);
            m.counters.push_back({id, c.sid, c.pos});
        }
        trades_[pl->first] = t;
        Writer w;
        Encode(w, m);
        SendReliable(pl->second.peer, w);
        size_t stacks = 0;
        for (uint32_t id : t.counters) {
            std::vector<ItemState> items;
            if (world_.ReadInventory(entities_[id].handle, items)) stacks += items.size();
        }
        log_("[" + who + "] trades with " + merchant + " (" + std::to_string(t.counters.size()) + " shop counters holding " +
             std::to_string(stacks) + " stacks, the merchant has " + std::to_string(m.traderMoney) + " cats)");
    }
    scratchTradeReqs_.clear();
    for (auto it = trades_.begin(); it != trades_.end();) {
        const uint8_t pid = it->first;
        auto pl = players_.find(pid);
        auto trader = entities_.find(it->second.trader);
        auto looter = entities_.find(it->second.looter);
        bool open = false;
        for (uint32_t id : it->second.counters)
            if (auto e = entities_.find(id); e != entities_.end() && e->second.openBy.count(pid)) open = true;
        if (pl == players_.end() || !open || trader == entities_.end() || looter == entities_.end()) {
            if (pl != players_.end()) log_("[" + pl->second.name + "] closes the trade window");
            ++it;
            EndTrade(pid, {});
            continue;
        }
        // the merchant's cats changed (another player traded, or the host): the window shows them
        int32_t money = 0;
        if (world_.MoneyOf(trader->second.handle, money) && money != it->second.traderMoney) {
            it->second.traderMoney = money;
            TradeOpen m;
            m.traderNetId = it->second.trader;
            m.looterNetId = it->second.looter;
            m.traderMoney = money;
            Writer w;
            Encode(w, m);
            SendReliable(pl->second.peer, w);
        }
        // walked away from the merchant: the trade closes (not in its first seconds: the character
        // may still be walking up to the merchant)
        EntityState st;
        if (now - it->second.since > 5.0 && world_.Read(trader->second.handle, st) && world_.DistanceTo(looter->second.handle, st.pos) > 150.0f) {
            log_("[" + pl->second.name + "] walked away from the merchant: trade window closed");
            ++it;
            EndTrade(pid, "trop loin du marchand");
            continue;
        }
        ++it;
    }
}

void Session::ClientContainers(double now) {
    world_.TakeContainerRequests(scratchContainerReqs_);
    for (const auto& r : scratchContainerReqs_) {
        Entity* e = entityByHandle(r.looter);
        if (!e || !e->squad || e->owner != localId_) continue;
        ContainerOpen m;
        m.looterNetId = e->netId;
        m.sid = r.sid;
        m.pos = r.pos;
        Writer w;
        Encode(w, m);
        SendReliable(net_.serverPeer(), w);
    }
    scratchContainerReqs_.clear();
    // the window opens once the container holds the host's items
    if (pendingWindow_) {
        auto it = entities_.find(pendingWindow_);
        if (it == entities_.end()) pendingWindow_ = 0;
        else if (it->second.haveInv && !it->second.invDirty) {
            auto looter = entities_.find(it->second.looter);
            if (looter != entities_.end() && world_.OpenContainerWindow(looter->second.handle, it->second.handle)) windowOpenedAt_ = now;
            pendingWindow_ = 0;
        }
    }
    // a trade window: it opens once every counter holds the host's items, with the merchant's cats
    if (trade_.pending) {
        bool ready = true;
        for (uint32_t id : trade_.counters) {
            auto it = entities_.find(id);
            if (it == entities_.end() || !it->second.haveInv || it->second.invDirty) ready = false;
        }
        auto trader = entities_.find(trade_.trader);
        auto looter = entities_.find(trade_.looter);
        const bool here = trader != entities_.end() && looter != entities_.end() && trader->second.present && looter->second.present;
        if (trade_.pendingSince == 0) trade_.pendingSince = now;
        if (!here && now - trade_.pendingSince > 10.0) {
            log_("trade window cancelled: the merchant or our character is not here");
            for (uint32_t id : trade_.counters) {
                Writer w;
                Encode(w, ContainerClose{id, {}});
                SendReliable(net_.serverPeer(), w);
                entities_.erase(id);
            }
            EndClientTrade();
        } else if (here && ready) {
            trade_.pending = false;
            world_.SetMoneyOf(trader->second.handle, trade_.traderMoney);
            if (world_.OpenTradeWindow(looter->second.handle, trader->second.handle)) {
                windowOpenedAt_ = now;
                trade_.open = true;
                CaptureLocalSpend();
                unsentSpend_ = 0;
                size_t stacks = 0;
                for (uint32_t id : trade_.counters)
                    if (auto it = entities_.find(id); it != entities_.end()) stacks += it->second.inv.size();
                log_("trade window open: " + std::to_string(trade_.counters.size()) + " shop counters holding " + std::to_string(stacks) + " stacks");
            } else {
                log_("trade window could not open here");
                EndClientTrade();
            }
        }
    }
    // the stock changed under our window (not while an item is on the mouse): the window closes, the
    // counters take the host's stock, and it opens again (the pending path above)
    if (trade_.open && trade_.refresh && !world_.TradeWindowBusy()) {
        trade_.refresh = false;
        trade_.open = false;
        trade_.pending = true;
        trade_.pendingSince = 0;
        windowOpenedAt_ = -1;   // not the player closing it
        world_.CloseContainerWindows();
        log_("trade window closed for a moment: the merchant's stock changed");
    }
    // the player closed it: the host forgets it for us
    if (windowOpenedAt_ > 0 && now - windowOpenedAt_ > 1.5 && !world_.ContainerWindowOpen()) {
        windowOpenedAt_ = -1;
        if (trade_.open) log_("trade window closed");
        EndClientTrade();
        for (auto it = entities_.begin(); it != entities_.end();) {
            if (!it->second.container) { ++it; continue; }
            Writer w;
            Encode(w, ContainerClose{it->first, {}});
            SendReliable(net_.serverPeer(), w);
            it = entities_.erase(it);
        }
    }
}

bool Session::IsTradeCounter(uint32_t netId) const {
    return (trade_.open || trade_.pending) && std::find(trade_.counters.begin(), trade_.counters.end(), netId) != trade_.counters.end();
}

void Session::EndClientTrade() {
    trade_ = ClientTrade{};
    unsentSpend_ = 0;
}

// Client: our cats changed without the host saying so: only our trade window does that (the price
// of a purchase or sale), counted for the next item move it made.
void Session::CaptureLocalSpend() {
    int32_t local = 0;
    if (!world_.ReadMoney(local)) return;
    if (haveMoneyBase_ && local != moneyBase_ && trade_.open) unsentSpend_ += moneyBase_ - local;
    moneyBase_ = local;
    haveMoneyBase_ = true;
}

void Session::ApplyHostMoney(int32_t money) {
    CaptureLocalSpend();
    world_.ApplyMoney(money);
    moneyBase_ = money;
    haveMoneyBase_ = true;
}

void Session::QueueLog(std::string line) {
    if (!isClient()) return;
    if (logOut_.size() >= 512) { ++logDropped_; return; }
    logOut_.push_back(std::move(line));
}

void Session::AnswerDialog(int index) {
    if (state_ != SessionState::Connected || !dialog_.open || dialog_.waiting || index < 0 || index >= int(dialog_.replies.size())) return;
    DialogReply r;
    r.dialogId = dialog_.id;
    r.index = index;
    Writer w;
    Encode(w, r);
    SendReliable(net_.serverPeer(), w);
    dialog_.waiting = true;
}

// Skill levels change slowly (a few thousandths per hit or per minute of training): what changed is
// sent once a second, everything every 20 s; the money when it changes.
void Session::SendProgress(double now) {
    std::unordered_map<uint32_t, CharProgress> cur;
    for (auto& [id, e] : entities_) {
        CharProgress p;
        p.netId = id;
        if (world_.ReadProgress(e.handle, p.stats, p.modes, p.style) && p.stats.size() == kStatCount) {
            world_.ReadTool(e.handle, p.tool);
            cur[id] = std::move(p);
        }
    }
    int32_t money = 0;
    const bool haveMoney = world_.ReadMoney(money);
    const bool moneyNew = haveMoney && (!moneySent_ || money != lastMoney_ || now - moneyAt_ > 20.0);
    if (moneyNew) { moneySent_ = true; lastMoney_ = money; moneyAt_ = now; }
    for (auto& [pid, p] : players_) {
        if (!p.inGame) continue;
        auto& sent = sync_[pid].sent;
        ProgressMsg m;
        m.hasMoney = haveMoney;
        m.money = money;
        for (const auto& [id, prog] : cur) {
            Sent& last = sent[id];
            if (!StatsChanged(prog.stats, last.stats) && prog.modes == last.modes && prog.style == last.style && prog.tool == last.tool &&
                now - last.statsAt < 20.0)
                continue;
            last.stats = prog.stats;
            last.modes = prog.modes;
            last.style = prog.style;
            last.tool = prog.tool;
            last.statsAt = now;
            m.chars.push_back(prog);
            if (m.chars.size() >= 64) {
                Writer w;
                Encode(w, m);
                SendReliable(p.peer, w);
                m.chars.clear();
            }
        }
        if (m.chars.empty() && !moneyNew) continue;
        Writer w;
        Encode(w, m);
        SendReliable(p.peer, w);
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
        // One Steam account, one player: a second connection from the same account (two games on
        // one PC) is told apart by its name only.
        bool steamTaken = h.steamId != 0 && h.steamId == cfg_.steamId;
        if (steamTaken) h.steamId = 0;
        // the same account still connected: a player who restarted the game before the old
        // connection timed out. The old one goes; the player gets their character back.
        for (auto it = players_.begin(); h.steamId && it != players_.end(); ++it) {
            if (it->second.steamId != h.steamId) continue;
            log_(it->second.name + " reconnected: the old connection is closed");
            net_.Kick(it->second.peer);
            OnDisconnect(it->second.peer);
            break;
        }
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
        np.steamId = h.steamId;
        np.peer = peer;
        np.inGame = false;
        players_[id] = np;
        PlayerSync s;
        s.joinedAt = clock_();
        sync_[id] = std::move(s);
        AddChat("* " + h.name + " rejoint la partie...", "* " + h.name + " is joining...");
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
    case Msg::ContainerOpen: {
        ContainerOpen m;
        if (!pl->inGame || !Decode(r, m)) break;
        if (containerAsks_.size() < 16) containerAsks_.emplace_back(pl->id, std::move(m));
        break;
    }
    case Msg::ContainerClose: {
        ContainerClose m;
        if (!Decode(r, m)) break;
        if (auto it = entities_.find(m.netId); it != entities_.end() && it->second.container) {
            it->second.openBy.erase(pl->id);
            log_("[" + pl->name + "] closes the container");
        }
        break;
    }
    case Msg::Appearance: {
        AppearanceMsg m;
        if (!pl->inGame || !Decode(r, m)) break;
        if (pendingLooks_.size() < 16) pendingLooks_.emplace_back(pl->id, std::move(m));
        break;
    }
    case Msg::ClientLog: {
        ClientLog m;
        if (!Decode(r, m)) break;
        for (const auto& l : m.lines) log_("[" + pl->name + "] " + l);
        break;
    }
    case Msg::ClientReport: {
        ClientReport m;
        if (!Decode(r, m)) break;
        pl->report = m;
        const double now = clock_();
        pl->reportAt = now;
        const bool bad = m.missingSquad > 0 || m.farOff > 3 || m.maxErr > 30.0f || (m.fps > 0 && m.fps < 15);
        if (now - pl->reportLoggedAt > (bad ? 15.0 : 60.0)) {
            pl->reportLoggedAt = now;
            char b[200];
            snprintf(b, sizeof(b), "[%s] sync%s: %u characters followed, %u NPCs not there yet, %u squad members missing, %u off, max offset %.1f, %u fps",
                     pl->name.c_str(), bad ? " NEEDS A LOOK" : "", unsigned(m.entities), unsigned(m.missingNpcs), unsigned(m.missingSquad),
                     unsigned(m.farOff), double(m.maxErr), unsigned(m.fps));
            log_(b);
        }
        break;
    }
    case Msg::EditState: {
        EditState m;
        if (!Decode(r, m)) break;
        if (m.editing != pl->editing) {
            pl->editing = m.editing;
            pl->editingSince = clock_();
            log_(pl->name + (m.editing ? " opened the character editor: the game waits for them" : " closed the character editor"));
            if (m.editing) AddChat("* " + pl->name + " crée son personnage : partie en pause.", "* " + pl->name + " is making their character: game paused");
        }
        break;
    }
    case Msg::DialogReply: {
        DialogReply a;
        if (!pl->inGame || !Decode(r, a)) break;
        auto o = dialogOwner_.find(a.dialogId);
        if (o == dialogOwner_.end() || o->second != pl->id) { log_("ignored an answer to a conversation player " + std::to_string(pl->id) + " is not in"); break; }
        if (pendingAnswers_.size() < 64) pendingAnswers_.push_back(a);
        if (auto rr = dialogReplies_.find(a.dialogId); rr != dialogReplies_.end() && a.index < int(rr->second.size()))
            log_("[" + pl->name + "] answers: \"" + rr->second[size_t(a.index)] + "\"");
        break;
    }
    case Msg::BuildPlace:
    case Msg::BuildAction:
        HostBuildingPacket(*pl, type, r);   // lot E
        break;
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
    case Msg::DoorRequest: HostDoorPacket(pl->id, r); break;   // lot A
    default: break;  // host ignores host-bound-only messages from clients
    }
}

void Session::SendInventories(double now, bool force, PeerId onlyTo) {
    (void)now;
    for (auto& [id, e] : entities_) {
        if (e.container && e.openBy.empty()) continue;
        std::vector<ItemState> items;
        if (!world_.ReadInventory(e.handle, items)) continue;
        const uint64_t h = InventoryHash(items);
        if (!force && h == e.invHash) continue;
        InventoryMsg m;
        m.netId = id;
        m.items = std::move(items);
        Writer w(1024);
        Encode(w, m);
        if (e.container) {   // only to the players who have it open
            e.invHash = h;
            for (uint8_t pid : e.openBy)
                if (auto pl = players_.find(pid); pl != players_.end()) SendReliable(pl->second.peer, w);
            continue;
        }
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
namespace {
// `first` empties the place `later` fills
bool FreesPlaceFor(const InvOp& first, const InvOp& later) {
    return first.kind == InvOpKind::Move && later.kind == InvOpKind::Move && first.fromNetId == later.toNetId &&
           first.item.section == later.toSection && first.item.x == later.toX && first.item.y == later.toY;
}
// Reorder so a move into a place another move empties comes after it (cycles keep their order).
template <class Get>
void FreeingFirst(std::vector<size_t>& idx, Get get) {
    for (size_t guard = 0; guard < idx.size() * idx.size() + 1; ++guard) {
        bool moved = false;
        for (size_t i = 0; i < idx.size() && !moved; ++i)
            for (size_t j = i + 1; j < idx.size() && !moved; ++j)
                if (FreesPlaceFor(get(idx[j]), get(idx[i])) && !FreesPlaceFor(get(idx[i]), get(idx[j]))) {
                    std::rotate(idx.begin() + ptrdiff_t(i), idx.begin() + ptrdiff_t(j), idx.begin() + ptrdiff_t(j) + 1);
                    moved = true;
                }
        if (!moved) return;
    }
}
} // namespace

// One round of a client's moves. Moves dropped on an occupied slot come as a pair (the item that
// arrived, the one it pushed out to where the first came from): those run as one swap, the only way
// both can land. The rest run so that a move into a place another move frees comes after it.
void Session::HostInvOps() {
    std::vector<std::pair<uint8_t, InvOp>>& ops = pendingInvOps_;
    std::vector<bool> done(ops.size(), false);
    auto isSwap = [](const InvOp& a, const InvOp& b) {
        return a.kind == InvOpKind::Move && b.kind == InvOpKind::Move && !a.traderNetId && !b.traderNetId && a.fromNetId == b.toNetId &&
               a.toNetId == b.fromNetId && a.toSection == b.item.section && a.toX == b.item.x && a.toY == b.item.y;
    };
    for (size_t i = 0; i < ops.size(); ++i) {
        if (done[i]) continue;
        for (size_t j = i + 1; j < ops.size(); ++j) {
            if (done[j] || ops[j].first != ops[i].first) continue;
            const InvOp& a = ops[i].second;
            const InvOp& b = ops[j].second;
            if (!isSwap(a, b) && !isSwap(b, a)) continue;
            if (isSwap(a, b)) HostInvOp(ops[i].first, a, &b);
            else HostInvOp(ops[i].first, b, &a);
            done[i] = done[j] = true;
            break;
        }
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < ops.size(); ++i)
        if (!done[i]) order.push_back(i);
    FreeingFirst(order, [&](size_t i) -> const InvOp& { return ops[i].second; });
    for (size_t i : order) HostInvOp(ops[i].first, ops[i].second);
}

void Session::HostInvOp(uint8_t from, const InvOp& op, const InvOp* swapWith) {
    if (op.traderNetId) { HostTradeOp(from, op); return; }
    auto src = entities_.find(op.fromNetId);
    if (src == entities_.end()) return;
    auto isDown = [&](const Entity& e) {
        EntityState st;
        EntityVitals v;
        return (world_.Read(e.handle, st) && (st.flags & (kFlagDown | kFlagDead)) != 0) ||
               (world_.ReadVitals(e.handle, v) && (v.flags & (kVitUnconscious | kVitDead)) != 0);
    };
    // what this player may take from and put into: its own characters, a container it opened, an
    // NPC knocked out or dead (the game lets one strip a body and also put things on it)
    auto mayTouch = [&](const Entity& e) {
        return (e.squad && e.owner == from) || (e.container && e.openBy.count(from)) || (!e.squad && !e.container && isDown(e));
    };
    const bool srcOk = mayTouch(src->second);
    if (op.kind == InvOpKind::Drop) {
        if (src->second.owner == from) world_.ExecuteInvOp(src->second.handle, src->second.handle, op);
        src->second.invHash = 0;
        return;
    }
    auto dst = entities_.find(op.toNetId);
    const bool dstOk = dst != entities_.end() && mayTouch(dst->second);
    // Taking from a container that is not ours is stealing: the game decides, on the host, like it
    // does for its own player (crime, the owners may notice).
    if (dstOk && srcOk && src->second.container && !dst->second.container) {
        const int theft = world_.TheftCheck(dst->second.handle, src->second.handle, op.item);
        const std::string who = players_.count(from) ? players_[from].name : "?";
        if (theft == 2) {
            log_("[" + who + "] caught stealing " + world_.TemplateName(op.item.templateSid));
            src->second.openBy.erase(from);
            if (auto pl = players_.find(from); pl != players_.end()) {
                Writer w;
                Encode(w, ContainerClose{op.fromNetId, "Pris en train de voler !"});
                SendReliable(pl->second.peer, w);
            }
            src->second.invHash = 0;
            dst->second.invHash = 0;
            return;
        }
        if (theft == 1) log_("[" + who + "] steals " + world_.TemplateName(op.item.templateSid) + " (unseen)");
    }
    if (!dstOk || !srcOk) {
        log_("refused an inventory move from player " + std::to_string(from) + ": " + op.item.templateSid + " " +
             (srcOk ? "" : dst == entities_.end() ? "(unknown destination) " : "(may not take from there) ") + (dstOk ? "" : "(may not put there)"));
        src->second.invHash = 0;   // resend the true state so the client's prediction is undone
        if (dst != entities_.end()) dst->second.invHash = 0;
        return;
    }
    if (swapWith) {
        // both ways are between the same two inventories, both already allowed above
        if (!world_.ExecuteInvSwap(src->second.handle, dst->second.handle, op, *swapWith)) {
            // not a real swap here (the host's places differ): one after the other, the outgoing first
            world_.ExecuteInvOp(dst->second.handle, src->second.handle, *swapWith);
            world_.ExecuteInvOp(src->second.handle, dst->second.handle, op);
        }
        src->second.invHash = 0;
        dst->second.invHash = 0;
        return;
    }
    world_.ExecuteInvOp(src->second.handle, dst->second.handle, op);
    // Whatever happened, everyone (the requester first) gets the real state of both inventories.
    src->second.invHash = 0;
    dst->second.invHash = 0;
}

// A purchase or sale in a merchant's trade window: the item moves between the shop's counters and
// the player's character, and the price the player's game counted between the player faction's
// cats and the merchant's. Whatever happens, the player gets the true state back (items and cats).
void Session::HostTradeOp(uint8_t from, const InvOp& op) {
    auto pl = players_.find(from);
    const std::string who = pl != players_.end() ? pl->second.name : "player " + std::to_string(from);
    auto src = entities_.find(op.fromNetId);
    auto dst = entities_.find(op.toNetId);
    auto t = trades_.find(from);
    auto refuse = [&](const std::string& why, const std::string& note) {
        log_("[" + who + "] trade refused: " + why);
        if (src != entities_.end()) src->second.invHash = 0;
        if (dst != entities_.end()) dst->second.invHash = 0;
        moneyAt_ = -1e9;       // their cats as the host has them, now
        nextProgress_ = 0;
        if (!note.empty() && pl != players_.end()) {
            Chat c;
            c.from = 0;   // a notice, not a player's line
            c.text = note;
            Writer w;
            Encode(w, c);
            SendReliable(pl->second.peer, w);
        }
    };
    if (t == trades_.end() || t->second.trader != op.traderNetId) return refuse("no trade window open with that merchant", "Le commerce est fermé.");
    if (src == entities_.end() || dst == entities_.end()) return refuse("unknown inventory", {});
    auto counter = [&](uint32_t id) { return std::find(t->second.counters.begin(), t->second.counters.end(), id) != t->second.counters.end(); };
    auto own = [&](const Entity& e) { return e.squad && e.owner == from; };
    const bool buying = counter(op.fromNetId) && own(dst->second);
    const bool selling = own(src->second) && counter(op.toNetId);
    // A purchase the player could not pay puts the item back in the first counter with room, maybe
    // another one: the shop's stock moves between its counters, nothing is paid.
    if (counter(op.fromNetId) && counter(op.toNetId) && op.price == 0) {
        if (!world_.ExecuteInvOp(src->second.handle, dst->second.handle, op)) return refuse("the shop's stock could not move", {});
        src->second.invHash = 0;
        dst->second.invHash = 0;
        return;
    }
    if (!buying && !selling) return refuse("the item does not go between the shop and their character", {});
    if (buying ? op.price <= 0 : op.price > 0)
        return refuse("price " + std::to_string(op.price) + " does not fit a " + (buying ? "purchase" : "sale"), "Le prix n'a pas pu être compté : rien n'a changé.");
    auto trader = entities_.find(t->second.trader);
    if (trader == entities_.end()) return refuse("the merchant is gone", {});
    const Handle& player = buying ? dst->second.handle : src->second.handle;
    int32_t playerMoney = 0, traderMoney = 0;
    world_.MoneyOf(player, playerMoney);
    world_.MoneyOf(trader->second.handle, traderMoney);
    if (buying && playerMoney < op.price)
        return refuse("not enough cats (" + std::to_string(playerMoney) + " for " + std::to_string(op.price) + ")", "Pas assez d'argent.");
    if (selling && traderMoney < -op.price)
        return refuse("the merchant cannot pay (" + std::to_string(traderMoney) + " for " + std::to_string(-op.price) + ")", "Le marchand n'a pas assez d'argent.");
    if (!world_.ExecuteInvOp(src->second.handle, dst->second.handle, op)) return refuse("the item could not move", "L'objet n'est plus disponible.");
    const bool paid = world_.PayTrade(player, trader->second.handle, op.price);
    log_("[" + who + "] " + (buying ? "buys " : "sells ") + world_.TemplateName(op.item.templateSid) + " x" + std::to_string(op.item.quantity) +
         (buying ? " from " : " to ") + world_.CharacterNameOf(trader->second.handle) + " for " + std::to_string(buying ? op.price : -op.price) +
         " cats" + (paid ? "" : " (PAYMENT FAILED)"));
    src->second.invHash = 0;
    dst->second.invHash = 0;
    moneyAt_ = -1e9;
    nextProgress_ = 0;    // the new cats go out with the new items
    world_.RefreshTradeWindow(trader->second.handle);   // the host's own window on that merchant, if open
}

// Client: compare what our characters hold with what the host last told us; a difference is
// the local player moving things in the inventory UI. Turn it into item movements for the host.
void Session::ClientInventoryDiff(double now) {
    if (now < nextInvDiff_) return;
    nextInvDiff_ = now + 0.2;
    CaptureLocalSpend();
    struct Delta { uint32_t netId; ItemState item; };
    std::vector<Delta> gone, added;
    std::vector<uint32_t> involved;
    for (auto& [id, e] : entities_) {
        if (!e.present || !e.haveInv || e.invDirty || now < e.invPendingUntil) continue;
        std::vector<ItemState> local;
        if (!world_.ReadInventory(e.handle, local)) continue;
        if (local == e.inv) { e.invUnmatchedSince = 0; continue; }
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
    // Only one's own characters' inventories (and bodies to loot) can be handled: anything touching
    // another player's character goes straight back, with the reason.
    std::string foreignOwner;
    auto foreign = [&](uint32_t netId) {
        auto it = entities_.find(netId);
        if (it == entities_.end() || !it->second.squad || it->second.owner == localId_) return false;
        auto pl = players_.find(it->second.owner);
        foreignOwner = pl != players_.end() ? pl->second.name : (it->second.owner == 1 ? "l'hote" : "un autre joueur");
        return true;
    };
    bool refused = false;
    for (const auto& g : gone) refused = foreign(g.netId) || refused;
    for (const auto& a : added) refused = foreign(a.netId) || refused;
    if (refused) {
        for (uint32_t id : involved)
            if (auto it = entities_.find(id); it != entities_.end()) { it->second.invDirty = true; it->second.invUnmatchedSince = 0; }
        if (now >= nextForeignNote_) {
            nextForeignNote_ = now + 3.0;
            AddChat("* Ce personnage appartient à " + foreignOwner + " : tu ne peux pas gérer son inventaire.",
                    "inventory change refused: that character belongs to " + foreignOwner);
        }
        return;
    }
    std::vector<InvOp> ops;
    // 1. Items that went from one inventory to another, counted by kind in each inventory: a whole
    //    stack or part of one, landing alone or on a stack already there (trading and looting do
    //    all of these).
    struct Flow { uint32_t netId; ItemState kind; int qty; };
    std::vector<Flow> flows;
    auto flowOf = [&](uint32_t netId, const ItemState& kind) -> Flow& {
        for (auto& f : flows)
            if (f.netId == netId && f.kind.sameKind(kind)) return f;
        flows.push_back({netId, kind, 0});
        return flows.back();
    };
    for (const auto& g : gone) flowOf(g.netId, g.item).qty -= g.item.quantity;
    for (const auto& a : added) flowOf(a.netId, a.item).qty += a.item.quantity;
    const std::vector<Flow> before = flows;
    for (auto& lost : flows) {
        for (auto& got : flows) {
            if (lost.qty >= 0) break;
            if (got.qty <= 0 || got.netId == lost.netId || !got.kind.sameKind(lost.kind)) continue;
            // the host's stack it came out of (the biggest that shrank or went) and where it landed
            const ItemState* from = nullptr;
            const ItemState* to = nullptr;
            for (const auto& g : gone)
                if (g.netId == lost.netId && g.item.sameKind(lost.kind) && (!from || g.item.quantity > from->quantity)) from = &g.item;
            for (const auto& a : added)
                if (a.netId == got.netId && a.item.sameKind(got.kind) && (!to || a.item.quantity > to->quantity)) to = &a.item;
            if (!from || !to) continue;
            const int n = std::min(-lost.qty, got.qty);
            InvOp op;
            op.kind = InvOpKind::Move;
            op.fromNetId = lost.netId;
            op.toNetId = got.netId;
            op.item = *from;
            op.item.quantity = n;
            op.toSection = to->section;
            op.toX = to->x;
            op.toY = to->y;
            if (IsTradeCounter(lost.netId) || IsTradeCounter(got.netId)) op.traderNetId = trade_.trader;
            ops.push_back(op);
            lost.qty += n;
            got.qty -= n;
        }
    }
    // 2. Moves inside one inventory (rearranging, equipping): a stack gone from one place and found,
    //    whole or in part, at another place of the same inventory.
    auto netBefore = [&](uint32_t netId, const ItemState& kind) {
        for (const auto& f : before)
            if (f.netId == netId && f.kind.sameKind(kind)) return f.qty;
        return 0;
    };
    std::vector<bool> addUsed(added.size(), false);
    for (const auto& g : gone) {
        if (netBefore(g.netId, g.item) != 0) continue;   // part of a move between inventories (above)
        size_t pick = added.size();
        for (size_t i = 0; i < added.size(); ++i) {
            const auto& a = added[i];
            if (addUsed[i] || a.netId != g.netId || !a.item.sameKind(g.item) || a.item.quantity > g.item.quantity) continue;
            if (a.item.section == g.item.section && a.item.x == g.item.x && a.item.y == g.item.y) continue;
            if (pick == added.size() || a.item.quantity == g.item.quantity) pick = i;
            if (a.item.quantity == g.item.quantity) break;
        }
        if (pick == added.size()) continue;
        addUsed[pick] = true;
        InvOp op;
        op.kind = InvOpKind::Move;
        op.fromNetId = g.netId;
        op.toNetId = g.netId;
        op.item = g.item;
        op.item.quantity = added[pick].item.quantity;
        op.toSection = added[pick].item.section;
        op.toX = added[pick].item.x;
        op.toY = added[pick].item.y;
        ops.push_back(op);
    }
    // Only what the player can touch in the game's windows is the player's doing: the squad's
    // characters, an open container, a body on the ground (anything else the host refuses, and the
    // client is told). A standing NPC's gear differing on its own (a character that just streamed
    // in) is not: no move is asked, it simply goes back to the host's state.
    auto touchable = [&](uint32_t netId) {
        auto it = entities_.find(netId);
        if (it == entities_.end()) return false;
        const Entity& e = it->second;
        const bool down = (!e.buf.empty() && (e.buf.back().s.flags & (kFlagDown | kFlagDead))) ||
                          (e.haveVitals && (e.vitals.flags & (kVitUnconscious | kVitDead)));
        return e.squad || e.container || down;
    };
    ops.erase(std::remove_if(ops.begin(), ops.end(), [&](const InvOp& op) { return !touchable(op.fromNetId) && !touchable(op.toNetId); }), ops.end());
    for (uint32_t id : involved) {
        if (touchable(id)) continue;
        const bool moved = std::any_of(ops.begin(), ops.end(), [&](const InvOp& op) { return op.fromNetId == id || op.toNetId == id; });
        if (auto it = entities_.find(id); it != entities_.end() && !moved) { it->second.invDirty = true; it->second.invUnmatchedSince = 0; }
    }
    // A move into a place another move empties is sent after it (the host runs them in order).
    {
        std::vector<size_t> idx(ops.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        FreeingFirst(idx, [&](size_t i) -> const InvOp& { return ops[i]; });
        std::vector<InvOp> sorted;
        for (size_t i : idx) sorted.push_back(ops[i]);
        ops.swap(sorted);
    }
    // The price our trade window counted goes with the first purchase or sale of this round.
    bool priced = false;
    for (auto& op : ops) {
        if (op.traderNetId && !priced) {
            op.price = unsentSpend_;
            unsentSpend_ = 0;
            priced = true;
        }
        log_("inventory move asked of the host: " + op.item.templateSid + " x" + std::to_string(op.item.quantity) + " " + op.item.section +
             " -> " + op.toSection + " " + std::to_string(op.toX) + "," + std::to_string(op.toY) +
             (op.traderNetId ? " (trade, " + std::to_string(op.price) + " cats)" : std::string{}));
        Writer w;
        Encode(w, op);
        SendReliable(net_.serverPeer(), w);
    }
    const size_t sent = ops.size();
    for (uint32_t id : involved) {
        auto it = entities_.find(id);
        if (it == entities_.end()) continue;
        if (sent) {
            it->second.invPendingUntil = now + 3.0;   // wait for the host's answer
            it->second.invUnmatchedSince = 0;
            continue;
        }
        // An item that left without landing anywhere is usually on the mouse, being dragged: give the
        // player time to put it down before going back to the host's state.
        double& since = it->second.invUnmatchedSince;
        if (since == 0) since = now;
        if (now - since < 10.0) continue;
        since = 0;
        it->second.invDirty = true;                   // nothing we can ask for: back to the host's state
    }
}

void Session::SendLocalDrops() {
    std::vector<std::pair<Handle, ItemState>> drops;
    world_.TakeLocalDrops(drops);
    for (const auto& [h, item] : drops) {
        for (auto& [id, e] : entities_) {
            if (e.handle != h) continue;
            InvOp op;
            op.kind = InvOpKind::Drop;
            op.fromNetId = id;
            op.item = item;
            Writer w;
            Encode(w, op);
            SendReliable(net_.serverPeer(), w);
            break;
        }
    }
}

void Session::ApplyCommand(uint8_t from, const Command& c) {
    const std::string who = players_.count(from) ? players_[from].name : "player " + std::to_string(from);
    auto it = entities_.find(c.netId);
    if (it == entities_.end()) { log_("[" + who + "] order for an unknown character (ignored)"); return; }
    if (!it->second.squad || it->second.owner != from) {
        log_("[" + who + "] order refused: not their character");
        return;
    }
    std::string what;
    switch (c.kind) {
    case CommandKind::MoveTo: what = "move"; break;
    case CommandKind::Stop: what = "stop"; break;
    case CommandKind::PickUp: what = "pick up " + world_.TemplateName(c.itemSid); break;
    case CommandKind::Task:
        if (c.via == TaskVia::SetOrder) what = std::string("mode \"") + StandingOrderLabel(c.task) + "\"";
        else what = std::string("order \"") + TaskLabel(c.task) + "\" (" + std::to_string(c.task) + ")" + (c.itemSid.empty() ? "" : " on " + world_.TemplateName(c.itemSid));
        break;
    case CommandKind::SquadMove: what = "change squad"; break;
    }
    const bool ok = world_.Order(it->second.handle, c);
    if (c.kind != CommandKind::MoveTo || !ok) log_("[" + who + "] " + what + (ok ? " -> ok" : " -> FAILED"));
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
            AddChat("* dans le monde de l'hôte (joueur " + std::to_string(localId_) + ")", "* in the host's world as player " + std::to_string(localId_));
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
        if (e.container) continue;   // furniture: found by kind and place when opened
        if (fullCheck || !e.checked) {
            const bool was = e.present;
            e.present = world_.Exists(e.handle);
            e.checked = true;
            // streamed in, adopted or recreated: a new local object, whose items are not the host's yet
            if (e.present && !was && e.haveInv) { e.invDirty = true; e.invRetry = 0; }
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
                if (e->present && e->haveInv) { e->invDirty = true; e->invRetry = 0; }
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
            if (e.present && e.haveInv) { e.invDirty = true; e.invRetry = 0; }
        }
    }
    for (auto it = floors_.begin(); it != floors_.end();) {   // fix G6
        auto e = entities_.find(it->first);
        if (e == entities_.end()) { it = floors_.erase(it); continue; }
        if (e->second.present) world_.ApplyFloor(e->second.handle, it->second);
        ++it;
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
            // Carrying someone: the same body on the same shoulder as on the host.
            const uint32_t carry = e.buf.back().s.carrying;
            if (carry != e.carryApplied || now >= e.carryReapply) {
                auto t = carry ? entities_.find(carry) : entities_.end();
                if (!carry) world_.ApplyCarry(e.handle, false, Handle{});
                else if (t != entities_.end() && t->second.present) world_.ApplyCarry(e.handle, true, t->second.handle);
                e.carryApplied = carry;
                e.carryReapply = now + 1.0;
            }
        }
    }
    ClientInventoryDiff(now);
    SendLocalDrops();
    for (auto& [id, e] : entities_) {
        if (!e.present || !e.invDirty || now < e.invRetry) continue;
        // Trading: the host's stock differs from what our window shows (another player bought or
        // sold, or the host placed it otherwise): the window shows it again. Our character or a
        // counter going back to the host's state undoes a purchase the host never got: its cats too.
        if (trade_.open && (IsTradeCounter(id) || id == trade_.looter)) {
            std::vector<ItemState> local;
            const bool differs = !(world_.ReadInventory(e.handle, local) && local == e.inv);
            if (differs) {
                if (haveMoney_) ApplyHostMoney(hostMoney_);
                unsentSpend_ = 0;
            }
            // The game's trade window shows the counters' own items (not copies): rebuilding a counter
            // under it would leave the window holding destroyed items. The window closes first, the
            // counters take the host's stock, then it opens again (ClientContainers).
            if (IsTradeCounter(id)) {
                if (differs) trade_.refresh = true;
                else { e.invDirty = false; e.invFailures = 0; }
                continue;
            }
        }
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
    // Skill levels and money: the client gains no experience of its own (see hk_increaseStat), so
    // the host's values only need imposing when they arrive, and now and then (respawned stand-ins).
    const bool restats = now >= nextStatsApply_;
    if (restats) nextStatsApply_ = now + 3.0;
    for (auto& [id, e] : entities_) {
        if (!e.present || e.stats.empty() || !(restats || e.statsDirty)) continue;
        world_.ApplyProgress(e.handle, e.stats, e.modes, e.style);
        world_.ApplyTool(e.handle, e.tool);
        e.statsDirty = false;
    }
    if (restats && haveMoney_) ApplyHostMoney(hostMoney_);
    // Our log lines go to the host's log, and every 5 s a report on how well we follow.
    if (now >= nextLogSend_ && (!logOut_.empty() || logDropped_)) {
        nextLogSend_ = now + 0.5;
        ClientLog m;
        if (logDropped_) { m.lines.push_back("(" + std::to_string(logDropped_) + " lignes non transmises)"); logDropped_ = 0; }
        while (!logOut_.empty() && m.lines.size() < kMaxLogLines) { m.lines.push_back(std::move(logOut_.front())); logOut_.erase(logOut_.begin()); }
        Writer w;
        Encode(w, m);
        SendReliable(net_.serverPeer(), w);
    }
    if (now >= nextReport_) {
        ClientReport rep;
        world_.TakeSyncStats(rep.maxErr, rep.farOff);
        rep.entities = uint16_t(std::min<size_t>(entities_.size(), 65535));
        rep.missingNpcs = uint16_t(std::min<size_t>(missingNpcs(), 65535));
        rep.missingSquad = uint16_t(std::min<uint32_t>(missingSquad_, 65535));
        rep.fps = uint16_t(now > reportStart_ ? std::min<double>(frames_ / std::max(0.5, now - reportStart_), 999.0) : 0);
        frames_ = 0;
        reportStart_ = now;
        nextReport_ = now + 5.0;
        Writer w;
        Encode(w, rep);
        SendReliable(net_.serverPeer(), w);
    }
    // Tell the host whether our character editor is open: it holds the game meanwhile.
    if (const bool open = world_.CharacterEditorOpen(); open != editingSent_) {
        editingSent_ = open;
        Writer w;
        Encode(w, EditState{open});
        SendReliable(net_.serverPeer(), w);
    }
    if (editRequest_) {
        auto it = entities_.find(editRequest_);
        if (it != entities_.end() && it->second.present && world_.OpenCharacterEditor(it->second.handle)) {
            editRequest_ = 0;
            AddChat("* Crée ton personnage, puis valide : tout le monde le verra ainsi.", "the host asks us to make our character (editor open)");
        }
    }
    SendEditedAppearances();
    ClientContainers(now);
    ClientFactionsTick(now);   // lot B
    ClientDoors(now);   // lot A
    ClientCaptives(now);   // lot D: prisons
    ClientRanged(now);   // lot C
    ClientBuildings(now);
    // Squads: split our characters as the host does (again now and then: stand-ins, late arrivals).
    if (haveSquads_ && now - squadsAt_ > 2.0) {
        squadsAt_ = now;
        std::vector<IWorld::WorldSquad> ws;
        for (const auto& s : lastSquads_.squads) {
            IWorld::WorldSquad w;
            w.name = s.name;
            for (uint32_t id : s.members)
                if (auto it = entities_.find(id); it != entities_.end() && it->second.present) w.members.push_back(it->second.handle);
            if (!w.members.empty()) ws.push_back(std::move(w));
        }
        world_.ApplySquads(ws);
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
    if (ClientFactionsPacket(type, r)) return;   // lot B
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
        AddChat("* connecté (joueur " + std::to_string(localId_) + "), réception du monde de l'hôte...",
                "* joined as player " + std::to_string(localId_) + ", receiving the host's world...");
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
        AddChat("* " + m.name + " rejoint la partie...", "* " + m.name + " is joining...");
        break;
    }
    case Msg::Floors: {   // fix G6
        FloorsMsg m;
        if (!Decode(r, m)) break;
        for (const auto& e : m.entries) floors_[e.netId] = e.group;
        break;
    }
    case Msg::Stall: {   // fix G6
        StallMsg m;
        if (!Decode(r, m)) break;
        net_.ExpectSilence(kNoPeer, m.seconds);
        log_("the host teleports us far: the connection waits while the zone loads");
        break;
    }
    case Msg::PlayerLeft: {
        PlayerLeft m;
        if (!Decode(r, m)) break;
        auto it = players_.find(m.id);
        if (it != players_.end()) { AddChat("* " + it->second.name + " a quitté la partie", "* " + it->second.name + " left"); players_.erase(it); }
        break;
    }
    case Msg::Chat: {
        Chat m;
        if (!Decode(r, m)) break;
        if (m.from == 0) {   // a notice from the host's game for this player (trade refused...)
            AddChat("* " + SanitizeChat(m.text), "notice from the host: " + SanitizeChat(m.text));
            break;
        }
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
    case Msg::Dialog: {
        DialogMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        for (auto& e : m.events) {
            auto it = entities_.find(e.netId);
            switch (e.kind) {
            case DialogKind::Say:
                if (it != entities_.end() && it->second.present) world_.ApplySay(it->second.handle, e.text, e.shout);
                break;
            case DialogKind::Open:
                // the game may say what the other one says before it opens the window: keep it
                if (dialog_.id != e.dialogId) dialog_ = DialogView{};
                dialog_.open = true;
                dialog_.id = e.dialogId;
                dialog_.name = e.text;
                break;
            case DialogKind::Text:
                if (!dialog_.open || dialog_.id != e.dialogId) { dialog_ = DialogView{}; dialog_.open = true; dialog_.id = e.dialogId; }
                if (!e.text.empty()) dialog_.text = std::move(e.text);
                dialog_.replies = std::move(e.replies);
                dialog_.waiting = false;
                break;
            case DialogKind::Close:
                if (dialog_.id == e.dialogId) dialog_ = DialogView{};
                break;
            }
        }
        break;
    }
    case Msg::Appearance: {
        AppearanceMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        if (auto it = entities_.find(m.netId); it != entities_.end() && it->second.present) world_.ApplyAppearance(it->second.handle, m);
        break;
    }
    case Msg::ContainerOpened: {
        ContainerOpened m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        Handle local;
        if (!world_.FindContainer(m.sid, m.pos, local)) { AddChat("* Ce contenant est introuvable ici.", "container " + world_.TemplateName(m.sid) + " not found here"); break; }
        Entity e;
        e.netId = m.netId;
        e.handle = local;
        e.container = true;
        e.present = true;
        e.checked = true;
        e.containerPos = m.pos;
        e.looter = m.looterNetId;
        entities_[m.netId] = std::move(e);
        pendingWindow_ = m.netId;
        break;
    }
    case Msg::ContainerClose: {
        ContainerClose m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        if (auto it = entities_.find(m.netId); it != entities_.end() && it->second.container) {
            if (IsTradeCounter(m.netId)) EndClientTrade();   // the whole trade window goes
            entities_.erase(it);
            world_.CloseContainerWindows();
            windowOpenedAt_ = -1;
            if (!m.reason.empty()) AddChat("* " + m.reason, "the host closes the window: " + m.reason);
        }
        break;
    }
    case Msg::Captives: OnCaptives(r); break;   // lot D: prisons
    case Msg::BuildPlace:
    case Msg::BuildState:
    case Msg::BuildRemove:
    case Msg::BuildAction:
        if (state_ == SessionState::Connected) ClientBuildingPacket(type, r);   // lot E
        break;
    case Msg::TradeOpen: {
        TradeOpen m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        auto trader = entities_.find(m.traderNetId);
        if ((trade_.open || trade_.pending) && trade_.trader == m.traderNetId && m.counters.empty()) {   // the merchant's cats changed
            trade_.traderMoney = m.traderMoney;
            if (trader != entities_.end() && trader->second.present) world_.SetMoneyOf(trader->second.handle, m.traderMoney);
            break;
        }
        if (m.counters.empty()) {
            if (!m.note.empty()) AddChat("* " + m.note, "the host cannot open this trade window");
            break;
        }
        // a new trade window: the shop's counters are containers we look into
        pendingWindow_ = 0;
        std::vector<uint32_t> ids;
        bool missing = trader == entities_.end() || !trader->second.present;
        for (const auto& c : m.counters) {
            Handle local;
            if (!world_.FindContainer(c.sid, c.pos, local)) {
                missing = true;
                log_("trade: shop counter " + world_.TemplateName(c.sid) + " not found here");
                continue;
            }
            Entity e;
            e.netId = c.netId;
            e.handle = local;
            e.container = true;
            e.present = true;
            e.checked = true;
            e.containerPos = c.pos;
            e.looter = m.looterNetId;
            entities_[c.netId] = std::move(e);
            ids.push_back(c.netId);
        }
        if (missing) {
            AddChat("* Le commerce ne peut pas s'ouvrir ici : marchand ou comptoir introuvable.", "trade window cancelled: merchant or counter not found here");
            for (const auto& c : m.counters) {
                Writer w;
                Encode(w, ContainerClose{c.netId, {}});
                SendReliable(net_.serverPeer(), w);
                entities_.erase(c.netId);
            }
            break;
        }
        EndClientTrade();
        trade_.trader = m.traderNetId;
        trade_.looter = m.looterNetId;
        trade_.counters = std::move(ids);
        trade_.traderMoney = m.traderMoney;
        trade_.pending = true;
        log_("the host opens a trade window for us: " + std::to_string(trade_.counters.size()) + " shop counters, the merchant has " +
             std::to_string(m.traderMoney) + " cats");
        break;
    }
    case Msg::Resync:
        if (state_ == SessionState::Connected) {
            log_("the host asks for a resync: leaving and joining again");
            resyncRequested_ = true;
        }
        break;
    case Msg::EditCharacter: {
        EditCharacter m;
        if (state_ == SessionState::Connected && Decode(r, m)) editRequest_ = m.netId;
        break;
    }
    case Msg::Squads: {
        SquadsMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        lastSquads_ = std::move(m);
        haveSquads_ = true;
        squadsAt_ = -1e9;   // apply now
        break;
    }
    case Msg::Progress: {
        ProgressMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        if (m.hasMoney) { hostMoney_ = m.money; haveMoney_ = true; ApplyHostMoney(m.money); }
        for (auto& c : m.chars) {
            auto it = entities_.find(c.netId);
            if (it == entities_.end()) continue;
            it->second.stats = std::move(c.stats);
            it->second.modes = c.modes;
            it->second.style = c.style;
            it->second.tool = std::move(c.tool);
            it->second.statsDirty = true;
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
    case Msg::Ground: {
        GroundMsg m;
        if (state_ == SessionState::Connected && Decode(r, m))
            for (const auto& e : m.events) world_.ApplyGround(e);
        break;
    }
    case Msg::AnimFrame: {
        AnimFrameMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m) || !offsetValid_) break;
        const double age = std::max(0.0, clock_() + offset_ - m.hostTime);
        for (const auto& f : m.chars) {
            auto it = entities_.find(f.netId);
            if (it != entities_.end()) world_.ApplyAnimFrame(it->second.handle, f, age);
        }
        break;
    }
    case Msg::Anim: {
        AnimMsg m;
        if (state_ != SessionState::Connected || !Decode(r, m)) break;
        for (const auto& e : m.events) {
            auto it = entities_.find(e.netId);
            if (it != entities_.end()) world_.ApplyAnim(it->second.handle, e);
        }
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
    case Msg::Doors: ClientDoorsPacket(r); break;   // lot A
    case Msg::Shots: case Msg::Ranged: ClientRangedPacket(type, r); break;   // lot C
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
    const uint8_t expected = (*type == Msg::Snapshot || *type == Msg::AnimFrame) ? kChanSnapshot : *type == Msg::Vitals ? kChanVitals : kChanReliable;
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
    if (!pl->inGame)
        log_(name + " left while joining (" + (sync_[id].kicked ? "removed" : "connection lost: their game quit or crashed while loading") +
             "): the world is no longer held for them");
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
    AddChat("* " + name + " a quitté la partie", "* " + name + " left");
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

void Session::AddChat(const std::string& shown, const std::string& logged) {
    chat_.push_back(shown);
    while (chat_.size() > kMaxChatLines) chat_.pop_front();
    log_(logged.empty() ? shown : logged);
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
