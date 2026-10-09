// Game-agnostic co-op session: host and client roles on top of Net + protocol.
// The game is reached only through IWorld, so the whole multiplayer logic is unit-testable with a
// fake world (see tests/). Everything here runs on the game thread, inside Session::Tick().
//
// The host's game is the server: it alone simulates. Joining = receiving the host's world:
//  1. the client connects (from the main menu or from any loaded game);
//  2. the host holds its world still, saves it, and streams the save to the client;
//  3. the client loads that save and reports Ready; from then on it renders the host's world:
//     every character near the squad is replicated by game handle (positions in snapshots,
//     health in vitals, clock/pause/speed in TimeState), and local simulation is switched off.
// Only what changed is sent, plus a periodic refresh so unreliable losses heal by themselves.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kc/net.h"
#include "kc/protocol.h"

namespace kc {

struct HandleHash {
    size_t operator()(const Handle& h) const {
        uint64_t x = (uint64_t(h.index) << 32 | h.serial) ^ (uint64_t(h.type) << 56) ^ (uint64_t(h.container) << 24) ^ h.containerSerial;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
        return size_t(x);
    }
};

enum class ExportStatus { Pending, Done, Failed };

// What the session needs from the game. Implemented by the Kenshi layer and by tests.
// Methods that touch the game world are only called on ticks where the world is live.
// Client: a host character absent from the local world, and how to recreate it.
struct MissingChar {
    Handle handle;
    SpawnInfo spawn;
    Vec3 pos;
};

class IWorld {
public:
    virtual ~IWorld() = default;

    virtual bool Ready() = 0;                  // a world is loaded and running
    virtual uint32_t WorldGeneration() = 0;    // changes every time a (new) world finishes loading
    virtual uint64_t Fingerprint() = 0;        // identifies the loaded world (same on host/client)
    virtual uint64_t GameBuild() = 0;
    virtual uint64_t ModsHash() = 0;

    // Members of the shared player squad.
    virtual void PlayerCharacters(std::vector<Handle>& out) = 0;
    // Host: every character (NPCs included) within `radius` of any of `centers`.
    virtual void NearbyCharacters(const std::vector<Vec3>& centers, float radius, std::vector<Handle>& out) = 0;

    virtual bool Exists(const Handle& h) = 0;
    virtual bool Read(const Handle& h, EntityState& out) = 0;      // pos/rot/dest/flags (netId ignored)
    virtual bool ReadVitals(const Handle& h, EntityVitals& out) = 0;
    // Host: is the character in melee combat, and against whom.
    virtual bool ReadCombat(const Handle& h, Handle& target) = 0;
    // Client: make the local copy fight `target` (host handle), or stop fighting.
    virtual void ApplyCombat(const Handle& h, bool fight, const Handle& target) = 0;
    // Host: what a client needs to recreate this character if its world lacks it.
    virtual bool ReadSpawnInfo(const Handle& h, SpawnInfo& out) = 0;
    // Host: a stable identity of the object at `h` (0 if none). Kenshi changes a character's handle
    // when it dies or changes squad; the identity tells it is still the same character.
    virtual uint64_t Identity(const Handle& h) = 0;
    // Client: the host's character known as `from` is now known as `to`.
    virtual void Rehandle(const Handle& from, const Handle& to) = 0;
    // Client: create a local stand-in for host character `h` (later found through Exists/Read/...
    // under the host's handle), and remove it again.
    virtual bool Spawn(const Handle& h, const SpawnInfo& info, const EntityState& at) = 0;
    virtual void Despawn(const Handle& h) = 0;
    // Client: line the local NPC population up with the host's. `known` holds every character the
    // host simulates; a local one outside it was made by the local game on its own. Such a
    // stranger becomes the stand-in of a `missing` host character of the same kind (its handle is
    // returned in `adopted`), or is removed once it has lingered.
    virtual void Reconcile(const std::vector<Handle>& known, const std::vector<MissingChar>& missing, double now,
                           std::vector<Handle>& adopted) = 0;

    // Client side: drive a replicated character toward the host state.
    // `target` is the interpolated state for "now - delay", `latest` the newest received one.
    virtual void Apply(const Handle& h, const EntityState& target, const EntityState& latest) = 0;
    virtual void ApplyVitals(const Handle& h, const EntityVitals& v) = 0;

    // Host side: execute an order for a character (issued by a client).
    virtual bool Order(const Handle& h, const Command& c) = 0;

    // Orders the local player gave this frame (client: intercepted instead of executed).
    virtual void TakeLocalOrders(std::vector<std::pair<Handle, Command>>& out) = 0;

    // Weather: host reads every region; client imposes the host's (applied by the game's own
    // weather update, which may run on another thread, so ApplyWeather only stores it).
    virtual void ReadWeather(std::vector<RegionWeather>& out) = 0;
    virtual void ApplyWeather(const std::vector<RegionWeather>& regions) = 0;
    // Weather effects (lightning, storms, gas clouds). Host: what its game placed, moved and
    // removed since the last call (plus the complete live set when `full`). Client: hand the
    // host's to the game (applied by the weather update too, so this only stores them).
    virtual void ReadEffects(EffectsMsg& out, bool full) { out = EffectsMsg{}; (void)full; }
    virtual void ApplyEffects(const EffectsMsg& m) { (void)m; }
    // Animations. Host: what its characters started/stopped since the last call (with `state`, also
    // every character's current action and modes). Client: replay one on that character.
    virtual void TakeAnimEvents(std::vector<std::pair<Handle, AnimEvent>>& out, bool state) { out.clear(); (void)state; }
    virtual void ApplyAnim(const Handle& h, const AnimEvent& e) { (void)h; (void)e; }
    // Host: every animation the character is playing; false when it is not worth sending (far away).
    virtual bool ReadAnimFrame(const Handle& h, AnimFrame& out) { (void)h; out = AnimFrame{}; return false; }
    // Client: impose them (ageSeconds: how long ago the host sampled them).
    virtual void ApplyAnimFrame(const Handle& h, const AnimFrame& f, double ageSeconds) { (void)h; (void)f; (void)ageSeconds; }
    // Items on the ground. Host: dropped/picked up since the last call. Client: replay one.
    virtual void TakeGroundEvents(std::vector<GroundEvent>& out) { out.clear(); }
    virtual void ApplyGround(const GroundEvent& e) { (void)e; }
    // Client: items the local player dropped from a character (asked of the host, not done locally).
    virtual void TakeLocalDrops(std::vector<std::pair<Handle, ItemState>>& out) { out.clear(); }

    // Inventories. Host: read; execute a client's item movement (false = refused/impossible).
    virtual bool ReadInventory(const Handle& h, std::vector<ItemState>& out) = 0;
    virtual bool ExecuteInvOp(const Handle& from, const Handle& to, const InvOp& op) = 0;
    // Client: make the local copy hold exactly these items (false = could not, retry later).
    virtual bool ApplyInventory(const Handle& h, const std::vector<ItemState>& items) = 0;

    virtual TimeState GetTime() = 0;
    virtual void SetTime(const TimeState& t) = 0;

    // Host: freeze the world while players join (true), release it afterwards (false).
    virtual void HoldForJoin(bool hold) = 0;
    // Host, while holding for a join: the joining player's own character (found by name, else
    // created and recruited into the squad). It is then in the save the player receives.
    virtual bool EnsurePlayerCharacter(const std::string& playerName, Handle& out) = 0;
    // Host: save the current world for transfer; poll until Done (files filled) or Failed.
    virtual bool BeginWorldExport(std::string* err) = 0;
    virtual ExportStatus PollWorldExport(std::vector<WorldFile>& files, std::string* err) = 0;
    // Client: write the host's save locally and start loading it (may be called from a menu).
    virtual bool BeginWorldImport(const std::vector<WorldFile>& files, std::string* err) = 0;

    // Role hooks: on clients, the world must stop simulating anything (the host owns it all).
    virtual void SetRole(bool client, bool active) = 0;
    // Which squad members the local player may command.
    virtual void SetControllable(const std::vector<Handle>& handles) = 0;
};

struct SessionConfig {
    std::string name = "Player";
    uint16_t port = kDefaultPort;
    double snapshotRate = 20.0;        // Hz
    double vitalsRate = 5.0;           // Hz
    double interpDelay = 0.05;         // seconds of buffering on clients (one snapshot interval)
    double timeStateInterval = 0.5;    // host re-sends TimeState at least this often
    double refreshInterval = 1.0;      // unchanged entities are still re-sent this often
    double handshakeTimeout = 10.0;    // seconds a peer may stay connected without a valid Hello
    double exportTimeout = 120.0;      // host: saving the world for a joiner may take this long
    double loadTimeout = 300.0;        // client: downloading + loading the host's world
    double worldLostTimeout = 5.0;     // seconds without a live world before the session ends
    float snapDistance = 50.0f;        // samples further apart than this are not interpolated
    float interestRadius = 0.0f;       // NPCs within this distance of the squad are replicated (0 = all active)
    bool characterPerPlayer = true;    // host: every joining player gets a character of their own
};

enum class SessionState { Idle, Hosting, Connecting, Handshake, Downloading, Loading, Connected, Failed };

struct RemotePlayer {
    uint8_t id = 0;
    std::string name;
    PeerId peer = kNoPeer;   // host side only
    uint32_t rttMs = 0;
    bool inGame = true;      // host side: finished loading the world
};

class Session {
public:
    using LogFn = std::function<void(const std::string&)>;
    using ClockFn = std::function<double()>;   // monotonic wall clock, seconds

    Session(IWorld& world, SessionConfig cfg, ClockFn clock, LogFn log);
    ~Session();

    // Name and port used by the next Host/Join (false while a session is running).
    bool Configure(const std::string& name, uint16_t port);
    bool Host(std::string* err);
    bool Join(const std::string& address, uint16_t port, std::string* err);
    void Leave();

    // Call once per frame. `worldLive`: the game world is loaded and safe to touch this frame.
    void Tick(bool worldLive = true);

    // Host: hand a squad member to a player (host id = back to the host).
    void Assign(const Handle& h, uint8_t playerId);
    // Host: remove a player from the session (false: no such player, or not hosting).
    bool KickPlayer(uint8_t playerId);
    void SendChat(const std::string& text);

    SessionState state() const { return state_; }
    bool isHost() const { return state_ == SessionState::Hosting; }
    bool isClient() const {
        return state_ == SessionState::Connected || state_ == SessionState::Handshake || state_ == SessionState::Connecting ||
               state_ == SessionState::Downloading || state_ == SessionState::Loading;
    }
    uint8_t localId() const { return localId_; }
    const std::string& lastError() const { return lastError_; }
    const std::map<uint8_t, RemotePlayer>& players() const { return players_; }
    const std::deque<std::string>& chatLog() const { return chat_; }
    uint8_t ownerOf(const Handle& h) const;
    size_t entityCount() const { return entities_.size(); }
    size_t npcCount() const;
    uint32_t missingSquad() const { return missingSquad_; }   // client: squad members not found locally
    uint32_t missingNpcs() const;                             // client: NPCs the host has but we do not
    uint32_t spawnedNpcs() const;                             // client: stand-ins created for them
    uint32_t pingMs() const;
    double downloadProgress() const;                          // client: 0..1 while Downloading
    void ForEachEntity(const std::function<void(uint32_t netId, const Handle& h, uint8_t owner, bool squad, bool present)>& fn) const;
    // client (diagnostics): newest state received from the host and the state being rendered now
    bool TargetOf(const Handle& h, EntityState& latest, EntityState& rendered) const;
    size_t joiningPlayers() const;                            // host: players still loading the world

private:
    struct Sample { double t; EntityState s; };
    struct Entity {
        uint32_t netId = 0;
        Handle handle;
        uint8_t owner = 0;
        bool squad = false;
        // client
        bool present = false;                // handle resolved in the local world
        bool checked = false;                // presence evaluated at least once
        std::deque<Sample> buf;              // interpolation buffer, oldest first
        bool haveVitals = false;
        bool vitalsDirty = false;
        EntityVitals vitals;
        bool hasSpawn = false;               // host sent how to recreate it
        SpawnInfo spawn;
        bool spawned = false;                // client created a stand-in for it
        uint32_t combatApplied = 0;          // client: combat target last imposed (netId)
        double combatReapply = 0;
        // inventories
        bool haveInv = false;                // client: host inventory received
        bool invDirty = false;               // client: must be (re)applied locally
        double invRetry = 0;
        std::vector<ItemState> inv;          // client: the host's view; host: last broadcast
        uint64_t invHash = 0;                // host: hash of the last broadcast
        double invPendingUntil = 0;          // client: an InvOp is in flight, do not diff
        double invUnmatchedSince = 0;        // client: an item left with nowhere to go (held by the mouse?) since
        int invFailures = 0;                 // client: local rebuild attempts that did not match
        int spawnAttempts = 0;
        double nextSpawnTry = 0;
        double missingSince = -1;            // client: when it was last found missing locally
        // host
        bool keep = false;                   // scratch flag for interest updates
        uint64_t identity = 0;               // IWorld::Identity when bound
    };
    struct Sent {                            // host: what a player last received for an entity
        EntityState state;
        double at = -1e9;
        EntityVitals vitals;
        double vitalsAt = -1e9;
    };
    struct PlayerSync {
        std::unordered_map<uint32_t, Sent> sent;
        bool worldSent = false;              // the world save was streamed to this player
        double joinedAt = 0;
        uint64_t readyHash = 0;              // pending Ready to verify on the next live tick
        bool readyPending = false;
        bool kicked = false;
        Handle own;                          // the player's own character (characterPerPlayer)
        bool ownChecked = false;
    };

    void HostTick(double now, bool live);
    void ClientTick(double now, bool live);
    void OnPacket(PeerId peer, uint8_t chan, const uint8_t* data, size_t size);
    void HostPacket(PeerId peer, Msg type, Reader& r);
    void ClientPacket(Msg type, Reader& r);
    void OnDisconnect(PeerId peer);

    void HostJoinFlow(double now, bool live);
    void StreamWorld(const RemotePlayer& p);
    void FinishJoin(RemotePlayer& p);
    void UpdateInterest();                   // host: (un)bind squad members and nearby NPCs
    void SendSnapshots(double now);
    void SendVitals(double now);
    void SendBind(const Entity& e, PeerId to, const Handle& previous = Handle{});
    void SendInventories(double now, bool force, PeerId onlyTo);
    void ClientInventoryDiff(double now);
    void SendLocalDrops();
    void HostInvOp(uint8_t from, const InvOp& op);
    void SendReliable(PeerId to, const Writer& w);
    void BroadcastReliable(const Writer& w, bool inGameOnly, PeerId except = kNoPeer);
    void PushControllable();
    void Fail(const std::string& why);
    void AddChat(const std::string& line);
    void Kick(RemotePlayer& p, RejectReason why);
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
    const uint8_t hostId_ = 1;
    std::map<uint8_t, RemotePlayer> players_;      // excludes the local player
    std::map<uint8_t, PlayerSync> sync_;           // host: per-player state
    std::deque<std::string> chat_;

    std::unordered_map<uint32_t, Entity> entities_;  // netId -> entity
    std::unordered_map<Handle, uint32_t, HandleHash> byHandle_;  // handle -> netId
    std::vector<std::pair<Handle, uint8_t>> owners_;  // host: explicit squad assignments
    std::unordered_map<uint64_t, uint32_t> byIdentity_;   // host: IWorld::Identity -> netId
    uint32_t nextNetId_ = 1;
    uint32_t tick_ = 0;
    uint32_t cmdSeq_ = 0;
    double nextSnapshot_ = 0;
    double nextVitals_ = 0;
    double nextTimeState_ = 0;
    double nextInterest_ = 0;
    double nextPing_ = 0;
    double nextPresenceCheck_ = 0;
    double nextVitalsApply_ = 0;
    double lastLive_ = 0;
    TimeState lastTime_;
    double nextWeather_ = 0;
    double nextInventory_ = 0;
    double nextInvDiff_ = 0;
    std::vector<std::pair<uint8_t, InvOp>> pendingInvOps_;   // host: run on the next live tick
    double weatherForceAt_ = 0;
    double effectsFullAt_ = 0;
    double animStateAt_ = 0;
    double nextForeignNote_ = 0;
    double nextAnimFrame_ = 0;
    std::vector<RegionWeather> lastWeather_;
    bool controllableDirty_ = true;
    uint32_t missingSquad_ = 0;
    double connectStarted_ = 0;
    std::map<PeerId, double> pendingPeers_;          // host: connected, Hello not received yet
    std::vector<std::pair<uint8_t, Command>> pendingCommands_;  // host: run on the next live tick
    std::vector<Handle> despawnQueue_;               // client: stand-ins to remove on the next live tick

    // host world export (shared by everyone joining at the same time)
    bool holding_ = false;
    bool exporting_ = false;
    double exportStarted_ = 0;
    std::vector<WorldFile> exportFiles_;
    bool exportReady_ = false;
    uint64_t exportHash_ = 0;

    // client world download
    WorldBegin dlInfo_;
    std::vector<WorldFile> dlFiles_;
    uint64_t dlBytes_ = 0;
    uint64_t dlHash_ = 0;
    bool dlComplete_ = false;
    bool importStarted_ = false;
    uint32_t importGeneration_ = 0;
    double loadStarted_ = 0;
    double loadStableSince_ = 0;
    bool sawOtherWorld_ = false;

    // client: latest host time, applied on live ticks
    bool haveTime_ = false;
    TimeState hostTime_;

    // client clock sync: hostTime ~= localTime + offset_
    double offset_ = 0;
    bool offsetValid_ = false;
    uint32_t rttMs_ = 0;

    // host per-tick caches
    std::unordered_map<uint32_t, EntityState> stateCache_;
    std::unordered_map<uint32_t, EntityVitals> vitalsCache_;
    std::vector<Handle> scratchHandles_;
    std::vector<std::pair<Handle, Command>> scratchOrders_;
};

} // namespace kc
