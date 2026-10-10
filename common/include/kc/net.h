// ENet wrapper. Call Poll() once per game tick from the game thread: packet handlers run there,
// where game state may be touched. A keep-alive thread services ENet only while the game thread
// has not polled for a moment (the game froze loading a zone, saving...): it acknowledges what
// arrives and queues the events for the next Poll, so the other side tells a frozen game (still
// answering) from a crashed one (silent, dropped after the timeout). Every member locks mu_.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

struct _ENetHost;
struct _ENetPeer;
struct _ENetPacket;

namespace kc {

using PeerId = uint32_t;  // stable per connection, never reused during a process lifetime
constexpr PeerId kNoPeer = 0;

struct NetStats {
    uint32_t rttMs = 0;
    uint64_t bytesIn = 0, bytesOut = 0;
};

class Net {
public:
    struct Callbacks {
        std::function<void(PeerId)> onConnect;
        std::function<void(PeerId)> onDisconnect;
        std::function<void(PeerId, uint8_t channel, const uint8_t* data, size_t size)> onPacket;
    };

    Net();
    ~Net();
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;

    static bool GlobalInit();     // enet_initialize, refcounted
    static void GlobalShutdown();

    bool Listen(uint16_t port, size_t maxPeers, std::string* err);
    // mtu: largest datagram the path carries (0 = ENet's default), e.g. a relay with a smaller limit
    bool Connect(const std::string& host, uint16_t port, std::string* err, uint32_t mtu = 0);
    void Close();                 // graceful disconnect of all peers, then destroy the host

    void Poll(const Callbacks& cb);          // non-blocking; dispatches every pending event
    bool Send(PeerId peer, uint8_t channel, const void* data, size_t size, bool reliable);
    void Broadcast(uint8_t channel, const void* data, size_t size, bool reliable, PeerId except = kNoPeer);
    void Flush();                            // push queued packets to the socket now
    void Kick(PeerId peer);                  // disconnect after pending reliable data is sent
    // That peer may not answer for up to `seconds` (its game freezes while loading a zone): its
    // timeout grows meanwhile, then goes back to normal. kNoPeer on a client: the host.
    void ExpectSilence(PeerId peer, double seconds);

    bool active() const { return host_ != nullptr; }
    bool isServer() const { return server_; }
    PeerId serverPeer() const { return serverPeer_; }  // client side: the host connection once up
    NetStats stats(PeerId peer) const;
    // Milliseconds since that peer last sent anything (a live game answers ENet's pings every half
    // second, even frozen; a crashed one is silent). 0: unknown peer.
    uint32_t silentMs(PeerId peer) const;
    // Everything reliable sent to that peer so far has been acknowledged (nothing queued or in
    // flight): a big transfer has arrived. False for an unknown peer.
    bool reliableIdle(PeerId peer) const;

private:
    struct Event { int type = 0; PeerId id = 0; uint8_t channel = 0; _ENetPacket* packet = nullptr; };
    _ENetPeer* find(PeerId id) const;
    bool ServiceOne(Event& out);   // one ENet event, ids resolved (mu_ held)
    void StartKeepAlive();
    void StopKeepAlive();
    void KeepAlive();
    mutable std::recursive_mutex mu_;
    std::thread keepThread_;
    std::atomic<bool> keepRun_{false};
    std::atomic<double> lastPoll_{0};
    std::deque<Event> deferred_;   // serviced by the keep-alive thread, dispatched by the next Poll
    _ENetHost* host_ = nullptr;
    bool server_ = false;
    PeerId serverPeer_ = kNoPeer;
    PeerId nextId_ = 1;
    std::unordered_map<PeerId, double> quietUntil_;   // peers with a longer timeout, until then (steady seconds)
};

} // namespace kc
