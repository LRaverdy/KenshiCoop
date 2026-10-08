#include "world.h"

#include <algorithm>
#include <cmath>

#include "hooks.h"

namespace kcp {

namespace {
float Dist(const kc::Vec3& a, const kc::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

KenshiWorld::KenshiWorld(const Config& cfg) : cfg_(cfg) {}

void KenshiWorld::BeginFrame() {
    kenshi::PlayerCharacters(scratch_);
    chars_.clear();
    for (kenshi::Character* c : scratch_) {
        kc::Handle h;
        if (kenshi::GetHandle(c, h) && h.valid()) chars_[h] = c;
    }
}

void KenshiWorld::EndFrame() {
    // Clients run at the host's game speed; local speed keys are overridden every frame.
    if (active_ && client_ && haveHostTime_) {
        if (std::fabs(kenshi::GetFrameSpeed() - hostTime_.speed) > 1e-3f) kenshi::CallSetFrameSpeed(hostTime_.speed);
    }
    auto v = std::make_shared<HookView>();
    v->active = active_;
    v->client = client_;
    if (active_) {
        v->controllable = controllable_;
        for (auto& [h, c] : chars_) {
            if (!controllable_.count(h)) v->foreign.insert(c);
            if (client_) v->replicated.insert(c);
        }
    }
    clientActive_.store(v->active && v->client && !v->replicated.empty(), std::memory_order_relaxed);
    view_.store(std::shared_ptr<const HookView>(std::move(v)), std::memory_order_release);
}

bool KenshiWorld::Ready() { return kenshi::Player() != nullptr && !chars_.empty(); }

uint64_t KenshiWorld::Fingerprint() {
    // The set of player-faction character handles identifies "the same save" on every machine.
    std::vector<kc::Handle> hs;
    PlayerCharacters(hs);
    std::sort(hs.begin(), hs.end(), [](const kc::Handle& a, const kc::Handle& b) {
        return std::tie(a.type, a.container, a.containerSerial, a.index, a.serial) <
               std::tie(b.type, b.container, b.containerSerial, b.index, b.serial);
    });
    uint64_t f = 0xcbf29ce484222325ull;
    for (const auto& h : hs) {
        const uint32_t v[5] = {h.type, h.container, h.containerSerial, h.index, h.serial};
        f = kc::Fnv1a64(v, sizeof(v), f);
    }
    return f;
}

uint64_t KenshiWorld::ModsHash() { return HashFile(GameDir() + L"data\\mods.cfg"); }

void KenshiWorld::PlayerCharacters(std::vector<kc::Handle>& out) {
    for (auto& [h, c] : chars_) out.push_back(h);
}

kenshi::Character* KenshiWorld::Find(const kc::Handle& h) const {
    auto it = chars_.find(h);
    return it == chars_.end() ? nullptr : it->second;
}

bool KenshiWorld::Read(const kc::Handle& h, kc::EntityState& out) {
    kenshi::Character* c = Find(h);
    if (!c || !kenshi::GetPosition(c, out.pos)) return false;
    if (!kenshi::GetRotation(c, out.rot)) out.rot = {};
    bool moving = false;
    float speed = 0;
    if (!kenshi::GetMovement(c, out.dest, moving, speed)) { out.dest = out.pos; moving = false; }
    out.flags = 0;
    if (moving) out.flags |= kc::kFlagMoving;
    if (kenshi::IsDown(c)) out.flags |= kc::kFlagDown;
    return true;
}

void KenshiWorld::Apply(const kc::Handle& h, const kc::EntityState& target, const kc::EntityState& latest) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kc::Vec3 local;
    if (!kenshi::GetPosition(c, local)) return;

    // Far off (late join, lag spike, teleport on the host): snap straight to the host state.
    if (Dist(local, target.pos) > cfg_.snapDistance) {
        HostCallScope scope;
        kenshi::Teleport(c, target.pos, target.rot);
        lastDest_.erase(h);
        return;
    }
    // Otherwise let the game's own locomotion walk the puppet, so animations stay natural:
    // follow the host's destination while it moves, and settle on its exact position when idle.
    const kc::Vec3 want = (latest.flags & kc::kFlagMoving) ? latest.dest : target.pos;
    auto it = lastDest_.find(h);
    if (it == lastDest_.end() || Dist(it->second, want) > cfg_.destEpsilon) {
        HostCallScope scope;
        if (kenshi::SetDestination(c, want)) lastDest_[h] = want;
    }
}

bool KenshiWorld::Order(const kc::Handle& h, const kc::Command& cmd) {
    kenshi::Character* c = Find(h);
    if (!c) return false;
    HostCallScope scope;   // lets the call through our own order-blocking hooks
    switch (cmd.kind) {
    case kc::CommandKind::MoveTo: return CallPlayerMoveOrder(c, cmd.pos);
    case kc::CommandKind::Stop: return kenshi::Halt(c);
    }
    return false;
}

void KenshiWorld::QueueLocalOrder(const kc::Handle& h, const kc::Command& c) {
    std::lock_guard<std::mutex> lk(ordersMutex_);
    if (orders_.size() < 256) orders_.emplace_back(h, c);
}

void KenshiWorld::TakeLocalOrders(std::vector<std::pair<kc::Handle, kc::Command>>& out) {
    std::lock_guard<std::mutex> lk(ordersMutex_);
    out.swap(orders_);
    orders_.clear();
}

kc::TimeState KenshiWorld::GetTime() { return {kenshi::GetFrameSpeed(), kenshi::GetPaused()}; }

void KenshiWorld::SetTime(const kc::TimeState& t) {
    hostTime_ = t;
    haveHostTime_ = true;
}

void KenshiWorld::SetRole(bool client, bool active) {
    client_ = client;
    active_ = active;
    haveHostTime_ = false;
    lastDest_.clear();
    if (!active) controllable_.clear();
}

void KenshiWorld::SetControllable(const std::vector<kc::Handle>& handles) {
    controllable_.clear();
    controllable_.insert(handles.begin(), handles.end());
}

void KenshiWorld::Toast(const std::string& msg) {
    std::lock_guard<std::mutex> lk(toastMutex_);
    if (toasts_.size() < 16) toasts_.push_back(msg);
}

std::vector<std::string> KenshiWorld::TakeToasts() {
    std::lock_guard<std::mutex> lk(toastMutex_);
    return std::exchange(toasts_, {});
}

} // namespace kcp
