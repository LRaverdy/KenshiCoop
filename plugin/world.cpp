#include "world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "hooks.h"

namespace kcp {

namespace {
float Dist(const kc::Vec3& a, const kc::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// Save slot names (<= 15 chars: passed to the game as inline VS2010 strings).
constexpr const char* kExportSlot = "KenshiCoopHost";
constexpr const char* kImportSlot = "KenshiCoopJoin";

namespace fs = std::filesystem;

// Kenshi stores paths as narrow strings in the system code page.
fs::path FromGamePath(const std::string& s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), w.data(), n);
    fs::path p(w);
    return p.is_absolute() ? p : fs::path(GameDir()) / p;
}
std::string ToUtf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}
fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
} // namespace

KenshiWorld::KenshiWorld(const Config& cfg) : cfg_(cfg) {}

void KenshiWorld::BeginFrame(bool live) {
    live_ = live;
    squad_.clear();
    resolved_.clear();
    if (!live) {   // menus and loading screens: nothing in the world may be touched
        wasReady_ = false;
        return;
    }
    kenshi::PlayerCharacters(scratch_);
    for (kenshi::Character* c : scratch_) {
        kc::Handle h;
        if (kenshi::GetHandle(c, h) && h.valid()) squad_[h] = c;
    }
    // A (re)loaded world: a new generation, which tells a joining client its load completed.
    void* player = kenshi::Player();
    const bool ready = player && !squad_.empty();
    if (ready && (!wasReady_ || player != lastPlayer_)) {
        ++generation_;
        alias_.clear();                  // stand-ins belonged to the previous world
        pendingLoot_.clear();
        strangerSince_.clear();
        lastTarget_.clear();
        fallPrep_.clear();
        fellAt_.clear();
        kenshi::ResetLookupCaches();
    }
    wasReady_ = ready;
    lastPlayer_ = player;
}

void KenshiWorld::EndFrame() {
    // Clients run the host's clock: speed and pause are imposed every frame, so local keys
    // (space, F2/F3/F4) have no lasting effect.
    if (active_ && client_ && haveHostTime_) {
        // Same calls as the space bar / speed keys, so the game's own speed UI follows too. The pause
        // is immediate: running on a little to settle positions would put our clock ahead of the
        // host's (the game recomputes its clock from its own counter, it cannot be set back).
        if (kenshi::GetPaused() != hostTime_.paused) kenshi::CallUserPause(hostTime_.paused);
        if (!hostTime_.paused && std::fabs(kenshi::GetFrameSpeed() - hostTime_.speed) > 1e-3f) kenshi::CallSetFrameSpeed(hostTime_.speed);
    }
    // While players join, the host's world stays frozen even if someone presses unpause.
    if (holding_ && !kenshi::GetPaused()) {
        kenshi::CallUserPause(true);
        pausedByHold_ = true;
    }
    if (active_ && client_ && live_ && !pendingLoot_.empty()) UpdatePendingLoot();
    auto v = std::make_shared<HookView>();
    v->active = active_;
    v->client = client_;
    v->replicated.swap(applied_);
    applied_.clear();
    if (active_) {
        for (const kc::Handle& h : controllable_) {
            auto a = alias_.find(h);
            v->controllable.insert(a != alias_.end() ? a->second : h);
        }
        for (auto& [h, c] : squad_) if (!v->controllable.count(h)) v->squadForeign.insert(c);
    }
    clientActive_.store(active_ && client_, std::memory_order_relaxed);
    view_.store(std::shared_ptr<const HookView>(std::move(v)), std::memory_order_release);
}

bool KenshiWorld::Ready() { return live_ && kenshi::Player() != nullptr && !squad_.empty(); }

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
    // Bodies stay in the world after they leave the active list: the ones near the players are
    // replicated too, so they lie at the same place and can be looted.
    constexpr float kCorpseRadius = 1000.0f;
    std::unordered_set<kc::Handle, HandleHash> seen;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0) kenshi::ActiveCharacters(scratch_);
        else kenshi::DeadBodies(scratch_);
        const float r = pass == 0 ? radius : (radius > 0 ? std::min(radius, kCorpseRadius) : kCorpseRadius);
        for (kenshi::Character* c : scratch_) {
            kc::Vec3 p;
            if (!kenshi::GetPosition(c, p)) continue;
            bool inRange = r <= 0;   // 0 = every character the game keeps active
            for (const auto& ctr : centers) if (Dist(ctr, p) <= r) { inRange = true; break; }
            if (!inRange) continue;
            kc::Handle h;
            if (kenshi::GetHandle(c, h) && h.valid() && seen.insert(h).second) {
                out.push_back(h);
                resolved_[h] = c;
            }
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
    // a host character we recreated locally lives under its own local handle
    auto a = alias_.find(h);
    kenshi::Character* c = kenshi::Resolve(a != alias_.end() ? a->second : h);   // null when not loaded here
    resolved_[h] = c;
    return c;
}

bool KenshiWorld::ReadCombat(const kc::Handle& h, kc::Handle& target) {
    kenshi::Character* c = Find(h);
    if (!c || !kenshi::ReadCombat(c, target)) return false;
    target = HostHandleOf(target);   // host side: identity; harmless
    return true;
}

void KenshiWorld::ApplyCombat(const kc::Handle& h, bool fight, const kc::Handle& target) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    HostCallScope scope;
    if (!fight) { kenshi::EndCombat(c); return; }
    kenshi::Character* t = Find(target);   // host handle -> our copy (stand-ins included)
    kc::Handle local;
    if (t && kenshi::GetHandle(t, local)) kenshi::StartCombat(c, local);
}

bool KenshiWorld::ReadSpawnInfo(const kc::Handle& h, kc::SpawnInfo& out) {
    kenshi::Character* c = Find(h);
    return c && kenshi::ReadSpawnSource(c, out);
}

bool KenshiWorld::Spawn(const kc::Handle& h, const kc::SpawnInfo& info, const kc::EntityState& at) {
    if (alias_.count(h)) return false;
    std::string err;
    HostCallScope scope;
    kenshi::Character* c = kenshi::CreateCharacter(info, at.pos, &err);
    kc::Handle local;
    if (!c || !kenshi::GetHandle(c, local)) {
        Log("cannot recreate host character %s: %s", info.templateSid.c_str(), err.c_str());
        return false;
    }
    kenshi::Teleport(c, at.pos, at.rot);
    alias_[h] = local;
    resolved_.erase(h);
    return true;
}

// A character must fall where the host's fell, so it is moved there first. The ragdoll takes its
// velocity from the last poses of the skeleton: falling right after a teleport would throw the
// body far away, so the pose gets a moment to settle at the new place. False while it is being
// prepared (under a second: a body still sliding on the host is not chased forever).
bool KenshiWorld::ReadyToFall(const kc::Handle& h, kenshi::Character* c, const kc::EntityState& at, double now) {
    constexpr double kSettle = 0.25, kChase = 0.6;
    kc::Vec3 local;
    const bool inPlace = !kenshi::GetPosition(c, local) || Dist(local, at.pos) <= 0.5f;
    auto it = fallPrep_.find(h);
    const bool settled = it == fallPrep_.end() || now - it->second.lastMove >= kSettle;
    if (inPlace || (it != fallPrep_.end() && now - it->second.start >= kChase)) {
        if (!settled) return false;
        fallPrep_.erase(h);
        return true;
    }
    if (it == fallPrep_.end()) it = fallPrep_.emplace(h, FallPrep{now, now}).first;
    HostCallScope scope;
    kenshi::Halt(c);
    kenshi::Teleport(c, at.pos, at.rot);
    lastDest_.erase(h);
    it->second.lastMove = now;
    return false;
}

bool KenshiWorld::Exists(const kc::Handle& h) {
    auto a = alias_.find(h);
    if (a != alias_.end()) {
        // The real character streamed in after all (its zone loaded later here than on the
        // host): it replaces the stand-in, otherwise there would be two of them.
        kenshi::Character* real = kenshi::Resolve(h);
        if (real && kenshi::IsCharacter(real)) {
            if (kenshi::Character* standIn = kenshi::Resolve(a->second); standIn && standIn != real) {
                HostCallScope scope;
                kenshi::DestroyObject(standIn);
            }
            alias_.erase(a);
            resolved_[h] = real;
            return true;
        }
    }
    return Find(h) != nullptr;
}

void KenshiWorld::Reconcile(const std::vector<kc::Handle>& known, const std::vector<kc::MissingChar>& missing, double now,
                            std::vector<kc::Handle>& adopted) {
    constexpr double kLinger = 5.0;
    if (!client_ || !live_) return;
    std::unordered_set<kc::Handle, HandleHash> taken(known.begin(), known.end());
    for (const auto& [host, local] : alias_) taken.insert(local);
    struct Stranger { kenshi::Character* c; kc::Handle h; double since; bool used; };
    std::vector<Stranger> strangers;
    std::unordered_map<kc::Handle, double, HandleHash> seen;
    kenshi::ActiveCharacters(scratch_);
    for (kenshi::Character* c : scratch_) {
        kc::Handle h;
        if (!kenshi::GetHandle(c, h) || !h.valid() || squad_.count(h) || taken.count(h) || kenshi::IsDead(c)) continue;
        auto it = strangerSince_.find(h);
        const double since = it == strangerSince_.end() ? now : it->second;
        seen[h] = since;
        strangers.push_back({c, h, since, false});
    }
    strangerSince_ = std::move(seen);
    // A stranger of the same kind as a missing host character becomes its stand-in: this is how a
    // unique character the local game made on its own (and so cannot be created twice) still
    // matches the host's.
    constexpr float kAdoptRange = 300.0f;   // never drag a character across the map (into unloaded land)
    for (const kc::MissingChar& m : missing) {
        if (alias_.count(m.handle)) continue;
        Stranger* best = nullptr;
        float bestDist = kAdoptRange;
        for (Stranger& s : strangers) {
            kc::SpawnInfo info;
            kc::Vec3 p;
            if (s.used || !kenshi::ReadSpawnSource(s.c, info) || info.templateSid != m.spawn.templateSid ||
                info.factionSid != m.spawn.factionSid || !kenshi::GetPosition(s.c, p))
                continue;
            const float d = Dist(p, m.pos);
            if (d <= bestDist) { best = &s; bestDist = d; }
        }
        if (!best) continue;
        best->used = true;
        alias_[m.handle] = best->h;
        resolved_.erase(m.handle);
        strangerSince_.erase(best->h);
        kc::Quat rot;
        kenshi::GetRotation(best->c, rot);
        kenshi::Teleport(best->c, m.pos, rot);
        adopted.push_back(m.handle);
        Log("a local %s stands in for the host's (%.0f units away)", m.spawn.templateSid.c_str(), bestDist);
    }
    // Only where the host's world is loaded too: a character of a zone that only we loaded (it
    // streams in a little earlier here) comes from the same save, and the host will have it soon.
    constexpr float kHostArea = 300.0f;
    std::vector<kc::Vec3> hostArea;
    for (const kc::Handle& k : known) {
        kc::Vec3 p;
        if (kenshi::Character* c = Find(k); c && kenshi::GetPosition(c, p)) hostArea.push_back(p);
    }
    auto inHostArea = [&](kenshi::Character* c) {
        kc::Vec3 p;
        if (!kenshi::GetPosition(c, p)) return false;
        for (const kc::Vec3& q : hostArea) if (Dist(p, q) <= kHostArea) return true;
        return false;
    };
    int removed = 0;
    for (Stranger& s : strangers) {
        if (s.used || now - s.since < kLinger || !inHostArea(s.c)) continue;
        HostCallScope scope;
        if (kenshi::DestroyObject(s.c)) ++removed;
        strangerSince_.erase(s.h);
    }
    if (removed) {
        resolved_.clear();
        Log("removed %d local character(s) the host does not have", removed);
    }
}

void KenshiWorld::Rehandle(const kc::Handle& from, const kc::Handle& to) {
    // Our copy keeps whatever handle it has here: the host's new handle now points to it.
    auto a = alias_.find(from);
    const kc::Handle local = a != alias_.end() ? a->second : from;
    if (a != alias_.end()) alias_.erase(a);
    if (local != to) alias_[to] = local;
    resolved_.erase(from);
    resolved_.erase(to);
    if (auto t = lastTarget_.find(from); t != lastTarget_.end()) { lastTarget_[to] = t->second; lastTarget_.erase(from); }
}

void KenshiWorld::Despawn(const kc::Handle& h) {
    auto a = alias_.find(h);
    if (a == alias_.end()) return;
    if (kenshi::Character* c = kenshi::Resolve(a->second)) {
        HostCallScope scope;
        kenshi::DestroyObject(c);
    }
    alias_.erase(a);
    resolved_.erase(h);
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
    if (kenshi::IsDown(c) || kenshi::IsRagdoll(c)) out.flags |= kc::kFlagDown;
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
    applied_.insert(c);
    kc::EntityState& last = lastTarget_[h];
    last = target;
    last.flags = latest.flags;
    const double now = NowSeconds();
    // A ragdoll that is being set up must not be touched: moving the character then throws the
    // body across the map. Leave it alone while it falls.
    if (auto fell = fellAt_.find(h); fell != fellAt_.end()) {
        if (now - fell->second < 2.0) { lastDest_.erase(h); return; }
        fellAt_.erase(fell);
    }
    // Posture: lying on the ground (knocked out, knocked down) or standing must match the host.
    // Deaths come through vitals; getting up is normally decided by the AI, which clients do not run.
    const bool hostDead = (latest.flags & kc::kFlagDead) != 0;
    const bool hostDown = (latest.flags & kc::kFlagDown) != 0;
    const bool localDown = kenshi::IsRagdoll(c) || kenshi::IsDead(c);
    if (hostDown != localDown && !hostDead) {
        double& since = postureSince_[h];
        if (since == 0) since = now;
        double& fixed = postureFixed_[h];
        if (now - since > 0.3 && now - fixed > 1.0) {   // not a one-frame flicker, and not every frame
            if (!hostDown) {
                HostCallScope scope;
                kenshi::StandUp(c);
                fixed = now;
            } else if (ReadyToFall(h, c, target, now)) {
                HostCallScope scope;
                kenshi::SetRagdoll(c, true);
                fixed = now;
                fellAt_[h] = now;
            }
        }
    } else {
        postureSince_.erase(h);
    }
    kc::Vec3 local;
    if (!kenshi::GetPosition(c, local)) return;
    // A body on the ground belongs to ragdoll physics (teleports do not move it); it fell where the
    // host's did (see ReadyToFall).
    if (hostDown || hostDead || localDown) {
        lastDest_.erase(h);
        return;
    }

    const float err = Dist(local, target.pos);
    HostCallScope scope;
    // Far off (late join, lag spike, teleport on the host): snap straight to the host state.
    if (err > cfg_.snapDistance) {
        kenshi::Teleport(c, target.pos, target.rot);
        lastDest_.erase(h);
        return;
    }
    // The game's own locomotion animates the character (walking toward the host's destination)...
    const bool hostMoving = (latest.flags & kc::kFlagMoving) != 0;
    auto it = lastDest_.find(h);
    if (hostMoving) {
        if (it == lastDest_.end() || Dist(it->second, latest.dest) > cfg_.destEpsilon) {
            if (kenshi::SetDestination(c, latest.dest)) lastDest_[h] = latest.dest;
        }
    } else if (it != lastDest_.end() || kenshi::IsMoving(c)) {
        kenshi::Halt(c);   // the host stopped: stop walking, the correction below settles it exactly
        lastDest_.erase(h);
    }
    // ...while its position is continuously pulled onto the host's: no drift, even when paused.
    if (err > 0.01f) {
        const float k = err > 2.0f ? 0.5f : 0.25f;
        const kc::Vec3 p{local.x + (target.pos.x - local.x) * k, local.y + (target.pos.y - local.y) * k,
                         local.z + (target.pos.z - local.z) * k};
        kenshi::SetPositionSimple(c, err < 0.05f ? target.pos : p);
    }
    if (traceFrames > 0 && h == traceHandle && (--traceFrames % 15 == 0 || err > 1.0f || (haveHostTime_ && hostTime_.paused))) {
        kc::Vec3 after;
        kenshi::GetPosition(c, after);
        Log("trace t=%.3f local=(%.2f,%.2f) target=(%.2f,%.2f) latest=(%.2f,%.2f) err=%.2f after=(%.2f,%.2f) moving=%d/%d combat=%d", now,
            local.x, local.z, target.pos.x, target.pos.z, latest.pos.x, latest.pos.z, err, after.x, after.z, hostMoving ? 1 : 0,
            kenshi::IsMoving(c) ? 1 : 0, latest.combatTarget ? 1 : 0);
    }
}

void KenshiWorld::ApplyVitals(const kc::Handle& h, const kc::EntityVitals& v) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kenshi::WriteVitals(c, v);   // values only: clients never fall over on their own (see hk_ragdollMode)
    // Death and knockout are transitions, not values: replay them exactly when the host has them,
    // where the host's character collapsed.
    const bool dies = (v.flags & kc::kVitDead) && !kenshi::IsDead(c);
    const bool faints = !dies && (v.flags & kc::kVitUnconscious) && !kenshi::IsUnconscious(c) && !kenshi::IsDead(c);
    if (!dies && !faints) return;
    auto at = lastTarget_.find(h);
    const double now = NowSeconds();
    if (!kenshi::IsRagdoll(c) && at != lastTarget_.end() && !ReadyToFall(h, c, at->second, now)) return;
    HostCallScope scope;
    if (!kenshi::IsRagdoll(c)) fellAt_[h] = now;
    if (dies) {
        kenshi::CallDeclareDead(c);
    } else {
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

namespace {
constexpr float kLootRange = 12.0f;   // the looter stops next to the body; bodies lie within a few units of the host's copy
}

void KenshiWorld::RequestLoot(const kc::Handle& looter, kenshi::Character* target) {
    kc::Handle t;
    kc::Vec3 tp, lp;
    kenshi::Character* me = FindSquad(looter);
    if (!me || !kenshi::GetHandle(target, t) || !kenshi::GetPosition(target, tp) || !kenshi::GetPosition(me, lp)) return;
    pendingLoot_.erase(std::remove_if(pendingLoot_.begin(), pendingLoot_.end(), [&](const PendingLoot& p) { return p.looter == looter; }),
                       pendingLoot_.end());
    Log("loot order: %.0f units to go", Dist(lp, tp));
    if (Dist(lp, tp) <= kLootRange) {
        kenshi::OpenLootWindow(me, target);
        return;
    }
    kc::Command c;
    c.kind = kc::CommandKind::MoveTo;
    c.pos = tp;
    QueueLocalOrder(looter, c);
    pendingLoot_.push_back({looter, t, NowSeconds() + 30.0});
}

void KenshiWorld::UpdatePendingLoot() {
    const double now = NowSeconds();
    for (auto it = pendingLoot_.begin(); it != pendingLoot_.end();) {
        kenshi::Character* me = FindSquad(it->looter);
        kenshi::Character* target = kenshi::Resolve(it->target);
        kc::Vec3 lp, tp;
        const bool valid = me && target && (kenshi::IsDown(target) || kenshi::IsDead(target) || kenshi::IsRagdoll(target)) && now < it->until;
        if (valid && kenshi::GetPosition(me, lp) && kenshi::GetPosition(target, tp) && Dist(lp, tp) <= kLootRange) {
            kenshi::OpenLootWindow(me, target);
            it = pendingLoot_.erase(it);
        } else if (!valid) {
            it = pendingLoot_.erase(it);
        } else {
            ++it;
        }
    }
}

void KenshiWorld::TakeLocalOrders(std::vector<std::pair<kc::Handle, kc::Command>>& out) {
    std::lock_guard<std::mutex> lk(ordersMutex_);
    out.swap(orders_);
    orders_.clear();
    for (auto& [h, c] : out) h = HostHandleOf(h);   // orders name our characters as the host knows them
}

void KenshiWorld::WeatherRegionTick(void* region, bool afterUpdate) {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    if (!active_) return;
    if (afterUpdate) {   // remember every region's state after the game advanced it (host: what we send)
        kc::RegionWeather w;
        if (kenshi::ReadRegionWeather(region, w)) seenRegions_[region] = std::move(w);
        return;
    }
    if (!client_) return;   // client: impose the host's weather before the game's update runs
    kc::RegionWeather mine;
    if (!kenshi::ReadRegionWeather(region, mine)) return;
    auto it = hostWeather_.find(mine.regionSid);
    if (it != hostWeather_.end()) {
        HostCallScope scope;
        kenshi::WriteRegionWeather(region, it->second);
    }
}

bool KenshiWorld::ReadInventory(const kc::Handle& h, std::vector<kc::ItemState>& out) {
    kenshi::Character* c = Find(h);
    return c && kenshi::ReadInventory(c, out);
}

bool KenshiWorld::ExecuteInvOp(const kc::Handle& from, const kc::Handle& to, const kc::InvOp& op) {
    kenshi::Character* a = Find(from);
    kenshi::Character* b = Find(to);
    if (!a || !b) return false;
    std::string err;
    HostCallScope scope;
    if (kenshi::MoveInventoryItem(a, b, op, &err)) return true;
    Log("client item move refused: %s (%s x%d)", err.c_str(), op.item.templateSid.c_str(), op.item.quantity);
    return false;
}

bool KenshiWorld::ApplyInventory(const kc::Handle& h, const std::vector<kc::ItemState>& items) {
    kenshi::Character* c = Find(h);
    if (!c) return false;
    std::vector<kc::ItemState> local;
    if (kenshi::ReadInventory(c, local) && local == items) return true;
    std::string err;
    HostCallScope scope;
    if (!kenshi::RebuildInventory(c, items, &err)) Log("inventory rebuild incomplete: %s", err.c_str());
    // success means: the game now shows exactly the host's layout
    return kenshi::ReadInventory(c, local) && local == items;
}

size_t KenshiWorld::ExpireAllWeather() {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    size_t n = 0;
    for (auto& [r, w] : seenRegions_) n += kenshi::ExpireRegionWeather(r) ? 1 : 0;
    return n;
}

void KenshiWorld::ReadWeather(std::vector<kc::RegionWeather>& out) {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    out.clear();
    for (auto& [r, w] : seenRegions_) out.push_back(w);
    std::sort(out.begin(), out.end(), [](const kc::RegionWeather& a, const kc::RegionWeather& b) { return a.regionSid < b.regionSid; });
}

void KenshiWorld::ApplyWeather(const std::vector<kc::RegionWeather>& regions) {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    for (const auto& w : regions) hostWeather_[w.regionSid] = w;
}

kc::TimeState KenshiWorld::GetTime() {
    kc::TimeState t{kenshi::GetFrameSpeed(), kenshi::GetPaused(), 0.0};
    kenshi::GetGameHours(t.gameHours);
    return t;
}

// The clock itself is not written: the game recomputes its hours from an internal counter every
// frame. Clients start from the host's save and run at the host's speed, paused when it is, so
// both clocks stay together (measured: under 0.01 h apart after 15 minutes at x3).
void KenshiWorld::SetTime(const kc::TimeState& t) {
    hostTime_ = t;
    haveHostTime_ = true;
}

void KenshiWorld::HoldForJoin(bool hold) {
    if (hold == holding_) return;
    holding_ = hold;
    if (hold) {
        pausedByHold_ = !kenshi::GetPaused();
        if (pausedByHold_) kenshi::CallUserPause(true);   // the real pause: game speed 0
        Toast("A player is joining: the game is paused until they are in.");
    } else {
        if (pausedByHold_ && kenshi::GetPaused()) kenshi::CallUserPause(false);
        pausedByHold_ = false;
    }
}

bool KenshiWorld::BeginWorldExport(std::string* err) {
    if (kenshi::SaveManagerBusy()) { if (err) *err = "the game is already saving or loading"; return false; }
    std::string folder;
    if (!kenshi::RequestSave(kExportSlot, &folder) || folder.empty()) {
        if (err) *err = "the game refused to save now";
        return false;
    }
    exportFolder_ = folder;
    exporting_ = true;
    exportLastSize_ = 0;
    exportStableSince_ = 0;
    return true;
}

kc::ExportStatus KenshiWorld::PollWorldExport(std::vector<kc::WorldFile>& files, std::string* err) {
    if (!exporting_) { if (err) *err = "no save in progress"; return kc::ExportStatus::Failed; }
    if (kenshi::SaveManagerBusy()) return kc::ExportStatus::Pending;
    // Kenshi clears its request before the files are all on disk (it copies its working folder
    // into the slot afterwards): wait until quick.save exists and the folder stops growing.
    std::error_code ec;
    const fs::path dir = FromGamePath(exportFolder_) / kExportSlot;
    uint64_t size = 0;
    bool haveMain = false;
    if (fs::is_directory(dir, ec)) {
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            size += it->file_size(ec);
            if (it->path().filename() == "quick.save") haveMain = true;
        }
    }
    const double now = NowSeconds();
    if (!haveMain || size != exportLastSize_) {
        exportLastSize_ = size;
        exportStableSince_ = now;
        return kc::ExportStatus::Pending;   // the session's export timeout bounds this wait
    }
    if (now - exportStableSince_ < 1.0) return kc::ExportStatus::Pending;
    exporting_ = false;
    files.clear();
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        kc::WorldFile f;
        f.path = ToUtf8(fs::relative(it->path(), dir, ec));
        if (ec || !kc::ValidWorldPath(f.path)) { if (err) *err = "unexpected file in save: " + f.path; return kc::ExportStatus::Failed; }
        std::ifstream in(it->path(), std::ios::binary);
        f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        total += f.data.size();
        if (total > kc::kMaxWorldBytes || files.size() >= kc::kMaxWorldFiles) { if (err) *err = "save too large"; return kc::ExportStatus::Failed; }
        files.push_back(std::move(f));
    }
    if (ec || files.empty()) { if (err) *err = "cannot read the save folder"; return kc::ExportStatus::Failed; }
    return kc::ExportStatus::Done;
}

bool KenshiWorld::BeginWorldImport(const std::vector<kc::WorldFile>& files, std::string* err) {
    std::string folder;
    if (!kenshi::SaveFolder(folder)) { if (err) *err = "cannot find the save folder"; return false; }
    if (kenshi::SaveManagerBusy()) { if (err) *err = "the game is busy saving or loading"; return false; }
    const fs::path dir = FromGamePath(folder) / kImportSlot;
    std::error_code ec;
    if (dir.filename() != kImportSlot) { if (err) *err = "bad save folder"; return false; }
    fs::remove_all(dir, ec);   // only ever our own slot
    for (const auto& f : files) {
        if (!kc::ValidWorldPath(f.path)) { if (err) *err = "refused file path " + f.path; return false; }
        const fs::path target = dir / FromUtf8(f.path);
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
        if (!out) { if (err) *err = "cannot write " + ToUtf8(target); return false; }
    }
    if (!kenshi::RequestLoad(kImportSlot)) { if (err) *err = "the game refused to load the save"; return false; }
    return true;
}

bool KenshiWorld::EnsurePlayerCharacter(const std::string& playerName, kc::Handle& out) {
    const std::string name = playerName.substr(0, 15);   // what fits a character name here
    std::vector<kenshi::Character*> squad;
    kenshi::PlayerCharacters(squad);
    if (squad.empty()) return false;
    for (kenshi::Character* c : squad) {   // back again: same character as last time
        std::string n;
        if (kenshi::CharacterName(c, n) && n == name && kenshi::GetHandle(c, out)) return true;
    }
    kc::Vec3 p;
    if (!kenshi::GetPosition(squad.front(), p)) return false;
    p.x += 4.0f;
    p.z += 4.0f;
    std::string err;
    HostCallScope scope;
    kenshi::Character* c = kenshi::CreateRecruit(squad.front(), name, p, &err);
    if (!c || !kenshi::GetHandle(c, out)) {
        Log("cannot create %s's character: %s", name.c_str(), err.c_str());
        return false;
    }
    Log("created %s's own character", name.c_str());
    Toast(name + " joins with a character of their own.");
    return true;
}

void KenshiWorld::SetRole(bool client, bool active) {
    client_ = client;
    active_ = active;
    haveHostTime_ = false;
    lastDest_.clear();
    postureSince_.clear();
    postureFixed_.clear();
    {
        std::lock_guard<std::mutex> lk(weatherMutex_);
        seenRegions_.clear();
        hostWeather_.clear();
    }
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
