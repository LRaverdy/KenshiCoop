// Single-threaded ENet wrapper. Call Poll() once per game tick from the game thread: there is no
// network thread, so packet handlers run where game state may be touched and no locking exists.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

struct _ENetHost;
struct _ENetPeer;

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
    bool Connect(const std::string& host, uint16_t port, std::string* err);
    void Close();                 // graceful disconnect of all peers, then destroy the host

    void Poll(const Callbacks& cb);          // non-blocking; dispatches every pending event
    bool Send(PeerId peer, uint8_t channel, const void* data, size_t size, bool reliable);
    void Broadcast(uint8_t channel, const void* data, size_t size, bool reliable, PeerId except = kNoPeer);
    void Flush();                            // push queued packets to the socket now
    void Kick(PeerId peer);                  // disconnect after pending reliable data is sent

    bool active() const { return host_ != nullptr; }
    bool isServer() const { return server_; }
    PeerId serverPeer() const { return serverPeer_; }  // client side: the host connection once up
    NetStats stats(PeerId peer) const;

private:
    _ENetPeer* find(PeerId id) const;
    _ENetHost* host_ = nullptr;
    bool server_ = false;
    PeerId serverPeer_ = kNoPeer;
    PeerId nextId_ = 1;
};

} // namespace kc
