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
    bool Order(const kc::Handle& h, const kc::Command& c) override;
    void TakeLocalOrders(std::vector<std::pair<kc::Handle, kc::Command>>& out) override;
    bool ReadInventory(const kc::Handle& h, std::vector<kc::ItemState>& out) override;
    bool ExecuteInvOp(const kc::Handle& from, const kc::Handle& to, const kc::InvOp& op) override;
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
    bool EnsurePlayerCharacter(const std::string& playerName, kc::Handle& out) override;
    bool BeginWorldExport(std::string* err) override;
    kc::ExportStatus PollWorldExport(std::vector<kc::WorldFile>& files, std::string* err) override;
    bool BeginWorldImport(const std::vector<kc::WorldFile>& files, std::string* err) override;
    void SetRole(bool client, bool active) override;
    void SetControllable(const std::vector<kc::Handle>& handles) override;

    void SetGameBuild(uint64_t b) { build_ = b; }
    kenshi::Character* Find(const kc::Handle& h);   // squad first, then any live character
    kenshi::Character* FindSquad(const kc::Handle& h) const;
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
    void UpdatePendingPickups();   // host: characters walking to an item a client asked them to take
    // Client: the local player wants `looter` (one of its characters) to loot `target`, a knocked-out
    // or dead character. It walks there through the host; the loot window opens once it is close.
    void RequestLoot(const kc::Handle& looter, kenshi::Character* target);
    void Toast(const std::string& msg);
    std::vector<std::string> TakeToasts();

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
    std::unordered_map<kc::Handle, kc::Vec3, HandleHash> lastDest_;   // client: destination last issued
    std::unordered_map<kc::Handle, double, HandleHash> postureSince_; // client: when host/local posture started to differ
    std::unordered_map<kc::Handle, double, HandleHash> postureFixed_; // client: last forced posture change
    std::unordered_map<kc::Handle, kc::EntityState, HandleHash> lastTarget_;   // client: where the host has it
    struct FallPrep { double start, lastMove; };
    std::unordered_map<kc::Handle, FallPrep, HandleHash> fallPrep_;   // client: moving into place before a fall
    std::unordered_map<kc::Handle, double, HandleHash> fellAt_;      // client: when we made it fall
    std::unordered_set<const void*> applied_;                        // client: characters Apply drove this frame
    std::unordered_map<const void*, double> replicatedAt_;           // client: when Apply last drove each one
    bool ReadyToFall(const kc::Handle& h, kenshi::Character* c, const kc::EntityState& at, double now);
    std::mutex ordersMutex_;
    std::vector<std::pair<kc::Handle, kc::Command>> orders_;
    struct PendingLoot { kc::Handle looter, target; double until; };
    std::vector<PendingLoot> pendingLoot_;   // client, game thread only
    void UpdatePendingLoot();
    std::vector<kenshi::Character*> scratch_;
    bool active_ = false, client_ = false;
    bool live_ = false;
    bool wasReady_ = false;
    void* lastPlayer_ = nullptr;
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
    struct PendingPickup { kc::Handle item; double until; };
    std::unordered_map<kc::Handle, PendingPickup, kc::HandleHash> pickups_;   // host: character -> item it goes to take             // client: last damage number shown on each               // client: "<char>|<anim>" -> last attempt
    std::mutex toastMutex_;
    std::vector<std::string> toasts_;

    static inline std::atomic<std::shared_ptr<const HookView>> view_{std::make_shared<const HookView>()};
    static inline std::atomic<bool> clientActive_{false};
};

KenshiWorld* TheWorld();   // set by the plugin entry point

} // namespace kcp
