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
constexpr double kClockTolerance = 0.02;  // in-game hours (~1 game minute) before the clock is corrected
} // namespace

KenshiWorld::KenshiWorld(const Config& cfg) : cfg_(cfg) {}

void KenshiWorld::BeginFrame() {
    kenshi::PlayerCharacters(scratch_);
    squad_.clear();
    resolved_.clear();
    for (kenshi::Character* c : scratch_) {
        kc::Handle h;
        if (kenshi::GetHandle(c, h) && h.valid()) squad_[h] = c;
    }
}

void KenshiWorld::EndFrame() {
    // Clients run the host's clock: speed and pause are imposed every frame, so local keys
    // (space, F2/F3/F4) have no lasting effect.
    if (active_ && client_ && haveHostTime_) {
        if (std::fabs(kenshi::GetFrameSpeed() - hostTime_.speed) > 1e-3f) kenshi::CallSetFrameSpeed(hostTime_.speed);
        if (kenshi::GetPaused() != hostTime_.paused) kenshi::CallTogglePause(hostTime_.paused);
    }
    auto v = std::make_shared<HookView>();
    v->active = active_;
    v->client = client_;
    if (active_) {
        v->controllable = controllable_;
        for (auto& [h, c] : squad_) if (!controllable_.count(h)) v->squadForeign.insert(c);
    }
    clientActive_.store(active_ && client_, std::memory_order_relaxed);
    view_.store(std::shared_ptr<const HookView>(std::move(v)), std::memory_order_release);
}

bool KenshiWorld::Ready() { return kenshi::Player() != nullptr && !squad_.empty(); }

uint64_t KenshiWorld::Fingerprint() {
    // The set of squad handles identifies "the same save" on every machine.
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
    for (auto& [h, c] : squad_) out.push_back(h);
}

void KenshiWorld::NearbyCharacters(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::Handle>& out) {
    kenshi::ActiveCharacters(scratch_);
    for (kenshi::Character* c : scratch_) {
        kc::Vec3 p;
        if (!kenshi::GetPosition(c, p)) continue;
        bool near = false;
        for (const auto& ctr : centers) if (Dist(ctr, p) <= radius) { near = true; break; }
        if (!near) continue;
        kc::Handle h;
        if (kenshi::GetHandle(c, h) && h.valid()) {
            out.push_back(h);
            resolved_[h] = c;
        }
    }
}

kenshi::Character* KenshiWorld::FindSquad(const kc::Handle& h) const {
    auto it = squad_.find(h);
    return it == squad_.end() ? nullptr : it->second;
}

kenshi::Character* KenshiWorld::Find(const kc::Handle& h) {
    if (kenshi::Character* c = FindSquad(h)) return c;
    auto it = resolved_.find(h);
    if (it != resolved_.end()) return it->second;
    kenshi::Character* c = kenshi::Resolve(h);   // null when the character is not loaded here
    resolved_[h] = c;
    return c;
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
    if (kenshi::IsDead(c)) out.flags |= kc::kFlagDead;
    return true;
}

bool KenshiWorld::ReadVitals(const kc::Handle& h, kc::EntityVitals& out) {
    kenshi::Character* c = Find(h);
    return c && kenshi::ReadVitals(c, out);
}

void KenshiWorld::Apply(const kc::Handle& h, const kc::EntityState& target, const kc::EntityState& latest) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    // Bodies on the ground are moved by ragdoll physics, not by their legs.
    if ((target.flags | latest.flags) & (kc::kFlagDown | kc::kFlagDead)) { lastDest_.erase(h); return; }
    kc::Vec3 local;
    if (!kenshi::GetPosition(c, local)) return;

    // Far off (late join, lag spike, teleport on the host): snap straight to the host state.
    if (Dist(local, target.pos) > cfg_.snapDistance) {
        HostCallScope scope;
        kenshi::Teleport(c, target.pos, target.rot);
        lastDest_.erase(h);
        return;
    }
    // Otherwise let the game's own locomotion walk the character, so animations stay natural:
    // follow the host's destination while it moves, and settle on its exact position when idle.
    const kc::Vec3 want = (latest.flags & kc::kFlagMoving) ? latest.dest : target.pos;
    auto it = lastDest_.find(h);
    if (it == lastDest_.end() || Dist(it->second, want) > cfg_.destEpsilon) {
        HostCallScope scope;
        if (kenshi::SetDestination(c, want)) lastDest_[h] = want;
    }
}

void KenshiWorld::ApplyVitals(const kc::Handle& h, const kc::EntityVitals& v) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kenshi::WriteVitals(c, v);
    // Death and knockout are transitions, not values: replay them exactly when the host has them.
    HostCallScope scope;
    if ((v.flags & kc::kVitDead) && !kenshi::IsDead(c)) {
        kenshi::CallDeclareDead(c);
    } else if ((v.flags & kc::kVitUnconscious) && !kenshi::IsUnconscious(c) && !kenshi::IsDead(c)) {
        kenshi::CallKnockout(c);
        kenshi::WriteVitals(c, v);   // knockout() picks its own timer; the host's wins
    }
}

bool KenshiWorld::Order(const kc::Handle& h, const kc::Command& cmd) {
    kenshi::Character* c = FindSquad(h);
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

kc::TimeState KenshiWorld::GetTime() {
    kc::TimeState t{kenshi::GetFrameSpeed(), kenshi::GetPaused(), 0.0};
    kenshi::GetGameHours(t.gameHours);
    return t;
}

void KenshiWorld::SetTime(const kc::TimeState& t) {
    hostTime_ = t;
    haveHostTime_ = true;
    double local = 0;
    if (t.gameHours > 0 && kenshi::GetGameHours(local) && std::fabs(local - t.gameHours) > kClockTolerance)
        kenshi::SetGameHours(t.gameHours);
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
