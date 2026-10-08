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
    bool Exists(const kc::Handle& h) override { return Find(h) != nullptr; }
    bool Read(const kc::Handle& h, kc::EntityState& out) override;
    bool ReadVitals(const kc::Handle& h, kc::EntityVitals& out) override;
    bool ReadSpawnInfo(const kc::Handle& h, kc::SpawnInfo& out) override;
    bool Spawn(const kc::Handle& h, const kc::SpawnInfo& info, const kc::EntityState& at) override;
    void Despawn(const kc::Handle& h) override;
    void Apply(const kc::Handle& h, const kc::EntityState& target, const kc::EntityState& latest) override;
    void ApplyVitals(const kc::Handle& h, const kc::EntityVitals& v) override;
    bool Order(const kc::Handle& h, const kc::Command& c) override;
    void TakeLocalOrders(std::vector<std::pair<kc::Handle, kc::Command>>& out) override;
    kc::TimeState GetTime() override;
    void SetTime(const kc::TimeState& t) override;
    void HoldForJoin(bool hold) override;
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

    // Called from hooks (game thread).
    void QueueLocalOrder(const kc::Handle& h, const kc::Command& c);
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
    std::unordered_map<kc::Handle, kc::Vec3, HandleHash> lastDest_;   // client: destination last issued
    std::mutex ordersMutex_;
    std::vector<std::pair<kc::Handle, kc::Command>> orders_;
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
    std::mutex toastMutex_;
    std::vector<std::string> toasts_;

    static inline std::atomic<std::shared_ptr<const HookView>> view_{std::make_shared<const HookView>()};
    static inline std::atomic<bool> clientActive_{false};
};

KenshiWorld* TheWorld();   // set by the plugin entry point

} // namespace kcp
