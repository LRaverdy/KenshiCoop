#include "kc/net.h"

#include <atomic>
#include <chrono>
#include <enet/enet.h>

#include "kc/protocol.h"

namespace kc {

namespace {
constexpr uint32_t kTimeoutMinMs = 5000, kTimeoutMaxMs = 15000;           // a crashed game is noticed in 15 s
constexpr uint32_t kQuietTimeoutMinMs = 30000, kQuietTimeoutMaxMs = 120000;   // a game loading a zone
// The keep-alive thread answers for a game thread that has not polled for kKeepAfter seconds, for
// kKeepAtMost seconds at most (a game hung for good is then dropped like a crashed one).
constexpr double kKeepAfter = 0.3, kKeepAtMost = 90.0;
constexpr size_t kMaxDeferred = 20000;   // events queued for the game thread; beyond, ENet is left alone
double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace


namespace {
std::atomic<int> g_initCount{0};

PeerId IdOf(const ENetPeer* p) { return static_cast<PeerId>(reinterpret_cast<uintptr_t>(p->data)); }
} // namespace

bool Net::GlobalInit() {
    if (g_initCount.fetch_add(1) == 0) return enet_initialize() == 0;
    return true;
}
void Net::GlobalShutdown() {
    if (g_initCount.fetch_sub(1) == 1) enet_deinitialize();
}

Net::Net() = default;
Net::~Net() { Close(); }

void Net::StartKeepAlive() {
    lastPoll_ = SteadySeconds();
    keepRun_ = true;
    keepThread_ = std::thread([this] { KeepAlive(); });
}

void Net::StopKeepAlive() {
    keepRun_ = false;
    if (keepThread_.joinable()) keepThread_.join();
}

void Net::KeepAlive() {
    while (keepRun_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double idle = SteadySeconds() - lastPoll_.load();
        if (idle < kKeepAfter || idle > kKeepAtMost) continue;
        // never wait for the game thread: it may be closing this Net (and joining this thread)
        std::unique_lock<std::recursive_mutex> lk(mu_, std::try_to_lock);
        if (!lk.owns_lock() || !host_) continue;
        Event ev;
        while (deferred_.size() < kMaxDeferred && ServiceOne(ev)) deferred_.push_back(ev);
    }
}

bool Net::ServiceOne(Event& out) {
    ENetEvent ev;
    // enet_host_service with timeout 0 never blocks
    if (!host_ || enet_host_service(host_, &ev, 0) <= 0) return false;
    out = Event{};
    out.type = int(ev.type);
    switch (ev.type) {
    case ENET_EVENT_TYPE_CONNECT:
        if (server_) {
            ev.peer->data = reinterpret_cast<void*>(uintptr_t(nextId_++));
            enet_peer_timeout(ev.peer, 0, kTimeoutMinMs, kTimeoutMaxMs);
        } else {
            serverPeer_ = IdOf(ev.peer);
        }
        out.id = IdOf(ev.peer);
        break;
    case ENET_EVENT_TYPE_RECEIVE:
        out.id = IdOf(ev.peer);
        out.channel = ev.channelID;
        out.packet = ev.packet;
        break;
    case ENET_EVENT_TYPE_DISCONNECT:
        out.id = IdOf(ev.peer);
        ev.peer->data = nullptr;
        if (!server_) serverPeer_ = kNoPeer;
        break;
    default: break;
    }
    return true;
}

bool Net::Listen(uint16_t port, size_t maxPeers, std::string* err) {
    Close();
    std::lock_guard<std::recursive_mutex> lk(mu_);
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    host_ = enet_host_create(&addr, maxPeers, kChannelCount, 0, 0);
    if (!host_) {
        if (err) *err = "cannot listen on UDP port " + std::to_string(port) + " (already in use?)";
        return false;
    }
    host_->maximumPacketSize = kMaxPacketSize;
    host_->duplicatePeers = 4;
    server_ = true;
    StartKeepAlive();
    return true;
}

bool Net::Connect(const std::string& hostName, uint16_t port, std::string* err, uint32_t mtu) {
    Close();
    std::lock_guard<std::recursive_mutex> lk(mu_);
    ENetAddress addr{};
    if (enet_address_set_host(&addr, hostName.c_str()) != 0) {
        if (err) *err = "cannot resolve address '" + hostName + "'";
        return false;
    }
    addr.port = port;
    host_ = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (!host_) {
        if (err) *err = "cannot create network socket";
        return false;
    }
    host_->maximumPacketSize = kMaxPacketSize;
    if (mtu) host_->mtu = mtu;   // the host agrees to the smaller of both sides' MTU
    ENetPeer* peer = enet_host_connect(host_, &addr, kChannelCount, 0);
    if (!peer) {
        if (err) *err = "cannot start connection";
        Close();
        return false;
    }
    peer->data = reinterpret_cast<void*>(uintptr_t(nextId_++));
    enet_peer_timeout(peer, 0, kTimeoutMinMs, kTimeoutMaxMs);
    server_ = false;
    StartKeepAlive();
    return true;
}

void Net::Close() {
    StopKeepAlive();
    std::lock_guard<std::recursive_mutex> lk(mu_);
    for (auto& e : deferred_) if (e.packet) enet_packet_destroy(e.packet);
    deferred_.clear();
    quietUntil_.clear();
    if (!host_) return;
    for (size_t i = 0; i < host_->peerCount; ++i) {
        ENetPeer* p = &host_->peers[i];
        if (p->state == ENET_PEER_STATE_CONNECTED) enet_peer_disconnect_now(p, 0);
    }
    enet_host_flush(host_);
    enet_host_destroy(host_);
    host_ = nullptr;
    serverPeer_ = kNoPeer;
    server_ = false;
}

void Net::Poll(const Callbacks& cb) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    lastPoll_ = SteadySeconds();
    if (!host_) return;
    if (!quietUntil_.empty()) {
        const double now = SteadySeconds();
        for (auto it = quietUntil_.begin(); it != quietUntil_.end();) {
            if (now < it->second) { ++it; continue; }
            if (ENetPeer* p = find(it->first)) enet_peer_timeout(p, 0, kTimeoutMinMs, kTimeoutMaxMs);
            it = quietUntil_.erase(it);
        }
    }
    // what the keep-alive thread received while this thread was busy comes first, in order;
    // then everything pending (loop until the queue is drained)
    Event ev;
    for (;;) {
        if (!deferred_.empty()) {
            ev = deferred_.front();
            deferred_.pop_front();
        } else if (!ServiceOne(ev)) {
            break;
        }
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (cb.onConnect) cb.onConnect(ev.id);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (cb.onPacket && ev.id != kNoPeer) cb.onPacket(ev.id, ev.channel, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (cb.onDisconnect && ev.id != kNoPeer) cb.onDisconnect(ev.id);
            break;
        default: break;
        }
        if (!host_) {   // a handler closed the connection
            for (auto& e : deferred_) if (e.packet) enet_packet_destroy(e.packet);
            deferred_.clear();
            break;
        }
    }
}

ENetPeer* Net::find(PeerId id) const {
    if (!host_ || id == kNoPeer) return nullptr;
    for (size_t i = 0; i < host_->peerCount; ++i) {
        ENetPeer* p = &host_->peers[i];
        if (p->state == ENET_PEER_STATE_CONNECTED && IdOf(p) == id) return p;
    }
    return nullptr;
}

bool Net::Send(PeerId peer, uint8_t channel, const void* data, size_t size, bool reliable) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    ENetPeer* p = find(peer);
    if (!p || channel >= kChannelCount) return false;
    ENetPacket* pkt = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if (!pkt) return false;
    if (enet_peer_send(p, channel, pkt) != 0) {
        enet_packet_destroy(pkt);
        return false;
    }
    return true;
}

void Net::Broadcast(uint8_t channel, const void* data, size_t size, bool reliable, PeerId except) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!host_ || channel >= kChannelCount) return;
    // One packet shared by all peers (ENet refcounts it).
    ENetPacket* pkt = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if (!pkt) return;
    for (size_t i = 0; i < host_->peerCount; ++i) {
        ENetPeer* p = &host_->peers[i];
        if (p->state != ENET_PEER_STATE_CONNECTED || IdOf(p) == kNoPeer || IdOf(p) == except) continue;
        enet_peer_send(p, channel, pkt);
    }
    if (pkt->referenceCount == 0) enet_packet_destroy(pkt);
}

void Net::Flush() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (host_) enet_host_flush(host_);
}

void Net::ExpectSilence(PeerId peer, double seconds) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (peer == kNoPeer) peer = serverPeer_;
    ENetPeer* p = find(peer);
    if (!p) return;
    enet_peer_timeout(p, 0, kQuietTimeoutMinMs, kQuietTimeoutMaxMs);
    quietUntil_[peer] = SteadySeconds() + seconds;
}

void Net::Kick(PeerId peer) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (ENetPeer* p = find(peer)) enet_peer_disconnect_later(p, 0);
}

NetStats Net::stats(PeerId peer) const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    NetStats s;
    if (ENetPeer* p = find(peer)) {
        s.rttMs = p->roundTripTime;
        s.bytesIn = p->incomingDataTotal;
        s.bytesOut = p->outgoingDataTotal;
    }
    return s;
}

} // namespace kc

namespace kc {
uint32_t Net::silentMs(PeerId peer) const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    ENetPeer* p = find(peer);
    if (!p || !host_) return 0;
    const uint32_t now = enet_time_get();
    return now - p->lastReceiveTime < 86400000u ? now - p->lastReceiveTime : 0u;   // wraps like ENet time
}
} // namespace kc
