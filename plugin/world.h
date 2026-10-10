// IWorld implementation backed by the running Kenshi game, plus the state the hooks consult.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kc/session.h"
#include "kenshi.h"
#include "util.h"

namespace kcp {

using kc::HandleHash;

// Immutable view published by the game thread each frame and read by hooks that may run on other
// game threads (AI updates).
struct HookView {
    bool active = false;   // a session is running
    bool client = false;   // we are a client: the host simulates everything
    std::unordered_set<const void*> squadForeign;  // squad members this machine may not command
    std::unordered_set<kc::Handle, HandleHash> controllable;  // handles this machine may command
    std::unordered_set<const void*> replicated;    // client: characters driven by the host's state
    std::unordered_map<const void*, kc::Vec3> facing;   // client: CharMovement -> where the host's character faces
    struct AnimTarget {
        std::vector<kc::AnimEntry> anims;
        float masterTime = 0, masterSpeed = 0;
        double sampledAt = 0;   // NowSeconds() when the host sampled them
    };
    std::unordered_map<const void*, std::shared_ptr<const AnimTarget>> anims;   // client: AnimationClass -> host's animations
    float gameSpeed = 1.0f;
};

class KenshiWorld final : public kc::IWorld {
public:
    explicit KenshiWorld(const Config& cfg);

    void BeginFrame(bool live);   // refresh caches (game thread, once per tick)
    void EndFrame();              // enforce host time on clients, publish the hook view (live ticks)

    // IWorld
    bool Ready() override;
    uint32_t WorldGeneration() override { return generation_; }
    uint64_t Fingerprint() override;
    uint64_t GameBuild() override { return build_; }
    uint64_t ModsHash() override;
    void PlayerCharacters(std::vector<kc::Handle>& out) override;
    void NearbyCharacters(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::Handle>& out) override;
    bool Exists(const kc::Handle& h) override;
    bool Read(const kc::Handle& h, kc::EntityState& out) override;
    bool ReadVitals(const kc::Handle& h, kc::EntityVitals& out) override;
    bool ReadSpawnInfo(const kc::Handle& h, kc::SpawnInfo& out) override;
    uint64_t Identity(const kc::Handle& h) override { return reinterpret_cast<uint64_t>(Find(h)); }
    void Rehandle(const kc::Handle& from, const kc::Handle& to) override;
    bool ReadCombat(const kc::Handle& h, kc::Handle& target) override;
    void ApplyCombat(const kc::Handle& h, bool fight, const kc::Handle& target) override;
    bool Spawn(const kc::Handle& h, const kc::SpawnInfo& info, const kc::EntityState& at) override;
    void Despawn(const kc::Handle& h) override;
    void Reconcile(const std::vector<kc::Handle>& known, const std::vector<kc::MissingChar>& missing, double now,
                   std::vector<kc::Handle>& adopted) override;
    void Apply(const kc::Handle& h, const kc::EntityState& target, const kc::EntityState& latest) override;
    void ApplyVitals(const kc::Handle& h, const kc::EntityVitals& v) override;
    bool ReadProgress(const kc::Handle& h, std::vector<float>& stats, uint16_t& modes, uint8_t& style) override;
    bool ReadCarry(const kc::Handle& h, kc::Handle& carried) override;
    void ApplyCarry(const kc::Handle& h, bool carry, const kc::Handle& carried) override;
    // Host: put every character of a player next to that point (unsticks them). Returns how many.
    int TeleportCharacters(const std::vector<kc::Handle>& who, const kc::Vec3& to);
    void ApplyProgress(const kc::Handle& h, const std::vector<float>& stats, uint16_t modes, uint8_t style) override;
    bool ReadMoney(int32_t& money) override { return kenshi::ReadPlayerMoney(money); }
    bool ReadJobs(const kc::Handle& h, std::vector<int32_t>& jobs) override;      // fix G5
    void ApplyJobs(const kc::Handle& h, const std::vector<int32_t>& jobs) override;
    bool ReadTool(const kc::Handle& h, std::string& sid) override;
    void ApplyTool(const kc::Handle& h, const std::string& sid) override;
    bool ReadFloor(const kc::Handle& h, uint8_t& group) override;   // fix G6
    void ApplyFloor(const kc::Handle& h, uint8_t group) override;
    void ReadSquads(std::vector<WorldSquad>& out) override;
    bool FindContainer(const std::string& sid, const kc::Vec3& pos, kc::Handle& out) override;
    bool ContainerKind(const kc::Handle& container, std::string& sid) override;
    float DistanceTo(const kc::Handle& who, const kc::Vec3& pos) override;
    int TheftCheck(const kc::Handle& thief, const kc::Handle& container, const kc::ItemState& item) override;
    void TakeContainerRequests(std::vector<ContainerRequest>& out) override;
    bool OpenContainerWindow(const kc::Handle& looter, const kc::Handle& container) override;
    bool ContainerWindowOpen() override { return kenshi::OpenInventoryWindows() > 0; }
    void CloseContainerWindows() override { kenshi::CloseInventoryWindows(); }
    void QueueContainerRequest(kenshi::Character* looter, void* container);   // hooks: a right click on a container
    void TakeTradeRequests(std::vector<TradeRequest>& out) override;
    bool ShopCounters(const kc::Handle& trader, std::vector<ShopCounter>& out) override;
    bool MoneyOf(const kc::Handle& who, int32_t& money) override;
    bool PayTrade(const kc::Handle& buyer, const kc::Handle& trader, int32_t price) override;
    void RefreshTradeWindow(const kc::Handle& trader) override;
    bool OpenTradeWindow(const kc::Handle& looter, const kc::Handle& trader) override;
    bool TradeWindowBusy() override { return kenshi::MouseHoldsItem(); }
    int TradeWindowStock() override;
    void SetMoneyOf(const kc::Handle& who, int32_t money) override;
    std::string CharacterNameOf(const kc::Handle& h) override;
    // ---- lot B: factions (plugin/factions.cpp)
    bool ReadFactions(kc::FactionsMsg& out) override;
    size_t ApplyFactions(const kc::FactionsMsg& m) override;
    bool ReadBounties(const kc::Handle& h, kc::CharBounties& out) override;
    size_t ApplyBounties(const kc::Handle& h, const kc::CharBounties& b) override;
    // ---- lot A: doors and locks (plugin/doors.cpp)
    void ReadDoors(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::DoorState>& out) override;
    bool ContainerLocked(const kc::Handle& container) override;
    bool ExecuteDoorRequest(const kc::DoorRequest& r) override;
    bool ApplyDoor(const kc::DoorState& d) override;
    void TakeDoorRequests(std::vector<kc::DoorRequest>& out) override;
    void QueueDoorRequest(void* door, kc::DoorAction action);   // hooks (client): a door button the player clicked
    void* FindDoor(const std::string& sid, const kc::Vec3& pos);   // the same door here (kind and place)
    // ---- end lot A
    // ---- lot D: prisons (plugin/prisons.cpp)
    bool ReadCaptive(const kc::Handle& h, kc::CaptiveState& out) override;
    void ApplyCaptive(const kc::Handle& h, const kc::CaptiveState& s) override;
    bool CaptiveHold(const kc::Handle& h, kenshi::Character* c);   // client: the host keeps it in a cage here
    // Hooks (host): the game asked for a trade window for another player's character (any thread);
    // the host's own trade window (to show it again when another player changed the stock).
    void QueueTradeRequest(const kc::Handle& looter, const kc::Handle& trader);
    void NoteHostTradeWindow(const kc::Handle& looter, const kc::Handle& trader);
    // ---- lot E: buildings (plugin/buildings.cpp)
    void TakeLocalPlacements(std::vector<LocalPlacement>& out) override;
    bool ExecutePlacement(const kc::BuildPlace& p, kc::Handle& created, kc::Vec3& worldPos) override;
    bool FindBuilding(const std::string& sid, const kc::Vec3& pos, kc::Handle& out) override;
    bool BuildingIdentity(const kc::Handle& h, std::string& sid, kc::Vec3& pos) override;
    bool ReadBuildState(const kc::Handle& h, float& progress, uint8_t& flags) override;
    void ApplyBuildState(const kc::Handle& h, float progress, uint8_t flags) override;
    void ConstructionSitesNear(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::Handle>& out) override;
    void TrackBuilding(const kc::Handle& h) override;
    void TakeBuildingRemovals(std::vector<kc::Handle>& out) override;
    bool RemoveBuilding(const kc::Handle& h) override;
    void TakeLocalBuildActions(std::vector<kc::BuildAction>& out) override;
    bool ExecuteBuildAction(const kc::BuildAction& a, std::string& refused) override;
    // What RootObjectFactory::createBuilding gets in build mode.
    struct BuildArgs {
        void* data = nullptr;
        kc::Vec3 pos;
        kc::Quat rot;
        void* town = nullptr;
        void* callback = nullptr;
        void* layout = nullptr;
        void* indoors = nullptr;
        int floor = 0;
        bool outside = false;
    };
    // Hooks: build mode placed something here (client: nothing was built; host: `created`); a
    // purchase / dismantling confirmed in the game's window (client: asked of the host); the host's
    // own purchase went through; the game destroyed an object for good.
    void NoteBuildCapture(const BuildArgs& a, void* created);
    void NoteLocalBuildAction(void* building, kc::BuildActionKind kind, int answer);
    void NoteHostBought(void* building);
    void NoteObjectDestroyed(void* obj);
    // tests: a placement as build mode would make it (client: asked of the host; host: built and
    // announced); the building of that kind and place, or the nearest whose name contains `part`
    bool DebugPlace(const kc::BuildPlace& p);
    void* BuildingAt(const std::string& sid, const kc::Vec3& pos);
    static bool IsPlayerBuilding(void* b);
    static bool IsBuildingForSale(void* b);   // tests: Building::isForSale
    static bool ReadBuildStateOf(void* building, float& progress, uint8_t& flags);   // false: not a building
    void* NearestBuilding(const kc::Vec3& from, const std::string& part, float radius, int want);   // want: 0 any, 1 for sale, 2 ours, 3 ours unfinished
    void TakeEditedCharacters(std::vector<kc::Handle>& out) override;
    bool ReadAppearance(const kc::Handle& h, kc::AppearanceMsg& out) override;
    void ApplyAppearance(const kc::Handle& h, const kc::AppearanceMsg& m) override;
    bool OpenCharacterEditor(const kc::Handle& h) override;
    bool CharacterEditorOpen() override { return kenshi::CharacterEditorOpen(); }
    void TakeSyncStats(float& maxErr, uint16_t& farOff) override;
    std::string TemplateName(const std::string& sid) override;
    void NoteEdited(kenshi::Character* c);   // hooks: the character editor was confirmed for it
    void ApplySquads(const std::vector<WorldSquad>& squads) override;
    void ApplyMoney(int32_t money) override { kenshi::WritePlayerMoney(money); }
    bool Order(const kc::Handle& h, const kc::Command& c) override;
    std::string TakeOrderRefusal(kc::ResultReason& reason) override {
        reason = orderRefusal_.empty() ? kc::ResultReason::None : orderRefusalReason_;
        return std::exchange(orderRefusal_, std::string{});
    }
    void OrderRejected(const kc::Handle& h, const kc::Command& c) override;
    // host: what a client order's subject is (kc::TargetFlags); orders refused for their target
    uint32_t TargetFlagsOf(kenshi::Character* actor, void* subject, bool named);
    int targetRefusals() const { return targetRefusals_; }
    void HaltCharacter(const kc::Handle& h) override;
    void TakeLocalOrders(std::vector<std::pair<kc::Handle, kc::Command>>& out) override;
    bool ReadInventory(const kc::Handle& h, std::vector<kc::ItemState>& out) override;
    bool ExecuteInvOp(const kc::Handle& from, const kc::Handle& to, const kc::InvOp& op) override;
    bool ExecuteInvSwap(const kc::Handle& from, const kc::Handle& to, const kc::InvOp& a, const kc::InvOp& b) override;
    bool ApplyInventory(const kc::Handle& h, const std::vector<kc::ItemState>& items) override;
    void ReadWeather(std::vector<kc::RegionWeather>& out) override;
    void ApplyWeather(const std::vector<kc::RegionWeather>& regions) override;
    void ReadEffects(kc::EffectsMsg& out, bool full) override;
    void TakeAnimEvents(std::vector<std::pair<kc::Handle, kc::AnimEvent>>& out, bool state) override;
    bool ReadAnimFrame(const kc::Handle& h, kc::AnimFrame& out) override;
    void ApplyAnimFrame(const kc::Handle& h, const kc::AnimFrame& f, double ageSeconds) override;
    void TakeGroundEvents(std::vector<kc::GroundEvent>& out) override;
    void ApplyGround(const kc::GroundEvent& e) override;
    void NoteGround(kc::GroundEvent e);   // host: a hook saw an item dropped / picked up (any thread)
    kc::Handle GroundCopyOf(const kc::Handle& hostItem) const {   // tests (client)
        auto it = groundAlias_.find(hostItem);
        return it != groundAlias_.end() ? it->second : hostItem;
    }
    void ApplyAnim(const kc::Handle& h, const kc::AnimEvent& e) override;
    // Host: an animation hook saw one of our characters start/stop something (any game thread).
    void NoteAnim(kenshi::Character* c, kc::AnimEvent e);
    void ApplyEffects(const kc::EffectsMsg& m) override;
    kc::TimeState GetTime() override;
    // Called by the WeatherRegion::updateBT hook (background thread).
    void WeatherRegionTick(void* region, bool afterUpdate);
    size_t ExpireAllWeather();   // tests: every known region rolls a new weather
    std::string EffectsReport();  // tests: live weather effects per region
    size_t AnimTargetCount() const { return animTargets_.size(); }
    size_t LiveEffects();         // weather effects the clients have (host) / placed for the host (client)
    std::string WeatherGroups();  // tests: each region's weather, effect groups and possible weathers
    size_t HurryEffects();        // tests: host groups place their next effect now
    void ForceWeather(const std::string& regionSid, const std::string& seasonSid, const std::string& weatherSid);   // tests (host)
    void SetTime(const kc::TimeState& t) override;
    void HoldForJoin(bool hold) override;
    bool EnsurePlayerCharacter(const std::string& playerName, uint64_t steamId, kc::Handle& out, bool& created) override;
    bool BeginWorldExport(std::string* err) override;
    kc::ExportStatus PollWorldExport(std::vector<kc::WorldFile>& files, std::string* err) override;
    bool BeginWorldImport(const std::vector<kc::WorldFile>& files, std::string* err) override;
    void SetRole(bool client, bool active) override;
    void SetControllable(const std::vector<kc::Handle>& handles) override;

    void SetGameBuild(uint64_t b) { build_ = b; }
    kenshi::Character* Find(const kc::Handle& h);   // squad first, then any live character
    kenshi::Character* FindSquad(const kc::Handle& h) const;
    void LocalRehandled(const kc::Handle& before, const kc::Handle& after);
    // client: the host handle a local stand-in replaces (or `local` itself)
    kc::Handle HostHandleOf(const kc::Handle& local) const {
        for (const auto& [host, l] : alias_) if (l == local) return host;
        return local;
    }
    size_t CharacterCount() const { return squad_.size(); }
    kc::Handle traceHandle;   // tests: log what Apply does to this character for traceFrames frames
    int traceFrames = 0;

    // Called from hooks (game thread).
    void QueueLocalOrder(const kc::Handle& h, const kc::Command& c);
    // client: a permanent job our copy took at once (the host's game gives it too): kept by
    // ApplyJobs for a few seconds, until the host's job list has it
    void NoteLocalJob(const kc::Handle& h, int task);
    // host: a body being teleported (admin TP): the game must not lay it down again before it got there
    bool HoldsUpright(const void* chr) const {
        for (const auto& r : reRagdoll_)
            if (r.body && r.chr == chr) return true;
        return false;
    }
    void UpdatePendingPickups();   // host: characters walking to an item a client asked them to take
    void TakeLocalDrops(std::vector<std::pair<kc::Handle, kc::ItemState>>& out) override;
    // Conversations (hooks, any thread for NoteSay; the game thread for the window ones). The window
    // ones return true when the conversation is another player's (the host's window stays shut).
    void NoteSay(void* dialogue, const std::string& text);
    bool NoteDialogWindow(void* dialogue, bool open);
    bool NoteDialogText(void* dialogue);
    void TakeDialogEvents(std::vector<WorldDialog>& out) override;
    void ApplySay(const kc::Handle& speaker, const std::string& text, bool shout) override;
    void DialogAnswer(uint32_t dialogId, int index) override;
    int saysApplied = 0;          // tests (client): speech bubbles replayed
    std::string lastSay;
    void QueueLocalDrop(kenshi::Character* c, void* item);   // client: the player dropped it (any thread)
    // Client: the local player wants `looter` (one of its characters) to loot `target`, a knocked-out
    // or dead character. It walks there through the host; the loot window opens once it is close.
    void RequestLoot(const kc::Handle& looter, kenshi::Character* target);
    void Toast(const std::string& msg);
    std::vector<std::string> TakeToasts();

    // ---- lot C: ranged combat (plugin/ranged.cpp)
    void TakeShots(std::vector<WorldShot>& out) override;
    bool ReplayShot(const WorldShot& s) override;
    bool ReadRangedAim(const kc::Handle& h, WorldAim& out) override;
    void ApplyRangedAim(const kc::Handle& h, bool ranged, const WorldAim& a) override;
    void ReadTurrets(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::TurretAim>& out) override;
    void ApplyTurret(const kc::TurretAim& t) override;
    // Host (shoot hook, any thread): a projectile the game just fired.
    void NoteShot(void* gun, kenshi::Character* me, void* target, int stat, const float* aimPos, void* projectile);
    void* FindTurret(const std::string& sid, const kc::Vec3& pos);   // the local turret of that kind at that place
    struct RangedCounters { uint64_t noted = 0, replayed = 0, noGun = 0, noTurret = 0, oriented = 0; };
    RangedCounters rangedCounters;                                     // tests
private:
    std::mutex shotsMutex_;
    std::vector<WorldShot> shotsOut_;                                  // host, under shotsMutex_
    std::unordered_map<std::string, std::vector<std::pair<int, unsigned long long>>> localJobs_;   // client: key -> (task, kept until GetTickCount64)
    std::unordered_map<std::string, kc::Handle> turretCache_;          // client: "sid@x,z" -> local turret
public:
    static std::shared_ptr<const HookView> View() { return view_.load(std::memory_order_acquire); }
    // Cheap check for very hot hooks (AI and damage run for every character every frame).
    static bool ClientActive() { return clientActive_.load(std::memory_order_relaxed); }

private:
    Config cfg_;
    uint64_t build_ = 0;
    std::unordered_map<kc::Handle, kenshi::Character*, HandleHash> squad_;      // this frame's squad
    std::unordered_map<kc::Handle, kenshi::Character*, HandleHash> resolved_;   // this frame's lookups
    std::unordered_set<kc::Handle, HandleHash> controllable_;
    // client: host handle -> handle of the local stand-in we created for it
    std::unordered_map<kc::Handle, kc::Handle, HandleHash> alias_;
    std::unordered_map<kc::Handle, double, HandleHash> strangerSince_;   // client: local-only NPCs, first seen
    struct FloorTry { uint8_t group = 0; double at = 0; int tries = 0; };
    std::unordered_map<kc::Handle, FloorTry, HandleHash> floorTry_;   // client: floor placements tried (fix G7)
    std::unordered_map<kc::Handle, kc::Vec3, HandleHash> lastDest_;   // client: destination last issued
    std::unordered_map<kc::Handle, double, HandleHash> postureSince_; // client: when host/local posture started to differ
    std::unordered_map<kc::Handle, double, HandleHash> postureFixed_; // client: last forced posture change
    std::unordered_map<kc::Handle, kc::EntityState, HandleHash> lastTarget_;   // client: where the host has it
    struct FallPrep { double start, lastMove; };
    std::unordered_map<kc::Handle, FallPrep, HandleHash> fallPrep_;   // client: moving into place before a fall
    std::unordered_map<kc::Handle, double, HandleHash> fellAt_;      // client: when we made it fall
    std::unordered_map<kc::Handle, double, HandleHash> relaidAt_;    // client: when a body lying away from the host's was stood up to fall again
    std::unordered_set<kc::Handle, HandleHash> carriedHere_;
    std::unordered_map<kc::Handle, kc::CharBounties, HandleHash> hostBounties_;   // client: the host's, shown, not written
    struct HandTool { std::string sid; void* item = nullptr; kenshi::Character* who = nullptr; };
    std::unordered_map<kc::Handle, HandTool, HandleHash> handTools_;   // client: tools we put in hands   // client: host characters someone carries here
    std::unordered_set<const void*> applied_;                        // client: characters Apply drove this frame
    std::unordered_map<const void*, double> replicatedAt_;           // client: when Apply last drove each one
    bool ReadyToFall(const kc::Handle& h, kenshi::Character* c, const kc::EntityState& at, double now);
    std::mutex ordersMutex_;
    std::string orderRefusal_;
    kc::ResultReason orderRefusalReason_ = kc::ResultReason::None;
    int targetRefusals_ = 0;   // host: why the last client order was refused for safety (French)
    std::vector<std::pair<kc::Handle, kc::Command>> orders_;
    struct PendingLoot { kc::Handle looter, target; double until; };
    std::vector<PendingLoot> pendingLoot_;   // client, game thread only
    void UpdatePendingLoot();
    std::vector<kenshi::Character*> scratch_;
    bool active_ = false, client_ = false;
    bool live_ = false;
    bool wasReady_ = false;
    void* lastPlayer_ = nullptr;
    std::vector<const void*> lastSquadPtrs_;   // the squad of the world last seen (sorted): same ones, same world
    void ResetWorldBound();
    uint32_t generation_ = 0;
    bool holding_ = false, pausedByHold_ = false;
    bool exporting_ = false;
    std::string exportFolder_;
    uint64_t exportLastSize_ = 0;      // the save is complete once its size stops changing
    double exportStableSince_ = 0;
    bool haveHostTime_ = false;
    kc::TimeState hostTime_;
    std::mutex weatherMutex_;   // weather is advanced by a game background thread
    std::unordered_map<void*, kc::RegionWeather> seenRegions_;          // host: last state of each region
    std::unordered_map<std::string, kc::RegionWeather> hostWeather_;    // client: what the host has
    // Weather effects (weather thread and session thread, under weatherMutex_).
    void HostEffectsTick(void* region, const std::string& regionSid);
    void ClientEffectsTick(void* region, const std::string& regionSid);
    struct HostFx {
        uint32_t id;
        void* group;
        kc::EffectKind kind;
        bool sent;           // near a player: the clients have it
        bool done;           // fading out (its time is up, or the game stopped it): over for the clients
        double movedAt;      // wandering: state last sent
        kc::Vec3 turnTo;     //            heading sent with it
    };
    std::unordered_map<void*, std::unordered_map<void*, HostFx>> hostFx_;   // host: region -> handler -> effect
    std::unordered_map<uint32_t, kc::WeatherEffect> fxLive_;                // host: every effect the clients have
    std::vector<kc::Vec3> fxCenters_;                                       // host: where the players' characters are
    double fxCentersAt_ = -1e9;
    struct FxStats {
        uint64_t placed = 0, failed = 0, noGroup = 0, over = 0, stopped = 0;
        uint64_t corrections = 0;            // wandering states applied
        double correctionSum = 0, correctionMax = 0;   // how far our copy was from the host's state then
    } fxStats_;
    kc::EffectsMsg fxOut_;                                                  // host: not handed to the session yet
    uint32_t nextFxId_ = 1;
    struct ClientFx {
        void* group;
        void* handler;
        std::string regionSid;
        kc::EffectKind kind;
        double placedAt;
        float life;          // when placed
        kc::WeatherEffect last;   // the host's latest state of it
    };
    void FxLog(uint32_t id, const char* what, const ClientFx* c);
public:
    void NoteEffectStop(void* handler, uintptr_t callerRva);   // diagnostics (any thread)
    bool TakeEffectsRebuild(void* region);   // client: the host switched this region's weather (weather thread)
private:   // first few lifecycle events (tests)
    uint32_t fxLogged_ = 0;
    struct PendingFx { kc::WeatherEffect e; double since; };
    std::unordered_map<uint32_t, ClientFx> clientFx_;                       // client: host id -> our copy
    std::unordered_map<std::string, std::vector<PendingFx>> fxSpawn_;       // client: region -> to place
    std::unordered_set<uint32_t> fxPending_, fxHandled_;                    // client: waiting / placed once
    std::unordered_map<uint32_t, kc::EffectState> fxMove_;                  // client: newest state per effect
    std::unordered_set<uint32_t> fxEnd_;                                    // client: to stop
    std::unordered_set<uint32_t> fxLiveIds_;                                // client: host's last complete set
    uint64_t fxFullGen_ = 0;
    std::unordered_map<std::string, uint64_t> fxFullDone_;                  // client: region -> set applied
    std::unordered_map<void*, std::unordered_set<void*>> fxStopped_;        // client: region -> handlers we stopped
    std::unordered_set<void*> fxRebuild_;                                   // client: regions whose weather we switched
    std::unordered_map<std::string, std::pair<std::string, std::string>> forcedWeather_;   // tests: region -> season, weather
    std::mutex animMutex_;
    std::vector<std::pair<kenshi::Character*, kc::AnimEvent>> animOut_;   // host: not taken by the session yet
    std::unordered_map<kenshi::Character*, uint32_t> animLast_;            // host: last event reported per character
    std::vector<kc::Vec3> animCenters_;                                     // host: players' characters, for relevance
    double animCentersAt_ = -1e9;
    std::unordered_map<kenshi::Character*, std::shared_ptr<const HookView::AnimTarget>> animTargets_;   // client
    std::unordered_map<std::string, double> animCreateTried_;
    std::unordered_map<kenshi::Character*, void*> lastFloater_;
    std::mutex groundMutex_;
    std::vector<kc::GroundEvent> groundOut_;                                // host: not sent yet
    std::unordered_map<kc::Handle, kc::Handle, kc::HandleHash> groundAlias_;   // client: host item -> our copy
    std::vector<std::pair<kenshi::Character*, kc::ItemState>> localDrops_;   // client, under groundMutex_
    std::vector<kenshi::Character*> edited_;   // characters whose looks the editor just changed
    std::vector<ContainerRequest> containerReqs_;   // client: right clicks on containers, for the host
    // ---- lot A: doors
    std::mutex doorMutex_;
    std::vector<kc::DoorRequest> doorReqs_;                          // client, under doorMutex_
    std::unordered_map<std::string, kc::Handle> doorCache_;         // kind and place -> our handle of it
    // ---- end lot A
    // ---- lot D: prisons
    std::unordered_set<kc::Handle, kc::HandleHash> captiveHold_;      // client: caged on the host (no position corrections)
    std::unordered_set<kc::Handle, kc::HandleHash> captiveMissing_;   // client: cage not found here (logged once)
    std::mutex tradeMutex_;
    std::vector<TradeRequest> tradeReqs_;            // host, under tradeMutex_
    kc::Handle hostTradeLooter_, hostTradeTrader_;   // host: our own last trade window (under tradeMutex_)
    // ---- lot E: buildings (under buildMutex_)
    std::mutex buildMutex_;
    std::vector<LocalPlacement> localPlacements_;
    std::vector<kc::BuildAction> localBuildActions_;
    std::unordered_map<const void*, kc::Handle> trackedBuildings_;   // host: followed buildings
    std::vector<kc::Handle> removedBuildings_;                      // host: destroyed since the last call
    bool DescribeBuildArgs(const BuildArgs& a, kc::BuildPlace& out);
    void* InventoryHolder(const kc::Handle& h);       // a character, or a container
    double pauseSeenAt_ = -1;   // client: when the host's pause arrived (we pause a little later)
    std::unordered_map<kc::Handle, double, kc::HandleHash> taskDropAt_;   // client: when its local tasks were last dropped
    float syncMaxErr_ = 0;      // client: largest correction since the last report
public:
    uint64_t masterCorrections = 0, masterChecks = 0;   // tests: animation clock corrections on clients (fighting and fallen characters not counted)
    uint64_t stuckFixes = 0, farSnaps = 0;               // tests (client): characters freed from a wall / far walkers put back on the host's path
private:
    struct Stuck { double since = 0; float bestErr = 0; };
    std::unordered_map<kc::Handle, Stuck, kc::HandleHash> stuck_;          // client: no progress toward the host's position since
    std::unordered_map<kc::Handle, double, kc::HandleHash> farSnapAt_;     // client: last time a far walker was put back
    float NearestSquadDistance(const kc::Vec3& p);                         // client: how far from our squad (far: the game moves it rarely)
    struct ReRagdoll { kc::Handle h; double at; kc::Vec3 to; kc::Quat rot; bool body; int tries; const void* chr; };   // fix G7: checked, retried
    std::vector<ReRagdoll> reRagdoll_;                                     // host: teleported bodies to lay down again
    void UpdateReRagdolls();
    std::unordered_map<kc::Handle, float, kc::HandleHash> syncErr_;   // client: last error per standing character
    double nextPauseTry_ = 0;
    bool pauseRefusedLogged_ = false;
    std::mutex dialogMutex_;
    std::vector<WorldDialog> dialogEvents_;                 // host, under dialogMutex_
    std::unordered_map<void*, uint32_t> remoteDialogs_;    // host: Dialogue* shown to another player -> id
    uint32_t nextDialogId_ = 1;
    // the other player's character in it (and who it talks with), or false
    bool RemoteDialogParties(void* dialogue, kenshi::Character*& pc, kenshi::Character*& other);
    // gameOrder: the game's own pick up order was given (we only watch it, and walk the character
    // there ourselves if it has not moved by checkAt); startDist: its distance to the item then
    struct PendingPickup { kc::Handle item; double until; bool gameOrder = false; double checkAt = 0; float startDist = 0; bool pauseLogged = false; };
    double lastPickupTick_ = 0;   // host: last UpdatePendingPickups (paused time is not counted)
    std::unordered_map<kc::Handle, PendingPickup, kc::HandleHash> pickups_;   // host: character -> item it goes to take             // client: last damage number shown on each               // client: "<char>|<anim>" -> last attempt
    std::mutex toastMutex_;
    std::vector<std::string> toasts_;

    static inline std::atomic<std::shared_ptr<const HookView>> view_{std::make_shared<const HookView>()};
    static inline std::atomic<bool> clientActive_{false};
};

KenshiWorld* TheWorld();   // set by the plugin entry point

} // namespace kcp
