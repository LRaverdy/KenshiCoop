#include "kc/session.h"

#include <algorithm>
#include <cmath>

namespace kc {

namespace {

constexpr size_t kMaxChatLines = 50;
constexpr double kBufferSeconds = 1.0;       // client keeps this much interpolation history
constexpr double kBindSyncInterval = 0.5;
constexpr double kPingInterval = 1.0;
constexpr double kConnectTimeout = 10.0;

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
Quat Nlerp(Quat a, const Quat& b, float t) {
    // take the short way round
    if (a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z < 0) { a.w = -a.w; a.x = -a.x; a.y = -a.y; a.z = -a.z; }
    Quat q{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (len > 1e-6f) { q.w /= len; q.x /= len; q.y /= len; q.z /= len; } else q = b;
    return q;
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
            h.worldHash = world_.Fingerprint();
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
    world_.SetRole(false, true);
    controllableDirty_ = true;
    nextBindSync_ = 0;
    log_("hosting on UDP port " + std::to_string(cfg_.port));
    return true;
}

bool Session::Join(const std::string& address, uint16_t port, std::string* err) {
    Leave();
    if (!world_.Ready()) {
        if (err) *err = "load the host's save before joining";
        return false;
    }
    if (!net_.Connect(address, port, err)) return false;
    state_ = SessionState::Connecting;
    lastError_.clear();
    connectStarted_ = clock_();
    offsetValid_ = false;
    log_("connecting to " + address + ":" + std::to_string(port));
    return true;
}

void Session::Leave() {
    const bool wasActive = state_ == SessionState::Hosting || state_ == SessionState::Connected ||
                           state_ == SessionState::Handshake || state_ == SessionState::Connecting;
    net_.Close();
    entities_.clear();
    players_.clear();
    pendingPeers_.clear();
    owners_.clear();
    missing_ = 0;
    localId_ = 0;
    if (wasActive) {
        world_.SetRole(false, false);
        world_.SetControllable({});
        log_("session closed");
    }
    if (state_ != SessionState::Failed) state_ = SessionState::Idle;
}

void Session::Fail(const std::string& why) {
    log_("session failed: " + why);
    Leave();
    lastError_ = why;
    state_ = SessionState::Failed;
}

void Session::Tick() {
    if (state_ == SessionState::Idle || state_ == SessionState::Failed) return;
    const double now = clock_();
    net_.Poll(cb_);
    if (state_ == SessionState::Idle || state_ == SessionState::Failed) return;  // poll may have ended it
    if (isHost()) HostTick(now);
    else ClientTick(now);
    net_.Flush();
}

// ============================== host ==============================

void Session::HostTick(double now) {
    if (!world_.Ready()) { Fail("world unloaded"); return; }

    for (auto it = pendingPeers_.begin(); it != pendingPeers_.end();) {
        if (now - it->second > cfg_.handshakeTimeout) { net_.Kick(it->first); it = pendingPeers_.erase(it); }
        else ++it;
    }

    if (now >= nextBindSync_) {
        nextBindSync_ = now + kBindSyncInterval;
        SyncBindings();
    }
    if (controllableDirty_) PushControllable();

    // The host's own orders execute natively in its world; nothing to intercept.
    scratchOrders_.clear();
    world_.TakeLocalOrders(scratchOrders_);

    const TimeState t = world_.GetTime();
    if (now >= nextTimeState_ || t.speed != lastTime_.speed || t.paused != lastTime_.paused) {
        lastTime_ = t;
        nextTimeState_ = now + cfg_.timeStateInterval;
        Writer w;
        Encode(w, t);
        BroadcastReliable(w);
    }

    if (now >= nextSnapshot_ && !players_.empty()) {
        nextSnapshot_ = std::max(nextSnapshot_ + 1.0 / cfg_.snapshotRate, now - 0.5 / cfg_.snapshotRate);
        Snapshot s;
        s.tick = ++tick_;
        s.hostTime = now;
        s.entities.reserve(entities_.size());
        for (auto& [id, e] : entities_) {
            EntityState st;
            if (!world_.Read(e.handle, st)) continue;
            st.netId = id;
            s.entities.push_back(st);
        }
        for (const auto& pkt : EncodeSnapshot(s))
            for (auto& [pid, p] : players_) net_.Send(p.peer, kChanSnapshot, pkt.data(), pkt.size(), false);
    }

    if (now >= nextPing_) {
        nextPing_ = now + kPingInterval;
        for (auto& [pid, p] : players_) p.rttMs = net_.stats(p.peer).rttMs;
    }
}

void Session::SyncBindings() {
    scratchHandles_.clear();
    world_.PlayerCharacters(scratchHandles_);

    // unbind characters that left the player faction or no longer exist
    for (auto it = entities_.begin(); it != entities_.end();) {
        const Handle h = it->second.handle;
        const bool keep = world_.Exists(h) &&
                          std::find(scratchHandles_.begin(), scratchHandles_.end(), h) != scratchHandles_.end();
        if (!keep) {
            Writer w;
            Encode(w, Unbind{it->first});
            BroadcastReliable(w);
            it = entities_.erase(it);
            controllableDirty_ = true;
        } else ++it;
    }
    // bind new ones
    for (const Handle& h : scratchHandles_) {
        if (!h.valid() || entityByHandle(h)) continue;
        Entity e;
        e.netId = nextNetId_++;
        e.handle = h;
        e.owner = hostId_;
        for (const auto& [oh, pid] : owners_) if (oh == h) e.owner = pid;
        auto& ref = entities_[e.netId] = std::move(e);
        for (auto& [pid, p] : players_) SendBind(ref, p.peer);
        controllableDirty_ = true;
    }
}

void Session::SendBind(const Entity& e, PeerId to) {
    Bind b;
    b.netId = e.netId;
    b.handle = e.handle;
    b.owner = e.owner;
    Writer w;
    Encode(w, b);
    SendReliable(to, w);
}

void Session::Assign(const Handle& h, uint8_t playerId) {
    if (!isHost()) return;
    if (playerId != hostId_ && !players_.count(playerId)) return;
    auto it = std::find_if(owners_.begin(), owners_.end(), [&](auto& o) { return o.first == h; });
    if (it != owners_.end()) it->second = playerId; else owners_.emplace_back(h, playerId);
    if (Entity* e = entityByHandle(h)) {
        e->owner = playerId;
        for (auto& [pid, p] : players_) SendBind(*e, p.peer);  // Bind doubles as an ownership update
    }
    controllableDirty_ = true;
}

void Session::PushControllable() {
    controllableDirty_ = false;
    std::vector<Handle> mine;
    for (auto& [id, e] : entities_) if (e.owner == localId_) mine.push_back(e.handle);
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
        if (h.worldHash != world_.Fingerprint()) return reject(RejectReason::WorldMismatch);
        if (!ValidName(h.name)) return reject(RejectReason::BadName);
        uint8_t id = 0;
        for (uint8_t i = 2; i <= kMaxPlayers; ++i) if (!players_.count(i)) { id = i; break; }
        if (!id) return reject(RejectReason::Full);
        pendingPeers_.erase(peer);

        Welcome wm;
        wm.yourId = id;
        wm.worldHash = world_.Fingerprint();
        wm.hostTime = clock_();
        wm.players.push_back({hostId_, cfg_.name});
        for (auto& [pid, p] : players_) wm.players.push_back({pid, p.name});
        Writer w;
        Encode(w, wm);
        SendReliable(peer, w);

        Writer j;
        Encode(j, PlayerInfo{id, h.name});
        BroadcastReliable(j);

        RemotePlayer np;
        np.id = id;
        np.name = h.name;
        np.peer = peer;
        players_[id] = np;
        for (auto& [nid, e] : entities_) SendBind(e, peer);
        Writer t;
        Encode(t, world_.GetTime());
        SendReliable(peer, t);
        AddChat("* " + h.name + " joined");
        return;
    }
    if (!pl) return;  // everything else requires a completed handshake

    switch (type) {
    case Msg::Command: {
        Command c;
        if (Decode(r, c)) ApplyCommand(pl->id, c);
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
        BroadcastReliable(w);
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

void Session::ApplyCommand(uint8_t from, const Command& c) {
    auto it = entities_.find(c.netId);
    if (it == entities_.end()) return;
    if (it->second.owner != from) {
        log_("ignored command for a character player " + std::to_string(from) + " does not own");
        return;
    }
    world_.Order(it->second.handle, c);
}

// ============================== client ==============================

void Session::ClientTick(double now) {
    if (state_ == SessionState::Connecting || state_ == SessionState::Handshake) {
        if (now - connectStarted_ > kConnectTimeout) Fail("no answer from host (wrong address, port closed or firewall?)");
        return;
    }
    if (!world_.Ready()) { Fail("world unloaded"); return; }
    if (controllableDirty_) PushControllable();

    // Local orders never execute locally: they become commands, the host runs them, and the
    // result comes back through snapshots.
    scratchOrders_.clear();
    world_.TakeLocalOrders(scratchOrders_);
    for (auto& [h, cmd] : scratchOrders_) {
        Entity* e = entityByHandle(h);
        if (!e || e->owner != localId_) continue;
        Command c = cmd;
        c.seq = ++cmdSeq_;
        c.netId = e->netId;
        Writer w;
        Encode(w, c);
        SendReliable(net_.serverPeer(), w);
    }

    if (!offsetValid_) return;
    const double renderTime = now + offset_ - cfg_.interpDelay;
    for (auto& [id, e] : entities_) {
        if (!e.present || e.buf.empty()) continue;
        world_.Apply(e.handle, Interpolate(e, renderTime), e.buf.back().s);
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
        state_ = SessionState::Connected;
        world_.SetRole(true, true);
        controllableDirty_ = true;
        AddChat("* connected as player " + std::to_string(localId_));
        break;
    }
    case Msg::Reject: {
        Reject m;
        Fail(Decode(r, m) ? std::string("rejected by host: ") + ToString(m.reason) : "rejected by host");
        break;
    }
    case Msg::PlayerJoined: {
        PlayerInfo m;
        if (!Decode(r, m) || m.id == localId_) break;
        players_[m.id] = RemotePlayer{m.id, m.name};
        AddChat("* " + m.name + " joined");
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
        if (!Decode(r, m)) break;
        Entity& e = entities_[m.netId];
        const bool isNew = e.netId == 0;
        e.netId = m.netId;
        e.handle = m.handle;
        e.owner = m.owner;
        if (isNew) {
            e.present = world_.Exists(m.handle);
            if (!e.present) {
                ++missing_;
                log_("host character not found in local world (save mismatch?) netId=" + std::to_string(m.netId));
            }
        }
        controllableDirty_ = true;
        break;
    }
    case Msg::Unbind: {
        Unbind m;
        if (!Decode(r, m)) break;
        auto it = entities_.find(m.netId);
        if (it == entities_.end()) break;
        if (!it->second.present && missing_) --missing_;
        entities_.erase(it);
        controllableDirty_ = true;
        break;
    }
    case Msg::Snapshot: {
        Snapshot s;
        if (!Decode(r, s)) break;
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
            buf.push_back({s.hostTime, st});
            while (buf.size() > 2 && buf.front().t < s.hostTime - kBufferSeconds) buf.pop_front();
        }
        break;
    }
    case Msg::TimeState: {
        TimeState t;
        if (Decode(r, t)) world_.SetTime(t);
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
    // Snapshots only travel on the snapshot channel, everything else on the reliable one.
    if ((*type == Msg::Snapshot) != (chan == kChanSnapshot)) return;
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
    // the leaver's characters go back to the host so they are never left uncontrolled
    for (auto& o : owners_) if (o.second == id) o.second = hostId_;
    for (auto& [nid, e] : entities_) {
        if (e.owner != id) continue;
        e.owner = hostId_;
        for (auto& [pid, p] : players_) SendBind(e, p.peer);
    }
    controllableDirty_ = true;
    Writer w;
    Encode(w, PlayerLeft{id});
    BroadcastReliable(w);
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
        BroadcastReliable(w);
        AddChat(cfg_.name + ": " + clean);
    } else if (state_ == SessionState::Connected) {
        Encode(w, c);
        SendReliable(net_.serverPeer(), w);
    }
}

void Session::SendReliable(PeerId to, const Writer& w) { net_.Send(to, kChanReliable, w.data(), w.size(), true); }

void Session::BroadcastReliable(const Writer& w, PeerId except) {
    // Only peers that completed the handshake receive game traffic.
    for (auto& [pid, p] : players_)
        if (p.peer != except) net_.Send(p.peer, kChanReliable, w.data(), w.size(), true);
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
    for (auto& [id, e] : entities_) if (e.handle == h) return &e;
    return nullptr;
}

uint8_t Session::ownerOf(const Handle& h) const {
    for (auto& [id, e] : entities_) if (e.handle == h) return e.owner;
    return 0;
}

uint32_t Session::pingMs() const {
    if (isHost()) return 0;
    return rttMs_;
}

} // namespace kc
