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
    // A character stays "driven by the host" for a second after its last update: a frame without
    // one (paused, a skipped tick) must not hand it back to the client's own game for that frame.
    const double nowView = NowSeconds();
    for (const void* c : applied_) replicatedAt_[c] = nowView;
    applied_.clear();
    for (auto it = replicatedAt_.begin(); it != replicatedAt_.end();) {
        if (nowView - it->second > 1.0 || !active_ || !client_) { it = replicatedAt_.erase(it); continue; }
        v->replicated.insert(it->first);
        ++it;
    }
    v->gameSpeed = kenshi::GetFrameSpeed();
    if (active_ && client_) {
        for (auto it = animTargets_.begin(); it != animTargets_.end();) {
            if (nowView - it->second->sampledAt > 1.0 || !v->replicated.count(it->first)) { it = animTargets_.erase(it); continue; }
            if (void* ac = kenshi::AnimationOf(it->first)) {
                v->anims[ac] = it->second;
                const auto& tg = *it->second;
                kenshi::WriteAnimMaster(ac, tg.masterTime + tg.masterSpeed * float(nowView - tg.sampledAt) * v->gameSpeed, tg.masterSpeed);
            }
            ++it;
        }
    } else {
        animTargets_.clear();
    }
    if (active_ && client_)
        for (const auto& [h, st] : lastTarget_)
            if (kenshi::Character* c = Find(h); c && v->replicated.count(c))
                if (void* m = kenshi::MovementOf(c)) v->facing[m] = kenshi::ForwardOf(st.rot);
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
    if (!kenshi::ReadPace(c, out.gait, out.pace)) { out.gait = 0; out.pace = 0; }
    if (out.pace > 6553.0f) out.pace = 6553.0f;
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
    // at the host's pace: a running character runs (animation included), a walking one walks
    kenshi::WritePace(c, latest.gait, latest.pace >= 6553.0f ? 999.0f : latest.pace);
    auto it = lastDest_.find(h);
    if (hostMoving) {
        if (it == lastDest_.end() || Dist(it->second, latest.dest) > cfg_.destEpsilon) {
            if (kenshi::SetDestination(c, latest.dest)) lastDest_[h] = latest.dest;
        }
    } else if (it != lastDest_.end() || kenshi::IsMoving(c)) {
        kenshi::Halt(c);   // the host stopped: stop walking, the correction below settles it exactly
        lastDest_.erase(h);
    }
    // Standing still, it faces where the host's does (moving, the path turns it; in combat the
    // combat movement hook imposes the host's facing).
    if (!hostMoving || latest.combatTarget != 0) {
        kc::Vec3 mine;
        const kc::Vec3 want = kenshi::ForwardOf(target.rot);
        if (kenshi::GetFacing(c, mine) && mine.x * want.x + mine.z * want.z < 0.999f) kenshi::FaceDirection(c, want);
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
        if (kenshi::ReadRegionWeather(region, w)) {
            if (!client_) HostEffectsTick(region, w.regionSid);
            seenRegions_[region] = std::move(w);
        }
        return;
    }
    if (!client_) {
        if (forcedWeather_.empty()) return;
        kc::RegionWeather w;
        if (!kenshi::ReadRegionWeather(region, w)) return;
        auto f = forcedWeather_.find(w.regionSid);
        if (f == forcedWeather_.end()) return;
        w.seasonSid = f->second.first;
        w.weatherSid = f->second.second;
        forcedWeather_.erase(f);
        HostCallScope scope;
        Log("test: region %s weather -> %s: %s", w.regionSid.c_str(), w.weatherSid.c_str(), kenshi::WriteRegionWeather(region, w) ? "ok" : "failed");
        return;
    }
    // client: impose the host's weather before the game's update runs
    kc::RegionWeather mine;
    if (!kenshi::ReadRegionWeather(region, mine)) return;
    auto it = hostWeather_.find(mine.regionSid);
    if (it != hostWeather_.end()) {
        HostCallScope scope;
        if (kenshi::WriteRegionWeather(region, it->second) && mine.weatherSid != it->second.weatherSid) fxRebuild_.insert(region);
    }
    ClientEffectsTick(region, mine.regionSid);
}

// Host: after the game advanced a region, note every effect its groups placed, moved or removed.
// Only effects near a player's character matter (an effect shows only in the zones loaded around a
// camera): one that comes near is announced like a new one, one that goes away like a removed one.
namespace {
constexpr float kFxRadius = 9000.0f;            // two loading zones
constexpr float kFxForget = kFxRadius + 1000.0f;
constexpr double kFxRefresh = 2.0;              // wandering effects: state sent at least this often
bool NearAny(const kc::Vec3& p, const std::vector<kc::Vec3>& centers, float r) {
    for (const auto& c : centers) {
        const float dx = p.x - c.x, dz = p.z - c.z;
        if (dx * dx + dz * dz < r * r) return true;
    }
    return false;
}
} // namespace

void KenshiWorld::HostEffectsTick(void* region, const std::string& regionSid) {
    std::vector<kenshi::EffectGroupInfo> groups;
    if (!kenshi::ReadEffectGroups(region, groups)) return;
    auto& known = hostFx_[region];
    const double now = NowSeconds();
    auto drop = [&](HostFx& f) {
        if (!f.sent) return;
        fxOut_.ended.push_back(f.id);
        fxLive_.erase(f.id);
        f.sent = false;
    };
    std::unordered_set<void*> seen;
    for (const auto& g : groups) {
        for (void* h : g.handlers) {
            seen.insert(h);
            auto it = known.find(h);
            if (it != known.end() && it->second.group != g.group) {   // that address now holds another group's effect
                drop(it->second);
                known.erase(it);
                it = known.end();
            }
            if (it == known.end()) it = known.emplace(h, HostFx{nextFxId_++, g.group, g.kind, false, false, 0.0, {}}).first;   // movedAt 0: never sent
            HostFx& f = it->second;
            if (f.done) continue;
            kc::WeatherEffect e;
            if (!kenshi::ReadEffect(h, g.kind, e)) continue;
            if (!e.endless && e.life <= 0) {   // fading from now on: the clients' copies fade with it
                drop(f);
                f.done = true;
                continue;
            }
            if (!f.sent) {
                if (!NearAny(e.pos, fxCenters_, kFxRadius)) continue;
                if (f.movedAt != 0.0) f.id = nextFxId_++;   // it was sent before, then went away: new to the clients
                e.id = f.id;
                e.kind = g.kind;
                e.regionSid = regionSid;
                e.effectSid = g.effectSid;
                e.ordinal = g.ordinal;
                f.sent = true;
                f.movedAt = now;
                f.turnTo = e.turnTo;
                fxLive_[e.id] = e;
                fxOut_.spawned.push_back(std::move(e));
                continue;
            }
            if (!NearAny(e.pos, fxCenters_, kFxForget)) {
                drop(f);
                continue;
            }
            kc::WeatherEffect& live = fxLive_[f.id];
            kenshi::ReadEffect(h, g.kind, live);
            if (g.kind != kc::EffectKind::Wandering) continue;
            const bool turned = live.turnTo.x != f.turnTo.x || live.turnTo.y != f.turnTo.y || live.turnTo.z != f.turnTo.z;
            if (turned || now - f.movedAt >= kFxRefresh) {
                fxOut_.moved.push_back({f.id, live.pos, live.dir, live.turnTo});
                f.movedAt = now;
                f.turnTo = live.turnTo;
            }
        }
    }
    for (auto it = known.begin(); it != known.end();) {
        if (seen.count(it->first)) { ++it; continue; }
        drop(it->second);
        it = known.erase(it);
    }
}

// Client: before the game advances a region, its groups place nothing of their own; what the host
// placed is placed here (same spot, same random rolls), moved like the host's, and removed with it.
void KenshiWorld::ClientEffectsTick(void* region, const std::string& regionSid) {
    std::vector<kenshi::EffectGroupInfo> groups;
    if (!kenshi::ReadEffectGroups(region, groups)) return;
    std::unordered_map<void*, void*> groupOf;   // live handler -> its group
    for (const auto& g : groups) {
        kenshi::BlockEffectSpawns(g.group);
        for (void* h : g.handlers) groupOf[h] = g.group;
    }
    auto& stopped = fxStopped_[region];
    for (auto it = stopped.begin(); it != stopped.end();) it = groupOf.count(*it) ? std::next(it) : stopped.erase(it);
    auto stop = [&](void* h) {
        HostCallScope scope;
        if (stopped.insert(h).second && kenshi::StopEffect(h)) ++fxStats_.stopped;
    };
    // forget our copies the game has removed (they ran their course)
    for (auto it = clientFx_.begin(); it != clientFx_.end();) {
        if (it->second.regionSid != regionSid) { ++it; continue; }
        auto g = groupOf.find(it->second.handler);
        if (g == groupOf.end() || g->second != it->second.group) {
            FxLog(it->first, g == groupOf.end() ? "gone by itself" : "gone (address reused)", &it->second);
            // a moving effect the host still has (our group was replaced under it): place it again now
            const ClientFx& c = it->second;
            const float lived = float(NowSeconds() - c.placedAt);
            if (c.kind == kc::EffectKind::Wandering && !fxEnd_.count(it->first) && (c.last.endless || c.life - lived > 1.0f) &&
                fxPending_.insert(it->first).second) {
                kc::WeatherEffect e = c.last;
                e.age += lived;
                e.life = c.life - lived;
                fxSpawn_[regionSid].push_back({e, NowSeconds()});
            }
            it = clientFx_.erase(it);
        } else {
            ++it;
        }
    }
    // removed on the host
    for (auto it = fxEnd_.begin(); it != fxEnd_.end();) {
        auto c = clientFx_.find(*it);
        if (c == clientFx_.end()) { it = fxEnd_.erase(it); continue; }
        if (c->second.regionSid != regionSid) { ++it; continue; }
        FxLog(c->first, "ended by the host", &c->second);
        stop(c->second.handler);
        clientFx_.erase(c);
        it = fxEnd_.erase(it);
    }
    // gone from the host's complete set
    if (fxFullDone_[regionSid] != fxFullGen_) {
        fxFullDone_[regionSid] = fxFullGen_;
        for (auto it = clientFx_.begin(); it != clientFx_.end();) {
            if (it->second.regionSid == regionSid && !fxLiveIds_.count(it->first)) {
                FxLog(it->first, "not in the host's set", &it->second);
                stop(it->second.handler);
                it = clientFx_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // placed on the host (not into groups the game is about to replace with the new weather's)
    const double now = NowSeconds();
    auto pend = fxRebuild_.count(region) ? fxSpawn_.end() : fxSpawn_.find(regionSid);
    if (pend != fxSpawn_.end()) {
        std::vector<PendingFx> keep;
        for (auto& p : pend->second) {
            if (clientFx_.count(p.e.id) || (p.e.kind == kc::EffectKind::Point && fxHandled_.count(p.e.id))) {
                fxPending_.erase(p.e.id);
                continue;
            }
            const kenshi::EffectGroupInfo* group = nullptr;
            for (const auto& g : groups)
                if (g.kind == p.e.kind && g.effectSid == p.e.effectSid && g.ordinal == p.e.ordinal) group = &g;
            if (!group) {   // our region may still be switching to the host's weather
                if (now - p.since < 3.0) keep.push_back(std::move(p));
                else { fxPending_.erase(p.e.id); ++fxStats_.noGroup; }
                continue;
            }
            kc::WeatherEffect e = p.e;
            if (fxHandled_.count(e.id) && !e.endless && e.life < 1.0f) { fxPending_.erase(e.id); continue; }   // ours just ended too
            const float late = float(now - p.since);
            e.age += late;
            e.strikeIn -= late;
            if (!e.endless) e.life -= late;
            fxPending_.erase(e.id);
            if (!e.endless && e.life <= 0) { ++fxStats_.over; continue; }   // over by now
            void* h = nullptr;
            {
                HostCallScope scope;
                h = kenshi::SpawnEffect(group->group, e);
            }
            if (!h) { ++fxStats_.failed; continue; }
            ++fxStats_.placed;
            fxHandled_.insert(e.id);
            clientFx_[e.id] = {group->group, h, regionSid, e.kind, now, e.life, e};
            groupOf[h] = group->group;
        }
        pend->second.swap(keep);
    }
    // moved on the host
    for (auto it = fxMove_.begin(); it != fxMove_.end();) {
        auto c = clientFx_.find(it->first);
        if (c == clientFx_.end()) { it = fxPending_.count(it->first) ? std::next(it) : fxMove_.erase(it); continue; }
        if (c->second.regionSid != regionSid) { ++it; continue; }
        kc::WeatherEffect mine;
        if (kenshi::ReadEffect(c->second.handler, kc::EffectKind::Wandering, mine)) {
            const double d = Dist(mine.pos, it->second.pos);
            ++fxStats_.corrections;
            fxStats_.correctionSum += d;
            fxStats_.correctionMax = std::max(fxStats_.correctionMax, d);
        }
        kenshi::WriteEffectState(c->second.handler, it->second);
        c->second.last.pos = it->second.pos;
        c->second.last.dir = it->second.dir;
        c->second.last.turnTo = it->second.turnTo;
        it = fxMove_.erase(it);
    }
    // anything else (left from before we joined) fades out
    std::unordered_set<void*> ours;
    for (const auto& [id, c] : clientFx_)
        if (c.regionSid == regionSid) ours.insert(c.handler);
    for (const auto& [h, g] : groupOf)
        if (!ours.count(h)) stop(h);
}

void KenshiWorld::FxLog(uint32_t id, const char* what, const ClientFx* c) {
    if (fxLogged_ >= 400) return;
    ++fxLogged_;
    if (c)
        Log("effect %u %s after %.1f s (placed with %.1f s to live)", id, what, NowSeconds() - c->placedAt, c->life);
    else
        Log("effect %u %s", id, what);
}

bool KenshiWorld::TakeEffectsRebuild(void* region) {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    return fxRebuild_.erase(region) > 0;
}

void KenshiWorld::NoteEffectStop(void* handler, uintptr_t callerRva) {
    std::unique_lock<std::mutex> lk(weatherMutex_, std::try_to_lock);
    uint32_t id = 0;
    if (lk.owns_lock())
        for (const auto& [cid, c] : clientFx_)
            if (c.handler == handler) id = cid;
    static std::atomic<int> logged{0};
    if (id && logged.fetch_add(1) < 300) Log("effect %u stopped by the game (caller rva %llx)", id, (unsigned long long)callerRva);
}

void KenshiWorld::NoteAnim(kenshi::Character* c, kc::AnimEvent e) {
    if (client_ || !active_ || !c) return;
    std::lock_guard<std::mutex> lk(animMutex_);
    // the game ends attacks, actions and stumbles every frame "just in case": one end is enough
    const bool isEnd = e.kind == kc::AnimKind::EndCombat || e.kind == kc::AnimKind::StopAction || e.kind == kc::AnimKind::EndStumble ||
                       e.kind == kc::AnimKind::Sheathe;
    auto& last = animLast_[c];
    const uint32_t sig = (uint32_t(e.kind) << 24) ^ uint32_t(std::hash<std::string>{}(e.name) & 0xFFFFFF);
    if (isEnd && last == sig) return;
    last = sig;
    if (animOut_.size() < 20000) animOut_.emplace_back(c, std::move(e));
}

void KenshiWorld::TakeAnimEvents(std::vector<std::pair<kc::Handle, kc::AnimEvent>>& out, bool state) {
    out.clear();
    std::vector<std::pair<kenshi::Character*, kc::AnimEvent>> raw;
    {
        std::lock_guard<std::mutex> lk(animMutex_);
        raw.swap(animOut_);
    }
    if (client_ || !active_) return;
    for (auto& [c, e] : raw) {
        kc::Handle h;
        if (kenshi::IsCharacter(c) && kenshi::GetHandle(c, h) && h.valid()) out.emplace_back(h, std::move(e));
    }
    if (!state) return;
    // every character the clients see: its current action and modes (the session drops unknown handles)
    std::vector<kenshi::Character*> chars;
    kenshi::ActiveCharacters(chars);
    for (kenshi::Character* c : chars) {
        kenshi::AnimModes m;
        kc::Handle h;
        if (!kenshi::ReadAnimModes(c, m) || !kenshi::GetHandle(c, h)) continue;
        kc::AnimEvent e;
        e.kind = kc::AnimKind::State;
        e.name = m.action;
        e.flags = uint8_t((m.combat ? 1 : 0) | (m.carried ? 2 : 0) | (m.carryLeft ? 4 : 0) | (m.carryRight ? 8 : 0) | (m.guardLegs ? 16 : 0) |
                          (m.guardUpper ? 32 : 0));
        out.emplace_back(h, std::move(e));
        kc::AnimEvent w;
        w.kind = kc::AnimKind::WeaponState;
        std::string sid, sec;
        if (kenshi::WeaponInHands(c, sid, sec)) w.name = sid + "\t" + sec;
        out.emplace_back(h, std::move(w));
    }
}

void KenshiWorld::NoteGround(kc::GroundEvent e) {
    if (client_ || !active_) return;
    std::lock_guard<std::mutex> lk(groundMutex_);
    if (groundOut_.size() < 4096) groundOut_.push_back(std::move(e));
}

void KenshiWorld::TakeGroundEvents(std::vector<kc::GroundEvent>& out) {
    std::lock_guard<std::mutex> lk(groundMutex_);
    out.swap(groundOut_);
    groundOut_.clear();
}

void KenshiWorld::ApplyGround(const kc::GroundEvent& e) {
    if (!client_) return;
    HostCallScope scope;
    if (e.kind == kc::GroundKind::PickedUp) {
        // the same item here (from the shared save), or the copy we made when the host dropped it
        auto a = groundAlias_.find(e.item);
        void* item = kenshi::ResolveItem(a != groundAlias_.end() ? a->second : e.item);
        const bool onGround = item && kenshi::ItemOnGround(item);
        const bool gone = onGround && kenshi::DestroyItem(item);
        Log("host picked up item %u:%u: %s", e.item.index, e.item.serial,
            gone ? "removed here" : !item ? "we do not have it" : !onGround ? "not on the ground here" : "could not remove it");
        if (a != groundAlias_.end()) groundAlias_.erase(a);
        return;
    }
    kc::Handle mine;
    std::string why;
    if (kenshi::CreateGroundItem(e.state, e.pos, mine, &why)) {
        groundAlias_[e.item] = mine;
        Log("host dropped item %u:%u (%s): placed here as %u:%u", e.item.index, e.item.serial, e.state.templateSid.c_str(), mine.index, mine.serial);
    }
    else Log("cannot place the host's dropped item %s: %s", e.state.templateSid.c_str(), why.c_str());
}

bool KenshiWorld::ReadAnimFrame(const kc::Handle& h, kc::AnimFrame& frame) {
    frame = kc::AnimFrame{};
    std::vector<kc::AnimEntry>& out = frame.anims;
    if (client_) return false;
    const double now = NowSeconds();
    if (now - animCentersAt_ > 0.25) {
        animCentersAt_ = now;
        animCenters_.clear();
        for (auto& [sh, sc] : squad_) {
            kc::Vec3 p;
            if (kenshi::GetPosition(sc, p)) animCenters_.push_back(p);
        }
    }
    kenshi::Character* c = Find(h);
    kc::Vec3 p;
    if (!c || !kenshi::GetPosition(c, p)) return false;
    constexpr float kAnimRadius = 1500.0f;   // 150 m: beyond, nobody makes out a step
    bool closeBy = false;
    for (const auto& q : animCenters_) closeBy = closeBy || ((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z) < kAnimRadius * kAnimRadius);
    if (!closeBy) return false;
    std::vector<kenshi::PlayingAnim> playing;
    if (!kenshi::ReadPlayingAnims(c, playing)) return false;
    kenshi::ReadAnimMaster(c, frame.masterTime, frame.masterSpeed);
    for (const auto& a : playing) {
        if (a.weight < 0.002f && a.desired < 0.002f) continue;   // idle entries the blender keeps around
        kc::AnimEntry e;
        e.layer = a.layer; e.looped = a.looped; e.fadingOut = a.fadingOut;
        e.anim = a.anim; e.data = a.data;
        e.time = a.time; e.weight = a.weight; e.desired = a.desired; e.speed = a.speed;
        out.push_back(std::move(e));
    }
    return true;
}

void KenshiWorld::ApplyAnimFrame(const kc::Handle& h, const kc::AnimFrame& frame, double ageSeconds) {
    const std::vector<kc::AnimEntry>& anims = frame.anims;
    kenshi::Character* c = Find(h);
    if (!c || !client_ || kenshi::IsDead(c)) return;
    const double now = NowSeconds();
    auto t = std::make_shared<HookView::AnimTarget>();
    t->anims = anims;
    t->masterTime = frame.masterTime;
    t->masterSpeed = frame.masterSpeed;
    t->sampledAt = now - ageSeconds;
    animTargets_[c] = t;
    // what the host plays and we do not: start it (the update hook then sets its time and weight)
    std::vector<kenshi::PlayingAnim> mine;
    kenshi::ReadPlayingAnims(c, mine);
    for (const auto& e : anims) {
        if (e.fadingOut || e.desired < 0.05f) continue;
        bool have = false;   // playing here too, and not on its way out
        for (const auto& m : mine) have = have || (m.anim == e.anim && !m.fadingOut);
        if (have) continue;
        double& tried = animCreateTried_[std::to_string(reinterpret_cast<uintptr_t>(c)) + "|" + e.anim];
        if (now - tried < 0.2) continue;
        tried = now;
        AnimReplayScope scope;
        void* found = nullptr;
        bool called = false;
        if (!e.data.empty()) {
            if ((found = kenshi::FindAnimData(c, e.data))) called = kenshi::CallRunAnimation(c, found, e.speed, e.layer, 0.0f);
        } else if ((found = kenshi::FindTechnique(e.anim))) {
            called = kenshi::CallStartCombatAnim(kenshi::FnAddr(kenshi::FnAnimRunCombat), c, found, e.speed);
        }
        static int createLogged = 0;
        if (createLogged < 0) {
            ++createLogged;
            std::vector<kenshi::PlayingAnim> after;
            kenshi::ReadPlayingAnims(c, after);
            bool present = false;
            for (const auto& a : after) present = present || a.anim == e.anim;
            Log("anim create: '%s' data='%s' L%d found=%d called=%d present=%d (now %zu playing)", e.anim.c_str(), e.data.c_str(), int(e.layer),
                found ? 1 : 0, called ? 1 : 0, present ? 1 : 0, after.size());
        }
    }
    if (animCreateTried_.size() > 4096) animCreateTried_.clear();
    // and set every one now (paused, the update hook does not run)
    kenshi::ReadPlayingAnims(c, mine);
    for (const auto& m : mine) {
        const kc::AnimEntry* match = nullptr;
        for (const auto& e : anims)
            if (e.anim == m.anim) { match = &e; break; }
        if (!match || match->fadingOut != m.fadingOut) {   // not the host's (or the host's is the other copy)
            if (!match) kenshi::WriteSingleAnim(m.single, m.time, 0.0f, 0.0f, 0.0f);
            continue;
        }
        const float want = match->time + match->speed * float(ageSeconds) * kenshi::GetFrameSpeed();
        kenshi::WriteSingleAnim(m.single, kenshi::SyncedAnimTime(m.single, m.time, want, match->looped), match->speed, match->weight, match->desired);
    }
}

void KenshiWorld::ApplyAnim(const kc::Handle& h, const kc::AnimEvent& e) {
    kenshi::Character* c = Find(h);
    static int logged = 0;
    if (logged < 300 && e.kind != kc::AnimKind::State && e.kind != kc::AnimKind::WeaponState && e.kind != kc::AnimKind::CombatMode) {
        ++logged;
        Log("anim in: idx=%u kind=%d name='%s' found=%d tech=%d data=%d", h.index, int(e.kind), e.name.c_str(), c ? 1 : 0,
            e.kind <= kc::AnimKind::CombatRun && kenshi::FindTechnique(e.name) ? 1 : 0, c && kenshi::FindAnimData(c, e.name) ? 1 : 0);
    }
    if (!c || !client_ || kenshi::IsDead(c)) return;
    // a character whose animations are mirrored keeps what the host's shows: ends come with it
    const bool mirrored = animTargets_.count(c) > 0;
    if (mirrored && (e.kind == kc::AnimKind::EndCombat || e.kind == kc::AnimKind::StopAction || e.kind == kc::AnimKind::EndStumble ||
                     e.kind == kc::AnimKind::State))
        return;
    AnimReplayScope scope;
    switch (e.kind) {
    case kc::AnimKind::Combat:
    case kc::AnimKind::CombatRun:
        if (void* t = kenshi::FindTechnique(e.name))
            kenshi::CallStartCombatAnim(kenshi::FnAddr(e.kind == kc::AnimKind::Combat ? kenshi::FnAnimStartCombat : kenshi::FnAnimRunCombat), c, t, e.a);
        break;
    case kc::AnimKind::EndCombat: kenshi::CallAnimVoid(kenshi::FnAddr(kenshi::FnAnimEndCombat), c); break;
    case kc::AnimKind::Action:
        if (void* a = kenshi::FindAnimData(c, e.name)) kenshi::CallPlayAction(c, a, e.a, e.b, (e.flags & 1) != 0);
        break;
    case kc::AnimKind::StopAction: kenshi::CallStopActionNamed(c, e.name); break;
    case kc::AnimKind::Stumble:
        if (void* a = kenshi::FindAnimData(c, e.name)) kenshi::CallStartStumble(c, a);
        break;
    case kc::AnimKind::EndStumble: kenshi::CallAnimVoid(kenshi::FnAddr(kenshi::FnAnimEndStumble), c); break;
    case kc::AnimKind::CombatMode: kenshi::CallSetCombatMode(c, (e.flags & 1) != 0); break;
    case kc::AnimKind::Carry: kenshi::CallSetCarryMode(c, (e.flags & 1) != 0, (e.flags & 2) != 0, (e.flags & 4) != 0); break;
    case kc::AnimKind::Floater:
    case kc::AnimKind::FloaterColor: {
        float col[4] = {1, 1, 1, 1};
        const std::string hex = e.name.substr(0, 8);
        for (int i = 0; i < 4 && hex.size() == 8; ++i) col[i] = float(std::strtoul(hex.substr(size_t(i) * 2, 2).c_str(), nullptr, 16)) / 255.0f;
        if (e.kind == kc::AnimKind::Floater) {
            const size_t bar = e.name.find('|');
            lastFloater_[c] = kenshi::ShowFloater(c, bar == std::string::npos ? "" : e.name.substr(bar + 1), col, e.flags & 0xF, e.flags >> 4);
        } else if (auto it = lastFloater_.find(c); it != lastFloater_.end()) {
            kenshi::SetLabelColor(it->second, col);
        }
        break;
    }
    case kc::AnimKind::GuardLegs: kenshi::CallSetGuard(c, true, (e.flags & 1) != 0); break;
    case kc::AnimKind::GuardUpper: kenshi::CallSetGuard(c, false, (e.flags & 1) != 0); break;
    case kc::AnimKind::DrawWeapon: {
        const size_t tab = e.name.find('\t');
        kenshi::CallDrawWeapon(c, e.name.substr(0, tab), tab == std::string::npos ? "" : e.name.substr(tab + 1));
        break;
    }
    case kc::AnimKind::Sheathe: kenshi::CallSheathe(c); break;
    case kc::AnimKind::WeaponState: {
        std::string sid, sec;
        const bool armed = kenshi::WeaponInHands(c, sid, sec);
        const std::string mine = armed ? sid + "\t" + sec : "";
        if (mine == e.name) break;
        if (armed) kenshi::CallSheathe(c);
        if (!e.name.empty()) {
            const size_t tab = e.name.find('\t');
            kenshi::CallDrawWeapon(c, e.name.substr(0, tab), tab == std::string::npos ? "" : e.name.substr(tab + 1));
        }
        break;
    }
    case kc::AnimKind::State: {   // heal whatever an event did not carry (joined late, action started before we knew it)
        kenshi::AnimModes m;
        if (!kenshi::ReadAnimModes(c, m)) break;
        if (m.action != e.name) {
            if (e.name.empty()) kenshi::CallStopActionNamed(c, "");
            else if (void* a = kenshi::FindAnimData(c, e.name)) kenshi::CallPlayAction(c, a, 1.0f, 0.0f, false);
        }
        const bool combat = (e.flags & 1) != 0, carried = (e.flags & 2) != 0, left = (e.flags & 4) != 0, right = (e.flags & 8) != 0;
        if (m.combat != combat) kenshi::CallSetCombatMode(c, combat);
        if (m.carried != carried || m.carryLeft != left || m.carryRight != right) kenshi::CallSetCarryMode(c, carried, left, right);
        if (m.guardLegs != ((e.flags & 16) != 0)) kenshi::CallSetGuard(c, true, (e.flags & 16) != 0);
        if (m.guardUpper != ((e.flags & 32) != 0)) kenshi::CallSetGuard(c, false, (e.flags & 32) != 0);
        break;
    }
    }
}

void KenshiWorld::ReadEffects(kc::EffectsMsg& out, bool full) {
    std::vector<kc::Vec3> centers;
    const double now = NowSeconds();
    const bool refresh = now - fxCentersAt_ >= 0.5;
    if (refresh) {
        fxCentersAt_ = now;
        for (auto& [h, c] : squad_) {
            kc::Vec3 p;
            if (kenshi::GetPosition(c, p)) centers.push_back(p);
        }
    }
    std::lock_guard<std::mutex> lk(weatherMutex_);
    if (refresh) fxCenters_.swap(centers);
    out = std::move(fxOut_);
    fxOut_ = kc::EffectsMsg{};
    if (!full) return;
    out.full = true;
    out.spawned.clear();
    out.spawned.reserve(fxLive_.size());
    for (const auto& [id, e] : fxLive_) out.spawned.push_back(e);
    std::sort(out.spawned.begin(), out.spawned.end(), [](const kc::WeatherEffect& a, const kc::WeatherEffect& b) { return a.id < b.id; });
}

void KenshiWorld::ApplyEffects(const kc::EffectsMsg& m) {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    if (!client_) return;
    const double now = NowSeconds();
    for (const auto& e : m.spawned) {
        if (clientFx_.count(e.id) || (e.kind == kc::EffectKind::Point && fxHandled_.count(e.id)) || !fxPending_.insert(e.id).second) continue;
        fxSpawn_[e.regionSid].push_back({e, now});
    }
    for (const auto& s : m.moved) fxMove_[s.id] = s;
    for (uint32_t id : m.ended) {
        if (clientFx_.count(id)) fxEnd_.insert(id);
        fxMove_.erase(id);
        if (fxPending_.erase(id))
            for (auto& [sid, list] : fxSpawn_)
                list.erase(std::remove_if(list.begin(), list.end(), [id](const PendingFx& p) { return p.e.id == id; }), list.end());
    }
    if (!m.full) return;
    fxLiveIds_.clear();
    for (const auto& e : m.spawned) fxLiveIds_.insert(e.id);
    ++fxFullGen_;
    for (auto it = fxHandled_.begin(); it != fxHandled_.end();) it = fxLiveIds_.count(*it) ? std::next(it) : fxHandled_.erase(it);
}

std::string KenshiWorld::WeatherGroups() {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    std::string out;
    for (const auto& [region, w] : seenRegions_) {
        std::vector<kenshi::EffectGroupInfo> groups;
        kenshi::ReadEffectGroups(region, groups);
        out += "region " + w.regionSid + (region == kenshi::CameraWeatherRegion() ? " camera" : "") + " season=" + w.seasonSid +
               " weather=" + w.weatherSid + " groups:";
        for (const auto& g : groups)
            out += std::string(" ") + (g.kind == kc::EffectKind::Point ? "point:" : "wander:") + g.effectSid + "x" + std::to_string(g.handlers.size());
        out += "\n" + kenshi::DescribeRegionWeathers(region);
    }
    return out;
}

size_t KenshiWorld::HurryEffects() {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    size_t n = 0;
    for (const auto& [region, w] : seenRegions_) {
        std::vector<kenshi::EffectGroupInfo> groups;
        if (!kenshi::ReadEffectGroups(region, groups)) continue;
        for (const auto& g : groups) {
            kenshi::HurryEffectSpawn(g.group);
            ++n;
        }
    }
    return n;
}

void KenshiWorld::ForceWeather(const std::string& regionSid, const std::string& seasonSid, const std::string& weatherSid) {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    forcedWeather_[regionSid] = {seasonSid, weatherSid};
}

size_t KenshiWorld::LiveEffects() {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    return client_ ? clientFx_.size() : fxLive_.size();
}

std::string KenshiWorld::EffectsReport() {
    std::lock_guard<std::mutex> lk(weatherMutex_);
    char head[320];
    snprintf(head, sizeof(head), "fxstats placed=%llu failed=%llu nogroup=%llu over=%llu stopped=%llu live=%zu corr=%llu corr_avg=%.2f corr_max=%.2f\n",
             (unsigned long long)fxStats_.placed, (unsigned long long)fxStats_.failed, (unsigned long long)fxStats_.noGroup,
             (unsigned long long)fxStats_.over, (unsigned long long)fxStats_.stopped, client_ ? clientFx_.size() : fxLive_.size(),
             (unsigned long long)fxStats_.corrections, fxStats_.corrections ? fxStats_.correctionSum / double(fxStats_.corrections) : 0.0,
             fxStats_.correctionMax);
    std::string out = head;
    for (const auto& [region, w] : seenRegions_) {
        std::vector<kenshi::EffectGroupInfo> groups;
        if (!kenshi::ReadEffectGroups(region, groups)) continue;
        for (const auto& g : groups) {
            for (void* h : g.handlers) {
                kc::WeatherEffect e;
                if (!kenshi::ReadEffect(h, g.kind, e)) continue;
                uint32_t id = 0;
                if (client_) {
                    for (const auto& [cid, c] : clientFx_)
                        if (c.handler == h) id = cid;
                } else {
                    auto k = hostFx_.find(region);
                    if (k != hostFx_.end()) {
                        auto f = k->second.find(h);
                        if (f != k->second.end() && f->second.sent) id = f->second.id;
                    }
                }
                char line[400];
                snprintf(line, sizeof(line), "fx %u %s %s %s %u %.1f,%.1f,%.1f age=%.2f life=%.2f endless=%d struck=%d shown=%d\n", id,
                         g.kind == kc::EffectKind::Point ? "point" : "wander", w.regionSid.c_str(), g.effectSid.c_str(), unsigned(g.ordinal),
                         e.pos.x, e.pos.y, e.pos.z, e.age, e.life, e.endless ? 1 : 0, e.struck ? 1 : 0, kenshi::EffectShown(h) ? 1 : 0);
                out += line;
            }
        }
    }
    return out;
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
        hostFx_.clear();
        fxLive_.clear();
        fxCenters_.clear();
        fxCentersAt_ = -1e9;
        fxStats_ = FxStats{};
        fxLogged_ = 0;
        fxOut_ = kc::EffectsMsg{};
        clientFx_.clear();
        fxSpawn_.clear();
        fxPending_.clear();
        fxHandled_.clear();
        fxMove_.clear();
        fxEnd_.clear();
        fxLiveIds_.clear();
        fxFullDone_.clear();
        fxStopped_.clear();
        fxRebuild_.clear();
        groundAlias_.clear();
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
