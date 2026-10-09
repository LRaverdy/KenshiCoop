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
    // A frame without the game's main loop (a zone loading after a long walk or a far teleport, a
    // hitch) also passes through "not live": the same characters at the same addresses afterwards
    // mean the same world, whose stand-ins and caches must survive. Any reload frees them all.
    void* player = kenshi::Player();
    const bool ready = player && !squad_.empty();
    if (ready && (!wasReady_ || player != lastPlayer_)) {
        std::vector<const void*> now;
        for (auto& [h, c] : squad_) now.push_back(c);
        std::sort(now.begin(), now.end());
        if (player != lastPlayer_ || now != lastSquadPtrs_) {
            ++generation_;
            ResetWorldBound();
            Log("new world (generation %u): every per-world state reset", generation_);
        }
        lastSquadPtrs_ = std::move(now);
    }
    wasReady_ = ready;
    lastPlayer_ = player;
}

// Everything that names objects of the loaded world (pointers, local handles, requests about
// them). After a reload (a resync, a load) they are gone: keeping any of it makes the next
// frame call the game on freed memory.
void KenshiWorld::ResetWorldBound() {
    alias_.clear();   // stand-ins belonged to the previous world
    pendingLoot_.clear();
    strangerSince_.clear();
    lastTarget_.clear();
    fallPrep_.clear();
    fellAt_.clear();
    lastDest_.clear();
    postureSince_.clear();
    postureFixed_.clear();
    carriedHere_.clear();
    handTools_.clear();
    applied_.clear();
    replicatedAt_.clear();
    animTargets_.clear();
    animLast_.clear();
    animCreateTried_.clear();
    lastFloater_.clear();
    edited_.clear();
    containerReqs_.clear();
    taskDropAt_.clear();
    stuck_.clear();
    farSnapAt_.clear();
    reRagdoll_.clear();
    syncErr_.clear();
    syncMaxErr_ = 0;
    remoteDialogs_.clear();
    turretCache_.clear();
    pickups_.clear();
    captiveHold_.clear();
    captiveMissing_.clear();
    resolved_.clear();
    { std::lock_guard<std::mutex> lk(groundMutex_); localDrops_.clear(); groundAlias_.clear(); }
    { std::lock_guard<std::mutex> lk(doorMutex_); doorReqs_.clear(); doorCache_.clear(); }
    { std::lock_guard<std::mutex> lk(tradeMutex_); tradeReqs_.clear(); hostTradeLooter_ = {}; hostTradeTrader_ = {}; }
    { std::lock_guard<std::mutex> lk(buildMutex_); trackedBuildings_.clear(); localPlacements_.clear(); localBuildActions_.clear(); removedBuildings_.clear(); }
    { std::lock_guard<std::mutex> lk(dialogMutex_); dialogEvents_.clear(); }
    kenshi::ResetLookupCaches();
    kenshi::ForgetGodModes();
}

void KenshiWorld::EndFrame() {
    // Clients run the host's clock: speed and pause are imposed every frame, so local keys
    // (space, F2/F3/F4) have no lasting effect.
    if (active_ && client_ && haveHostTime_) {
        // Same calls as the space bar / speed keys, so the game's own speed UI follows too.
        // Paused, the game commits no position written to a character: before pausing, a client
        // runs a few frames at a crawl (its clock barely moves: the game recomputes it from its own
        // counter and it cannot be set back) while Apply puts every character exactly where the
        // host's stopped.
        constexpr double kSettleBeforePause = 0.3;
        constexpr float kCrawl = 0.01f;
        const double now = NowSeconds();
        if (hostTime_.paused && !kenshi::GetPaused()) {
            if (pauseSeenAt_ < 0) {
                pauseSeenAt_ = now;
                kenshi::CallSetFrameSpeed(kCrawl);
            }
            // the game may refuse a pause for a moment (an editor, a menu): ask again until it takes
            if (now - pauseSeenAt_ >= kSettleBeforePause && now >= nextPauseTry_) {
                kenshi::CallUserPause(true);
                nextPauseTry_ = now + 0.5;
                if (!kenshi::GetPaused() && now - pauseSeenAt_ > 3.0 && !pauseRefusedLogged_) {
                    pauseRefusedLogged_ = true;
                    Log("the host paused but this game refuses to pause (an editor or a menu open?)");
                }
            }
        } else if (!hostTime_.paused) {
            pauseSeenAt_ = -1;
            pauseRefusedLogged_ = false;
            if (kenshi::GetPaused()) kenshi::CallUserPause(false);
            if (std::fabs(kenshi::GetFrameSpeed() - hostTime_.speed) > 1e-3f) kenshi::CallSetFrameSpeed(hostTime_.speed);
        }
    }
    // While players join, the host's world stays frozen even if someone presses unpause.
    if (holding_ && !kenshi::GetPaused()) {
        kenshi::CallUserPause(true);
        pausedByHold_ = true;
    }
    if (active_ && client_ && live_ && !pendingLoot_.empty()) UpdatePendingLoot();
    if (active_ && !client_ && live_ && !pickups_.empty()) UpdatePendingPickups();
    if (active_ && !client_ && live_ && !reRagdoll_.empty()) UpdateReRagdolls();
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
        // fighting and fallen characters play the host's combat and fall animations: their master
        // clock is still corrected, but not counted (the tests measure walking cycles)
        std::unordered_set<const void*> busy;
        for (const auto& [h, st] : lastTarget_)
            if (st.combatTarget || (st.flags & (kc::kFlagDown | kc::kFlagDead)))
                if (kenshi::Character* c = Find(h)) busy.insert(c);
        for (auto it = animTargets_.begin(); it != animTargets_.end();) {
            if (nowView - it->second->sampledAt > 1.0 || !v->replicated.count(it->first)) { it = animTargets_.erase(it); continue; }
            if (void* ac = kenshi::AnimationOf(it->first)) {
                v->anims[ac] = it->second;
                const auto& tg = *it->second;
                // the master clock is a phase (0..1, wrapping): corrected only when it is really off,
                // otherwise it keeps advancing smoothly on its own (jumping it to every sample made
                // walking characters shake, worse at higher game speeds)
                float want = tg.masterTime + tg.masterSpeed * float(nowView - tg.sampledAt) * v->gameSpeed;
                want -= std::floor(want);
                float mine = 0, mineSpeed = 0;
                float d = kenshi::ReadAnimMasterOf(ac, mine, mineSpeed) ? want - mine : 1.0f;
                d -= std::round(d);
                // far off: jump; a little off: ease a tenth of the way per frame (invisible); close: ours
                const float jump = std::min(0.35f, 0.15f * std::max(1.0f, v->gameSpeed));
                if (!busy.count(it->first)) {
                    ++masterChecks;
                    masterCorrections += std::fabs(d) > jump;
                }
                float t = mine;
                if (std::fabs(d) > jump) t = want;
                else if (std::fabs(d) > 0.01f) t = mine + d * 0.1f;
                t -= std::floor(t);
                kenshi::WriteAnimMaster(ac, t, tg.masterSpeed);
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
    if (it == squad_.end())   // client: the host's handle of a character that has another one here
        if (auto a = alias_.find(h); a != alias_.end()) it = squad_.find(a->second);
    return it == squad_.end() ? nullptr : it->second;
}

// Client: a character of ours changed squads here, and with it its handle: the host's handle for it
// now points to the new one.
void KenshiWorld::LocalRehandled(const kc::Handle& before, const kc::Handle& after) {
    if (before == after) return;
    bool found = false;
    for (auto& [host, local] : alias_)
        if (local == before) { local = after; found = true; }
    if (!found) alias_[before] = after;
    resolved_.clear();
    if (auto it = squad_.find(before); it != squad_.end()) { squad_[after] = it->second; squad_.erase(it); }
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
namespace {
std::string KeyOf(const kc::Handle& h) {
    char b[80];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}
} // namespace

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

float KenshiWorld::NearestSquadDistance(const kc::Vec3& p) {
    float best = 1e9f;
    kc::Vec3 q;
    for (const auto& [h, c] : squad_)
        if (kenshi::GetPosition(c, q)) best = std::min(best, Dist(p, q));
    return best;
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
        // The stand-in is gone (killed and cleaned up, or dropped with its zone by the local game):
        // the alias must go too, or Spawn and Reconcile would skip this character forever.
        if (!kenshi::Resolve(a->second)) {
            Log("stand-in %s for the host's %s is gone here: it can be recreated", KeyOf(a->second).c_str(), KeyOf(h).c_str());
            alias_.erase(a);
            resolved_.erase(h);
            return false;
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
        if (kenshi::InventoryWindowShows(s.c)) {
            kenshi::CloseInventoryWindows();
            Log("closed the inventory windows: the character they show is being removed");
        }
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
        if (kenshi::InventoryWindowShows(c)) {
            kenshi::CloseInventoryWindows();
            Log("closed the inventory windows: the character they show is being removed");
        }
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
    // The tasks it had in the save (sit on a stool, patrol, wander...) would keep running here and
    // fight the host's positions (characters jumping back and forth between two spots): it drops
    // them, the game's own way. Nothing gives it new ones on a client.
    if (kenshi::LocalTaskCount(c) > 0) {
        double& at = taskDropAt_[h];
        if (NowSeconds() - at > 2.0) {
            at = NowSeconds();
            HostCallScope scope;
            kenshi::DropLocalTasks(c);
        }
    }
    applied_.insert(c);
    // on someone's shoulder here: never pulled, stood up or teleported off it
    if (carriedHere_.count(h) && kenshi::IsRagdoll(c)) return;
    kc::EntityState& last = lastTarget_[h];
    last = target;
    last.flags = latest.flags;
    const double now = NowSeconds();
    if (CaptiveHold(h, c)) { lastDest_.erase(h); return; }   // lot D: the host keeps it in a cage
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
                const bool ok = kenshi::SetRagdoll(c, true);
                fixed = now;
                fellAt_[h] = now;
                Log("posture: %s falls as on the host (%s, ragdoll now %d)", KeyOf(h).c_str(), ok ? "ok" : "call failed", int(kenshi::IsRagdoll(c)));
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
        stuck_.erase(h);
        return;
    }

    const float err = Dist(local, target.pos);
    // Far from our squad (beyond what the player sees) the local game hardly runs a character and
    // ignores most positions written to it: its offset there is not a desync anyone sees, and it
    // put hundreds of units in the reports ("max offset" 300-466). Measured near our squad only.
    constexpr float kSeenRange = 300.0f;
    if (err > 5.0f && NearestSquadDistance(local) > kSeenRange) {
        syncErr_.erase(h);
    } else {
        syncMaxErr_ = std::max(syncMaxErr_, err);
        if (!(latest.flags & kc::kFlagMoving)) syncErr_[h] = err;
        else syncErr_.erase(h);
    }
    HostCallScope scope;
    // Far off (late join, lag spike, teleport on the host): snap straight to the host state.
    if (err > cfg_.snapDistance) {
        kenshi::Teleport(c, target.pos, target.rot);
        lastDest_.erase(h);
        return;
    }
    // Stuck: a wall, a closed door or another floor between our copy and the host's position (an
    // NPC shut in a house here while it fights outside on the host): neither the locomotion nor the
    // pull below gets it there. No progress for a second: put it exactly where the host has it.
    if (haveHostTime_ && hostTime_.paused) stuck_.erase(h);   // nothing moves while paused: start over afterwards
    // Stuck detection: off. In real fights it teleported characters dozens of times a second
    // (walls crossed, invisible or moonwalking NPCs, flying bodies); the continuous pull is enough.
    static bool kStuckDetection = false;   // (static: one place to turn it back on)
    if (kStuckDetection && !(haveHostTime_ && hostTime_.paused)) {
        const float stuckErr = 3.0f * std::max(1.0f, haveHostTime_ ? hostTime_.speed : 1.0f);
        Stuck& sk = stuck_[h];
        if (err > stuckErr) {
            if (sk.since == 0 || err < sk.bestErr * 0.8f) {
                sk.since = now;
                sk.bestErr = err;
            } else if (now - sk.since > 1.0) {
                kenshi::Teleport(c, target.pos, target.rot);
                lastDest_.erase(h);
                ++stuckFixes;
                Log("stuck: %s made no progress toward the host's position for 1 s (%.1f units off%s): put there", KeyOf(h).c_str(), err,
                    latest.combatTarget ? ", fighting" : "");
                sk = Stuck{};
                return;
            }
        } else {
            sk = Stuck{};
        }
        // Far from our squad the game moves a character only a few times per second and keeps its
        // own idea of where it is (writes do not stick): put it back on the host's position a few
        // times a second instead of pulling.
        if (err > 1.0f && NearestSquadDistance(local) > 300.0f) {
            double& at = farSnapAt_[h];
            if (now - at > 0.25) {
                at = now;
                kenshi::Teleport(c, target.pos, target.rot);
                lastDest_.erase(h);
                ++farSnaps;
            }
        }
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
    // It faces where the host's does: exactly when standing (in combat the combat movement hook
    // imposes it too); walking, our own path turns it, unless that path goes another way than the
    // host's (more than ~25 degrees off).
    {
        kc::Vec3 mine;
        const kc::Vec3 want = kenshi::ForwardOf(target.rot);
        const float limit = (!hostMoving || latest.combatTarget != 0) ? 0.999f : 0.9f;
        if (kenshi::GetFacing(c, mine) && mine.x * want.x + mine.z * want.z < limit) kenshi::FaceDirection(c, want);
    }
    // The host paused: its characters stopped mid-stride. Put them exactly there while the game still
    // runs (at a crawl, see EndFrame): paused, it would not commit the position.
    if (haveHostTime_ && hostTime_.paused) {
        if (kenshi::IsMoving(c)) kenshi::Halt(c);
        if (err > 0.02f) kenshi::Teleport(c, target.pos, target.rot);
        lastDest_.erase(h);
        return;
    }
    // ...while its position is continuously pulled onto the host's: no drift.
    if (err > 0.01f) {
        const float k = err > 2.0f ? 0.5f : 0.25f;
        kc::Vec3 d{target.pos.x - local.x, target.pos.y - local.y, target.pos.z - local.z};
        // Walking a little ahead of the host's point is normal (the game walks it at its own pace): pulling
        // it straight back would slide it backwards while it walks forwards (a "moonwalk"). Behind it,
        // only the sideways part is pulled at once; the part along the walk is caught up gently.
        if (hostMoving && !latest.combatTarget && err < 0.8f) {
            const kc::Vec3 f = kenshi::ForwardOf(target.rot);
            const float along = d.x * f.x + d.z * f.z;
            if (along < 0) {
                d.x -= f.x * along * 0.9f;
                d.z -= f.z * along * 0.9f;
            }
        }
        const kc::Vec3 p{local.x + d.x * k, local.y + d.y * k, local.z + d.z * k};
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

void KenshiWorld::NoteEdited(kenshi::Character* c) {
    if (std::find(edited_.begin(), edited_.end(), c) == edited_.end()) edited_.push_back(c);
}

void KenshiWorld::TakeEditedCharacters(std::vector<kc::Handle>& out) {
    out.clear();
    for (kenshi::Character* c : edited_) {
        kc::Handle h;
        if (kenshi::IsCharacter(c) && kenshi::GetHandle(c, h)) out.push_back(HostHandleOf(h));
    }
    edited_.clear();
}

bool KenshiWorld::ReadAppearance(const kc::Handle& h, kc::AppearanceMsg& out) {
    kenshi::Character* c = Find(h);
    return c && kenshi::ReadAppearance(c, out);
}

void KenshiWorld::ApplyAppearance(const kc::Handle& h, const kc::AppearanceMsg& m) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    HostCallScope scope;
    Log("appearance of %s applied: %s", m.name.c_str(), kenshi::WriteAppearance(c, m) ? "ok" : "failed");
}

bool KenshiWorld::OpenCharacterEditor(const kc::Handle& h) {
    kenshi::Character* c = Find(h);
    if (!c || !live_) return false;
    HostCallScope scope;
    const bool ok = kenshi::OpenCharacterEditor(c);
    Log("character editor opened: %s", ok ? "ok" : "failed");
    return ok;
}

void KenshiWorld::TakeSyncStats(float& maxErr, uint16_t& farOff) {
    maxErr = 0;
    farOff = 0;
    for (auto& [h, e] : syncErr_) {
        farOff += e > 5.0f;
        maxErr = std::max(maxErr, e);
    }
    syncMaxErr_ = 0;
    syncErr_.clear();
}

std::string KenshiWorld::TemplateName(const std::string& sid) {
    std::string name;
    return kenshi::TemplateDisplayName(sid, name) ? name : sid;
}

// Containers are furniture: found by kind and place (handles differ between machines).
bool KenshiWorld::FindContainer(const std::string& sid, const kc::Vec3& pos, kc::Handle& out) {
    std::vector<void*> around;
    kenshi::ObjectsNear(pos, 60.0f, around);
    void* best = nullptr;
    float bestD = 30.0f;
    for (void* o : around) {
        std::string s;
        kc::Vec3 p;
        std::vector<kc::ItemState> items;
        if (!kenshi::ObjectTemplate(o, s) || s != sid || !kenshi::ObjectPosition(o, p) || !kenshi::ReadInventory(o, items)) continue;
        const float d = Dist(p, pos);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best && kenshi::ObjectHandle(best, out);
}

bool KenshiWorld::ContainerKind(const kc::Handle& container, std::string& sid) {
    void* o = kenshi::ResolveObject(container);
    return o && kenshi::ObjectTemplate(o, sid);
}

float KenshiWorld::DistanceTo(const kc::Handle& who, const kc::Vec3& pos) {
    kenshi::Character* c = Find(who);
    kc::Vec3 p;
    return c && kenshi::GetPosition(c, p) ? Dist(p, pos) : 1e9f;
}

int KenshiWorld::TheftCheck(const kc::Handle& thief, const kc::Handle& container, const kc::ItemState& item) {
    kenshi::Character* c = Find(thief);
    void* cont = kenshi::ResolveObject(container);
    void* it = cont ? kenshi::FindItemIn(cont, item) : nullptr;
    if (!c || !it) return 0;
    HostCallScope scope;
    return kenshi::StealCheck(c, cont, it);
}

void KenshiWorld::QueueContainerRequest(kenshi::Character* looter, void* container) {
    ContainerRequest r;
    if (!kenshi::GetHandle(looter, r.looter) || !kenshi::ObjectTemplate(container, r.sid) || !kenshi::ObjectPosition(container, r.pos)) return;
    r.looter = HostHandleOf(r.looter);
    if (containerReqs_.size() < 8) containerReqs_.push_back(r);
}

void KenshiWorld::TakeContainerRequests(std::vector<ContainerRequest>& out) {
    out.swap(containerReqs_);
    containerReqs_.clear();
}

void KenshiWorld::QueueTradeRequest(const kc::Handle& looter, const kc::Handle& trader) {
    std::lock_guard<std::mutex> lk(tradeMutex_);
    if (tradeReqs_.size() < 8) tradeReqs_.push_back({looter, trader});
}

void KenshiWorld::NoteHostTradeWindow(const kc::Handle& looter, const kc::Handle& trader) {
    std::lock_guard<std::mutex> lk(tradeMutex_);
    hostTradeLooter_ = looter;
    hostTradeTrader_ = trader;
}

void KenshiWorld::TakeTradeRequests(std::vector<TradeRequest>& out) {
    std::lock_guard<std::mutex> lk(tradeMutex_);
    out.swap(tradeReqs_);
    tradeReqs_.clear();
}

bool KenshiWorld::ShopCounters(const kc::Handle& trader, std::vector<ShopCounter>& out) {
    out.clear();
    std::vector<void*> found;
    if (!kenshi::ShopCounters(Find(trader), found)) return false;
    for (void* f : found) {
        ShopCounter c;
        if (kenshi::ObjectHandle(f, c.handle) && kenshi::ObjectTemplate(f, c.sid) && kenshi::ObjectPosition(f, c.pos)) out.push_back(std::move(c));
    }
    return !out.empty();
}

bool KenshiWorld::MoneyOf(const kc::Handle& who, int32_t& money) { return kenshi::MoneyOf(Find(who), money); }

void KenshiWorld::SetMoneyOf(const kc::Handle& who, int32_t money) { kenshi::SetMoneyOf(Find(who), money); }

// The price of a purchase: the buyer's cats (the player faction's) go to the merchant; a sale
// (negative price) the other way. Checked by the caller: whoever pays can.
bool KenshiWorld::PayTrade(const kc::Handle& buyer, const kc::Handle& trader, int32_t price) {
    kenshi::Character* b = Find(buyer);
    kenshi::Character* t = Find(trader);
    if (!b || !t || price == 0) return price == 0;
    HostCallScope scope;
    kenshi::Character* payer = price > 0 ? b : t;
    kenshi::Character* payee = price > 0 ? t : b;
    const int32_t amount = price > 0 ? price : -price;
    if (!kenshi::TakeMoney(payer, amount)) return false;
    kenshi::TakeMoney(payee, -amount);
    return true;
}

// Our own trade window on that merchant shows a copy of its stock made when it opened: after
// another player bought or sold there, open it again (the game rebuilds the copy).
void KenshiWorld::RefreshTradeWindow(const kc::Handle& trader) {
    kc::Handle looter, open;
    {
        std::lock_guard<std::mutex> lk(tradeMutex_);
        looter = hostTradeLooter_;
        open = hostTradeTrader_;
    }
    kenshi::Character* t = Find(trader);
    if (!t || open != trader || kenshi::NpcTrader() != t || kenshi::MouseHoldsItem()) return;
    kenshi::Character* me = Find(looter);
    if (me && kenshi::OpenTradeWindow(me, t)) Log("trade: our own window on that merchant shows its new stock");
}

bool KenshiWorld::OpenTradeWindow(const kc::Handle& looter, const kc::Handle& trader) {
    return kenshi::OpenTradeWindow(Find(looter), Find(trader));
}

std::string KenshiWorld::CharacterNameOf(const kc::Handle& h) {
    std::string name;
    kenshi::Character* c = Find(h);
    if (c && kenshi::CharacterName(c, name) && !name.empty()) return name;
    return "a merchant";
}

bool KenshiWorld::OpenContainerWindow(const kc::Handle& looter, const kc::Handle& container) {
    kenshi::Character* me = Find(looter);
    void* cont = kenshi::ResolveObject(container);
    return me && cont && kenshi::OpenLootWindow(me, cont);
}

void KenshiWorld::ReadSquads(std::vector<WorldSquad>& out) {
    out.clear();
    std::vector<kenshi::Character*> all;
    kenshi::PlayerCharacters(all);
    std::vector<void*> order;   // squads in the order their first member appears in the player's list
    for (kenshi::Character* c : all) {
        void* sq = kenshi::SquadOf(c);
        if (sq && std::find(order.begin(), order.end(), sq) == order.end()) order.push_back(sq);
    }
    for (void* sq : order) {
        WorldSquad s;
        kenshi::SquadName(sq, s.name);
        std::vector<kenshi::Character*> members;
        kenshi::SquadMembers(sq, members);
        for (kenshi::Character* c : members) {
            kc::Handle h;
            if (kenshi::GetHandle(c, h)) s.members.push_back(HostHandleOf(h));
        }
        if (!s.members.empty()) out.push_back(std::move(s));
    }
}

// Clients: each host squad takes the local squad that already holds most of its members (or a new
// one), then every member goes there, in the host's order.
void KenshiWorld::ApplySquads(const std::vector<WorldSquad>& squads) {
    std::vector<void*> taken;
    int created = 0;
    HostCallScope scope;
    for (const auto& s : squads) {
        std::vector<kenshi::Character*> members;
        for (const auto& h : s.members)
            if (kenshi::Character* c = FindSquad(h)) members.push_back(c);
        if (members.empty()) continue;
        std::unordered_map<void*, int> count;
        for (kenshi::Character* c : members) if (void* sq = kenshi::SquadOf(c)) ++count[sq];
        void* target = nullptr;
        int best = 0;
        for (auto& [sq, n] : count)
            if (n > best && std::find(taken.begin(), taken.end(), sq) == taken.end()) { best = n; target = sq; }
        if (!target) {
            if (created >= 4) continue;   // never a flood of empty squads, whatever goes wrong
            target = kenshi::NewSquad();
            if (!target) continue;
            ++created;
            Log("squads: a new squad for '%s'", s.name.c_str());
        }
        taken.push_back(target);
        for (size_t i = 0; i < members.size(); ++i) {
            if (kenshi::SquadOf(members[i]) == target && kenshi::SquadMemberIndex(members[i]) == int(i)) continue;
            kc::Handle before, after;
            kenshi::GetHandle(members[i], before);
            kenshi::MoveToSquad(target, members[i], int(i));
            if (kenshi::GetHandle(members[i], after)) LocalRehandled(before, after);
        }
        kenshi::SetSquadName(target, s.name);
    }
    // The squad bar shows one of our squads, not an empty one the game switched to.
    std::vector<kenshi::Character*> shown;
    kenshi::SquadMembers(kenshi::ShownSquad(), shown);
    if (shown.empty())
        for (const auto& h : controllable_)
            if (kenshi::Character* c = FindSquad(h)) { kenshi::ShowSquad(kenshi::SquadOf(c)); break; }
}

bool KenshiWorld::ReadCarry(const kc::Handle& h, kc::Handle& carried) {
    kenshi::Character* c = Find(h);
    if (!c || !kenshi::ReadCarried(c, carried)) return false;
    carried = HostHandleOf(carried);
    return true;
}

void KenshiWorld::ApplyCarry(const kc::Handle& h, bool carry, const kc::Handle& carried) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kc::Handle local;
    const bool carrying = kenshi::ReadCarried(c, local);
    kenshi::Character* who = carry ? Find(carried) : nullptr;
    HostCallScope scope;
    if (carrying && (!carry || !who || kenshi::Resolve(local) != who)) {
        kenshi::DropCarried(c);
        Log("carry: %s puts a body down as on the host", carry ? "swaps and" : "");
    }
    if (carry && who) carriedHere_.insert(carried);
    else if (carrying) carriedHere_.erase(HostHandleOf(local));
    if (carry && who && (!carrying || kenshi::Resolve(local) != who)) {
        const bool ok = kenshi::CarryCharacter(c, who);
        Log("carry: a body goes on the shoulder as on the host (%s)", ok ? "ok" : "failed");
    }
}

bool KenshiWorld::ReadTool(const kc::Handle& h, std::string& sid) {
    sid.clear();
    void* tool = kenshi::JobTool(Find(h));
    if (tool) kenshi::ObjectTemplate(tool, sid);
    return true;
}

// The host's character holds a tool for its job (a pickaxe while mining): the same one in the same
// hand here, taken away when the host's is.
void KenshiWorld::ApplyTool(const kc::Handle& h, const std::string& sid) {
    kenshi::Character* c = Find(h);
    auto it = handTools_.find(h);
    if (it != handTools_.end() && (it->second.who != c || !c)) { handTools_.erase(it); it = handTools_.end(); }   // another body now
    const std::string have = it == handTools_.end() ? std::string{} : it->second.sid;
    if (!c || have == sid) return;
    HostCallScope scope;
    void* now = kenshi::SetHandTool(c, it == handTools_.end() ? nullptr : it->second.item, sid);
    if (sid.empty() || !now) handTools_.erase(h);
    else handTools_[h] = {sid, now, c};
    Log("tool: %s %s", KeyOf(h).c_str(), sid.empty() ? "puts its tool away" : ("holds " + TemplateName(sid) + (now ? "" : " (failed)")).c_str());
}

// ---- fix G6: floors
bool KenshiWorld::ReadFloor(const kc::Handle& h, uint8_t& group) {
    int32_t g = 0;
    kenshi::Character* c = Find(h);
    if (!c || !kenshi::ReadFloorGroup(c, g) || g < 0 || g > 255) return false;
    group = uint8_t(g);
    return true;
}

void KenshiWorld::ApplyFloor(const kc::Handle& h, uint8_t group) {
    kenshi::Character* c = Find(h);
    int32_t g = 0;
    if (!c || !kenshi::ReadFloorGroup(c, g) || g == group) return;
    if (kenshi::WriteFloorGroup(c, group)) Log("floor: %s now on floor group %d (was %d), as on the host", KeyOf(h).c_str(), int(group), int(g));
}

int KenshiWorld::TeleportCharacters(const std::vector<kc::Handle>& who, const kc::Vec3& to) {
    int n = 0;
    HostCallScope scope;
    std::vector<kenshi::Character*> all;
    for (const auto& h : who) {
        kenshi::Character* c = FindSquad(h);
        kc::Quat rot;
        if (!c || !kenshi::GetRotation(c, rot)) continue;
        // on someone's shoulder: put down first (the carrier stays where it is)
        if (all.empty()) kenshi::ActiveCharacters(all);
        for (kenshi::Character* other : all) {
            kc::Handle carried;
            if (other != c && kenshi::ReadCarried(other, carried) && carried == h) {
                kenshi::DropCarried(other);
                Log("tp: %s was carried: put down first", KeyOf(h).c_str());
                break;
            }
        }
        // the engine does not move an active ragdoll: the body gets up, moves, and lies down again
        // a moment later (a ragdoll started right after a teleport would be thrown away)
        const bool body = kenshi::IsRagdoll(c);
        if (body) kenshi::SetRagdoll(c, false);
        const kc::Vec3 p{to.x + 6.0f * float(n + 1), to.y + 2.0f, to.z + 4.0f};
        if (kenshi::Teleport(c, p, rot)) {
            ++n;
            if (body) reRagdoll_.push_back({h, NowSeconds() + 0.5});
            Log("tp: %s moved%s", KeyOf(h).c_str(), body ? " (it was lying on the ground)" : "");
        }
    }
    return n;
}

void KenshiWorld::UpdateReRagdolls() {
    const double now = NowSeconds();
    HostCallScope scope;
    for (auto it = reRagdoll_.begin(); it != reRagdoll_.end();) {
        if (now < it->at) { ++it; continue; }
        kenshi::Character* c = FindSquad(it->h);
        if (c && (kenshi::IsUnconscious(c) || kenshi::IsDead(c)) && !kenshi::IsRagdoll(c)) kenshi::SetRagdoll(c, true);
        it = reRagdoll_.erase(it);
    }
}

bool KenshiWorld::ReadProgress(const kc::Handle& h, std::vector<float>& stats, uint16_t& modes, uint8_t& style) {
    kenshi::Character* c = Find(h);
    if (!c || !kenshi::ReadStats(c, stats)) return false;
    modes = kenshi::ReadModes(c, style);
    return true;
}

// Skill levels as values; standing orders (stealth, hold position, passive...) through the game's
// own function, so that walking crouched and the squad bar's toggles follow.
void KenshiWorld::ApplyProgress(const kc::Handle& h, const std::vector<float>& stats, uint16_t modes, uint8_t style) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kenshi::WriteStats(c, stats);
    uint8_t localStyle = 0;
    const uint16_t local = kenshi::ReadModes(c, localStyle);
    HostCallScope scope;
    if ((local ^ modes) & kc::kModeStealth) kenshi::SetStandingOrder(c, (modes & kc::kModeStealth) ? 3 : 4, true);
    struct Toggle { uint16_t bit; int order; };
    static const Toggle toggles[] = {{kc::kModeDefensive, 11}, {kc::kModeRanged, 17}, {kc::kModeTaunt, 14}, {kc::kModeHold, 12}, {kc::kModePassive, 13}, {kc::kModeChase, 15}};
    for (const auto& t : toggles)
        if ((local ^ modes) & t.bit) kenshi::SetStandingOrder(c, t.order, (modes & t.bit) != 0);
    if (localStyle != style && style <= 2) kenshi::SetStandingOrder(c, 5 + style, true);   // AGG, DEF, EVADE
}

void KenshiWorld::ApplyVitals(const kc::Handle& h, const kc::EntityVitals& v) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kenshi::WriteVitals(c, v);   // values only: clients never fall over on their own (see hk_ragdollMode)
    // Death and knockout are transitions, not values: replay them exactly when the host has them,
    // where the host's character collapsed.
    const bool dies = (v.flags & kc::kVitDead) && !kenshi::IsDead(c);
    const bool faints = !dies && (v.flags & kc::kVitUnconscious) && !kenshi::IsUnconscious(c) && !kenshi::IsDead(c);
    if (faints && CaptiveHold(h, c)) { kenshi::SetUnconscious(c, true); return; }   // lot D: in its cage, no fall
    // Already on the ground: only the state is missing. Sleep, hunger and blood loss knock out
    // without a timer, and what keeps them going on the host (its AI tasks) does not run here, so
    // the local medical update would wake them at once: impose the host's state.
    // In a bed or a cage (sleep knocks out too), or carried (on a shoulder: IsRagdoll): the state
    // only, never a fall that would throw it out of there.
    int inside = 0;
    if (faints && (kenshi::IsRagdoll(c) || kenshi::IsDown(c) || (kenshi::ReadInSomething(c, inside) && inside != 0))) {
        kenshi::SetUnconscious(c, true);
        return;
    }
    if (!dies && !faints) return;
    auto at = lastTarget_.find(h);
    const double now = NowSeconds();
    if (!kenshi::IsRagdoll(c) && at != lastTarget_.end() && !ReadyToFall(h, c, at->second, now)) return;
    HostCallScope scope;
    if (!kenshi::IsRagdoll(c)) fellAt_[h] = now;
    if (dies) {
        kenshi::CallDeclareDead(c);
    } else {
        // knockout() only arms a timer, which the host's values replace at once (blood loss and
        // hunger knock out with no timer at all), and clients never collapse on their own: the
        // state itself, then the fall
        kenshi::SetUnconscious(c, true);
        kenshi::SetRagdoll(c, true);
    }
    Log("vitals: %s %s as on the host (now down=%d unconscious=%d dead=%d)", KeyOf(h).c_str(), dies ? "dies" : "faints",
        int(kenshi::IsRagdoll(c)), int(kenshi::IsUnconscious(c)), int(kenshi::IsDead(c)));
}

bool KenshiWorld::Order(const kc::Handle& h, const kc::Command& cmd) {
    kenshi::Character* c = FindSquad(h);
    if (!c) return false;
    // A client's order only ever moves that client's characters: never one the host commands (a
    // stale handle after a squad change once made a player's orders move the host's character).
    if (!client_) {
        auto v = View();
        if (!v->squadForeign.count(c)) {
            Log("client order refused: the character it resolves to is not another player's");
            return false;
        }
    }
    HostCallScope scope;   // lets the call through our own order-blocking hooks
    switch (cmd.kind) {
    case kc::CommandKind::MoveTo: return CallPlayerMoveOrder(c, cmd.pos);
    case kc::CommandKind::Stop: {
        // the client's "stop": also ends the task it was doing (follow, operate...), not just the walk
        const bool ok = kenshi::Halt(c);
        kenshi::DropLocalTasks(c);
        Log("client stop: character halted and its current task dropped");
        return ok;
    }
    case kc::CommandKind::PickUp: {
        // the item the client meant: same thing lying there
        std::vector<void*> around;
        kenshi::GroundItemsNear(cmd.pos, 30.0f, around);
        void* item = nullptr;
        float best = 15.0f;
        kc::Handle itemHandle;
        for (void* it : around) {
            kc::Handle ih;
            kc::ItemState st;
            kc::Vec3 p;
            if (!kenshi::DescribeGroundItem(it, ih, st, p) || st.templateSid != cmd.itemSid) continue;
            const float d = Dist(p, cmd.pos);
            if (d < best) { best = d; item = it; itemHandle = ih; }
        }
        if (!item) return false;
        pickups_[h] = {itemHandle, NowSeconds() + 30.0};
        return CallPlayerMoveOrder(c, cmd.pos);
    }
    case kc::CommandKind::SquadMove: {
        kenshi::Character* other = cmd.subject.valid() ? kenshi::Resolve(cmd.subject) : nullptr;
        void* target = other ? kenshi::SquadOf(other) : kenshi::NewSquad();
        const bool ok = kenshi::MoveToSquad(target, c, cmd.task);
        Log("client squad change run for a character: %s", ok ? "ok" : "failed");
        return ok;
    }
    case kc::CommandKind::Task: {
        // town furniture, machines and houses get other handles on every machine: the same kind
        // of object at the same place
        auto byKindAndPlace = [](const std::string& sid, const kc::Vec3& at) -> void* {
            std::vector<void*> around;
            kenshi::ObjectsNear(at, 60.0f, around);
            void* found = nullptr;
            float best = 40.0f;
            for (void* o : around) {
                std::string s;
                kc::Vec3 p;
                if (!kenshi::ObjectTemplate(o, s) || s != sid || !kenshi::ObjectPosition(o, p)) continue;
                const float d = Dist(p, at);
                if (d < best) { best = d; found = o; }
            }
            return found;
        };
        // a handle resolved here may name another object than on the client: it must be of the
        // kind the client named, where the client saw it
        auto sameThing = [](void* o, const std::string& sid, const kc::Vec3& at) {
            if (!o || sid.empty() || kenshi::IsCharacter(o)) return o != nullptr;
            std::string s;
            kc::Vec3 p;
            return kenshi::ObjectTemplate(o, s) && s == sid && kenshi::ObjectPosition(o, p) && Dist(p, at) < 40.0f;
        };
        void* subject = kenshi::ResolveObject(cmd.subject);
        void* building = kenshi::ResolveObject(cmd.building);
        if (subject && !sameThing(subject, cmd.itemSid, cmd.subjectPos)) {
            Log("client task %d: its subject handle names another object here", cmd.task);
            subject = nullptr;
        }
        if (building && !sameThing(building, cmd.buildingSid, cmd.buildingPos)) building = nullptr;
        if (cmd.subject.valid() && !subject && !cmd.itemSid.empty()) {
            subject = byKindAndPlace(cmd.itemSid, cmd.subjectPos);
            Log("client task %d: subject %sfound by kind and place", cmd.task, subject ? "" : "NOT ");
        }
        if (cmd.building.valid() && !building && !cmd.buildingSid.empty()) {
            building = byKindAndPlace(cmd.buildingSid, cmd.buildingPos);
            Log("client task %d: destination building %sfound by kind and place", cmd.task, building ? "" : "NOT ");
        }
        if (cmd.subject.valid() && !subject) Log("client task %d: its subject is not in the host's world", cmd.task);
        const bool ok = RunPlayerTask(c, cmd, subject, building);
        Log("client task %d (via %d) run for a character: %s", cmd.task, int(cmd.via), ok ? "ok" : "failed");
        return ok;
    }
    }
    return false;
}

// ---- conversations
void KenshiWorld::NoteSay(void* dialogue, const std::string& text) {
    kenshi::Character* me = kenshi::DialogueOwner(dialogue);
    kc::Handle h;
    if (!me || !kenshi::GetHandle(me, h)) return;
    WorldDialog d;
    d.kind = kc::DialogKind::Say;
    d.speaker = h;
    d.text = text;
    d.shout = kenshi::DialogueShouting(dialogue);
    std::lock_guard<std::mutex> lk(dialogMutex_);
    if (dialogEvents_.size() < 512) dialogEvents_.push_back(std::move(d));
}

bool KenshiWorld::RemoteDialogParties(void* dialogue, kenshi::Character*& pc, kenshi::Character*& other) {
    auto v = View();
    kenshi::Character* me = kenshi::DialogueOwner(dialogue);
    kenshi::Character* target = kenshi::DialogueTarget(dialogue);
    if (me && v->squadForeign.count(me)) { pc = me; other = target; return true; }
    if (target && v->squadForeign.count(target)) { pc = target; other = me; return true; }
    return false;
}

bool KenshiWorld::NoteDialogWindow(void* dialogue, bool open) {
    std::lock_guard<std::mutex> lk(dialogMutex_);
    auto known = remoteDialogs_.find(dialogue);
    kenshi::Character* pc = nullptr;
    kenshi::Character* other = nullptr;
    const bool remote = RemoteDialogParties(dialogue, pc, other);
    if (!remote && known == remoteDialogs_.end()) return false;
    WorldDialog d;
    d.kind = open ? kc::DialogKind::Open : kc::DialogKind::Close;
    if (pc) kenshi::GetHandle(pc, d.pc);
    if (other) {
        kenshi::GetHandle(other, d.speaker);
        kenshi::CharacterName(other, d.text);
    }
    if (open) {
        if (known == remoteDialogs_.end()) known = remoteDialogs_.emplace(dialogue, nextDialogId_++).first;
        d.dialogId = known->second;
        Log("conversation %u opened for another player's character", d.dialogId);
    } else {
        if (known == remoteDialogs_.end()) return true;
        d.dialogId = known->second;
        remoteDialogs_.erase(known);
        Log("conversation %u closed", d.dialogId);
    }
    dialogEvents_.push_back(std::move(d));
    return true;
}

bool KenshiWorld::NoteDialogText(void* dialogue) {
    std::lock_guard<std::mutex> lk(dialogMutex_);
    auto known = remoteDialogs_.find(dialogue);
    kenshi::Character* pc = nullptr;
    kenshi::Character* other = nullptr;
    if (known == remoteDialogs_.end()) {
        if (!RemoteDialogParties(dialogue, pc, other)) return false;
        known = remoteDialogs_.emplace(dialogue, nextDialogId_++).first;   // text before the window: open it now
    } else {
        RemoteDialogParties(dialogue, pc, other);
    }
    WorldDialog d;
    d.kind = kc::DialogKind::Text;
    d.dialogId = known->second;
    if (pc) kenshi::GetHandle(pc, d.pc);
    if (other) kenshi::GetHandle(other, d.speaker);
    kenshi::ReadDialogueWindowText(dialogue, d.text, d.replies);
    dialogEvents_.push_back(std::move(d));
    return true;
}

void KenshiWorld::TakeDialogEvents(std::vector<WorldDialog>& out) {
    std::lock_guard<std::mutex> lk(dialogMutex_);
    out.swap(dialogEvents_);
    dialogEvents_.clear();
}

void KenshiWorld::ApplySay(const kc::Handle& speaker, const std::string& text, bool shout) {
    if (kenshi::Character* c = Find(speaker); c && ReplaySay(c, text, shout)) {
        ++saysApplied;
        lastSay = text;
    }
}

void KenshiWorld::DialogAnswer(uint32_t dialogId, int index) {
    void* dialogue = nullptr;
    {
        std::lock_guard<std::mutex> lk(dialogMutex_);
        for (const auto& [d, id] : remoteDialogs_)
            if (id == dialogId) { dialogue = d; break; }
    }
    if (!dialogue) return;
    Log("conversation %u: the player answered %d (%s)", dialogId, index, CallReplyClicked(dialogue, index) ? "ok" : "failed");
}

void KenshiWorld::QueueLocalDrop(kenshi::Character* c, void* item) {
    kc::ItemState s;
    if (!kenshi::DescribeInventoryItem(item, s)) return;
    std::lock_guard<std::mutex> lk(groundMutex_);
    if (localDrops_.size() < 256) localDrops_.emplace_back(c, s);
}

void KenshiWorld::TakeLocalDrops(std::vector<std::pair<kc::Handle, kc::ItemState>>& out) {
    out.clear();
    std::vector<std::pair<kenshi::Character*, kc::ItemState>> raw;
    {
        std::lock_guard<std::mutex> lk(groundMutex_);
        raw.swap(localDrops_);
    }
    for (auto& [c, s] : raw) {
        kc::Handle h;
        if (kenshi::IsCharacter(c) && kenshi::GetHandle(c, h) && h.valid()) out.emplace_back(h, std::move(s));
    }
}

void KenshiWorld::UpdatePendingPickups() {
    constexpr float kReach = 15.0f;
    const double now = NowSeconds();
    for (auto it = pickups_.begin(); it != pickups_.end();) {
        kenshi::Character* c = FindSquad(it->first);
        void* item = kenshi::ResolveItem(it->second.item);
        kc::Vec3 cp, ip;
        kc::Handle ih;
        kc::ItemState st;
        if (!c || !item || !kenshi::ItemOnGround(item) || now > it->second.until || !kenshi::DescribeGroundItem(item, ih, st, ip)) {
            it = pickups_.erase(it);
            continue;
        }
        if (kenshi::GetPosition(c, cp) && Dist(cp, ip) < kReach) {
            HostCallScope scope;
            kenshi::CallGiveItem(c, item);
            it = pickups_.erase(it);
            continue;
        }
        ++it;
    }
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
    for (auto& [h, c] : out) {   // orders name our characters (and what they act on) as the host knows them
        h = HostHandleOf(h);
        if ((c.kind != kc::CommandKind::Task && c.kind != kc::CommandKind::SquadMove) || !c.subject.valid()) continue;
        c.subject = HostHandleOf(c.subject);
        for (const auto& [host, local] : groundAlias_)
            if (local == c.subject) { c.subject = host; break; }
    }
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
        if (!item || !kenshi::ItemOnGround(item)) {   // an item from the save: same thing at the same spot
            std::vector<void*> around;
            kenshi::GroundItemsNear(e.pos, 30.0f, around);
            float best = 15.0f;
            for (void* it : around) {
                kc::Handle ih;
                kc::ItemState st;
                kc::Vec3 p;
                if (!kenshi::DescribeGroundItem(it, ih, st, p) || st.templateSid != e.state.templateSid) continue;
                const float d = Dist(p, e.pos);
                if (d < best) { best = d; item = it; }
            }
        }
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
        kenshi::WriteSingleAnim(m.single, kenshi::SyncedAnimTime(m.single, m.time, want, match->looped, kenshi::GetFrameSpeed()), match->speed, match->weight,
                                match->desired);
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

// The object holding an inventory: one of the characters, or a container (furniture).
void* KenshiWorld::InventoryHolder(const kc::Handle& h) {
    if (kenshi::Character* c = Find(h)) return c;
    return h.type == 0 ? kenshi::ResolveObject(h) : nullptr;   // type 0: a building
}

bool KenshiWorld::ReadInventory(const kc::Handle& h, std::vector<kc::ItemState>& out) {
    void* c = InventoryHolder(h);
    return c && kenshi::ReadInventory(c, out);
}

bool KenshiWorld::ExecuteInvOp(const kc::Handle& from, const kc::Handle& to, const kc::InvOp& op) {
    void* a = InventoryHolder(from);
    void* b = InventoryHolder(to);
    if (!a || !b) return false;
    std::string err;
    HostCallScope scope;
    if (kenshi::MoveInventoryItem(a, b, op, &err)) {
        Log("client item move done: %s %s -> %s %d,%d", op.item.templateSid.c_str(), op.item.section.c_str(), op.toSection.c_str(), op.toX, op.toY);
        return true;
    }
    Log("client item move refused: %s (%s x%d)", err.c_str(), op.item.templateSid.c_str(), op.item.quantity);
    return false;
}

bool KenshiWorld::ApplyInventory(const kc::Handle& h, const std::vector<kc::ItemState>& items) {
    void* c = InventoryHolder(h);
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
        Toast("Un joueur arrive : la partie est en pause jusqu'à son arrivée.");
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

namespace {
// Which character each Steam account plays, kept next to the game (one line per account:
// "<steamId> <character handle> <name>"). Handles are the save's own object ids: the same
// character keeps its handle from one save to the next.
std::filesystem::path PlayersLedger() { return std::filesystem::path(GameDir()) / L"KenshiCoop-players.txt"; }
struct LedgerEntry { uint64_t steamId = 0; kc::Handle handle; std::string name; };
std::vector<LedgerEntry> ReadLedger() {
    std::vector<LedgerEntry> out;
    std::ifstream f(PlayersLedger());
    std::string line;
    while (std::getline(f, line)) {
        LedgerEntry e;
        char key[96] = {}, name[64] = {};
        unsigned long long id = 0;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (sscanf(line.c_str(), "%llu %95s %63[^\n]", &id, key, name) < 2) continue;
        if (sscanf(key, "%u:%u:%u:%u:%u", &e.handle.type, &e.handle.container, &e.handle.containerSerial, &e.handle.index, &e.handle.serial) != 5) continue;
        e.steamId = id;
        e.name = name;
        out.push_back(e);
    }
    return out;
}
void WriteLedger(const std::vector<LedgerEntry>& entries) {
    std::ofstream f(PlayersLedger(), std::ios::trunc);
    for (const auto& e : entries)
        f << e.steamId << ' ' << e.handle.type << ':' << e.handle.container << ':' << e.handle.containerSerial << ':' << e.handle.index << ':'
          << e.handle.serial << ' ' << e.name << '\n';
}
} // namespace

bool KenshiWorld::EnsurePlayerCharacter(const std::string& playerName, uint64_t steamId, kc::Handle& out, bool& created) {
    created = false;
    std::string name = playerName.substr(0, 15);   // what fits a character name here
    std::vector<kenshi::Character*> squad;
    kenshi::PlayerCharacters(squad);
    if (squad.empty()) return false;
    auto ledger = ReadLedger();
    auto inSquad = [&](const kc::Handle& h) {
        for (kenshi::Character* c : squad) { kc::Handle sh; if (kenshi::GetHandle(c, sh) && sh == h) return c; }
        return static_cast<kenshi::Character*>(nullptr);
    };
    // 1. the character this Steam account played last time (whatever name the player uses now)
    if (steamId)
        for (const auto& e : ledger)
            if (e.steamId == steamId && inSquad(e.handle)) {
                out = e.handle;
                Log("%s is back with their character", name.c_str());
                return true;
            }
    // 2. a squad member with the player's name that no other account owns
    auto ownedByOther = [&](const kc::Handle& h) {
        for (const auto& e : ledger) if (e.handle == h && e.steamId != steamId) return true;
        return false;
    };
    for (kenshi::Character* c : squad) {
        std::string n;
        kc::Handle h;
        if (kenshi::CharacterName(c, n) && n == name && kenshi::GetHandle(c, h) && !(steamId && ownedByOther(h))) {
            out = h;
            if (steamId) { ledger.push_back({steamId, h, name}); WriteLedger(ledger); }
            return true;
        }
    }
    // 3. a new one (its name made unique in the squad)
    for (int n = 2; n < 100; ++n) {
        bool clash = false;
        for (kenshi::Character* c : squad) { std::string cn; clash |= kenshi::CharacterName(c, cn) && cn == name; }
        if (!clash) break;
        name = playerName.substr(0, 12) + " " + std::to_string(n);
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
    created = true;
    if (steamId) {
        ledger.erase(std::remove_if(ledger.begin(), ledger.end(), [&](const LedgerEntry& e) { return e.steamId == steamId; }), ledger.end());
        ledger.push_back({steamId, out, name});
        WriteLedger(ledger);
    }
    Log("created %s's own character", name.c_str());
    Toast(name + " joins with a character of their own.");
    return true;
}

void KenshiWorld::SetRole(bool client, bool active) {
    client_ = client;
    active_ = active;
    haveHostTime_ = false;
    pauseSeenAt_ = -1;
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
