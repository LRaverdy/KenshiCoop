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
#include <set>
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
    // Carrying a character on the shoulder. Host: who `h` carries. Client: carry that one / put it down.
    virtual bool ReadCarry(const Handle& h, Handle& carried) { (void)h; (void)carried; return false; }
    virtual void ApplyCarry(const Handle& h, bool carry, const Handle& carried) { (void)h; (void)carry; (void)carried; }
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
    // Progression. Host: a character's skill levels (kStatCount values), the player faction's money.
    // Client: impose the host's.
    virtual bool ReadProgress(const Handle& h, std::vector<float>& stats, uint16_t& modes, uint8_t& style) {
        (void)h; (void)modes; (void)style; stats.clear(); return false;
    }
    virtual void ApplyProgress(const Handle& h, const std::vector<float>& stats, uint16_t modes, uint8_t style) { (void)h; (void)stats; (void)modes; (void)style; }
    // The tool a character's current job puts in its hands (template; empty: none). Client: show it.
    virtual bool ReadTool(const Handle& h, std::string& sid) { (void)h; sid.clear(); return false; }
    virtual void ApplyTool(const Handle& h, const std::string& sid) { (void)h; (void)sid; }
    // The floor a character is on inside a building (the game's floor group); client: put it there,
    // which is what makes the floor shown follow a character taking the stairs. fix G6
    virtual bool ReadFloor(const Handle& h, uint8_t& group) { (void)h; (void)group; return false; }
    virtual void ApplyFloor(const Handle& h, uint8_t group) { (void)h; (void)group; }
    virtual bool ReadMoney(int32_t& money) { (void)money; return false; }
    virtual void ApplyMoney(int32_t money) { (void)money; }
    // Conversations. Host: lines said and conversation windows of other players' characters since the
    // last call (`speaker` says / talks with `pc`). Client: show a line said by `speaker`.
    struct WorldDialog {
        DialogKind kind = DialogKind::Say;
        uint32_t dialogId = 0;
        Handle speaker, pc;
        std::string text;
        bool shout = false;
        std::vector<std::string> replies;
    };
    virtual void TakeDialogEvents(std::vector<WorldDialog>& out) { out.clear(); }
    virtual void ApplySay(const Handle& speaker, const std::string& text, bool shout) { (void)speaker; (void)text; (void)shout; }
    virtual void DialogAnswer(uint32_t dialogId, int index) { (void)dialogId; (void)index; }   // host
    // Squads of the player faction. Host: each squad's name and members (in squad order).
    // Client: split the local characters the same way.
    struct WorldSquad {
        std::string name;
        std::vector<Handle> members;
    };
    virtual void ReadSquads(std::vector<WorldSquad>& out) { out.clear(); }
    virtual void ApplySquads(const std::vector<WorldSquad>& squads) { (void)squads; }
    // Appearance (the game's character editor). Characters whose looks were just edited here; read,
    // apply and edit them.
    virtual void TakeEditedCharacters(std::vector<Handle>& out) { out.clear(); }
    virtual bool ReadAppearance(const Handle& h, AppearanceMsg& out) { (void)h; (void)out; return false; }
    virtual void ApplyAppearance(const Handle& h, const AppearanceMsg& m) { (void)h; (void)m; }
    virtual bool OpenCharacterEditor(const Handle& h) { (void)h; return false; }
    virtual bool CharacterEditorOpen() { return false; }
    // Client: the largest position correction since the last call, and how many characters stand
    // more than 5 units off the host's position now.
    virtual void TakeSyncStats(float& maxErr, uint16_t& farOff) { maxErr = 0; farOff = 0; }
    // A readable name for a game object template (string id), for the log.
    virtual std::string TemplateName(const std::string& sid) { return sid; }
    // Containers. Either side: the container of that kind at that place (handles differ between
    // machines). Host: walking distance check, theft (0 not theft, 1 theft unseen, 2 caught).
    // Client: the player right-clicked one (which of our characters, what, where); open the game's
    // window between our character and it; whether that window is still open.
    virtual bool FindContainer(const std::string& sid, const Vec3& pos, Handle& out) { (void)sid; (void)pos; (void)out; return false; }
    virtual float DistanceTo(const Handle& who, const Vec3& pos) { (void)who; (void)pos; return 1e9f; }
    virtual bool ContainerKind(const Handle& container, std::string& sid) { (void)container; sid.clear(); return false; }
    virtual int TheftCheck(const Handle& thief, const Handle& container, const ItemState& item) { (void)thief; (void)container; (void)item; return 0; }
    struct ContainerRequest { Handle looter; std::string sid; Vec3 pos; };
    virtual void TakeContainerRequests(std::vector<ContainerRequest>& out) { out.clear(); }
    virtual bool OpenContainerWindow(const Handle& looter, const Handle& container) { (void)looter; (void)container; return false; }
    virtual bool ContainerWindowOpen() { return false; }
    virtual void CloseContainerWindows() {}
    // Client: items the local player dropped from a character (asked of the host, not done locally).
    virtual void TakeLocalDrops(std::vector<std::pair<Handle, ItemState>>& out) { out.clear(); }
    // Trade with merchants. Host: trade windows the game asked to open for another player's character
    // (who trades, with whom); a merchant's shop counters (the containers its trade window sells
    // from); a character's cats (ours: the player faction's; a merchant: its own); move the price of
    // a purchase from the buyer's cats to the merchant's (negative: a sale); show the host's own trade
    // window on that merchant again after its stock changed. Client: open the game's trade window
    // between our character and the merchant; impose the merchant's cats.
    struct TradeRequest { Handle looter, trader; };
    virtual void TakeTradeRequests(std::vector<TradeRequest>& out) { out.clear(); }
    struct ShopCounter { Handle handle; std::string sid; Vec3 pos; };
    virtual bool ShopCounters(const Handle& trader, std::vector<ShopCounter>& out) { (void)trader; out.clear(); return false; }
    virtual bool MoneyOf(const Handle& who, int32_t& money) { (void)who; (void)money; return false; }
    virtual bool PayTrade(const Handle& buyer, const Handle& trader, int32_t price) { (void)buyer; (void)trader; (void)price; return false; }
    virtual void RefreshTradeWindow(const Handle& trader) { (void)trader; }
    virtual bool OpenTradeWindow(const Handle& looter, const Handle& trader) { (void)looter; (void)trader; return false; }
    virtual bool TradeWindowBusy() { return false; }   // client: an item is on the mouse (do not reopen the window now)
    virtual int TradeWindowStock() { return -1; }      // client: stacks the open trade window's merchant side shows (-1: no window)
    virtual void SetMoneyOf(const Handle& who, int32_t money) { (void)who; (void)money; }
    virtual std::string CharacterNameOf(const Handle& h) { (void)h; return {}; }
    // ---- lot B: factions. Host: the player faction's relations with every faction (both ways) and
    // a character's bounties and crime state. Client: impose the host's (the local game may not
    // change them itself). Apply returns how many values had to change here.
    virtual bool ReadFactions(FactionsMsg& out) { out = FactionsMsg{}; return false; }
    virtual size_t ApplyFactions(const FactionsMsg& m) { (void)m; return 0; }
    virtual bool ReadBounties(const Handle& h, CharBounties& out) { (void)h; out = CharBounties{}; return false; }
    virtual size_t ApplyBounties(const Handle& h, const CharBounties& b) { (void)h; (void)b; return 0; }
    // ---- fix G5: the job list (Tâches panel) of a character, by kind in order. Host: read it.
    // Client: remove from ours the jobs the host's no longer has.
    virtual bool ReadJobs(const Handle& h, std::vector<int32_t>& jobs) { (void)h; jobs.clear(); return false; }
    virtual void ApplyJobs(const Handle& h, const std::vector<int32_t>& jobs) { (void)h; (void)jobs; }
    // ---- lot A: doors and locks. Host: the doors and locked furniture within `radius` of the points,
    // as they are; is this container locked (it cannot be looked into); run a door button a client
    // clicked. Client: impose one door's state (found by kind and place; false: not here); the door
    // buttons the local player clicked (never run locally).
    virtual void ReadDoors(const std::vector<Vec3>& centers, float radius, std::vector<DoorState>& out) { (void)centers; (void)radius; out.clear(); }
    virtual bool ContainerLocked(const Handle& container) { (void)container; return false; }
    virtual bool ExecuteDoorRequest(const DoorRequest& r) { (void)r; return false; }
    virtual bool ApplyDoor(const DoorState& d) { (void)d; return false; }
    virtual void TakeDoorRequests(std::vector<DoorRequest>& out) { out.clear(); }
    // ---- end lot A
    // ---- lot D: prisons. Host: what holds a character captive (cage, shackles, slavery,
    // sentence); its netId is left 0. Client: impose it (the same local cage, the same flags), and
    // keep a caged character where the cage holds it.
    virtual bool ReadCaptive(const Handle& h, CaptiveState& out) { (void)h; (void)out; return false; }
    virtual void ApplyCaptive(const Handle& h, const CaptiveState& s) { (void)h; (void)s; }
    // ---- lot C: ranged combat. Host: the shots its game fired since the last call (fired on any
    // thread; the world queues them); where a character in ranged combat aims (false: not in it);
    // turrets near these points and where they aim. Client: fire the host's shot here (visual only:
    // the projectile's damage is refused like every other), impose a character's aim (ranged = false:
    // it left ranged combat), turn a turret.
    struct WorldShot {
        Handle shooter, target;      // target: invalid when none
        uint8_t stat = 0;
        Vec3 aimPos;
        Quat dir;
        std::string turretSid;       // a turret's shot (empty: the shooter's own weapon)
        Vec3 turretPos;
    };
    virtual void TakeShots(std::vector<WorldShot>& out) { out.clear(); }
    virtual bool ReplayShot(const WorldShot& s) { (void)s; return false; }
    struct WorldAim {
        uint8_t state = 0;
        Vec3 aimPos;
        Handle target;
    };
    virtual bool ReadRangedAim(const Handle& h, WorldAim& out) { (void)h; (void)out; return false; }
    virtual void ApplyRangedAim(const Handle& h, bool ranged, const WorldAim& a) { (void)h; (void)ranged; (void)a; }
    virtual void ReadTurrets(const std::vector<Vec3>& centers, float radius, std::vector<TurretAim>& out) { (void)centers; (void)radius; out.clear(); }
    virtual void ApplyTurret(const TurretAim& t) { (void)t; }
    // ---- lot E: buildings. Placements made in build mode here (client: not built, asked of the
    // host; host: built, `created` is the new building). Build a placement (the host's, or a
    // client's on the host). Find a building by kind and place; its kind and place. Construction
    // state: read (host) and impose (client). Player buildings being built or dismantled near
    // these points (host). Buildings the game destroyed for good (host, of those tracked). Remove
    // one (client). Buy / dismantle asked in the game's windows here (client), and executed.
    struct LocalPlacement { BuildPlace place; Handle created; };
    virtual void TakeLocalPlacements(std::vector<LocalPlacement>& out) { out.clear(); }
    virtual bool ExecutePlacement(const BuildPlace& p, Handle& created, Vec3& worldPos) { (void)p; (void)created; (void)worldPos; return false; }
    // Host: build mode's own checks on a placement before it is built (water or acid, slope, town,
    // inside or on top of another building). False: refused; why (English, for the log) and whyFr
    // (for the player who asked).
    virtual bool CheckPlacement(const BuildPlace& p, std::string& why, std::string& whyFr) { (void)p; why.clear(); whyFr.clear(); return true; }
    virtual bool FindBuilding(const std::string& sid, const Vec3& pos, Handle& out) { (void)sid; (void)pos; (void)out; return false; }
    virtual bool BuildingIdentity(const Handle& h, std::string& sid, Vec3& pos) { (void)h; (void)sid; (void)pos; return false; }
    virtual bool ReadBuildState(const Handle& h, float& progress, uint8_t& flags) { (void)h; (void)progress; (void)flags; return false; }
    virtual void ApplyBuildState(const Handle& h, float progress, uint8_t flags) { (void)h; (void)progress; (void)flags; }
    virtual void ConstructionSitesNear(const std::vector<Vec3>& centers, float radius, std::vector<Handle>& out) { (void)centers; (void)radius; out.clear(); }
    virtual void TrackBuilding(const Handle& h) { (void)h; }
    virtual void TakeBuildingRemovals(std::vector<Handle>& out) { out.clear(); }
    virtual bool RemoveBuilding(const Handle& h) { (void)h; return false; }
    virtual void TakeLocalBuildActions(std::vector<BuildAction>& out) { out.clear(); }
    // ok: done; refused: why not (French, for the player)
    virtual bool ExecuteBuildAction(const BuildAction& a, std::string& refused) { (void)a; refused.clear(); return false; }

    // Inventories. Host: read; execute a client's item movement (false = refused/impossible).
    virtual bool ReadInventory(const Handle& h, std::vector<ItemState>& out) = 0;
    virtual bool ExecuteInvOp(const Handle& from, const Handle& to, const InvOp& op) = 0;
    // Two items trading places in one go (a, from `from`, goes where b was in `to`; b goes to a's
    // place): what the game does when an item is dropped on an occupied slot. False: nothing moved.
    virtual bool ExecuteInvSwap(const Handle& from, const Handle& to, const InvOp& a, const InvOp& b) {
        (void)from; (void)to; (void)a; (void)b;
        return false;
    }
    // Client: make the local copy hold exactly these items (false = could not, retry later).
    virtual bool ApplyInventory(const Handle& h, const std::vector<ItemState>& items) = 0;

    virtual TimeState GetTime() = 0;
    virtual void SetTime(const TimeState& t) = 0;

    // Host: freeze the world while players join (true), release it afterwards (false).
    virtual void HoldForJoin(bool hold) = 0;
    // Host, while holding for a join: the joining player's own character (found by name, else
    // created and recruited into the squad). It is then in the save the player receives.
    // The joining player's own character: the one recorded for that Steam account (or, without
    // one, the squad member with the player's name), created if there is none. created: it is new.
    virtual bool EnsurePlayerCharacter(const std::string& playerName, uint64_t steamId, Handle& out, bool& created) = 0;
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
    uint64_t steamId = 0;              // this player's Steam account (0: none)
};

enum class SessionState { Idle, Hosting, Connecting, Handshake, Downloading, Loading, Connected, Failed };

struct RemotePlayer {
    uint8_t id = 0;
    std::string name;
    uint64_t steamId = 0;    // host side: the Steam account the player joined with (0: unknown)
    bool editing = false;    // host side: the player has the character editor open
    double editingSince = 0;
    ClientReport report;     // host side: the player's last report on their sync
    double reportAt = -1;
    double reportLoggedAt = -1e9;
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
    bool Join(const std::string& address, uint16_t port, std::string* err, uint32_t mtu = 0);
    void Leave();

    // Call once per frame. `worldLive`: the game world is loaded and safe to touch this frame.
    void Tick(bool worldLive = true);

    // Host: hand a squad member to a player (host id = back to the host).
    void Assign(const Handle& h, uint8_t playerId);
    // Host: remove a player from the session (false: no such player, or not hosting).
    bool KickPlayer(uint8_t playerId);
    // Host: that player's game is about to freeze (a far teleport): the link survives it. fix G6
    void ExpectStall(uint8_t playerId, double seconds);
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
    // Client: the conversation window of one of our characters (open = false: none).
    struct DialogView {
        bool open = false;
        uint32_t id = 0;
        std::string name, text;
        std::vector<std::string> replies;
        bool waiting = false;   // an answer was sent, the next line has not come yet
    };
    const DialogView& dialog() const { return dialog_; }
    // Client: lines of our log for the host's log (sent a few times a second); frames counted for
    // the sync reports.
    void QueueLog(std::string line);
    void CountFrame() { ++frames_; }
    // Client: open the game's character editor on our own character (its looks go to everyone).
    bool EditOwnCharacter();
    // Host: that player (0: everyone) leaves and joins again at once, reloading the host's world as
    // it is now. Client: the host asked for it (the caller leaves and joins again).
    size_t RequestResync(uint8_t playerId);
    bool TakeResyncRequest() { return std::exchange(resyncRequested_, false); }
    void AnswerDialog(int index);
    // Client: the trade window the host opened for us (tests, overlay).
    struct TradeView {
        bool pending = false, open = false;
        uint32_t trader = 0;
        size_t counters = 0;
        int32_t unsentSpend = 0;
    };
    TradeView tradeView() const {
        return {trade_.pending, trade_.open, trade_.trader, trade_.counters.size(), unsentSpend_};
    }
    size_t hostTrades() const { return trades_.size(); }   // host: trade windows open by players
    // ---- lot B: factions (tests): messages received / values corrected here (client), sent (host)
    struct FactionsView { size_t received = 0, bountiesReceived = 0, corrected = 0, sent = 0; };
    FactionsView factionsView() const { return factionsView_; }
    // ---- lot A: doors (tests): doors sent (host) / known and applied here (client)
    size_t doorsKnown() const { return isHost() ? doorsSent_.size() : clientDoors_.size(); }
    size_t doorsApplied() const { return doorsApplied_; }
    // ---- lot D: prisons (tests): captive characters this side knows of (host: sent; client: received)
    size_t captiveCount() const;
    // ---- lot C: ranged combat (tests, overlay): shots the host sent, the client fired / could not fire.
    struct RangedStats {
        uint64_t shotsSent = 0, shotsReplayed = 0, shotsFailed = 0, aimsApplied = 0, turretsApplied = 0;
    };
    const RangedStats& rangedStats() const { return rangedStats_; }
    // lot E: buildings followed (host: placed in the session or under construction near players;
    // client: those the host told us about), and how many are found here.
    size_t buildingCount() const { return buildings_.size(); }
    size_t buildingsResolved() const;

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
        std::vector<float> stats;            // client: the host's skill levels (empty = none yet)
        uint16_t modes = 0;
        uint8_t style = 0;
        std::string tool;                    // client: the host's tool in its hands
        bool statsDirty = false;
        bool hasSpawn = false;               // host sent how to recreate it
        SpawnInfo spawn;
        bool spawned = false;                // client created a stand-in for it
        uint32_t combatApplied = 0;          // client: combat target last imposed (netId)
        uint32_t carryApplied = 0;           // client: who it was last made to carry (netId)
        double carryReapply = 0;
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
        bool container = false;              // a container a player has open (no character)
        std::set<uint8_t> openBy;            // host: players who have it open
        Vec3 containerPos;                   // where it is
        uint32_t looter = 0;                 // client: our character looking into it
        uint64_t identity = 0;               // IWorld::Identity when bound
    };
    struct Sent {                            // host: what a player last received for an entity
        EntityState state;
        double at = -1e9;
        EntityVitals vitals;
        double vitalsAt = -1e9;
        std::vector<float> stats;
        uint16_t modes = 0xFFFF;
        uint8_t style = 0xFF;
        std::string tool;
        double statsAt = -1e9;
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
        bool ownCreated = false;             // made for this join (the player has never had one)
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
    void SendProgress(double now);
    void SendDialogs();
    void SendSquads(double now);
    void SendEditedAppearances();
    void SendBind(const Entity& e, PeerId to, const Handle& previous = Handle{});
    void SendInventories(double now, bool force, PeerId onlyTo);
    void ClientInventoryDiff(double now);
    void SendLocalDrops();
    void HostInvOp(uint8_t from, const InvOp& op, const InvOp* swapWith = nullptr);
    void HostInvOps();
    void HostTradeOp(uint8_t from, const InvOp& op);
    void SendReliable(PeerId to, const Writer& w);
    void BroadcastReliable(const Writer& w, bool inGameOnly, PeerId except = kNoPeer);
    void PushControllable();
    void Fail(const std::string& why);
    void AddChat(const std::string& shown, const std::string& logged = {});   // logged: the log's English line (default: shown)
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
    double nextProgress_ = 0;
    double nextStatsApply_ = 0;
    bool moneySent_ = false;
    int32_t lastMoney_ = 0;
    double moneyAt_ = -1e9;
    DialogView dialog_;                    // client
    SquadsMsg lastSquads_;                 // host: last sent / client: last received
    bool haveSquads_ = false;
    double squadsAt_ = -1e9, nextSquads_ = 0;
    std::unordered_map<uint32_t, uint8_t> dialogOwner_;   // host: conversation -> the player it was sent to
    std::vector<IWorld::WorldDialog> scratchDialogs_;
    std::vector<DialogReply> pendingAnswers_;   // host
    struct PendingContainer { uint8_t player; uint32_t looter; uint32_t netId; double until; };
    std::vector<PendingContainer> walkingToContainers_;   // host: looters on their way
    std::vector<std::pair<uint8_t, ContainerOpen>> containerAsks_;   // host: requests to handle on a live tick
    uint32_t pendingWindow_ = 0;           // client: open the window once this container's items are in
    double windowOpenedAt_ = -1;           // client: when our container window opened
    std::vector<IWorld::ContainerRequest> scratchContainerReqs_;
    void HostContainers(double now);
    void ClientContainers(double now);
    // ---- lot E: buildings
    struct BuildingRec {
        Handle handle;                       // in this world (client: found by kind and place)
        std::string sid;
        Vec3 pos;                            // world position
        bool resolved = false;               // client: found here
        double tryAt = 0;                    // client: next attempt to find it
        float progress = -1;                 // last sent (host) / applied (client)
        uint8_t flags = 0xFF;
        double sentAt = -1e9;                // host
        bool placed = false;                 // host: placed during the session (kept until removed)
        double seenBuilding = 0;             // host: last time it was under construction
        bool haveWant = false;               // client: the host's state of it
        float wantProgress = 0;
        uint8_t wantFlags = 0;
    };
    std::map<uint32_t, BuildingRec> buildings_;
    std::vector<std::pair<uint8_t, BuildPlace>> pendingPlaces_;          // host: clients' placements
    std::vector<std::pair<uint8_t, BuildAction>> pendingBuildActions_;   // host: clients' buy / dismantle
    std::vector<BuildPlace> hostPlaces_;     // client: the host's buildings to build here
    std::vector<BuildAction> hostActions_;   // client: purchases to replay
    std::vector<BuildStateEntry> hostStates_;   // client: to apply on the next live tick
    std::vector<BuildRemove> hostRemoves_;
    std::set<uint8_t> buildSyncedPlayers_;   // host: players who got every followed building once
    double nextBuildStates_ = 0, nextSiteScan_ = 0, nextBuildFull_ = 0;
    std::vector<IWorld::LocalPlacement> scratchPlacements_;
    std::vector<BuildAction> scratchBuildActions_;
    std::vector<Handle> scratchRemoved_;
    void HostBuildings(double now);
    // ---- fix G6: floors
    void HostFloors(double now);
    std::unordered_map<uint32_t, uint8_t> floorSent_;   // host: last floor group sent per netId
    double nextFloors_ = 0, nextFloorsFull_ = 0;
    size_t floorPlayers_ = 0;                           // host: players in the world at the last send
    std::unordered_map<uint32_t, uint8_t> floors_;      // client: the host's floor group per netId
    void ClientBuildings(double now);
    void HostBuildingPacket(RemotePlayer& from, Msg type, Reader& r);
    void ClientBuildingPacket(Msg type, Reader& r);
    uint32_t TrackBuilding(const Handle& h, const std::string& sid, const Vec3& pos, bool placed);
    void SendBuildStates(double now, bool full, PeerId onlyTo);
    void ResetBuildings();
    // Trade windows. Host: what each player has open (the merchant, their character, the shop's
    // counters); a merchant's cats last sent to them.
    struct HostTrade {
        uint32_t trader = 0, looter = 0;
        std::vector<uint32_t> counters;
        int32_t traderMoney = 0;
        double since = 0;
    };
    std::map<uint8_t, HostTrade> trades_;
    std::vector<IWorld::TradeRequest> scratchTradeReqs_;
    void HostTrades(double now);
    void EndTrade(uint8_t player, const std::string& reason);   // host: close it (reason shown to the player)
    bool InTrade(uint8_t player, uint32_t container) const;
    // Client: the trade window the host opened for us. Our game counts the price of each purchase or
    // sale itself: the cats it took (moneyBase_ - our cats) go with the item move to the host.
    struct ClientTrade {
        uint32_t trader = 0, looter = 0;
        std::vector<uint32_t> counters;
        int32_t traderMoney = 0;
        bool pending = false, open = false, refresh = false;
        double pendingSince = 0;
        double checkAt = 0;   // when to check that the window shows the counters' stock (0: checked)
        int reopens = 0;      // times it was opened again because it showed none of it
    };
    ClientTrade trade_;
    bool IsTradeCounter(uint32_t netId) const;
    void EndClientTrade();
    int32_t moneyBase_ = 0;                // client: our cats as last seen or set
    bool haveMoneyBase_ = false;
    int32_t unsentSpend_ = 0;              // client: cats our trade window took (gave: negative), not sent yet
    void CaptureLocalSpend();
    void ApplyHostMoney(int32_t money);
    // ---- lot B: factions, relations, bounties (session_factions.cpp)
    void SendFactions(double now);                    // host: relations and bounties when they change
    void ClientFactionsTick(double now);              // client: impose the host's, now and then
    bool ClientFactionsPacket(Msg type, Reader& r);   // client: Factions / Bounties (true: handled)
    void ResetFactions();
    double nextFactions_ = 0, nextFactionsFull_ = 0, nextFactionsApply_ = 0;
    uint64_t factionsHash_ = 0, bountiesHash_ = 0;
    std::set<PeerId> factionsServed_;                  // host: players who got the full state
    FactionsMsg hostFactions_;                         // client: the host's latest
    BountiesMsg hostBounties_;
    bool haveFactions_ = false, haveBounties_ = false, factionsDirty_ = false;
    FactionsView factionsView_;
    // ---- lot A: doors and locks. Host: what each door near the players looked like when last sent
    // (key: kind and place); door buttons clients clicked. Client: the host's doors, applied (and
    // re-applied now and then: a door of a zone that was not loaded yet, a local change).
    std::unordered_map<std::string, DoorState> doorsSent_;
    double nextDoors_ = 0, nextDoorsFull_ = 0;
    std::vector<DoorState> scratchDoors_;
    std::vector<std::pair<uint8_t, DoorRequest>> pendingDoorReqs_;
    std::vector<DoorRequest> scratchDoorReqs_;
    struct ClientDoor { DoorState state; bool dirty = true; bool found = false; };
    std::unordered_map<std::string, ClientDoor> clientDoors_;
    double nextDoorsApply_ = 0;
    size_t doorsApplied_ = 0;
    void HostDoors(double now);
    void ClientDoors(double now);
    void HostDoorPacket(uint8_t from, Reader& r);
    void ClientDoorsPacket(Reader& r);
    void ResetDoors();
    // ---- end lot A
    // ---- fix G5: job lists (session_jobs.cpp). Host: what each squad member's list was when last
    // sent. Client: the host's lists, imposed (dirty: just received).
    std::unordered_map<uint32_t, std::vector<int32_t>> jobsSent_;
    std::unordered_map<uint32_t, std::vector<int32_t>> hostJobs_;
    std::unordered_set<uint32_t> jobsDirty_;
    double nextJobs_ = 0, jobsFullAt_ = 0, jobsReapplyAt_ = 0;
    void HostJobs(double now);
    void ClientJobs(double now);
    void ClientJobsPacket(Reader& r);
    void ResetJobs();
    // ---- lot D: prisons (session_prisons.cpp)
    std::unordered_map<uint32_t, CaptiveState> captiveSent_;   // host: last state sent per character
    double nextCaptives_ = 0, captivesFullAt_ = 0;
    std::unordered_map<uint32_t, CaptiveState> captives_;      // client: the host's state per character
    std::unordered_set<uint32_t> captivesDirty_;               // client: to impose now
    double captivesReapplyAt_ = 0;
    void HostCaptives(double now);
    void ClientCaptives(double now);
    void OnCaptives(Reader& r);
    // ---- lot C: ranged combat (session_ranged.cpp). Host: shots go out as they are fired, aims a few
    // times a second when they change. Client: fired / imposed on the next live tick.
    void HostRanged(double now);
    void ClientRanged(double now);
    void ClientRangedPacket(Msg type, Reader& r);
    void ResetRanged();
    std::vector<IWorld::WorldShot> scratchShots_;
    std::unordered_map<uint32_t, RangedAim> rangedSent_;    // host: aim last sent per character
    std::map<std::string, TurretAim> turretSent_;           // host: aim last sent per turret ("sid@x,z")
    double nextRangedAim_ = 0, rangedRefreshAt_ = 0;
    std::vector<ShotEvent> pendingShots_;                   // client: to fire on the next live tick
    std::unordered_map<uint32_t, RangedAim> clientAims_;    // client: the host's aim per character, imposed every tick
    std::vector<uint32_t> pendingStopped_;                  // client: left ranged combat
    std::map<std::string, TurretAim> pendingTurrets_;       // client: to turn
    RangedStats rangedStats_;
    std::vector<std::pair<uint8_t, AppearanceMsg>> pendingLooks_;   // host: from players, applied on the next live tick
    uint32_t editRequest_ = 0;             // client: the host asked us to make our new character
    bool editingSent_ = false;             // client: what we last told the host about our editor
    bool holdForEditor_ = false;           // host: the world is held still while a player edits
    bool resyncRequested_ = false;         // client: the host asked us to reload its world
    std::vector<std::string> logOut_;      // client: log lines waiting to go to the host
    size_t logDropped_ = 0;
    double nextLogSend_ = 0, nextReport_ = 0, reportStart_ = 0;
    uint32_t frames_ = 0;
    std::unordered_map<uint32_t, std::vector<std::string>> dialogReplies_;   // host: last answers offered per conversation
    std::vector<Handle> scratchEdited_;
    bool haveMoney_ = false;               // client
    int32_t hostMoney_ = 0;
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
