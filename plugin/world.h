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

struct HandleHash {
    size_t operator()(const kc::Handle& h) const {
        return std::hash<uint64_t>()((uint64_t(h.index) << 32) ^ h.serial ^ (uint64_t(h.type) << 48) ^ h.container);
    }
};

// Immutable view published by the game thread each frame and read lock-free by hooks that may
// run on other game threads (AI updates).
struct HookView {
    bool active = false;   // a session is running
    bool client = false;   // we are a client: replicated characters are puppets
    std::unordered_set<const void*> replicated;    // client: characters driven by the host
    std::unordered_set<const void*> foreign;       // characters this machine may not command
    std::unordered_set<kc::Handle, HandleHash> controllable;  // handles this machine may command
};

class KenshiWorld final : public kc::IWorld {
public:
    explicit KenshiWorld(const Config& cfg);

    void BeginFrame();   // refresh the character cache (game thread, once per tick)
    void EndFrame();     // enforce host time on clients, publish the hook view

    // IWorld
    bool Ready() override;
    uint64_t Fingerprint() override;
    uint64_t GameBuild() override { return build_; }
    uint64_t ModsHash() override;
    void PlayerCharacters(std::vector<kc::Handle>& out) override;
    bool Exists(const kc::Handle& h) override { return chars_.count(h) != 0; }
    bool Read(const kc::Handle& h, kc::EntityState& out) override;
    void Apply(const kc::Handle& h, const kc::EntityState& target, const kc::EntityState& latest) override;
    bool Order(const kc::Handle& h, const kc::Command& c) override;
    void TakeLocalOrders(std::vector<std::pair<kc::Handle, kc::Command>>& out) override;
    kc::TimeState GetTime() override;
    void SetTime(const kc::TimeState& t) override;
    void SetRole(bool client, bool active) override;
    void SetControllable(const std::vector<kc::Handle>& handles) override;

    void SetGameBuild(uint64_t b) { build_ = b; }
    kenshi::Character* Find(const kc::Handle& h) const;
    size_t CharacterCount() const { return chars_.size(); }

    // Called from hooks (game thread).
    void QueueLocalOrder(const kc::Handle& h, const kc::Command& c);
    void Toast(const std::string& msg);
    std::vector<std::string> TakeToasts();

    static std::shared_ptr<const HookView> View() { return view_.load(std::memory_order_acquire); }
    // Cheap pre-check for very hot hooks (AI updates run for every character every frame).
    static bool ClientActive() { return clientActive_.load(std::memory_order_relaxed); }

private:
    Config cfg_;
    uint64_t build_ = 0;
    std::unordered_map<kc::Handle, kenshi::Character*, HandleHash> chars_;
    std::unordered_set<kc::Handle, HandleHash> controllable_;
    std::unordered_map<kc::Handle, kc::Vec3, HandleHash> lastDest_;   // client: destination last issued
    std::mutex ordersMutex_;
    std::vector<std::pair<kc::Handle, kc::Command>> orders_;
    std::vector<kenshi::Character*> scratch_;
    bool active_ = false, client_ = false;
    bool haveHostTime_ = false;
    kc::TimeState hostTime_;
    std::mutex toastMutex_;
    std::vector<std::string> toasts_;

    static inline std::atomic<std::shared_ptr<const HookView>> view_{std::make_shared<const HookView>()};
    static inline std::atomic<bool> clientActive_{false};
};

KenshiWorld* TheWorld();   // set by the plugin entry point

} // namespace kcp
