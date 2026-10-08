// Game-agnostic co-op session: host and client roles on top of Net + protocol.
// The game is reached only through IWorld, so the whole multiplayer logic is unit-testable with a
// fake world (see tests/). Everything here runs on the game thread, inside Session::Tick().
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "kc/net.h"
#include "kc/protocol.h"

namespace kc {

// What the session needs from the game. Implemented by the Kenshi layer and by tests.
class IWorld {
public:
    virtual ~IWorld() = default;

    virtual bool Ready() = 0;                  // a world is loaded and running
    virtual uint64_t Fingerprint() = 0;        // identifies the loaded save (same on host/client)
    virtual uint64_t GameBuild() = 0;
    virtual uint64_t ModsHash() = 0;

    // Characters that belong to the player faction (host side: what gets replicated).
    virtual void PlayerCharacters(std::vector<Handle>& out) = 0;
    virtual bool Exists(const Handle& h) = 0;
    virtual bool Read(const Handle& h, EntityState& out) = 0;   // pos/rot/dest/flags (netId ignored)

    // Client side: drive a replicated character toward the host state.
    // `target` is the interpolated state for "now - delay", `latest` the newest received one.
    virtual void Apply(const Handle& h, const EntityState& target, const EntityState& latest) = 0;

    // Host side: execute an order for a character (issued locally by the host or by a client).
    virtual bool Order(const Handle& h, const Command& c) = 0;

    // Orders the local player gave this frame (client: intercepted instead of executed).
    virtual void TakeLocalOrders(std::vector<std::pair<Handle, Command>>& out) = 0;

    virtual TimeState GetTime() = 0;
    virtual void SetTime(const TimeState& t) = 0;

    // Role hooks: on clients, the world must stop simulating anything that is replicated.
    virtual void SetRole(bool client, bool active) = 0;
    // Which characters the local player may command (clients: their own; host: unassigned + own).
    virtual void SetControllable(const std::vector<Handle>& handles) = 0;
};

struct SessionConfig {
    std::string name = "Player";
    uint16_t port = kDefaultPort;
    double snapshotRate = 20.0;        // Hz
    double interpDelay = 0.10;         // seconds of buffering on clients
    double timeStateInterval = 1.0;    // host re-sends TimeState at least this often
    double handshakeTimeout = 10.0;    // seconds a peer may stay connected without a valid Hello
    float snapDistance = 1000.0f;      // samples further apart than this are not interpolated
};

enum class SessionState { Idle, Hosting, Connecting, Handshake, Connected, Failed };

struct RemotePlayer {
    uint8_t id = 0;
    std::string name;
    PeerId peer = kNoPeer;   // host side only
    uint32_t rttMs = 0;
};

class Session {
public:
    using LogFn = std::function<void(const std::string&)>;
    using ClockFn = std::function<double()>;   // monotonic wall clock, seconds

    Session(IWorld& world, SessionConfig cfg, ClockFn clock, LogFn log);
    ~Session();

    bool Host(std::string* err);
    bool Join(const std::string& address, uint16_t port, std::string* err);
    void Leave();

    void Tick();  // call once per game frame

    // Host: hand characters to a player (0 = back to the host).
    void Assign(const Handle& h, uint8_t playerId);
    void SendChat(const std::string& text);

    SessionState state() const { return state_; }
    bool isHost() const { return state_ == SessionState::Hosting; }
    bool isClient() const { return state_ == SessionState::Connected || state_ == SessionState::Handshake || state_ == SessionState::Connecting; }
    uint8_t localId() const { return localId_; }
    const std::string& lastError() const { return lastError_; }
    const std::map<uint8_t, RemotePlayer>& players() const { return players_; }
    const std::deque<std::string>& chatLog() const { return chat_; }
    uint8_t ownerOf(const Handle& h) const;
    size_t entityCount() const { return entities_.size(); }
    uint32_t missingEntities() const { return missing_; }   // client: bound but not found locally
    uint32_t pingMs() const;

private:
    struct Sample { double t; EntityState s; };
    struct Entity {
        uint32_t netId = 0;
        Handle handle;
        uint8_t owner = 0;
        bool present = true;                 // client: handle resolved in the local world
        std::deque<Sample> buf;              // client: interpolation buffer, oldest first
    };

    void HostTick(double now);
    void ClientTick(double now);
    void OnPacket(PeerId peer, uint8_t chan, const uint8_t* data, size_t size);
    void HostPacket(PeerId peer, Msg type, Reader& r);
    void ClientPacket(Msg type, Reader& r);
    void OnDisconnect(PeerId peer);

    void SyncBindings();                     // host: bind/unbind player characters
    void SendBind(const Entity& e, PeerId to);
    void SendReliable(PeerId to, const Writer& w);
    void BroadcastReliable(const Writer& w, PeerId except = kNoPeer);
    void PushControllable();
    void Fail(const std::string& why);
    void AddChat(const std::string& line);
    RemotePlayer* playerByPeer(PeerId p);
    Entity* entityByHandle(const Handle& h);
    void ApplyCommand(uint8_t from, const Command& c);
    EntityState Interpolate(const Entity& e, double renderTime) const;

    IWorld& world_;
    SessionConfig cfg_;
    ClockFn clock_;
    LogFn log_;
    Net net_;
    Net::Callbacks cb_;

    SessionState state_ = SessionState::Idle;
    std::string lastError_;
    uint8_t localId_ = 0;
    std::map<uint8_t, RemotePlayer> players_;      // excludes the local player
    std::deque<std::string> chat_;

    std::unordered_map<uint32_t, Entity> entities_;  // netId -> entity
    std::vector<std::pair<Handle, uint8_t>> owners_;          // host: explicit assignments
    uint32_t nextNetId_ = 1;
    uint32_t tick_ = 0;
    uint32_t cmdSeq_ = 0;
    double nextSnapshot_ = 0;
    double nextTimeState_ = 0;
    double nextBindSync_ = 0;
    double nextPing_ = 0;
    TimeState lastTime_;
    bool controllableDirty_ = true;
    uint32_t missing_ = 0;
    double connectStarted_ = 0;
    std::map<PeerId, double> pendingPeers_;          // host: connected, Hello not received yet
    uint8_t hostId_ = 1;

    // client clock sync: hostTime ~= localTime + offset_
    double offset_ = 0;
    bool offsetValid_ = false;
    uint32_t lastSnapshotTick_ = 0;
    uint32_t rttMs_ = 0;

    std::vector<Handle> scratchHandles_;
    std::vector<std::pair<Handle, Command>> scratchOrders_;
};

} // namespace kc
