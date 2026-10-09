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
    kc::TimeState GetTime() override;
    // Called by the WeatherRegion::updateBT hook (background thread).
    void WeatherRegionTick(void* region, bool afterUpdate);
    size_t ExpireAllWeather();   // tests: every known region rolls a new weather
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
    double hostPausedAt_ = 0;   // client: when the host's pause arrived (we settle a moment first)
    std::mutex weatherMutex_;   // weather is advanced by a game background thread
    std::unordered_map<void*, kc::RegionWeather> seenRegions_;          // host: last state of each region
    std::unordered_map<std::string, kc::RegionWeather> hostWeather_;    // client: what the host has
    std::mutex toastMutex_;
    std::vector<std::string> toasts_;

    static inline std::atomic<std::shared_ptr<const HookView>> view_{std::make_shared<const HookView>()};
    static inline std::atomic<bool> clientActive_{false};
};

KenshiWorld* TheWorld();   // set by the plugin entry point

} // namespace kcp
