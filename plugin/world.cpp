#include "world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "hooks.h"
#include "kc/talk.h"

namespace kcp {

namespace {
float Dist(const kc::Vec3& a, const kc::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
std::string KeyOf(const kc::Handle& h) {
    char b[80];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}
// Save slot names (<= 15 chars: passed to the game as inline VS2010 strings).
constexpr const char* kExportSlot = "KenshiCoopHost";
constexpr const char* kImportSlot = "KenshiCoopJoin";

// The save folder this game writes the host's world into. Two games on one PC (tests) may load a
// host's world at the same moment: each takes a slot of its own ("KenshiCoopJoin", then
// "KenshiCoopJoin2".."KenshiCoopJoin8"), claimed with a named mutex held for the life of the
// process, so one never deletes or overwrites the files the other is loading.
std::string ImportSlot() {
    static std::string slot;
    if (!slot.empty()) return slot;
    for (int n = 1; n <= 8; ++n) {
        const std::string name = n == 1 ? std::string(kImportSlot) : std::string(kImportSlot) + std::to_string(n);
        HANDLE m = CreateMutexA(nullptr, FALSE, ("Local\\KenshiCoopImportSlot_" + name).c_str());
        if (!m) continue;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); continue; }
        slot = name;   // the handle stays open: the slot is ours until the game exits
        return slot;
    }
    slot = std::string(kImportSlot) + "_" + std::to_string(GetCurrentProcessId());
    return slot;
}

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
    {
        const double now = NowSeconds();
        if (frameAt_ > 0) frameDt_ = std::clamp(now - frameAt_, 0.0, 0.1);
        frameAt_ = now;
    }
    PurgeDestroyed();
    squad_.clear();
    resolved_.clear();
    bySerial_.clear();
    bySerialBuilt_ = false;
    if (!live) {   // menus and loading screens: nothing in the world may be touched
        wasReady_ = false;
        identity_.Gap();
        return;
    }
    kenshi::PlayerCharacters(scratch_);
    for (kenshi::Character* c : scratch_) {
        kc::Handle h;
        if (kenshi::GetHandle(c, h) && h.valid()) squad_[h] = c;
    }
    // A (re)loaded world: a new generation, which tells a joining client its load completed.
    // A frame without the game's main loop (a zone loading after a long walk or a far teleport, a
    // window resized, a hitch) also passes through "not live": the same characters at the same
    // addresses afterwards mean the same world, whose stand-ins and caches must survive. Any
    // reload frees them all. Compared every live frame, the player's squad without the stand-ins
    // (other players' characters on a client): see kc::WorldIdentity, and the duplicated players
    // of the 4-player stress test.
    void* player = kenshi::Player();
    const bool ready = player && !squad_.empty();
    if (!ready) {
        identity_.Gap();
    } else {
        std::unordered_set<kc::Handle, HandleHash> standInHandles;
        for (const auto& [host, local] : alias_) standInHandles.insert(local);
        std::vector<const void*> own;
        for (const auto& [h, c] : squad_) if (!standInHandles.count(h)) own.push_back(c);
        std::vector<kc::WorldIdentity::Object> made;
        for (const kc::Handle& local : spawned_)
            if (kenshi::Character* c = kenshi::Resolve(local)) made.push_back({local, c});
        const kc::WorldIdentity::Verdict v = identity_.Observe(player, std::move(own), made, [](const kc::WorldIdentity::Object& o) {
            kenshi::Character* c = kenshi::Resolve(o.handle);
            return c && c == o.ptr && kenshi::IsCharacter(c);
        });
        if (v.newWorld) {
            // stand-ins we made that are still in the game: nothing would know them any more, and
            // the session would make new ones beside them
            int destroyed = 0;
            for (const kc::WorldIdentity::Object& o : v.leftovers) {
                kenshi::Character* c = kenshi::Resolve(o.handle);
                if (!c || c != o.ptr) continue;
                HostCallScope scope;
                if (kenshi::InventoryWindowShows(c)) kenshi::CloseInventoryWindows();
                if (kenshi::DestroyObject(c)) ++destroyed;
            }
            generation_ = identity_.generation();
            ResetWorldBound();
            if (!v.leftovers.empty())
                Log("new world (generation %u): every per-world state reset; %d stand-in(s) of the previous one still here, removed", generation_,
                    destroyed);
            else
                Log("new world (generation %u): every per-world state reset", generation_);
            squad_.clear();   // removed stand-ins may have been in it
            kenshi::PlayerCharacters(scratch_);
            for (kenshi::Character* c : scratch_) {
                kc::Handle h;
                if (kenshi::GetHandle(c, h) && h.valid()) squad_[h] = c;
            }
        }
        if (v.newWorld || !wasReady_) {
            // Same world, but the game ran frames we did not see (it opened a trade window, loaded a
            // zone): handle lookups are made again, and a stand-in whose local object is gone is
            // forgotten. A stand-in still there keeps its alias: forgetting them all (as after a reload)
            // made each one a stranger again, removed 5 s later ("removed N local character(s) the host
            // does not have"), the merchant of an open trade window with them (window closed, sale lost).
            resolved_.clear();
            kenshi::ResetLookupCaches();
            for (auto a = alias_.begin(); a != alias_.end();) {
                kc::Handle moved;
                if (kenshi::ResolveObject(a->second)) ++a;
                else if (FindMoved(a->second, moved)) { RenameSpawned(a->second, moved); a->second = moved; ++a; }   // moved to another squad meanwhile
                else { spawned_.erase(a->second); a = alias_.erase(a); }
            }
            pendingLoot_.clear();
            strangerSince_.clear();
            lastTarget_.clear();
            fallPrep_.clear();
            fellAt_.clear();
        }
    }
    wasReady_ = ready;
    lastPlayer_ = player;
    if (ready && active_) InventorySweep(NowSeconds());
}

// Every 3 s in a co-op game: the inventories of the player's characters and their backpacks checked
// (kenshi::CheckInventories). A problem seen by two sweeps in a row (not something the game was in the
// middle of) is repaired: a freed item out of a list, an item that left out of the list or section it
// stayed in. Saves already holding such backpacks (11 October) no longer crash the AI that reads them.
// Never while an item is dragged with the mouse (it is out of its section for that time).
void KenshiWorld::InventorySweep(double now) {
    constexpr double kEvery = 3.0;
    if (now < invSweepAt_) return;
    invSweepAt_ = now + kEvery;
    if (kenshi::MouseHoldsItem()) { invSweepLast_.clear(); return; }
    std::vector<std::string> lines;
    int inventories = 0, items = 0;
    if (CheckInventories(false, false, lines, inventories, items) == 0) { invSweepLast_.clear(); return; }
    const bool again = std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
        return std::find(invSweepLast_.begin(), invSweepLast_.end(), l) != invSweepLast_.end();
    });
    if (!again) {
        invSweepLast_ = lines;
        return;
    }
    std::vector<std::string> fixed;
    HostCallScope scope;
    CheckInventories(false, true, fixed, inventories, items);
    invSweepLast_.clear();
    for (const std::string& l : fixed) {
        const bool repaired = l.find("repaired") != std::string::npos;
        if (repaired) ++invRepairs_;
        // what cannot be repaired here (a state the check does not know, a freed item in a section)
        // is said once, not every 3 s
        if (repaired || (invSweepLogged_.size() < 1000 && invSweepLogged_.insert(l).second)) Log("INVCHECK sweep: %s", l.c_str());
    }
}

int KenshiWorld::CheckInventories(bool all, bool repair, std::vector<std::string>& lines, int& inventories, int& items) {
    std::vector<kenshi::Character*> chars;
    if (all) kenshi::ActiveCharacters(chars);
    else kenshi::PlayerCharacters(chars);
    std::vector<void*> holders(chars.begin(), chars.end());
    return kenshi::CheckInventories(holders, repair, lines, &inventories, &items);
}

void KenshiWorld::CheckAfterOp(const char* what, void* a, void* b) {
    if (!invCheckEachOp_) return;
    std::vector<void*> holders{a};
    if (b && b != a) holders.push_back(b);
    for (void* h : {a, b})   // a backpack: its wearer's inventory too (and so every section it shows)
        if (h && !kenshi::IsCharacter(h)) holders.push_back(kenshi::BackpackWearer(h));
    holders.erase(std::remove(holders.begin(), holders.end(), nullptr), holders.end());
    std::vector<std::string> lines;
    const int n = kenshi::CheckInventories(holders, false, lines);
    if (n == 0) Log("INVCHECK after %s: clean", what);
    for (const std::string& l : lines) Log("INVCHECK after %s: %s", what, l.c_str());
}

// Everything that names objects of the loaded world (pointers, local handles, requests about
// them). After a reload (a resync, a load) they are gone: keeping any of it makes the next
// frame call the game on freed memory.
void KenshiWorld::ResetWorldBound() {
    alias_.clear();   // stand-ins belonged to the previous world
    spawned_.clear();
    {
        std::lock_guard<std::mutex> lk(workshopMutex_);   // workshop: pointers of the previous world
        craftMeta_.clear();
        hostPoweredTowns_.clear();
        researchAsks_.clear();
        machineAsks_.clear();
    }
    pendingLoot_.clear();
    strangerSince_.clear();
    lastTarget_.clear();
    fallPrep_.clear();
    fellAt_.clear();
    postureBook_.clear();
    hostKnockedOut_.clear();
    hostCaptive_.clear();
    destCheckAt_.clear();
    fallFrom_.clear();
    fallShift_.clear();
    lastDest_.clear();
    postureSince_.clear();
    postureFixed_.clear();
    carriedHere_.clear();
    carrierOf_.clear();
    carryCheckAt_.clear();
    carryAnim_.clear();
    standTry_.clear();
    freeToldAt_.clear();
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
    roofBuilding_ = nullptr;
    focusPlaced_ = false;
    loadFocus_ = {};
    focusRetakes_ = 0;
    navRefreshed_.clear();
    reRagdoll_.clear();
    floorTry_.clear();
    syncErr_.clear();
    syncMaxErr_ = 0;
    remoteDialogs_.clear();
    turretCache_.clear();
    pickups_.clear();
    pendingTalks_.clear();
    captiveHold_.clear();
    captiveMissing_.clear();
    resolved_.clear();
    factoryRefused_.Clear();
    resyncSnap_.clear();
    farSet_.reset();
    farLeaveEnabled = true;
    { std::lock_guard<std::mutex> lk(destroyedMutex_); destroyed_.clear(); }
    ResetGround();
    { std::lock_guard<std::mutex> lk(doorMutex_); doorReqs_.clear(); doorCache_.clear(); doorTries_.clear(); }
    { std::lock_guard<std::mutex> lk(tradeMutex_); tradeReqs_.clear(); hostTradeLooter_ = {}; hostTradeTrader_ = {}; }
    { std::lock_guard<std::mutex> lk(buildMutex_); trackedBuildings_.clear(); localPlacements_.clear(); localBuildActions_.clear(); removedBuildings_.clear(); }
    { std::lock_guard<std::mutex> lk(dialogMutex_); dialogEvents_.clear(); }
    kenshi::ResetLookupCaches();
    kenshi::ForgetGodModes();
}

void KenshiWorld::EndFrame() {
    PurgeDestroyed();   // our own calls this frame may have unloaded or destroyed characters too
    // Clients run the host's clock: speed and pause are imposed every frame, so local keys
    // (space, F2/F3/F4) have no lasting effect.
    if (active_ && client_ && haveHostTime_) {
        // Same calls as the space bar / speed keys, so the game's own speed UI follows too.
        // Paused, the game commits no position written to a character: before pausing, a client
        // runs a few frames at a crawl (its clock barely moves: the game recomputes it from its own
        // counter and it cannot be set back) while Apply puts every character exactly where the
        // host's stopped.
        // 0.8 s at 0.05 (was 0.3 s at 0.01): far from this game's camera a character takes a written
        // position only on its own, rarer updates; with 0.3 s at 0.01 walkers far away stayed where
        // they were, 20-35 units behind the host's (faraway test, 11 October)
        constexpr double kSettleBeforePause = 0.8;
        constexpr float kCrawl = 0.05f;
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
            // the speed buttons' own setter, so that x2 / x3 show on the client's buttons too
            // (setFrameSpeedMultiplier alone changed the speed and left the buttons on x1)
            if (std::fabs(kenshi::GetFrameSpeed() - hostTime_.speed) > 1e-3f && !kenshi::CallSetGameSpeedUi(hostTime_.speed))
                kenshi::CallSetFrameSpeed(hostTime_.speed);
        }
    }
    // While players join, the host's world stays frozen even if someone presses unpause.
    if (holding_ && !kenshi::GetPaused()) {
        kenshi::CallUserPause(true);
        pausedByHold_ = true;
    }
    if (active_ && client_ && live_ && !pendingLoot_.empty()) UpdatePendingLoot();
    if (active_ && !client_ && live_ && !pickups_.empty()) UpdatePendingPickups();
    if (const bool farTalk = active_ && !client_; farTalk != farTalk_) {   // see kenshi::SetFarTalk
        const bool ok = kenshi::SetFarTalk(farTalk);
        farTalk_ = farTalk;
        Log("conversations far from the host's camera: %s", ok ? (farTalk ? "allowed" : "the game's rule again") : "CANNOT PATCH (unexpected bytes)");
    }
    if (active_ && !client_ && live_ && !pendingTalks_.empty()) UpdatePendingTalks();
    if (active_ && !client_ && live_) UpdateRemoteDialogs();   // 0.3.4: lines waiting for a player's answer
    if (active_ && !client_ && live_ && !reRagdoll_.empty()) UpdateReRagdolls();
    if (active_ && client_ && live_) GroundReconcile();
    if (active_ && client_ && live_) UpdateFloorFocus();
    if (active_ && !client_ && live_) UpdateHostRoof();
    auto v = std::make_shared<HookView>();
    v->active = active_;
    v->client = client_;
    // A character stays "driven by the host" for a second after its last update: a frame without
    // one (paused, a skipped tick) must not hand it back to the client's own game for that frame.
    const double nowView = NowSeconds();
    for (const void* c : applied_) replicatedAt_[c] = nowView;
    // Only characters Apply found this very frame are touched below: an address kept from an earlier
    // frame may belong to a character the game has unloaded since (0.3.1, rob's crashes).
    const std::unordered_set<const void*> appliedNow = std::move(applied_);
    applied_.clear();
    for (auto it = replicatedAt_.begin(); it != replicatedAt_.end();) {
        if (nowView - it->second > 1.0 || !active_ || !client_) { it = replicatedAt_.erase(it); continue; }
        v->replicated.insert(it->first);
        ++it;
    }
    v->gameSpeed = kenshi::GetFrameSpeed();
    if (active_ && !client_) v->remoteNear = remoteNear_;
    if (active_ && client_ && farLeaveEnabled) v->farAway = farSet_;
    if (active_ && client_) {
        // fighting and fallen characters play the host's combat and fall animations: their master
        // clock is still corrected, but not counted (the tests measure walking cycles)
        std::unordered_set<const void*> busy;
        for (const auto& [h, st] : lastTarget_)
            if (st.combatTarget || (st.flags & (kc::kFlagDown | kc::kFlagDead)))
                if (kenshi::Character* c = Find(h)) busy.insert(c);
        for (auto it = animTargets_.begin(); it != animTargets_.end();) {
            if (nowView - it->second->sampledAt > 1.0 || !v->replicated.count(it->first)) { it = animTargets_.erase(it); continue; }
            if (!appliedNow.count(it->first)) { ++it; continue; }
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
                // Never a jump any more (11 October, a client far from the host: 2700 jumps in 10 s
                // over ~100 NPCs, each one's animation popping every half second, "saccadé"): far off,
                // caught up a third of the way per frame (a few frames, no pop); a little off, a tenth.
                float t = mine;
                if (std::fabs(d) > jump) t = mine + d * 0.3f;
                else if (std::fabs(d) > 0.01f) t = mine + d * 0.1f;
                t -= std::floor(t);
                kenshi::WriteAnimMaster(ac, t, tg.masterSpeed);
            }
            ++it;
        }
    } else {
        animTargets_.clear();
    }
    // (3b: not for the far ones, standing where the host last sent them, on the game's far update)
    if (active_ && client_)
        for (const auto& [h, st] : lastTarget_)
            if (kenshi::Character* c = Find(h); c && v->replicated.count(c) && !(farLeaveEnabled && farSet_ && farSet_->count(c)))
                if (void* m = kenshi::MovementOf(c)) {
                    v->facing[m] = kenshi::ForwardOf(st.rot);
                    v->orient[m] = {c, st.rot};
                }
    if (active_) {
        for (const kc::Handle& h : controllable_) {
            auto a = alias_.find(h);
            v->controllable.insert(a != alias_.end() ? a->second : h);
        }
        for (auto& [h, c] : squad_) if (!v->controllable.count(h)) v->squadForeign.insert(c);
        if (!client_)
            for (auto& [h, c] : squad_)
                if (auto o = squadOwners_.find(h); o != squadOwners_.end()) v->squadOwner[c] = o->second;
        for (const kc::Handle& h : shared_) {
            auto a = alias_.find(h);
            v->shared.insert(a != alias_.end() ? a->second : h);
        }
    }
    if (active_ && client_)
        for (const kc::Handle& h : dropSources_) {   // the session names characters by the host's handle
            auto a = alias_.find(h);
            v->dropSources.insert(a != alias_.end() ? a->second : h);
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
void KenshiWorld::RenameSpawned(const kc::Handle& before, const kc::Handle& after) {
    if (spawned_.erase(before)) spawned_.insert(after);
}

void KenshiWorld::LocalRehandled(const kc::Handle& before, const kc::Handle& after) {
    if (before == after) return;
    RenameSpawned(before, after);
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
    // a template whose factory call faulted once is not tried again: each try may leave another
    // half set-up character in the game's update lists (see kenshi::CreateCharacter)
    if (factoryFaulted_.count(info.templateSid)) return false;
    // a template the factory refused lately waits (0.3.1): unique characters ('Cat', 'Ruka', 'Kang')
    // were asked for again every 2 to 15 s on a client, hundreds of times, each try costing a frame
    const double now = NowSeconds();
    if (!factoryRefused_.Allowed(info.templateSid, now)) return false;
    std::string err;
    const kc::ScopeCounts mark = MarkCallScopes();
    kenshi::Character* c = nullptr;
    {
        HostCallScope scope;
        c = kenshi::CreateCharacter(info, at.pos, &err);
    }
    // the factory runs our hooks: an exception it raised, caught by CreateCharacter, skipped the
    // destructors of the scopes they had opened
    RepairCallScopes(mark, "character factory");
    kc::Handle local;
    if (!c || !kenshi::GetHandle(c, local)) {
        const bool faulted = err.rfind("exception", 0) == 0;
        if (faulted) {
            factoryFaulted_.insert(info.templateSid);
            Log("cannot recreate host character %s: %s (not tried again)", info.templateSid.c_str(), err.c_str());
        } else {
            const double wait = factoryRefused_.Fail(info.templateSid, now);
            Log("cannot recreate host character %s: %s (that kind is tried again in %.0f s)", info.templateSid.c_str(), err.c_str(), wait);
        }
        return false;
    }
    factoryRefused_.Succeed(info.templateSid);
    HostCallScope scope;
    kenshi::Teleport(c, at.pos, at.rot);
    alias_[h] = local;
    spawned_.insert(local);
    resolved_.erase(h);
    // The factory makes a living, standing character. A body dead on the host (its stand-in
    // recreated after its zone unloaded and loaded again here) comes back dead and lying at once,
    // where it was made (the host's spot: no teleport before the ragdoll, which would throw it).
    if (kc::motion::SpawnsDead(at)) {
        kenshi::CallDeclareDead(c);
        kenshi::SetRagdoll(c, true);
        fellAt_[h] = NowSeconds();
        Log("stand-in for the host's dead %s made dead and lying (dead=%d)", KeyOf(h).c_str(), int(kenshi::IsDead(c)));
    }
    return true;
}

// Carried, shackled, caged, in a bed, a prisoner or a slave (here or on the host): its pose belongs
// to the game's captivity code (a shackled captive sits, a carried body hangs on a shoulder), and
// the posture code never stands it up or lays it down.
bool KenshiWorld::PostureHeld(const kc::Handle& h, kenshi::Character* c) {
    if (carriedHere_.count(h) || hostCaptive_.count(h)) return true;
    kenshi::Captivity k;
    if (!kenshi::ReadCaptivity(c, k)) return false;
    return k.chained || k.inSomething != 0 || k.slaveState != 0 || k.kidnapped || k.sentence > 0;
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

// Where our copy stands before falling so that its body ends where the host's lies: the host's
// spot, minus how far from its feet our copy's body came to rest the last time (fallShift_).
kc::EntityState KenshiWorld::FallSpot(const kc::Handle& h, const kc::EntityState& host) const {
    kc::EntityState at = host;
    if (auto s = fallShift_.find(h); s != fallShift_.end()) {
        at.pos.x -= s->second.x;
        at.pos.z -= s->second.z;
    }
    return at;
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
    if (a != alias_.end() && a->second != h && squad_.count(a->second) && HostKnows(a->second)) {
        // One of our squad members linked to another host character than its own (0.3.2, the
        // Stone case above, made before this check existed or by a handle change): the link goes,
        // the member stays its own host character's copy, and the other one is looked for again.
        Log("our squad member %s was linked to the host's %s, but it is the host's %s itself: link dropped", KeyOf(a->second).c_str(),
            KeyOf(h).c_str(), KeyOf(a->second).c_str());
        spawned_.erase(a->second);
        alias_.erase(a);
        resolved_.erase(h);
        a = alias_.end();
    }
    if (a != alias_.end()) {
        // The real character streamed in after all (its zone loaded later here than on the
        // host): it replaces the stand-in, otherwise there would be two of them.
        kenshi::Character* real = kenshi::Resolve(h);
        if (real && kenshi::IsCharacter(real)) {
            if (kenshi::Character* standIn = kenshi::Resolve(a->second); standIn && standIn != real) {
                HostCallScope scope;
                Log("stand-in %s replaced by the host's real character", KeyOf(a->second).c_str());
                // 0.3.4: a trade window selling for the stand-in goes on with the real character (it
                // used to close: every stand-in merchant replaced ~1 s after the window opened)
                if (kenshi::InventoryWindowShows(standIn)) {
                    int32_t cats = 0;
                    const bool haveCats = kenshi::MoneyOf(standIn, cats);
                    std::string why;
                    const int r = kenshi::RetargetTradeWindow(standIn, real, &why);
                    if (r > 0) {
                        if (haveCats) kenshi::SetMoneyOf(real, cats);   // the host's cats, set on the stand-in (the session sets them again)
                        Log("trade window: now on the host's real merchant %s (%d cats)", KeyOf(h).c_str(), haveCats ? int(cats) : -1);
                    } else if (r < 0) {
                        Log("trade window: cannot move it to the host's real merchant (%s): closed", why.c_str());
                    }
                }
                kenshi::DestroyObject(standIn);   // a window still showing it is closed first (hk_worldDestroy)
            }
            spawned_.erase(a->second);
            alias_.erase(a);
            resolved_[h] = real;
            return true;
        }
        // The stand-in is gone (killed and cleaned up, or dropped with its zone by the local game):
        // the alias must go too, or Spawn and Reconcile would skip this character forever.
        // ResolveObject: an alias may name any object, not only a character.
        if (!kenshi::ResolveObject(a->second)) {
            // Not gone: the local game moved our copy to another squad, which gave it another handle
            // (a death puts it in the dead squad). Recreating it there made a new body every few
            // seconds (54 copies of one dead squad member in the 20-minute soak).
            kc::Handle moved;
            if (FindMoved(a->second, moved)) {
                Log("our copy %s of the host's %s changed squads here (now %s): followed", KeyOf(a->second).c_str(), KeyOf(h).c_str(),
                    KeyOf(moved).c_str());
                LocalRehandled(a->second, moved);
                return true;
            }
            std::string why;
            {
                std::lock_guard<std::mutex> lk(destroyedMutex_);
                if (auto d = destroyed_.find(a->second); d != destroyed_.end()) { why = d->second; destroyed_.erase(d); }
            }
            uint32_t held = 0;
            if (goneLog_.Allow(NowSeconds(), &held)) {
                const std::string skipped = held ? " [" + std::to_string(held) + " more such lines not written]" : std::string();
                Log("stand-in %s for the host's %s is gone here (%s): it can be recreated%s", KeyOf(a->second).c_str(), KeyOf(h).c_str(),
                    why.empty() ? "no longer in the game" : why.c_str(), skipped.c_str());
            }
            spawned_.erase(a->second);
            alias_.erase(a);
            resolved_.erase(h);
            return false;
        }
    }
    if (Find(h)) return true;
    // Our copy still under the host's old handle, moved by the local game before the host's new
    // handle for it arrived (its death applied here first): the host's handle now names it.
    kc::Handle moved;
    if (client_ && FindMoved(h, moved)) {
        Log("our copy of the host's %s changed squads here (now %s): followed", KeyOf(h).c_str(), KeyOf(moved).c_str());
        alias_[h] = moved;
        resolved_.erase(h);
        if (auto it = squad_.find(h); it != squad_.end()) { squad_[moved] = it->second; squad_.erase(it); }
        return Find(h) != nullptr;
    }
    return false;
}

bool KenshiWorld::FindMoved(const kc::Handle& stale, kc::Handle& now) {
    if (!stale.valid() || stale.serial == 0) return false;
    if (!bySerialBuilt_) {   // once per frame: every character the game knows (squads, active, bodies)
        bySerialBuilt_ = true;
        std::vector<kenshi::Character*> all;
        for (int pass = 0; pass < 3; ++pass) {
            if (pass == 0) kenshi::PlayerCharacters(all);
            else if (pass == 1) kenshi::ActiveCharacters(all);
            else kenshi::DeadBodies(all);
            for (kenshi::Character* c : all) {
                kc::Handle h;
                if (kenshi::GetHandle(c, h) && h.valid()) bySerial_.emplace(h.serial, h);
            }
        }
    }
    // The handle keeps its type and serial when the game moves a character to another squad; only
    // the squad part changes. Exactly one candidate, or none is taken.
    kc::Handle found;
    int n = 0;
    auto [b, e] = bySerial_.equal_range(stale.serial);
    for (auto it = b; it != e; ++it) {
        const kc::Handle& h = it->second;
        if (h == stale || h.type != stale.type || h == found) continue;
        if (h.container == stale.container && h.containerSerial == stale.containerSerial) continue;
        found = h;
        ++n;
    }
    if (n != 1 || !kenshi::Resolve(found)) return false;
    // never one already standing in for another host character
    for (const auto& [host, local] : alias_) if (local == found) return false;
    // never one whose handle is a host character the session follows (0.3.2): it is that
    // character's own copy (11 October: the host's 1:317:...:1, gone, followed onto Stone, a squad
    // member of nass4 under its own handle; the owner's orders on Stone then named 1:317 and were
    // refused, ~900 lines a minute). Same serial, but conflicts go by handle.
    if (HostKnows(found)) {
        uint32_t held = 0;
        if (followLog_.Allow(NowSeconds(), &held))
            Log("our %s is the copy of the host's character with that handle: not taken for the host's %s [%u more such lines not written]",
                KeyOf(found).c_str(), KeyOf(stale).c_str(), held);
        return false;
    }
    now = found;
    return true;
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
    // 0.3.4 (A_FAIRE 12/10 point 2): the squads here that hold one of our copies of the host's
    // characters. On rob's side the copy of a knocked-out bandit the host had (followed into a local
    // bandit squad by our game, 22:11:11) vanished one second after a stranger of that squad was
    // removed (22:16:51, "no longer in the game", no destroy seen): removing the last standing member
    // of a squad can take the knocked-out ones along. Such a stranger stays (asked again in a minute).
    std::unordered_set<void*> copySquads;
    bool copySquadsBuilt = false;
    auto sharesSquadWithCopy = [&](kenshi::Character* c) {
        if (!copySquadsBuilt) {
            copySquadsBuilt = true;
            for (kenshi::Character* o : scratch_) {
                kc::Handle oh;
                if (kenshi::GetHandle(o, oh) && taken.count(oh))
                    if (void* sq = kenshi::SquadOf(o)) copySquads.insert(sq);
            }
        }
        void* sq = kenshi::SquadOf(c);
        return sq && copySquads.count(sq) != 0;
    };
    for (Stranger& s : strangers) {
        if (s.used || now - s.since < kLinger || !inHostArea(s.c)) continue;
        if (sharesSquadWithCopy(s.c)) {
            std::string name;
            kenshi::CharacterName(s.c, name);
            Log("not removed: the local character '%s' the host does not have is in a squad here with our copy of a host character (removing it could take that copy along)",
                name.c_str());
            strangerSince_[s.h] = now + 60.0;
            continue;
        }
        if (kenshi::InPlayerSquad(s.c)) {   // its portrait is in the squad bar: never deleted (kenshi::DestroyObject refuses it too)
            std::string name;
            kenshi::CharacterName(s.c, name);
            Log("not removed: the local character '%s' the host does not have is in one of the player's squads", name.c_str());
            strangerSince_[s.h] = now + 60.0;   // asked again in a minute, not every tick
            continue;
        }
        HostCallScope scope;
        if (kenshi::InventoryWindowShows(s.c)) {
            kenshi::CloseInventoryWindows();
            Log("closed the inventory windows: the character they show is being removed");
        }
        std::string name;
        kenshi::CharacterName(s.c, name);
        if (kenshi::DestroyObject(s.c)) { ++removed; Log("removed the local character '%s': the host does not have it", name.c_str()); }
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
    spawned_.erase(a->second);
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
    if (kc::motion::HostMoving(moving, speed)) out.flags |= kc::kFlagMoving;
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
    // on someone's shoulder here: never pulled, stood up or teleported off it (0.3.4: as long as
    // someone here really carries it; a stale mark left a body lying for good while the host's walked)
    if (carriedHere_.count(h) && StillCarried(h, c) && kenshi::IsRagdoll(c)) return;
    kc::EntityState& last = lastTarget_[h];
    last = target;
    last.flags = latest.flags;
    const double now = NowSeconds();
    if (!carriedHere_.count(h)) CheckStaleCarryAnim(h, c, now);
    if (CaptiveHold(h, c)) { lastDest_.erase(h); return; }   // lot D: the host keeps it in a cage
    // Posture: lying on the ground (knocked out, knocked down) or standing must match the host.
    // Deaths come through vitals; getting up is normally decided by the AI, which clients do not run.
    const bool hostDead = (latest.flags & kc::kFlagDead) != 0;
    const bool hostDown = (latest.flags & kc::kFlagDown) != 0;
    kc::motion::PostureFacts pf;
    pf.hostDown = hostDown;
    pf.hostDead = hostDead;
    pf.localDead = kenshi::IsDead(c);
    pf.localDown = kenshi::IsRagdoll(c) || pf.localDead;
    // Carried, shackled, caged, in a bed, a prisoner or a slave: the game's captivity code owns its
    // pose (a shackled captive sits). Forcing falls on them made nass4's captives fall, sit up and
    // fall again every 2 s (10/10 session): the posture code leaves them alone.
    pf.held = (hostDown || hostDead || pf.localDown) && PostureHeld(h, c);   // read only when posture matters
    kc::motion::PostureBook& book = postureBook_[h];
    // A ragdoll that is being set up must not be touched: moving the character then throws the
    // body across the map. Leave it alone while it falls; then see whether the fall took.
    if (auto fell = fellAt_.find(h); fell != fellAt_.end()) {
        if (now - fell->second < 2.0) { lastDest_.erase(h); return; }
        fellAt_.erase(fell);
        kc::motion::NoteFallSettled(book, pf.localDown, hostDown);
        if (book.gaveUp) Log("posture: %s does not stay down here after %d falls: left as it is for a minute", KeyOf(h).c_str(), book.falls);
    }
    // Dead on the host: dead and lying here too, forever (kc::motion::DeadBodyStep). A stand-in alive
    // here (recreated, death not replayed yet) dies at the host's spot; a corpse standing here while
    // the host's lies is laid down again (the per-body budget keeps a refused ragdoll from looping).
    // Nothing below ever stands a dead body up.
    if (hostDead) {
        const bool localRagdoll = kenshi::IsRagdoll(c);
        const kc::motion::DeadStep step = kc::motion::DeadBodyStep(true, hostDown, pf.localDead, localRagdoll, pf.held);
        double& fixed = postureFixed_[h];
        if (step != kc::motion::DeadStep::None && now - fixed > 1.0 && kc::motion::MayLayDead(book, now)) {
            const bool kill = step == kc::motion::DeadStep::Kill;
            if (!kill || localRagdoll || ReadyToFall(h, c, target, now)) {
                HostCallScope scope;
                if (kill) kenshi::CallDeclareDead(c);
                const bool ok = kenshi::SetRagdoll(c, true);
                kc::motion::NoteFall(book, now);
                fixed = now;
                fellAt_[h] = now;
                Log("posture: %s %s as on the host (%s, dead=%d)", KeyOf(h).c_str(), kill ? "dies and falls" : "dead body lies down again",
                    ok ? "ok" : "call failed", int(kenshi::IsDead(c)));
            }
        }
    }
    const bool localDown = pf.localDown;
    if (hostDown != localDown && !hostDead && !pf.held) {
        double& since = postureSince_[h];
        if (since == 0) since = now;
        double& fixed = postureFixed_[h];
        if (now - since > 0.3 && now - fixed > 1.0) {   // not a one-frame flicker, and not every frame
            if (!hostDown) {
                if (kc::motion::MayStand(pf)) {
                    HostCallScope scope;
                    const bool ok = kenshi::StandUp(c);
                    // 0.3.4 (A_FAIRE 12/10 point 12, "the name moves but not the body"): a copy the host
                    // has standing that stays down here is told (once in 30 s per body), with what holds
                    // it; the stand-up goes on every second (ragdoll off, posture normal).
                    StandTry& st = standTry_[h];
                    if (st.tries == 0) st.since = now;
                    ++st.tries;
                    if (st.tries >= 3 && now - st.toldAt > 30.0) {
                        st.toldAt = now;
                        int in = 0;
                        kenshi::ReadInSomething(c, in);
                        Log("posture: %s still down here %.0f s after %d stand-ups while the host's %s (ragdoll %d, unconscious %d, inSomething %d, call %s)",
                            KeyOf(h).c_str(), now - st.since, st.tries, (latest.flags & kc::kFlagMoving) ? "walks" : "stands",
                            int(kenshi::IsRagdoll(c)), int(kenshi::IsUnconscious(c)), in, ok ? "ok" : "failed");
                    }
                }
                fixed = now;
            } else if (kc::motion::MayFall(book, pf, now) && ReadyToFall(h, c, FallSpot(h, target), now)) {
                HostCallScope scope;
                kc::Vec3 from;
                if (kenshi::GetPosition(c, from)) fallFrom_[h] = from;
                // knocked out on the host: unconscious here too, or the local game stands it up again
                // (knocked-out NPCs seen standing by a client who could loot them)
                if (hostKnockedOut_.count(h) && !kenshi::IsUnconscious(c)) kenshi::SetUnconscious(c, true);
                const bool ok = kenshi::SetRagdoll(c, true);
                kc::motion::NoteFall(book, now);
                fixed = now;
                fellAt_[h] = now;
                Log("posture: %s falls as on the host (%s, fall %d/%d this minute)", KeyOf(h).c_str(), ok ? "ok" : "call failed", book.falls,
                    kc::motion::kMaxFalls);
            }
        }
    } else {
        postureSince_.erase(h);
        if (auto st = standTry_.find(h); st != standTry_.end()) {
            if (st->second.tries >= 3) Log("posture: %s up again after %d stand-ups (%.1f s)", KeyOf(h).c_str(), st->second.tries, now - st->second.since);
            standTry_.erase(st);
        }
    }
    kc::Vec3 local;
    if (!kenshi::GetPosition(c, local)) return;
    // A body on the ground belongs to ragdoll physics (teleports do not move it); it fell where the
    // host's did (see ReadyToFall).
    if (hostDown || hostDead || localDown) {
        lastDest_.erase(h);
        stuck_.erase(h);
        // A knocked-out body lying well away from the host's (it fell where our copy was when the
        // game ignored the positions written to it, far from every player: one lay 87 units off for
        // minutes in the far-zone test): once a player comes near, it is stood up, and the posture
        // code above puts it on the host's spot (ReadyToFall) and lays it down again. Never a dead
        // body, never a held one (carried, shackled, caged...), at most twice a minute, 10 s apart,
        // and only when a fall is left to lay it down again (kc::motion::MayRelay).
        // 20 units, not 5: a collapsing body ends 7-12 units from where its feet were, so a 5-unit
        // threshold stood up nearly every body after every fall (441 times on one client in the
        // 10/10 session, the counter reset by each change of posture).
        constexpr float kBodySeen = 300.0f;
        // Where the body came to rest, against where our copy stood when it fell (the fellAt_ wait
        // above: 2 s): a collapsing body does not lie where its feet were. The host's body position
        // is its fallen one already, so standing our copy there and letting it fall laid it one
        // more such offset away, every time: humans 7-12 units, a land bat 47-76, a spider 70-79
        // (stress4 logs, "lies N units from the host's body", the same N relay after relay). The
        // next fall starts that much short of the host's spot (FallSpot), so it ends on it.
        if (auto f = fallFrom_.find(h); f != fallFrom_.end() && localDown && !pf.localDead) {
            constexpr float kMaxShift = 150.0f;   // never more than a large animal's length
            kc::Vec3 shift{local.x - f->second.x, 0.0f, local.z - f->second.z};
            const float len = std::sqrt(shift.x * shift.x + shift.z * shift.z);
            if (len > kMaxShift) { shift.x *= kMaxShift / len; shift.z *= kMaxShift / len; }
            fallShift_[h] = shift;   // measured from where it really stood: already short of the host's spot or not
            fallFrom_.erase(f);
        }
        const float off = Dist(local, target.pos);
        if (kenshi::IsRagdoll(c) && kc::motion::MayRelay(book, pf, off, NearestSquadDistance(local) < kBodySeen, now)) {
            kc::motion::NoteRelay(book, now);
            HostCallScope scope;
            kenshi::SetRagdoll(c, false);
            postureFixed_[h] = 0;
            Log("posture: %s lies %.0f units from the host's body: stood up to fall where the host's lies (%d/%d this minute)", KeyOf(h).c_str(),
                off, book.relays, kc::motion::kMaxRelays);
        }
        return;
    }
    fallFrom_.erase(h);
    fallShift_.erase(h);

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
    // Far off (late join, lag spike, teleport on the host): snap straight to the host state. Also
    // once after a zone resync (0.3.1), whatever the offset.
    const bool resync = !resyncSnap_.empty() && resyncSnap_.erase(h) > 0 && err > 1.0f;
    if (err > cfg_.snapDistance || resync) {
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
        // Where our copy walks: the host's destination when it is close; farther, a point 60 units
        // ahead on the way the host's character really goes. The game turns a walker toward its own
        // path; with the far destination, its path took another way (the stairs of Okran's Fist and
        // its Shield: 25-40 walkers in 60 facing backwards while pulled along the host's route, 11
        // October). A near point keeps that path on the host's way.
        constexpr float kLookAhead = 60.0f, kFarDest = 90.0f;
        const kc::Vec3 way = kc::motion::TravelDir(target, latest);
        kc::Vec3 goal = latest.dest;
        if (Dist(target.pos, latest.dest) > kFarDest && (way.x * way.x + way.z * way.z) > 0.25f)
            goal = {target.pos.x + way.x * kLookAhead, target.pos.y, target.pos.z + way.z * kLookAhead};
        const float epsilon = goal.x == latest.dest.x && goal.z == latest.dest.z ? cfg_.destEpsilon : 20.0f;
        bool reissue = it == lastDest_.end() || Dist(it->second, goal) > epsilon;
        // Twice a second: is our copy really walking there? Its own path may have ended or gone
        // elsewhere (a destination the game dropped) while the one we issued still matches: it then
        // walked one way while the pull dragged it the other (walking backwards, up to 180 degrees off
        // in the speed 3 test).
        if (!reissue && now - destCheckAt_[h] > 0.5 && NearestSquadDistance(local) < kSeenRange) {
            destCheckAt_[h] = now;
            kc::Vec3 mineDest;
            bool mineMoving = false;
            float mineSpeed = 0;
            if (kenshi::GetMovement(c, mineDest, mineMoving, mineSpeed) && Dist(local, goal) > 2.0f &&
                (!mineMoving || Dist(mineDest, goal) > 2.0f * epsilon))
                reissue = true;
        }
        if (reissue && kenshi::SetDestination(c, goal)) lastDest_[h] = goal;
    } else if (it != lastDest_.end() || kenshi::IsMoving(c)) {
        kenshi::Halt(c);   // the host stopped: stop walking, the correction below settles it exactly
        lastDest_.erase(h);
    }
    // Where the host's character really goes (kc::motion::TravelDir) and where ours must face:
    // exactly the host's facing when standing or fighting (in combat the combat movement hook imposes
    // it too); walking, the host's facing while it agrees with the way it goes, else the way it goes:
    // never a walk played facing away from where the body moves (kc::motion::WantFacing).
    const bool fighting = latest.combatTarget != 0;
    const kc::Vec3 travel = kc::motion::TravelDir(target, latest);
    {
        kc::Vec3 mine;
        const kc::Vec3 want = kc::motion::WantFacing(kenshi::ForwardOf(target.rot), travel, hostMoving, fighting);
        const float limit = (!hostMoving || fighting) ? 0.999f : 0.9f;
        if (kenshi::GetFacing(c, mine) && mine.x * want.x + mine.z * want.z < limit) kenshi::FaceDirection(c, want);
        // the drawn rotation too (hk_moveUpdate does it after the game's movement update; this one
        // covers the frames the game does not update that character)
        if (!fighting) kenshi::WriteRotation(c, target.rot);
    }
    // The host paused: its characters stopped mid-stride. Put them exactly there while the game still
    // runs (at a crawl, see EndFrame): paused, it would not commit the position.
    if (haveHostTime_ && hostTime_.paused) {
        if (kenshi::IsMoving(c)) kenshi::Halt(c);
        if (err > 0.02f) kenshi::Teleport(c, target.pos, target.rot);
        lastDest_.erase(h);
        return;
    }
    // ...while its position is continuously pulled onto the host's: no drift. Frame-rate independent
    // (a client at 150 images per second pulled three times harder than one at 50: stiff, jerky
    // fights), and while it walks never backwards against the way it really goes: a copy a little
    // ahead of a late host position is held back gently, so it keeps moving forwards (the old rule
    // only did so within 0.8 units and along the host's facing, which a far NPC's sparse updates
    // and stale facing both defeated: NPCs sliding backwards in a crowded village).
    if (err > 0.01f) {
        kc::motion::PullParams pp;
        pp.dt = float(frameDt_);
        pp.gameSpeed = haveHostTime_ ? std::max(1.0f, hostTime_.speed) : 1.0f;
        pp.moving = hostMoving;
        pp.combat = fighting;
        kenshi::SetPositionSimple(c, kc::motion::Pull(local, target.pos, travel, pp));
    }
    if (traceFrames > 0 && h == traceHandle && (--traceFrames % 15 == 0 || err > 1.0f || (haveHostTime_ && hostTime_.paused))) {
        kc::Vec3 after;
        kenshi::GetPosition(c, after);
        Log("trace t=%.3f local=(%.2f,%.2f) target=(%.2f,%.2f) latest=(%.2f,%.2f) err=%.2f after=(%.2f,%.2f) moving=%d/%d combat=%d", now,
            local.x, local.z, target.pos.x, target.pos.z, latest.pos.x, latest.pos.z, err, after.x, after.z, hostMoving ? 1 : 0,
            kenshi::IsMoving(c) ? 1 : 0, latest.combatTarget ? 1 : 0);
    }
}

// Client (0.3.4): is that body really on someone's shoulder here? Checked at most once a second.
bool KenshiWorld::StillCarried(const kc::Handle& h, kenshi::Character* body) {
    const double now = NowSeconds();
    double& at = carryCheckAt_[h];
    if (now - at < 1.0) return true;
    at = now;
    auto it = carrierOf_.find(h);
    kenshi::Character* carrier = it != carrierOf_.end() ? Find(it->second) : nullptr;
    kc::Handle local;
    if (carrier && kenshi::ReadCarried(carrier, local) && kenshi::Resolve(local) == body) return true;
    carriedHere_.erase(h);
    carrierOf_.erase(h);
    carryCheckAt_.erase(h);
    Log("carry: %s is on nobody's shoulder here any more: let go (posture and position follow the host again)", KeyOf(h).c_str());
    return false;
}

// Client (0.3.4, A_FAIRE 12/10 points 11-12): a body nobody carries here that still plays the "being
// carried" animation is drawn hanging on a shoulder nobody has (rob carried Meow out of her bedroll at
// 23:18:52 and put her down 3 s later; nass4 and Geoffrey did not see her again until she had been
// put in a bed and taken out, 17 minutes later). The host's own end of the carry comes as an animation
// event; when it did not take, the mode is switched off here after 2 s.
void KenshiWorld::CheckStaleCarryAnim(const kc::Handle& h, kenshi::Character* c, double now) {
    CarryAnimCheck& k = carryAnim_[h];
    if (now - k.at < 2.0) return;
    k.at = now;
    kenshi::AnimModes m;
    if (!kenshi::ReadAnimModes(c, m) || !m.carried) {
        k.since = -1;
        return;
    }
    if (k.since < 0) {
        k.since = now;
        return;
    }
    AnimReplayScope scope;
    const bool ok = kenshi::CallSetCarryMode(c, false, false, false);
    kenshi::AnimModes after;
    const int still = kenshi::ReadAnimModes(c, after) ? int(after.carried) : -1;
    static kc::LogLimiter lim{4, 10.0};
    if (lim.Allow(now))
        Log("carry: %s still played the \"being carried\" animation %.0f s after it was put down here: switched off (%s, carried now %d)", KeyOf(h).c_str(),
        now - k.since, ok ? "ok" : "call failed", still);
    k.since = still == 1 ? now : -1;
}

// Client (0.3.4, A_FAIRE 12/10 point 13): a dormant copy (the host no longer streams it to us) stays
// where the host last had it. Apply was just given a still state (no destination, no fight); our
// game's far update (3b) may still have moved it a little since, or its walk may still run: halted,
// and put back where the host last had it beyond a few units. Never a body (ragdoll physics), a
// carried or caged one.
void KenshiWorld::HoldStill(const kc::Handle& h, const kc::EntityState& at) {
    constexpr float kDrift = 8.0f;
    kenshi::Character* c = Find(h);
    if (!c || carriedHere_.count(h) || CaptiveHold(h, c) || kenshi::IsRagdoll(c) || kenshi::IsDead(c)) return;
    kc::Vec3 p;
    if (!kenshi::GetPosition(c, p)) return;
    HostCallScope scope;
    if (kenshi::IsMoving(c)) {
        kenshi::Halt(c);
        lastDest_.erase(h);
        ++dormantHalts;
    }
    const float d = Dist(p, at.pos);
    if (d <= kDrift) return;
    kenshi::Teleport(c, at.pos, at.rot);
    lastDest_.erase(h);
    ++dormantPutBack;
    uint32_t held = 0;
    if (dormantLog_.Allow(NowSeconds(), &held))
        Log("dormant: %s (no longer sent to us) had moved %.0f units here from where the host last had it: put back%s", KeyOf(h).c_str(), d,
            held ? (" [" + std::to_string(held) + " more such lines not written]").c_str() : "");
}

// Tests ("posturecheck"): copies the host last had standing (not down, not dead, not held captive
// on the host) whose body here lies, is in a bed or a cage, or is marked carried by nobody.
int KenshiWorld::PostureMismatches(std::string& keys) {
    int n = 0;
    for (const auto& [h, t] : lastTarget_) {
        if (t.flags & (kc::kFlagDown | kc::kFlagDead)) continue;
        if (hostCaptive_.count(h) || captiveHold_.count(h)) continue;
        kenshi::Character* c = Find(h);
        if (!c) continue;
        int in = 0;
        kenshi::ReadInSomething(c, in);
        const bool carried = carriedHere_.count(h) != 0;
        const bool down = !carried && (kenshi::IsRagdoll(c) || kenshi::IsDead(c));
        kenshi::AnimModes m;
        const bool hangs = !carried && kenshi::ReadAnimModes(c, m) && m.carried;   // "being carried" by nobody
        if (!down && in == 0 && !hangs && !(carried && !StillCarried(h, c))) continue;
        ++n;
        if (keys.size() < 600) keys += " " + KeyOf(h) + (down ? "/down" : "") + (in ? "/in" + std::to_string(in) : "") + (hangs ? "/carriedanim" : "");
    }
    return n;
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

bool KenshiWorld::WornBackpack(const kc::Handle& wearer, kc::Handle& bag, std::string& sid) {
    void* b = kenshi::WornBackpack(Find(wearer));
    return b && kenshi::ObjectHandle(b, bag) && kenshi::ObjectTemplate(b, sid);
}

// The host's handles of the squad members a travelling merchant sells from (their worn backpacks).
bool KenshiWorld::TravellingCounters(const kc::Handle& trader, std::vector<kc::Handle>& wearers) {
    wearers.clear();
    std::vector<kenshi::Character*> found;
    if (!kenshi::TravellingWearers(Find(trader), found)) return false;
    for (kenshi::Character* c : found) {
        kc::Handle h;
        if (kenshi::GetHandle(c, h)) wearers.push_back(HostHandleOf(h));
    }
    return !wearers.empty();
}

bool KenshiWorld::IsAnimal(const kc::Handle& h) { return kenshi::IsAnimal(Find(h)); }
bool KenshiWorld::ReadBodySize(const kc::Handle& h, kc::loot::BodySize& out) {
    kenshi::Character* c = Find(h);
    return c && kenshi::ReadBodySize(c, out);
}

bool KenshiWorld::JoinSquadOf(const kc::Handle& who, const kc::Handle& leader) {
    kenshi::Character* c = Find(who);
    kenshi::Character* l = Find(leader);
    void* target = l ? kenshi::SquadOf(l) : nullptr;
    if (!c || !target || kenshi::SquadOf(c) == target) return false;
    std::vector<kenshi::Character*> members;
    kenshi::SquadMembers(target, members);
    bool ok = false;
    HostCallScope scope;
    kenshi::KeepSelection([&] { ok = kenshi::MoveToSquad(target, c, int(members.size())); });
    return ok;
}

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

bool KenshiWorld::OpenTradeWindow(const kc::Handle& looter, const kc::Handle& trader, const std::vector<kc::Handle>& counters) {
    kenshi::Character* t = Find(trader);
    // The game builds the window from the merchant's home building. Ours may be a stand-in made for
    // the host's merchant (its town was generated apart here: other handles), without a home: the
    // window stayed empty. It gets the building of the counters the host sells from.
    std::vector<void*> local;
    for (const kc::Handle& h : counters)
        if (void* c = kenshi::ResolveObject(h)) local.push_back(c);
    if (t && !local.empty()) {
        const int r = kenshi::GiveShopHome(t, local);
        if (r > 0) Log("trade: the merchant here did not sell from the host's %zu counter(s) (a stand-in without that shop as its home): given the shop's building", local.size());
        else if (r < 0) Log("trade: the merchant here does not sell from the host's %zu counter(s) and could not be given their building: the window may stay empty", local.size());
    }
    return kenshi::OpenTradeWindow(Find(looter), t);
}

int KenshiWorld::TradeWindowStock() {
    std::vector<kenshi::WindowItem> items;
    return kenshi::TradeWindowItems(true, items) ? int(items.size()) : -1;
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
        // Put down without its ragdoll: our carrier is not exactly where the host's is, so a body
        // falling from our shoulder landed a dozen units from the host's. The posture code then
        // moves it where the host's body is, lets the pose settle and only then starts the ragdoll
        // (ReadyToFall): a teleport while a ragdoll sets up is what threw bodies away.
        const kc::Handle dropped = HostHandleOf(local);
        kenshi::Character* body = kenshi::Resolve(local);
        // A dead body is put down in ragdoll at once: a corpse is never stood up, even for a moment.
        const bool deadBody = body && kenshi::IsDead(body);
        kenshi::DropCarried(c, deadBody);
        if (!deadBody && body && kenshi::IsRagdoll(body)) kenshi::SetRagdoll(body, false);   // the attach step left its flag on
        carriedHere_.erase(dropped);
        carrierOf_.erase(dropped);
        fellAt_.erase(dropped);
        fallPrep_.erase(dropped);
        lastDest_.erase(dropped);
        postureFixed_.erase(dropped);
        postureBook_.erase(dropped);   // put down by the host: a new fall, whatever happened before
        fallFrom_.erase(dropped);
        fallShift_.erase(dropped);
        Log("carry: %s%s puts %s down as on the host; it falls where the host's lies (ragdoll %d)", carry ? "swaps and " : "", KeyOf(h).c_str(),
            KeyOf(dropped).c_str(), body ? int(kenshi::IsRagdoll(body)) : -1);
    }
    if (carry && who && (!carrying || kenshi::Resolve(local) != who)) {
        // only once it really is on the shoulder is it left alone by posture and position code
        const bool ok = kenshi::CarryCharacter(c, who);
        if (ok) { carriedHere_.insert(carried); carrierOf_[carried] = h; }
        else { carriedHere_.erase(carried); carrierOf_.erase(carried); }
        fallPrep_.erase(carried);
        lastDest_.erase(carried);
        Log("carry: %s goes on the shoulder of %s as on the host (%s)", KeyOf(carried).c_str(), KeyOf(h).c_str(), ok ? "ok" : "refused by the game");
    } else if (carry && who) {
        carriedHere_.insert(carried);
        carrierOf_[carried] = h;
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
    // Disabled: the game puts its own value back every frame, and a client crashed (kenshi_x64+0x95484b,
    // read of -1) right after a burst of these writes inside a building. Not needed for the floor
    // view: the game computes each character's floor from where it stands, and the view follows the
    // focus character (UpdateFloorFocus).
    static bool kWriteFloors = false;
    if (!kWriteFloors) { (void)h; (void)group; return; }
    kenshi::Character* c = Find(h);
    int32_t g = 0;
    if (!c || !kenshi::ReadFloorGroup(c, g) || g == group) { floorTry_.erase(h); return; }
    // Not a raw write of floorGroup (the game put its value back every frame): the game's own
    // _setPositionAndTeleport(here, floor), at most every 2 s per character and 3 times per
    // target value. Groups below 9 have no floor number; a ragdoll is not teleported.
    if (group < 9 || kenshi::IsRagdoll(c)) return;
    const double now = NowSeconds();
    FloorTry& t = floorTry_[h];
    if (t.group != group) t = FloorTry{group, 0, 0};
    if (t.tries >= 3 || now < t.at) return;
    t.at = now + 2.0;
    ++t.tries;
    HostCallScope scope;
    const bool ok = kenshi::PlaceOnFloor(c, group);
    int32_t after = g;
    kenshi::ReadFloorGroup(c, after);
    Log("floor: %s placed on floor group %d as on the host (was %d, now %d%s)%s", KeyOf(h).c_str(), int(group), int(g), int(after),
        ok ? "" : ", call failed", t.tries >= 3 && after != group ? ": the game keeps its own floor, giving up" : "");
}

// The floor view (which floors and roofs show) follows the PlayerInterface's tracked character as it
// takes stairs or enters a building, as in solo. That hand (+0x270) is the camera's "track this
// character" (PlayerInterface::startTrackCharacter 0x7F5400 / stopTrackCharacter 0x7F2780): the game
// drops it (type 0xB) whenever the player moves the camera himself (CameraClass::move, 0x6AE5D1, and
// teleport, 0x6AF934, call stopTrackCharacter). Taking it back every 0.5 s (0.3.1) fought the
// player's camera all the time ("follows our 'Geoffrey' (it followed nobody)" twice a second, a
// camera that "goes everywhere", 11 October). So it is set once per loaded world: a client's game
// keeps the focus of the save it loaded (the host's character, or nothing), pointed at one of ours,
// the selected one first; taken back afterwards only if it is found on that save's character again
// (a few times at most). Nothing (the player moved the camera) or one of ours is the player's choice.
// The roofs above our character are hidden either way (UpdateRoof), without touching the focus.
void KenshiWorld::UpdateFloorFocus() {
    const double now = NowSeconds();
    if (now < nextFocusCheck_) return;
    nextFocusCheck_ = now + 0.5;
    constexpr int kMaxRetakes = 3;
    std::vector<std::pair<kc::Handle, kenshi::Character*>> mine;   // local handles
    for (const kc::Handle& h : controllable_) {
        kc::Handle local;
        if (kenshi::Character* c = Find(h); c && kenshi::GetHandle(c, local)) mine.emplace_back(local, c);
    }
    if (mine.empty()) return;
    kc::Handle focus;
    const bool hasFocus = kenshi::FloorFocus(focus);
    kenshi::Character* followed = nullptr;
    for (const auto& [l, c] : mine)
        if (hasFocus && l == focus) followed = c;
    std::vector<kc::Handle> selected;
    kenshi::SelectedHandles(selected);
    kenshi::Character* pick = mine.front().second;
    for (const auto& [l, c] : mine)
        if (std::find(selected.begin(), selected.end(), l) != selected.end()) { pick = c; break; }
    if (!focusPlaced_ && followed) {
        focusPlaced_ = true;   // already one of ours (the player double-tapped a squad key before we looked)
    } else if (!followed) {
        const bool first = !focusPlaced_;
        if (first) loadFocus_ = hasFocus ? focus : kc::Handle{};
        const bool backOnSave = focusPlaced_ && hasFocus && loadFocus_.valid() && focus == loadFocus_ && focusRetakes_ < kMaxRetakes;
        if ((first || backOnSave) && kenshi::SetFloorFocus(pick)) {
            focusPlaced_ = true;
            if (backOnSave) ++focusRetakes_;
            followed = pick;
            std::string name;
            kenshi::CharacterName(pick, name);
            Log("floor view: follows our '%s' (it followed %s%s); the camera is the player's from now on", name.c_str(),
                hasFocus ? KeyOf(focus).c_str() : "nobody", backOnSave ? ", the save's character again" : "");
        }
    }
    UpdateRoof(followed ? followed : pick);
}

// Host: the same for the host's selected (or else followed) character. The host's game hides the
// floors above it only in the buildings its own build mode made: one a client placed (made here by
// the mod) kept its roof over the host's character inside (11 October).
void KenshiWorld::UpdateHostRoof() {
    const double now = NowSeconds();
    if (now < nextHostRoofCheck_) return;
    nextHostRoofCheck_ = now + 0.5;
    kenshi::Character* pick = nullptr;
    std::vector<kc::Handle> selected;   // the selected character first (the one the player plays now)
    kenshi::SelectedHandles(selected);
    for (const kc::Handle& h : selected)
        if ((pick = kenshi::Resolve(h)) != nullptr) break;
    kc::Handle focus;
    if (!pick && kenshi::FloorFocus(focus)) pick = kenshi::Resolve(focus);
    UpdateRoof(pick);
}

// The floors above the one our followed character is on, in the building it is inside, hidden as the
// host's game hides them for its own (a cabin's roof is its floor 1: 4092 / 4093-TwoStorey.mod). The
// client's game did not: the player stood inside under a roof that never went (11 October; the floor
// view's numbers were the host's, the building and floor of the character right). Shown again when
// it goes out or into another building.
void KenshiWorld::UpdateRoof(kenshi::Character* c) {
    constexpr int kMaxFloors = 6;
    kc::Handle in;
    int32_t group = 0;
    void* building = nullptr;
    int floor = 0;
    if (c && kenshi::IndoorsOf(c, in) && in.type != 0xB && in.valid() && kenshi::ReadFloorGroup(c, group) && group >= 9) {
        building = kenshi::ResolveObject(in);
        floor = group - 9;
    }
    if (roofBuilding_ && (roofBuilding_ != building || roofFloor_ != floor)) {
        if (kenshi::ResolveObject(roofHandle_) == roofBuilding_)
            for (int f = roofFloor_ + 1; f < kMaxFloors; ++f) kenshi::SetBuildingFloorVisible(roofBuilding_, f, true);
        roofBuilding_ = nullptr;
    }
    if (building && building != roofBuilding_) {
        for (int f = floor + 1; f < kMaxFloors; ++f) kenshi::SetBuildingFloorVisible(building, f, false);
        roofBuilding_ = building;
        roofHandle_ = in;
        roofFloor_ = floor;
        std::string sid;
        kenshi::ObjectTemplate(building, sid);
        Log("floor view: inside %s on floor %d, the floors above hidden", sid.c_str(), floor);
    }
}

// ---- fix G5: job lists
bool KenshiWorld::ReadJobs(const kc::Handle& h, std::vector<int32_t>& jobs) {
    jobs.clear();
    kenshi::Character* c = FindSquad(h);
    if (!c) return false;
    const int n = kenshi::PermajobCount(c);
    for (int i = 0; i < n; ++i) jobs.push_back(kenshi::PermajobType(c, i));
    return true;
}

// Client: the jobs our copy has that the host's character no longer has are removed (a job never
// comes back once the host dropped it). Jobs are never added here: the host's game runs them.
void KenshiWorld::ApplyJobs(const kc::Handle& h, const std::vector<int32_t>& jobs) {
    kenshi::Character* c = FindSquad(h);
    if (!c || JobEditPending(h)) return;
    std::unordered_map<int32_t, int> left;
    for (int32_t j : jobs) ++left[j];
    // a job given here a moment ago: the host's list may not have it yet
    auto lj = localJobs_.find(CharKey(h));
    if (lj != localJobs_.end()) {
        const unsigned long long now = GetTickCount64();
        auto& v = lj->second;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const std::pair<int, unsigned long long>& p) {
                    return p.second < now || std::count(jobs.begin(), jobs.end(), p.first) > 0;
                }), v.end());
        for (const auto& p : v) ++left[p.first];
        if (v.empty()) localJobs_.erase(lj);
    }
    std::vector<int> drop;
    const int n = kenshi::PermajobCount(c);
    for (int i = 0; i < n; ++i) {
        auto it = left.find(kenshi::PermajobType(c, i));
        if (it != left.end() && it->second > 0) --it->second;
        else drop.push_back(i);
    }
    if (drop.empty()) return;
    HostCallScope scope;
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) kenshi::RemovePermajob(c, *it);
    Log("jobs: %s had %zu job(s) the host's character no longer has: removed", KeyOf(h).c_str(), drop.size());
}

// Client: the host rejected an order of ours: a job we showed at once (the Tâches panel) is not
// kept waiting for the host's list any longer (the next list removes it).
void KenshiWorld::OrderRejected(const kc::Handle& h, const kc::Command& c) {
    if (c.kind == kc::CommandKind::Task && (c.via == kc::TaskVia::AddJob || (c.via == kc::TaskVia::NewTask && c.shift))) localJobs_.erase(CharKey(h));
    // a job removal or move the host did not make: its lists apply again at once (our copy goes back)
    if (c.kind == kc::CommandKind::Task && (c.via == kc::TaskVia::RemovePermajob || c.via == kc::TaskVia::MovePermajob || c.via == kc::TaskVia::RemoveJob))
        jobEdits_.erase(CharKey(h));
    // a toggle the host did not set: the host's modes apply again at once
    if (c.kind == kc::CommandKind::Task && c.via == kc::TaskVia::SetOrder) modePredict_.Forget(CharKey(h));
    Log("the host rejected our order (task %d, via %d)", c.task, int(c.via));
}

void KenshiWorld::NoteLocalJob(const kc::Handle& h, int task) {
    localJobs_[CharKey(h)].push_back({task, GetTickCount64() + 5000});
}

// The character itself, whatever handle names it: our local one (the hooks', the selection's) or
// the host's (the session's, which Find resolves through its alias).
std::string KenshiWorld::CharKey(const kc::Handle& h) {
    if (kenshi::Character* c = Find(h)) {
        char b[24];
        snprintf(b, sizeof(b), "%p", static_cast<void*>(c));
        return b;
    }
    return KeyOf(h);
}

void KenshiWorld::PredictMode(const kc::Handle& h, uint16_t bit, bool on) { modePredict_.Predict(CharKey(h), bit, on, NowSeconds()); }

void KenshiWorld::NoteJobEdit(const kc::Handle& h) {
    if (jobEdits_.size() > 256) jobEdits_.clear();
    jobEdits_[CharKey(h)] = NowSeconds() + 3.0;   // the host's answer and its next lists (once a second) are in by then
}

bool KenshiWorld::JobEditPending(const kc::Handle& h) {
    auto it = jobEdits_.find(CharKey(h));
    if (it == jobEdits_.end()) return false;
    if (NowSeconds() < it->second) return true;
    jobEdits_.erase(it);
    return false;
}

int KenshiWorld::TeleportCharacters(const std::vector<kc::Handle>& who, const kc::Vec3& to, bool spread) {
    int n = 0;
    HostCallScope scope;
    std::vector<kenshi::Character*> all;
    const double now = NowSeconds();
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
        const kc::Vec3 p = spread ? kc::Vec3{to.x + 6.0f * float(n + 1), to.y + 2.0f, to.z + 4.0f} : to;
        // The engine does not move an active ragdoll, even one switched off in the same frame: the
        // teleport "took" (the character's position read right), then the body lying down again took
        // the ragdoll's own place back, where it lay before (the 0.3.1 suite: a knocked-out player's
        // character 392 units from the host after its admin TP). A body is stood up now, teleported on
        // a later frame, laid down there, and checked once lying (UpdateReRagdolls).
        const bool body = kenshi::IsRagdoll(c);
        ReRagdoll r{h, 0, p, rot, body, 0, c};
        // The walk and the orders it had end here, as with the game's "stop" (A_FAIRE 12/10 point 10:
        // after "TP moi" nass4's characters ran back to where they were going, and the suite's "TP
        // admin" check found the player's character 193-597 units from the host). Its permanent jobs
        // stay: it goes back to them by itself, from its new place.
        const bool stopped = kenshi::StopCharacter(c);
        if (body) {
            kenshi::SetRagdoll(c, false);
            r.stage = ReRagdoll::Stand;
            r.at = now + 0.15;
            ++n;
            Log("tp: %s lies on the ground: stood up, teleported next", KeyOf(h).c_str());
        } else if (kenshi::Teleport(c, p, rot)) {
            r.stage = ReRagdoll::Check;
            r.at = now + 0.3;   // checked a moment later: a character the game was still moving may stay put
            ++n;
            Log("tp: %s moved (walk and orders %s)", KeyOf(h).c_str(), stopped ? "ended" : "NOT ended: call failed");
        } else {
            continue;
        }
        reRagdoll_.erase(std::remove_if(reRagdoll_.begin(), reRagdoll_.end(), [&](const ReRagdoll& o) { return o.h == h; }), reRagdoll_.end());
        reRagdoll_.push_back(r);
    }
    return n;
}

void KenshiWorld::UpdateReRagdolls() {
    const double now = NowSeconds();
    HostCallScope scope;
    constexpr float kThere = 15.0f;
    constexpr int kTries = 6;
    for (auto it = reRagdoll_.begin(); it != reRagdoll_.end();) {
        if (now < it->at) { ++it; continue; }
        kenshi::Character* c = FindSquad(it->h);
        if (!c) { it = reRagdoll_.erase(it); continue; }
        kc::Vec3 at;
        const bool known = kenshi::GetPosition(c, at);
        const float off = known ? Dist(at, it->to) : 1e9f;
        const bool down = it->body && (kenshi::IsUnconscious(c) || kenshi::IsDead(c));
        switch (it->stage) {
        case ReRagdoll::Stand:
            // fix G7: still a ragdoll (the game laid it down again): stood up once more first
            if (kenshi::IsRagdoll(c) && ++it->tries < kTries) {
                kenshi::SetRagdoll(c, false);
                it->at = now + 0.15;
                break;
            }
            kenshi::Teleport(c, it->to, it->rot);
            it->stage = ReRagdoll::Check;
            it->at = now + 0.3;
            break;
        case ReRagdoll::Check:
            // still walking off (an order given in the same frame, an AI decision): stopped once more
            if (!it->body && kenshi::IsMoving(c) && !it->restopped) {
                it->restopped = true;
                Log("tp: %s still walking %.0f units from where it was sent: stopped again (%s)", KeyOf(it->h).c_str(), off,
                    kenshi::StopCharacter(c) ? "ok" : "call failed");
            }
            if (off >= kThere && ++it->tries < kTries) {
                if (kenshi::IsRagdoll(c)) {
                    kenshi::SetRagdoll(c, false);
                    it->stage = ReRagdoll::Stand;
                    it->at = now + 0.15;
                    Log("tp: %s still %.0f away and a ragdoll: stood up, teleported next (%d)", KeyOf(it->h).c_str(), off, it->tries);
                } else {
                    kenshi::Teleport(c, it->to, it->rot);
                    it->at = now + 0.4;
                    Log("tp: %s still %.0f away from where it was sent: teleported again (%d)", KeyOf(it->h).c_str(), off, it->tries);
                }
                break;
            }
            if (off >= kThere) Log("tp: %s did not reach its destination (%.0f away)", KeyOf(it->h).c_str(), off);
            if (down && !kenshi::IsRagdoll(c) && !it->noRelay) {
                kenshi::SetRagdoll(c, true);   // knocked out or dead: lying again, where it was sent
                it->stage = ReRagdoll::Settle;
                it->at = now + 0.6;
                break;
            }
            it = reRagdoll_.erase(it);
            continue;
        case ReRagdoll::Settle:
            // lying again: still there? A ragdoll that took its old place back is stood up and sent
            // again, and then left standing for the game to lay down (its own fall starts where it is)
            if (off >= kThere && ++it->tries < kTries) {
                Log("tp: %s went back %.0f away when it lay down again: stood up and sent again", KeyOf(it->h).c_str(), off);
                kenshi::SetRagdoll(c, false);
                it->stage = ReRagdoll::Stand;
                it->noRelay = true;
                it->at = now + 0.15;
                break;
            }
            it = reRagdoll_.erase(it);
            continue;
        }
        ++it;
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
    modes = modePredict_.Filter(CharKey(h), modes, NowSeconds());   // a toggle just clicked here: kept until the host's has it
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

void KenshiWorld::ApplyVitals(const kc::Handle& h, const kc::EntityVitals& hostVitals) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    // never revived: a body the host's state says is dead stays dead, whatever these values say
    kc::EntityVitals v = hostVitals;
    if (auto st = lastTarget_.find(h); st != lastTarget_.end()) v.flags = kc::motion::VitalsFlagsFor(v.flags, st->second.flags);
    kenshi::WriteVitals(c, v);   // values only: clients never fall over on their own (see hk_ragdollMode)
    // the posture code lays a knocked-out body down unconscious, or the local game stands it up again
    if ((v.flags & kc::kVitUnconscious) && !(v.flags & kc::kVitDead)) hostKnockedOut_.insert(h);
    else hostKnockedOut_.erase(h);
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

// Host (0.3.1): characters near a client player's own characters, resolved once per interest update.
void KenshiWorld::SetRemoteInterest(const std::vector<kc::Handle>& handles) {
    auto set = std::make_shared<std::unordered_set<const void*>>();
    if (!client_)
        for (const kc::Handle& h : handles)
            if (kenshi::Character* c = Find(h)) set->insert(c);
    remoteNear_ = std::move(set);
}

// Client (3b): the host's characters it no longer streams to us; see Session::UpdateFarCharacters.
// Resolved here, on the game thread; the addresses are only looked up by hk_charUpdateView, never
// read, and an address the game frees leaves the set at once (PurgeDestroyed).
void KenshiWorld::SetFarCharacters(const std::vector<kc::Handle>& handles) {
    if (!client_ || handles.empty()) {
        farSet_.reset();
        return;
    }
    auto set = std::make_shared<std::unordered_set<const void*>>();
    set->reserve(handles.size());
    for (const kc::Handle& h : handles)
        if (kenshi::Character* c = Find(h)) set->insert(c);
    farSet_ = std::move(set);
}

void KenshiWorld::ResyncCharacter(const kc::Handle& h) {
    if (client_) resyncSnap_.insert(h);
}

void KenshiWorld::NoteCharacterDestroyed(void* chr, bool justUnloaded, const char* info) {
    {
        std::lock_guard<std::mutex> lk(destroyedMutex_);
        if (destroyedPtrs_.size() < 100000) destroyedPtrs_.push_back(chr);
    }
    kc::Handle h;
    if (!kenshi::GetHandle(static_cast<kenshi::Character*>(chr), h) || !h.valid()) return;
    std::string why = justUnloaded ? "unloaded with its zone by our game" : "destroyed by our game";
    if (info) {
        const size_t n = strnlen(info, 60);
        if (n) why += " (" + std::string(info, n) + ")";
    }
    std::lock_guard<std::mutex> lk(destroyedMutex_);
    if (destroyed_.size() > 512) destroyed_.clear();
    destroyed_[h] = std::move(why);
}

// Game thread: forget every address of a character the game destroyed (see destroyedPtrs_).
void KenshiWorld::PurgeDestroyed() {
    std::vector<const void*> gone;
    {
        std::lock_guard<std::mutex> lk(destroyedMutex_);
        if (destroyedPtrs_.empty()) return;
        gone.swap(destroyedPtrs_);
    }
    const std::unordered_set<const void*> dead(gone.begin(), gone.end());
    uint64_t n = 0;
    auto drop = [&](auto& map) {
        for (auto it = map.begin(); it != map.end();) {
            if (dead.count(it->first)) { it = map.erase(it); ++n; }
            else ++it;
        }
    };
    drop(animTargets_);
    drop(replicatedAt_);
    drop(lastFloater_);
    for (auto* map : {&squad_, &resolved_})   // keyed by handle, the address is the value
        for (auto it = map->begin(); it != map->end();) {
            if (dead.count(it->second)) { it = map->erase(it); ++n; }
            else ++it;
        }
    {   // host: animation events not taken yet, and the last one per character
        std::lock_guard<std::mutex> lk(animMutex_);
        const size_t before = animOut_.size();
        animOut_.erase(std::remove_if(animOut_.begin(), animOut_.end(), [&](const auto& e) { return dead.count(e.first) != 0; }), animOut_.end());
        n += before - animOut_.size();
        drop(animLast_);
    }
    for (auto it = applied_.begin(); it != applied_.end();) {
        if (dead.count(*it)) { it = applied_.erase(it); ++n; }
        else ++it;
    }
    for (auto it = handTools_.begin(); it != handTools_.end();) {
        if (dead.count(it->second.who)) { it = handTools_.erase(it); ++n; }
        else ++it;
    }
    if (farSet_) {   // 3b: an address of the far set may come back as another, near, character
        size_t hit = 0;
        for (const void* p : gone) hit += farSet_->count(p);
        if (hit) {
            auto kept = std::make_shared<std::unordered_set<const void*>>(*farSet_);
            for (const void* p : gone) kept->erase(p);
            farSet_ = std::move(kept);
            n += hit;
        }
    }
    const size_t editedBefore = edited_.size();
    edited_.erase(std::remove_if(edited_.begin(), edited_.end(), [&](kenshi::Character* c) { return dead.count(c) != 0; }), edited_.end());
    n += editedBefore - edited_.size();
    animCreateTried_.clear();   // keyed by address too (a new character may reuse one)
    bySerial_.clear();
    bySerialBuilt_ = false;
    if (n) {
        purgedPointers += n;
        // the view the hooks read named them too: published again without them
        auto v = std::make_shared<HookView>(*View());
        for (const void* c : dead) {
            v->replicated.erase(c);
            v->squadForeign.erase(c);
        }
        // keyed by their animation and movement objects (freed with them): rebuilt by the next EndFrame
        v->anims.clear();
        v->facing.clear();
        view_.store(std::shared_ptr<const HookView>(std::move(v)), std::memory_order_release);
    }
}

// A gone player's character (left, crashed, connection lost): it stops walking and drops the task
// and the pick up it was given, and stays where it is. It still defends itself; what it carries
// stays carried. Never on a dead one.
void KenshiWorld::HaltCharacter(const kc::Handle& h) {
    kenshi::Character* c = FindSquad(h);
    pickups_.erase(h);
    if (!c || kenshi::IsDead(c)) return;
    HostCallScope scope;
    kenshi::Halt(c);
    kenshi::DropLocalTasks(c);
    Log("player gone: %s halted where it stands", KeyOf(h).c_str());
}

// What the subject of a client's order is (kc::TargetFlags), as this world sees it.
uint32_t KenshiWorld::TargetFlagsOf(kenshi::Character* actor, void* subject, bool named) {
    uint32_t f = named ? kc::kTgtNamed : 0;
    if (!subject) return f;
    f |= kc::kTgtNamed | kc::kTgtFound;
    if (kenshi::IsCharacter(subject)) {
        auto* ch = static_cast<kenshi::Character*>(subject);
        f |= kc::kTgtCharacter;
        if (ch == actor) f |= kc::kTgtSelf;
        const bool dead = kenshi::IsDead(ch);
        const bool down = kenshi::IsDown(ch) || kenshi::IsUnconscious(ch) || kenshi::IsRagdoll(ch);
        if (dead) f |= kc::kTgtDead;
        if (down) f |= kc::kTgtDown;
        if (!dead && !down) f |= kc::kTgtConscious;
        kc::Handle h;
        if (kenshi::GetHandle(ch, h) && FindSquad(h)) f |= kc::kTgtSquad;
        return f;
    }
    if (kenshi::ItemLoose(subject)) f |= kc::kTgtItem;
    kc::Handle h;
    const bool building = kenshi::ObjectHandle(subject, h) && h.type == 0;
    if (building) {
        f |= kc::kTgtBuilding;
        float progress = 0;
        uint8_t site = 0;
        if (ReadBuildStateOf(subject, progress, site) && !(site & kc::kSiteComplete)) f |= kc::kTgtUnfinished;
        if (IsPlayerBuilding(subject)) f |= kc::kTgtOurs;
        const int fn = kenshi::BuildingFunctionOf(subject);   // 1 mine, 6 bed, 8 cage, 9 shop, 12 turret, 18 decor, 27 natural deposit
        if (fn == 6) f |= kc::kTgtBed;
        else if (fn == 8) f |= kc::kTgtCage;
        else if (fn == 9) f |= kc::kTgtShop;
        else if (fn > 0 && fn != 18) f |= kc::kTgtMachine;
        kc::DoorState d;
        if (kenshi::ReadDoor(subject, d)) f |= kc::kTgtDoor;
    }
    std::vector<kc::ItemState> items;
    if (!(f & kc::kTgtItem) && kenshi::ReadInventory(subject, items)) f |= kc::kTgtContainer;
    return f;
}

bool KenshiWorld::Order(const kc::Handle& h, const kc::Command& cmd) {
    orderRefusal_.clear();
    orderNote_.clear();
    kenshi::Character* c = FindSquad(h);
    if (!c) {
        // never a fallback (the host's selection, its main character, the nearest one): nothing
        Log("refused: client order not run, its actor does not resolve to a character here");
        orderRefusal_ = "Action refusée : ton personnage est introuvable chez l'hôte (rien n'a été fait).";
        orderRefusalReason_ = kc::ResultReason::NoActor;
        return false;
    }
    // A client's order only ever moves that client's characters: never one the host commands (a
    // stale handle after a squad change once made a player's orders move the host's character).
    // The session checked that the actor is that player's (Session::AdmitActor).
    if (!client_ && !sharedOrder_) {   // (a nobody's character's settings: OrderShared, checked by the session)
        auto v = View();
        if (!v->squadForeign.count(c)) {
            Log("refused: client order not run, the character its actor resolves to is not another player's (the host's?)");
            orderRefusal_ = "Action refusée : ce personnage n'est pas le tien.";
            orderRefusalReason_ = kc::ResultReason::NotYourCharacter;
            return false;
        }
    }
    // any other order of that character cancels a talk it was walking to (UpdatePendingTalks)
    if (!client_ && !(cmd.kind == kc::CommandKind::Task && (cmd.task == kc::kTaskTalk || cmd.task == kc::kTaskTalkNearest))) pendingTalks_.erase(h);
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
        // the item the client meant: same thing lying there. Not only "ground" items in the strict
        // sense: shop goods and town clutter sit in item groups or are non-physical props, and the
        // client's copy may be some way off the host's (positions drift), so: any loose item of
        // that kind, the nearest within 40 units.
        std::vector<void*> around;
        kenshi::LooseItemsNear(cmd.pos, 60.0f, around);
        void* item = nullptr;
        float best = 40.0f;
        kc::Handle itemHandle;
        for (void* it : around) {
            kc::Handle ih;
            kc::ItemState st;
            kc::Vec3 p;
            if (!kenshi::DescribeGroundItem(it, ih, st, p) || st.templateSid != cmd.itemSid) continue;
            const float d = Dist(p, cmd.pos);
            if (d < best) { best = d; item = it; itemHandle = ih; }
        }
        if (!item) {
            // say why: nothing loose there at all, or that kind only farther away (positions drift)
            std::vector<void*> wide;
            kenshi::LooseItemsNear(cmd.pos, 400.0f, wide);
            float nearest = 1e30f;
            size_t sameKind = 0;
            for (void* it : wide) {
                kc::Handle ih;
                kc::ItemState st;
                kc::Vec3 p;
                if (!kenshi::DescribeGroundItem(it, ih, st, p) || st.templateSid != cmd.itemSid) continue;
                ++sameKind;
                nearest = std::min(nearest, Dist(p, cmd.pos));
            }
            Log("client pick up FAILED: no %s (%s) within 40 units of (%.0f,%.0f,%.0f): %zu loose items within 60, %zu of that kind within 400 (nearest %.0f)",
                TemplateName(cmd.itemSid).c_str(), cmd.itemSid.c_str(), cmd.pos.x, cmd.pos.y, cmd.pos.z, around.size(), sameKind,
                sameKind ? nearest : -1.0f);
            return false;
        }
        kc::Vec3 cp;
        const float startDist = kenshi::GetPosition(c, cp) ? Dist(cp, cmd.pos) : -1.0f;
        // the game's own order, as when the host clicks: it walks there and takes it, stealing it
        // (with the guards' reaction) when it is someone's. Watched: if the character does not move
        // off within a few seconds, our own walk-and-take takes over.
        const double now = NowSeconds();
        if (kenshi::OrderPickupItem(c, item)) {
            Log("client pick up: the game's pick up order given (item %.0f units from the client's spot, character %.0f units away)", best, startDist);
            pickups_[h] = {itemHandle, now + 30.0, true, now + 4.0, startDist};
            return true;
        }
        Log("client pick up: the game's pick up order could not be given (no player interface, or the item is not loose); walking the character there");
        pickups_[h] = {itemHandle, now + 30.0};
        return CallPlayerMoveOrder(c, cmd.pos);
    }
    case kc::CommandKind::SquadMove: {
        // the squad to join: another member of the player faction's squads, never an NPC's squad
        kenshi::Character* other = cmd.subject.valid() ? FindSquad(cmd.subject) : nullptr;
        if (cmd.subject.valid() && !other) {
            ++targetRefusals_;
            Log("refused: client squad change not run: the squad member it names is not in the player faction here");
            orderRefusal_ = "Action refusée : cette escouade n'est pas celle d'un joueur (rien n'a été fait).";
            orderRefusalReason_ = kc::ResultReason::WrongTarget;
            return false;
        }
        // creating or filling a squad selects it in the host's squad bar: the host's selection stays
        bool ok = false;
        kenshi::KeepSelection([&] {
            void* target = other ? kenshi::SquadOf(other) : kenshi::NewSquad();
            ok = kenshi::MoveToSquad(target, c, cmd.task);
        });
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
        // 0.3.4: "talk" names the NPC by its kind and where the client saw it too (a client's copy not
        // linked to the host's NPC has a handle that means nothing here, or another character)
        const bool talk = cmd.task == kc::kTaskTalk || cmd.task == kc::kTaskTalkNearest;
        if (talk && !cmd.itemSid.empty()) {
            if (subject && kenshi::IsCharacter(subject)) {
                std::string s;
                kc::Vec3 p;
                const bool same = kenshi::ObjectTemplate(subject, s) && kenshi::ObjectPosition(subject, p) &&
                                  kc::talk::SameNpc(s == cmd.itemSid, Dist(p, cmd.subjectPos));
                if (!same) {
                    Log("client talk: its subject handle names another character here (%s): looked for by kind and place", TemplateName(s).c_str());
                    subject = nullptr;
                }
            } else if (subject) {
                subject = nullptr;   // not a character: looked for below
            }
            if (!subject) {
                subject = NpcByKindAndPlace(cmd.itemSid, cmd.subjectPos, c);
                Log("client talk: the NPC (%s) %sfound by kind and place", TemplateName(cmd.itemSid).c_str(), subject ? "" : "NOT ");
            }
        }
        if (!talk && cmd.subject.valid() && !subject && !cmd.itemSid.empty()) {
            subject = byKindAndPlace(cmd.itemSid, cmd.subjectPos);
            Log("client task %d: subject %sfound by kind and place", cmd.task, subject ? "" : "NOT ");
        }
        if (cmd.building.valid() && !building && !cmd.buildingSid.empty()) {
            building = byKindAndPlace(cmd.buildingSid, cmd.buildingPos);
            Log("client task %d: destination building %sfound by kind and place", cmd.task, building ? "" : "NOT ");
        }
        if (cmd.subject.valid() && !subject) Log("client task %d: its subject is not in the host's world", cmd.task);
        if (building) {   // the destination must be a building (a stale or forged handle: none at all)
            kc::Handle bh;
            if (kenshi::IsCharacter(building) || !kenshi::ObjectHandle(building, bh) || bh.type != 0) {
                Log("client task %d: its destination is not a building here: dropped", cmd.task);
                building = nullptr;
            }
        }
        // actor safety: what the task expects, checked before the game's order functions get it (a
        // BUILD order aimed at an NPC crashed the host inside Character::addJob)
        const uint32_t flags = TargetFlagsOf(c, subject, cmd.subject.valid() || !cmd.itemSid.empty());
        std::string why;
        if (!kc::TaskTargetAllowed(cmd.via, cmd.task, flags, &why)) {
            ++targetRefusals_;
            Log("refused: client task %d (via %d) not run: %s (subject flags %#x)", cmd.task, int(cmd.via), why.c_str(), unsigned(flags));
            orderRefusal_ = "Action refusée : cet ordre ne s'applique pas à cette cible (rien n'a été fait).";
            orderRefusalReason_ = kc::ResultReason::WrongTarget;
            return false;
        }
        // what the order makes of the character's job list (Tâches panel): said to the player below
        const bool askOrder = cmd.via == kc::TaskVia::AddOrder || cmd.via == kc::TaskVia::NewTask || cmd.via == kc::TaskVia::TaskNearest ||
                              cmd.via == kc::TaskVia::AddJob;
        const int jobsBefore = kenshi::PermajobCount(c);
        bool hadSameKind = false;
        for (int i = 0; i < jobsBefore && !hadSameKind; ++i) hadSameKind = kenshi::PermajobType(c, i) == cmd.task;
        const bool ok = RunPlayerTask(c, cmd, subject, building, &orderRefusal_);
        if (!ok && !orderRefusal_.empty()) orderRefusalReason_ = kc::ResultReason::SelectionBusy;
        if (!client_ && ok && (cmd.task == kc::kTaskTalk || cmd.task == kc::kTaskTalkNearest) && subject && kenshi::IsCharacter(subject)) {
            kc::Handle nh;
            if (kenshi::GetHandle(static_cast<kenshi::Character*>(subject), nh)) pendingTalks_[h] = {nh, NowSeconds() + 60.0, 0};
        }
        Log("client task %d (via %d) run for a character: %s", cmd.task, int(cmd.via), ok ? "ok" : "failed");
        // What the player is told (kc::WorkOrderNote): a permanent job (Shift + right click, the Tâches
        // panel) added or not, and why it will wait (no free place, nothing to do, JOBS off); a one-time
        // "operate" on a machine with no work for now, no free place, or that this character already
        // works (the game drops the task or changes nothing: UseableStuff::tryOperate).
        const bool permanent = askOrder && cmd.shift;
        const bool operate = cmd.task == 87 || cmd.task == 152 || cmd.task == 221;
        if (ok && askOrder && (permanent || (operate && cmd.via != kc::TaskVia::AddJob))) {
            kc::WorkOrderFacts f;
            f.task = cmd.task;
            f.permanent = permanent;
            f.jobsBefore = jobsBefore;
            f.jobsAfter = kenshi::PermajobCount(c);
            f.hadSameKind = hadSameKind;
            f.jobsModeOn = kenshi::GetStandingOrder(c, 15);
            if (subject && !kenshi::IsCharacter(subject) && (permanent || operate)) FillMachineFacts(subject, c, f);
            orderNote_ = kc::WorkOrderNote(f);
            Log("work order %d%s: jobs %d -> %d%s%s%s, machine %s (%d/%d operators%s%s)", cmd.task, permanent ? " (permanent)" : " (one-time)", f.jobsBefore,
                f.jobsAfter, f.hadSameKind ? ", one of that kind already" : "", f.jobsModeOn ? "" : ", JOBS off", orderNote_.empty() ? "" : ", player told",
                f.machine ? f.machineName.c_str() : "none", f.operators, f.maxOperators, f.atWorkThere ? ", already at work there" : "",
                f.noWorkNow ? ", no work now" : "");
        }
        return ok;
    }
    }
    return false;
}

// ---- conversations
// Far from the host's camera the game walked a client's character to the NPC it was told to talk to
// and never ran the "arrived: talk" step (faraway test: six orders in a row at Admag and at Okran's
// Shield, the character standing next to the NPC, no conversation; near the host's camera it opens
// some 8 s after the order). Once the character has stood next to that NPC for 1.5 s with no
// conversation, it is started here, the game's own way (the NPC's default conversation with it,
// through our hooks: ConversationAllowed and the client's window apply as usual).
void KenshiWorld::UpdatePendingTalks() {
    constexpr float kTalkReach = 45.0f, kTalkStep = 40.0f;
    constexpr double kGameFirst = 1.5;
    const double now = NowSeconds();
    for (auto it = pendingTalks_.begin(); it != pendingTalks_.end();) {
        kenshi::Character* pc = FindSquad(it->first);
        kenshi::Character* npc = kenshi::Resolve(it->second.npc);
        void* d = npc ? kenshi::CharacterDialogue(npc) : nullptr;
        if (!pc || !npc || kenshi::IsDead(npc) || kenshi::IsDown(npc) || now > it->second.until ||
            (d && !kenshi::DialogueEnded(d) && kenshi::DialogueTarget(d) == pc)) {   // gone, too late, or the game did it
            it = pendingTalks_.erase(it);
            continue;
        }
        kc::Vec3 a, b;
        const bool close = kenshi::GetPosition(pc, a) && kenshi::GetPosition(npc, b) && std::hypot(a.x - b.x, a.z - b.z) < kTalkReach &&
                           std::fabs(a.y - b.y) < kTalkStep;
        if (!close) {
            it->second.nearSince = 0;
            ++it;
            continue;
        }
        if (it->second.nearSince == 0) it->second.nearSince = now;
        if (now - it->second.nearSince < kGameFirst) { ++it; continue; }   // the game's own step first
        bool started = false;
        {
            HostCallScope scope;
            kenshi::Halt(pc);
            started = kenshi::CallStartPlayerConversation(npc, pc);
            if (!started) started = kenshi::CallDialogueEvent(npc, pc, 1);   // EV_PLAYER_TALK_TO_ME, forced
        }
        Log("talk: %s next to its NPC for %.1f s without a conversation (far from the host's camera?): started by the mod (%s)",
            KeyOf(it->first).c_str(), kGameFirst, started ? "ok" : "the NPC has nothing to say");
        it = pendingTalks_.erase(it);
    }
}

// Host (0.3.4): the NPC of that kind nearest to where the client saw it (within kc::talk::kTalkFindRadius),
// never a player's character nor the one that would talk.
kenshi::Character* KenshiWorld::NpcByKindAndPlace(const std::string& sid, const kc::Vec3& at, kenshi::Character* actor) {
    if (sid.empty()) return nullptr;
    std::vector<kenshi::Character*> all;
    kenshi::ActiveCharacters(all);
    auto v = View();
    kenshi::Character* best = nullptr;
    float bestD = kc::talk::kTalkFindRadius;
    for (kenshi::Character* ch : all) {
        if (!ch || ch == actor || v->squadForeign.count(ch) || kenshi::IsDead(ch) || kenshi::InPlayerSquad(ch)) continue;
        std::string s;
        kc::Vec3 p;
        if (!kenshi::ObjectTemplate(ch, s) || s != sid || !kenshi::GetPosition(ch, p)) continue;
        const float d = Dist(p, at);
        if (d <= bestD) { bestD = d; best = ch; }
    }
    return best;
}

// Host, game thread (0.3.4): a conversation shown on a client's screen waits for the player's answer.
// Dialogue::update counts its line timers down while the game runs (the host's game is not paused by
// a client's window) and, once they ran out, ended the conversation or moved it on by itself: a
// guard's contraband check, its first line said aloud, closed 0.15 s and 1 s after it opened on
// rob1's and nass4's screens (12 October). The line timer is kept up while the player has answers to
// pick, as the single player pause keeps it (kc::talk::HoldLine).
void KenshiWorld::UpdateRemoteDialogs() {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lk(dialogMutex_);
    if (remoteDialogs_.empty()) return;
    // One conversation per player's character is on their screen: the one whose line came last (the
    // client shows the latest). An older one of the same character (a guard's check that goes on in
    // another dialogue, 12 October) is left to the game, which ends it or hands it over.
    std::unordered_map<kc::Handle, const RemoteDialog*, kc::HandleHash> latest;
    for (const auto& [dl, r] : remoteDialogs_) {
        const RemoteDialog*& l = latest[r.pcH];
        if (!l || r.waitingSince > l->waitingSince || (r.waitingSince == l->waitingSince && r.id > l->id)) l = &r;
    }
    for (auto& [dl, r] : remoteDialogs_) {
        if (latest[r.pcH] != &r) continue;
        const double waiting = now - r.waitingSince;
        if (!kc::talk::HoldLine(r.opened, r.replies, waiting)) {
            if (r.opened && r.replies && !r.holdReleased && waiting >= kc::talk::kMaxHold) {
                r.holdReleased = true;
                Log("conversation %u: no answer from the player for %.0f s: the game's timers run again", r.id, waiting);
            }
            continue;
        }
        // only a dialogue still owned by one of its two characters, both still there
        kenshi::Character* owner = kenshi::DialogueOwner(dl);
        kenshi::Character* pc = r.pcH.valid() ? kenshi::Resolve(r.pcH) : nullptr;
        kenshi::Character* other = r.otherH.valid() ? kenshi::Resolve(r.otherH) : nullptr;
        if (!owner || (owner != pc && owner != other) || kenshi::DialogueEnded(dl)) continue;
        kenshi::DialogueTimers t;
        if (!kenshi::ReadDialogueTimers(dl, t) || !t.label) continue;   // no label: the game counts nothing down
        if (std::isfinite(t.line) && t.line >= kc::talk::kHoldSeconds) continue;
        if (!kenshi::HoldDialogueLine(dl, kc::talk::kHoldSeconds)) continue;
        ++dialogLinesHeld;
        if (!r.holdLogged) {
            r.holdLogged = true;
            Log("conversation %u: line kept waiting for the player's answer (%zu answer(s); its timer was down to %.2f s, the game would have moved on)",
                r.id, r.replies, double(t.line));
        }
    }
}

// Client (0.3.4): RouteOrder could not give a talk order (none of the selected characters is ours...).
void KenshiWorld::TalkNotSent(void* target, const char* why) {
    ++talkClicksLogged;
    uint32_t held = 0;
    if (!talkClickLog_.Allow(NowSeconds(), &held)) return;
    std::string name;
    if (kenshi::IsCharacter(target)) kenshi::CharacterName(static_cast<kenshi::Character*>(target), name);
    Log("talk order to %s not sent: %s%s", name.empty() ? "?" : name.c_str(), why,
        held ? (" [" + std::to_string(held) + " more such lines not written]").c_str() : "");
}

// Client (0.3.4): a right click on a character gave no order at all (the game's own decision: it
// offers "talk" only when its check on our copy of the NPC says so). On 12 October Geoffrey (22:11
// to 22:15) and rob1 (23:00 to 23:10) could not talk to a merchant until a resync, and the host got no
// talk order from them. What we know of the NPC goes to the log; a standing NPC gets the talk order
// anyway, the host's world decides (kc::talk::DecideClick).
void KenshiWorld::ClickGaveNoOrder(kenshi::Character* t, bool otherMode) {
    if (!active_ || !client_ || !t) return;
    auto v = View();
    kc::Handle th;
    if (!kenshi::GetHandle(t, th) || !th.valid()) return;
    kc::talk::ClickFacts f;
    f.rightClick = true;
    f.character = true;
    f.otherMode = otherMode;
    f.animal = kenshi::IsAnimal(t);
    f.playerSide = v->squadForeign.count(t) || v->controllable.count(th) || FindSquad(th) || kenshi::InPlayerSquad(t);
    f.deadOrDown = kenshi::IsDead(t) || kenshi::IsDown(t) || kenshi::IsUnconscious(t) || kenshi::IsRagdoll(t);
    int in = 0;
    if (kenshi::ReadInSomething(t, in)) f.inSomething = in;
    kc::Vec3 tp{};
    const bool havePos = kenshi::GetPosition(t, tp);
    // our selected character nearest to it (the one the game would have sent)
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    kc::Handle actor;
    float actorD = -1;
    for (const auto& h : sel) {
        if (!v->controllable.count(h)) continue;
        kenshi::Character* ch = FindSquad(h);
        kc::Vec3 p;
        if (!ch) continue;
        const float d = havePos && kenshi::GetPosition(ch, p) ? Dist(p, tp) : 1e9f;
        if (!actor.valid() || d < actorD) { actor = h; actorD = d; }
    }
    f.ownSelected = actor.valid();
    const kc::talk::ClickDecision dec = kc::talk::DecideClick(f);
    if (dec.verdict == kc::talk::ClickVerdict::Ignore) return;
    // what we know of it: linked to the host's character (our stand-in for it, or the same handle the
    // host follows), driven by the host's state, captive here and on the host, the game's own check
    const kc::Handle host = HostHandleOf(th);
    const bool alias = !(host == th);
    const bool known = alias || HostKnows(th);
    const int canTalk = dec.verdict == kc::talk::ClickVerdict::SendTalk ? kenshi::CallCanTalk(t) : -2;
    std::string name;
    kenshi::CharacterName(t, name);
    const double now = NowSeconds();
    bool send = dec.verdict == kc::talk::ClickVerdict::SendTalk;
    auto& last = talkClickSentAt_[th];
    if (send && now - last < kc::talk::kResendAfter) send = false;   // the same click seen twice
    if (talkClickSentAt_.size() > 256) talkClickSentAt_.clear();
    ++talkClicksLogged;
    uint32_t held = 0;
    if (talkClickLog_.Allow(now, &held)) {
        char b[600];
        snprintf(b, sizeof(b),
                 "right click on %s (%s) gave no order here: %s; linked to the host's character: %s, driven by the host: %s, captive here: %d, on the "
                 "host: %s, the game's talk check here: %s, our nearest selected character %.0f units away%s%s",
                 name.empty() ? "?" : name.c_str(), KeyOf(th).c_str(), dec.why, known ? (alias ? "yes (our copy for the host's)" : "yes") : "NO (a local character)",
                 v->replicated.count(t) ? "yes" : "no", f.inSomething, hostCaptive_.count(host) ? "captive" : "free",
                 canTalk == 1 ? "yes" : canTalk == 0 ? "NO" : canTalk == -1 ? "failed" : "not asked", double(actorD),
                 send ? ": the talk order goes to the host anyway" : "", held ? (" [" + std::to_string(held) + " more such lines not written]").c_str() : "");
        Log("%s", b);
    }
    if (!send) return;
    last = now;
    kc::Command c;
    c.kind = kc::CommandKind::Task;
    c.via = kc::TaskVia::TaskNearest;
    c.task = kc::kTaskTalk;
    c.add = true;   // (addTaskNearestSelectedCharacter's noAnimals, as the game passes it for "talk")
    c.subject = th;   // our handle: TakeLocalOrders names it as the host knows it
    kenshi::ObjectTemplate(t, c.itemSid);
    if (havePos) {
        c.subjectPos = tp;
        c.pos = tp;
    }
    ++talkClicksSent;
    QueueLocalOrder(actor, c);
}

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

// (under dialogMutex_)
KenshiWorld::RemoteDialog& KenshiWorld::Remember(void* dialogue, kenshi::Character* pc, kenshi::Character* other) {
    auto it = remoteDialogs_.find(dialogue);
    if (it == remoteDialogs_.end()) {
        it = remoteDialogs_.emplace(dialogue, RemoteDialog{}).first;
        it->second.id = nextDialogId_++;
    }
    RemoteDialog& r = it->second;
    if (pc) { r.pc = pc; kenshi::GetHandle(pc, r.pcH); }
    if (other) {
        if (other != r.other) {   // someone else takes over (a leader): measured anew
            r.placed = false;
            r.fightAtStart = false;
            r.fightSweeps = 0;
        }
        r.other = other;
        kenshi::GetHandle(other, r.otherH);
        lastDialogNpc_ = r.otherH;
    }
    return r;
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
    if (open) {
        RemoteDialog& r = Remember(dialogue, pc, other);
        if (r.opened) return true;   // (opened already with its first line, NoteDialogText)
        r.opened = true;
        r.openedAt = NowSeconds();
        d.dialogId = r.id;
        d.pc = r.pcH;
        d.speaker = r.otherH;
        if (r.other) kenshi::CharacterName(r.other, d.text);
        r.otherName = d.text;
        Log("conversation %u opened for another player's character", d.dialogId);
    } else {
        if (known == remoteDialogs_.end()) return true;
        const RemoteDialog& r = known->second;
        d.dialogId = r.id;
        d.pc = r.pcH;
        d.speaker = r.otherH;
        // 0.3.4: closed by the game before the player could answer (a guard's check closed 0.15 s
        // after it opened on rob1's screen, 12 October): what the dialogue held, for the log
        const double openFor = r.opened ? NowSeconds() - r.openedAt : -1.0;
        if (!r.answered && openFor >= 0 && openFor < 5.0 && quickCloseLog_.Allow(NowSeconds())) {
            kenshi::DialogueTimers t;
            const bool read = kenshi::ReadDialogueTimers(dialogue, t);
            Log("conversation %u closed by the game %.2f s after it opened, before any answer (%zu answer(s) shown; dialogue: %s, speech label %s, "
                "line timer %.2f, pause timer %.2f, current line %s, next line %s)",
                r.id, openFor, r.replies, kenshi::DialogueEnded(dialogue) ? "ended" : "not ended", read ? (t.label ? "yes" : "no") : "?", double(t.line),
                double(t.pause), t.current ? "yes" : "no", t.next ? "yes" : "no");
        }
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
    const bool remote = RemoteDialogParties(dialogue, pc, other);
    if (known == remoteDialogs_.end() && !remote) return false;   // (text before the window: open it now)
    RemoteDialog& r = Remember(dialogue, pc, other);
    const double now = NowSeconds();
    if (!r.opened) {
        // 0.3.4: its first line before (or without) the window: the player's window opens now, with
        // the NPC's name. A conversation that goes on in the other character's dialogue (12 October:
        // rob1's guard check, conversation 26 had no name, "walks away from the conversation with ")
        r.opened = true;
        r.openedAt = now;
        WorldDialog o;
        o.kind = kc::DialogKind::Open;
        o.dialogId = r.id;
        o.pc = r.pcH;
        o.speaker = r.otherH;
        if (r.other) kenshi::CharacterName(r.other, o.text);
        r.otherName = o.text;
        Log("conversation %u opened for another player's character (its first line came before the window)", r.id);
        dialogEvents_.push_back(std::move(o));
    }
    WorldDialog d;
    d.kind = kc::DialogKind::Text;
    d.dialogId = r.id;
    d.pc = r.pcH;
    d.speaker = r.otherH;
    kenshi::ReadDialogueWindowText(dialogue, d.text, d.replies);
    if (d.replies.size() != r.replies || d.text != r.lastText || !r.waitingSince) {   // a new line: its answers wait from now
        r.waitingSince = now;
        r.holdLogged = r.holdReleased = false;
    }
    r.replies = d.replies.size();
    r.lastText = d.text;
    dialogEvents_.push_back(std::move(d));
    return true;
}

// Host, any thread: both characters belong to the same player (the published view: squadOwner).
bool KenshiWorld::SameOwner(const void* a, const void* b) {
    if (!a || !b) return false;
    auto v = View();
    auto x = v->squadOwner.find(a), y = v->squadOwner.find(b);
    return x != v->squadOwner.end() && y != v->squadOwner.end() && x->second == y->second;
}

bool KenshiWorld::ConversationAllowed(void* dialogue, void* target) {
    kenshi::Character* me = kenshi::DialogueOwner(dialogue);
    kenshi::Character* t = kenshi::IsCharacter(target) ? static_cast<kenshi::Character*>(target) : nullptr;
    if (!me && !t) return true;
    std::lock_guard<std::mutex> lk(dialogMutex_);
    for (const auto& [dl, r] : remoteDialogs_) {
        const bool npcIn = r.other && (r.other == me || r.other == t);
        const bool pcIn = r.pc && (r.pc == me || r.pc == t);
        if (!npcIn || pcIn) continue;   // its own conversation going on (a leader taking over...) stays allowed
        kenshi::Character* asker = r.other == me ? t : me;
        const double now = NowSeconds();
        // Another character of the same player (two of theirs selected for one "talk": 11 October,
        // nass4 and Logan, 1784 refusals and "Streak est occupé" in nass4's own conversation): refused
        // without a word, as in the game the second one just does not talk. Not counted as busy.
        if (asker && SameOwner(asker, r.pc)) {
            ++dialogsSameOwnerRefused;
            uint32_t held = 0;
            if (sameOwnerTalkLog_.Allow(now, &held))
                Log("conversation %u: another character of the same player asked to talk with that NPC: refused silently [%u more such refusals]",
                    r.id, held);
            return false;
        }
        // the NPC is in another player's conversation: refused, the one asking is told
        ++dialogsBusyRefused;
        auto& told = busyToldAt_[asker];
        if (asker && now - told > 3.0) {
            told = now;
            WorldDialog d;
            d.kind = kc::DialogKind::Busy;
            kenshi::GetHandle(asker, d.pc);
            d.speaker = r.otherH;
            kenshi::CharacterName(r.other, d.text);
            if (dialogEvents_.size() < 512) dialogEvents_.push_back(std::move(d));
            uint32_t held = 0;
            if (busyLog_.Allow(now, &held))
                Log("conversation refused: that NPC is in conversation %u with another player's character [%u more such lines not written]", r.id, held);
        }
        if (busyToldAt_.size() > 256) busyToldAt_.clear();
        return false;
    }
    return true;
}

bool KenshiWorld::TalkingWith(const kc::Handle& npc, kc::Handle& other) {
    kenshi::Character* c = Find(npc);
    if (!c) c = kenshi::Resolve(npc);
    void* d = c ? kenshi::CharacterDialogue(c) : nullptr;
    if (!d) return false;
    {
        std::lock_guard<std::mutex> lk(dialogMutex_);   // a client's conversation, whichever side holds it
        for (const auto& [dl, r] : remoteDialogs_)
            if (r.other == c && r.pcH.valid()) { other = r.pcH; return true; }
    }
    if (kenshi::DialogueEnded(d)) return false;
    kenshi::Character* t = kenshi::DialogueTarget(d);
    kc::Vec3 a, b;
    // a stale target (a conversation long over) is not "talking": only someone close by
    if (!t || t == c || kenshi::IsDead(t) || !kenshi::GetPosition(c, a) || !kenshi::GetPosition(t, b) || Dist(a, b) > 150.0f) return false;
    return kenshi::GetHandle(t, other) && other.valid();
}

// A conversation the game still has for that NPC (with a player's own character, no window showing
// it): ended, so that a new "talk" starts a fresh one.
void KenshiWorld::EndConversationOf(const kc::Handle& npc) {
    kenshi::Character* c = Find(npc);
    if (!c) c = kenshi::Resolve(npc);
    void* d = c ? kenshi::CharacterDialogue(c) : nullptr;
    if (!d || kenshi::DialogueEnded(d)) return;
    uint32_t shown = 0;
    {
        std::lock_guard<std::mutex> lk(dialogMutex_);
        if (auto it = remoteDialogs_.find(d); it != remoteDialogs_.end()) shown = it->second.id;
    }
    if (shown) { EndDialog(shown); return; }
    bool ended = false;
    {
        CallScopeGuard scopeRepair("dialogue end");   // the call runs our hooks (kc/call_scopes.h)
        ended = kenshi::CallEndDialogue(d);
    }
    Log("a stale conversation of that NPC ended by the mod (%s)", ended ? "ok" : "failed");
}

bool KenshiWorld::RemoteRecruitOf(void* c, kc::Handle& pc) {
    std::lock_guard<std::mutex> lk(dialogMutex_);
    for (const auto& [d, r] : remoteDialogs_)
        if (r.other == c && r.pcH.valid()) { pc = r.pcH; return true; }
    return false;
}

void KenshiWorld::NoteRecruit(const kc::Handle& before, uint64_t identity, const kc::Handle& pc, bool editor) {
    std::lock_guard<std::mutex> lk(dialogMutex_);
    if (recruits_.size() < 16) recruits_.push_back({before, identity, pc, editor});
}

void KenshiWorld::TakeRecruits(std::vector<WorldRecruit>& out) {
    std::lock_guard<std::mutex> lk(dialogMutex_);
    out.swap(recruits_);
    recruits_.clear();
}

// Client: the host's game recruited that character (Bind with the handle it had as an NPC): our copy
// joins our player faction the same way, with the game's own recruit (no editor). Its handle changes
// with its squad: the host's handle for it follows (LocalRehandled).
bool KenshiWorld::AdoptRecruit(const kc::Handle& h) {
    kenshi::Character* c = Find(h);
    if (!c) return false;
    if (kenshi::InPlayerSquad(c)) return true;
    kc::Handle before, after;
    kenshi::GetHandle(c, before);
    bool ok = false;
    {
        CallScopeGuard scopeRepair("recruit");
        HostCallScope scope;
        ok = kenshi::RecruitCharacter(c);
    }
    if (ok && kenshi::GetHandle(c, after)) LocalRehandled(before, after);
    const bool in = ok && kenshi::InPlayerSquad(c);
    Log("recruit adopted here: %s", in ? "in the player faction" : (ok ? "recruit() ran, not in the player faction" : "recruit() faulted"));
    return in;
}

std::vector<KenshiWorld::RemoteDialogInfo> KenshiWorld::RemoteDialogList() {
    std::vector<RemoteDialogInfo> out;
    std::lock_guard<std::mutex> lk(dialogMutex_);
    for (const auto& [dl, r] : remoteDialogs_) {
        RemoteDialogInfo i{r.id, {}, {}, r.otherH};
        if (r.pc) kenshi::CharacterName(r.pc, i.pc);
        if (r.other) kenshi::CharacterName(r.other, i.other);
        out.push_back(std::move(i));
    }
    return out;
}

kc::Handle KenshiWorld::LastDialogNpc() {
    std::lock_guard<std::mutex> lk(dialogMutex_);
    return lastDialogNpc_;
}

// A conversation is over for good when its dialogue is gone or ended, a side is dead, knocked out or
// down, the player's character is no longer another player's, one of them jumped away (a teleport:
// more than 20 m between two sweeps 0.5 s apart; nobody runs that fast), they ended up 100 m further
// apart than when it started, or a fight started (kc::talk::DecideSweep): the game may not close the
// window itself then. Ended in the game, and closed. (An absolute distance is wrong: a conversation
// may start from 40 m and more, as the in-game test showed: every conversation the Chef Voleur
// started was closed at once.)
// 0.3.4 (fix, in-game test of 13 October): a new conversation is placed on the first frame it is
// seen, not at the next sweep (a teleport 0.4 s after it opened came before its first positions and
// was never seen); and a fight that starts after it opened ends it (nothing did: the conversation
// stayed open on the client's screen through the whole fight).
void KenshiWorld::SweepDialogs() {
    const double now = NowSeconds();
    const bool full = now >= nextDialogSweep_;
    if (full) nextDialogSweep_ = now + 0.5;
    struct Gone { void* d; uint32_t id; kc::Handle pc, other; bool end; std::string why; };
    std::vector<Gone> gone;
    auto v = View();
    {
        std::lock_guard<std::mutex> lk(dialogMutex_);
        for (auto& [dl, r] : remoteDialogs_) {
            if (!full && r.placed) continue;   // between sweeps: only new conversations are placed
            kenshi::Character* pc = r.pcH.valid() ? kenshi::Resolve(r.pcH) : nullptr;
            kenshi::Character* other = r.otherH.valid() ? kenshi::Resolve(r.otherH) : nullptr;
            const bool alive = kenshi::DialogueOwner(dl) != nullptr;
            kc::Vec3 a, b;
            kc::Handle foe;
            const bool fighting = pc && other && (kenshi::ReadCombat(pc, foe) || kenshi::ReadCombat(other, foe));
            if (!r.placed) {
                if (alive && pc && pc == r.pc && other && other == r.other && kenshi::GetPosition(pc, a) && kenshi::GetPosition(other, b)) {
                    r.placed = true;
                    r.startDist = Dist(a, b);
                    r.pcAt = a;
                    r.otherAt = b;
                    r.fightAtStart = fighting;
                    r.fightSweeps = 0;
                }
                if (!full) continue;
            }
            std::string why;
            if (!alive) why = "its character is gone";
            else if (!pc || pc != r.pc || !other || other != r.other) why = "a character in it is gone";
            else if (kenshi::DialogueEnded(dl)) why = "the game ended it";
            else if (kenshi::IsDead(pc) || kenshi::IsDead(other) || kenshi::IsDown(pc) || kenshi::IsDown(other) || kenshi::IsUnconscious(pc) ||
                     kenshi::IsUnconscious(other))
                why = "a character in it is down";
            else if (!v->squadForeign.count(pc)) why = "the character is no longer another player's";
            else if (r.placed && kenshi::GetPosition(pc, a) && kenshi::GetPosition(other, b)) {
                kc::talk::SweepFacts f;
                f.pcMoved = Dist(a, r.pcAt);
                f.otherMoved = Dist(b, r.otherAt);
                f.dist = Dist(a, b);
                f.startDist = r.startDist;
                r.fightSweeps = kc::talk::CountFightSweeps(r.fightAtStart, fighting, r.fightSweeps);
                f.fightSweeps = r.fightSweeps;
                char line[200];
                switch (kc::talk::DecideSweep(f)) {
                case kc::talk::SweepEnd::Jump:
                    snprintf(line, sizeof(line), "a character in it jumped away (teleport: %.0f and %.0f units since the last sweep)", double(f.pcMoved),
                             double(f.otherMoved));
                    why = line;
                    break;
                case kc::talk::SweepEnd::Apart:
                    snprintf(line, sizeof(line), "they are far apart (%.0f units, %.0f when it started)", double(f.dist), double(f.startDist));
                    why = line;
                    break;
                case kc::talk::SweepEnd::Fight: why = "a fight started (a character in it is in combat)"; break;
                case kc::talk::SweepEnd::None: break;
                }
                r.pcAt = a;
                r.otherAt = b;
            }
            if (!why.empty()) gone.push_back({dl, r.id, r.pcH, r.otherH, alive && !kenshi::DialogueEnded(dl), std::move(why)});
        }
    }
    for (const Gone& g : gone) {
        Log("conversation %u ends: %s", g.id, g.why.c_str());
        if (g.end) {
            CallScopeGuard scopeRepair("dialogue end");   // the call runs our hooks (kc/call_scopes.h)
            kenshi::CallEndDialogue(g.d);   // closes the window through setInDialog(false) (hook: Close)
        }
        std::lock_guard<std::mutex> lk(dialogMutex_);
        auto it = remoteDialogs_.find(g.d);
        if (it == remoteDialogs_.end() || it->second.id != g.id) continue;   // closed by the game meanwhile
        remoteDialogs_.erase(it);
        WorldDialog d;
        d.kind = kc::DialogKind::Close;
        d.dialogId = g.id;
        d.pc = g.pc;
        d.speaker = g.other;
        dialogEvents_.push_back(std::move(d));
        ++dialogsSwept;
    }
}

void KenshiWorld::TakeDialogEvents(std::vector<WorldDialog>& out) {
    if (live_ && !View()->client) SweepDialogs();   // host, game thread
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
        for (auto& [d, r] : remoteDialogs_)
            if (r.id == dialogId) {
                dialogue = d;
                r.answered = true;
                r.replies = 0;   // the line is answered: no longer held (its next one is, once shown)
                break;
            }
    }
    if (!dialogue) return;
    if (index == kc::kDialogLeave) { EndDialog(dialogId); return; }
    Log("conversation %u: the player answered %d (%s)", dialogId, index, CallReplyClicked(dialogue, index) ? "ok" : "failed");
}

void KenshiWorld::EndDialog(uint32_t dialogId) {
    void* dialogue = nullptr;
    RemoteDialog r;
    {
        std::lock_guard<std::mutex> lk(dialogMutex_);
        for (const auto& [d, info] : remoteDialogs_)
            if (info.id == dialogId) { dialogue = d; r = info; break; }
    }
    if (!dialogue) return;
    bool ended = false;
    {
        CallScopeGuard scopeRepair("dialogue end");   // the call runs our hooks (kc/call_scopes.h)
        ended = kenshi::CallEndDialogue(dialogue);
    }
    Log("conversation %u ended by the mod (%s)", dialogId, ended ? "ok" : "its dialogue is gone");
    std::lock_guard<std::mutex> lk(dialogMutex_);
    auto it = remoteDialogs_.find(dialogue);
    if (it == remoteDialogs_.end() || it->second.id != dialogId) return;   // the game closed it (Close sent)
    remoteDialogs_.erase(it);
    WorldDialog d;
    d.kind = kc::DialogKind::Close;
    d.dialogId = dialogId;
    d.pc = r.pcH;
    d.speaker = r.otherH;
    dialogEvents_.push_back(std::move(d));
}

// Host: where an item a client asked to pick up went, once it left the ground (said in the log):
// the character that was sent should hold it.
std::string KenshiWorld::PickedItemWhere(kenshi::Character* c, const kc::Handle& itemHandle) {
    void* item = kenshi::ResolveItem(itemHandle);
    if (!item) return "joined a stack of the same kind in an inventory, or destroyed";
    if (c && kenshi::InventoryHolds(c, item)) return "in the inventory of the character that was sent";
    for (const auto& [h, o] : squad_)
        if (o != c && kenshi::InventoryHolds(o, item)) return "in ANOTHER squad member's inventory (" + CharacterNameOf(h) + ")";
    if (kenshi::ItemLoose(item)) return "loose again";
    return "in an inventory outside the squad (taken by someone else?)";
}

void KenshiWorld::UpdatePendingPickups() {
    constexpr float kReach = 15.0f;
    const double now = NowSeconds();
    // Paused (the host's pause, or the whole game held while a player makes their character), nobody
    // walks: the clock of every pending pick up stops with the game, it is not a failure.
    if (kenshi::GetPaused()) {
        const double dt = lastPickupTick_ > 0 ? std::min(now - lastPickupTick_, 1.0) : 0.0;   // capped: the tick stops while no pick up is pending
        for (auto& [h, p] : pickups_) {
            p.until += dt;
            if (p.checkAt > 0) p.checkAt += dt;
            if (!p.pauseLogged) {
                p.pauseLogged = true;
                Log("client pick up: the game is paused, the character will walk there once it resumes");
            }
        }
        lastPickupTick_ = now;
        return;
    }
    lastPickupTick_ = now;
    for (auto it = pickups_.begin(); it != pickups_.end();) {
        kenshi::Character* c = FindSquad(it->first);
        void* item = kenshi::ResolveItem(it->second.item);
        kc::Vec3 cp, ip;
        kc::Handle ih;
        kc::ItemState st;
        PendingPickup& p = it->second;
        const bool loose = c && item && kenshi::ItemLoose(item);
        const bool described = loose && kenshi::DescribeGroundItem(item, ih, st, ip);
        if (!loose || !described || now > p.until) {
            if (!c) Log("client pick up: the character is gone, pick up abandoned");
            else if (!loose) Log("client pick up: the item left the ground (taken): %s", PickedItemWhere(c, p.item).c_str());
            else if (!described) Log("client pick up: the item can no longer be read, pick up abandoned");
            else {
                const float d = kenshi::GetPosition(c, cp) ? Dist(cp, ip) : -1.0f;
                Log("client pick up FAILED: still %.0f units from the item after 30 s (game order=%d), given up", d, int(p.gameOrder));
            }
            it = pickups_.erase(it);
            continue;
        }
        const bool hasPos = kenshi::GetPosition(c, cp);
        if (hasPos && Dist(cp, ip) < kReach) {
            HostCallScope scope;
            const bool ok = kenshi::CallGiveItem(c, item);
            Log("client pick up: character next to the item, %s%s", ok ? "taken: " : "giveItem FAILED (inventory full?)",
                ok ? PickedItemWhere(c, p.item).c_str() : "");
            it = pickups_.erase(it);
            continue;
        }
        if (p.gameOrder && p.checkAt > 0 && now > p.checkAt) {
            p.checkAt = 0;
            const float d = hasPos ? Dist(cp, ip) : -1.0f;
            if (d >= 0 && d > p.startDist - 3.0f) {   // the game's order did not get it moving
                Log("client pick up: the game's order did not move the character (%.0f -> %.0f units), walking it there", p.startDist, d);
                HostCallScope scope;
                if (!CallPlayerMoveOrder(c, ip)) Log("client pick up FAILED: move order refused too");
            }
        }
        ++it;
    }
}

void KenshiWorld::QueueLocalOrder(const kc::Handle& h, const kc::Command& c) {
    std::lock_guard<std::mutex> lk(ordersMutex_);
    if (orders_.size() < 256) orders_.emplace_back(h, c);
}

// Client loot orders (task 26 on a body or an animal). The looter walks to the body through the host
// and the game's loot window opens here once it stands by it. How close is "by it" depends on the
// body: kc::loot::Reach grows with the race's size and the body's scale (a leviathan's position, the
// root of its ragdoll, is ~25 times as far from where a looter can stand as a human's). Until 0.3.1
// it was a flat 12 units in 3D: big animals (leviathans, beak things, swamp turtles, gorillos,
// spiders...) never opened, the order silently expired after 30 s.
// Client: where the host's copy of that character (our local handle) is, from its last state.
bool KenshiWorld::HostBodyPos(const kc::Handle& local, kc::Vec3& out) const {
    auto it = lastTarget_.find(HostHandleOf(local));
    if (it == lastTarget_.end()) return false;
    out = it->second.pos;
    return true;
}

void KenshiWorld::RequestLoot(const kc::Handle& looter, kenshi::Character* target) {
    kc::Handle t;
    kc::Vec3 tp, lp;
    kenshi::Character* me = FindSquad(looter);
    if (!me || !kenshi::GetHandle(target, t) || !kenshi::GetPosition(target, tp) || !kenshi::GetPosition(me, lp)) return;
    pendingLoot_.erase(std::remove_if(pendingLoot_.begin(), pendingLoot_.end(), [&](const PendingLoot& p) { return p.looter == looter; }),
                       pendingLoot_.end());
    kc::loot::BodySize body;
    std::string race;
    kenshi::ReadBodySize(target, body, &race);
    const float dist = kc::loot::GroundDist(lp, tp);
    kc::Vec3 hp;
    const bool haveHost = HostBodyPos(t, hp);
    Log("loot order: %.0f units to go (%s %s, race %s, scale %.2f, body radius %.0f, reach %.0f; the host's body %.0f units from ours)", dist,
        kenshi::IsAnimal(target) ? "animal" : "human", kenshi::IsDead(target) ? "dead" : kenshi::IsDown(target) ? "down" : "standing",
        race.c_str(), body.scale, kc::loot::BodyRadius(body), kc::loot::Reach(body), haveHost ? kc::loot::GroundDist(tp, hp) : -1.0f);
    if (kc::loot::InReachEither(lp, tp, haveHost ? &hp : nullptr, body)) {
        kenshi::OpenLootWindow(me, target);
        ++lootOpened;
        Log("loot window opened at %.0f units", dist);
        return;
    }
    kc::Command c;
    c.kind = kc::CommandKind::MoveTo;
    c.pos = kc::loot::ApproachPoint(lp, haveHost ? hp : tp, body);   // the host's looter walks to the host's body
    QueueLocalOrder(looter, c);
    pendingLoot_.push_back({looter, t, body, race, kc::loot::StartOrder(NowSeconds(), dist)});
}

void KenshiWorld::UpdatePendingLoot() {
    const double now = NowSeconds();
    for (auto it = pendingLoot_.begin(); it != pendingLoot_.end();) {
        kenshi::Character* me = FindSquad(it->looter);
        kenshi::Character* target = kenshi::Resolve(it->target);
        kc::Vec3 lp, tp;
        const bool lootable = target && (kenshi::IsDown(target) || kenshi::IsDead(target) || kenshi::IsRagdoll(target) || kenshi::IsAnimal(target));
        const bool havePos = me && lootable && kenshi::GetPosition(me, lp) && kenshi::GetPosition(target, tp);
        if (!me || !lootable) {
            Log("loot order dropped: %s", !me ? "the looter is gone" : !target ? "the body is gone here" : "it got up");
            ++lootDropped;
            it = pendingLoot_.erase(it);
            continue;
        }
        // paused (the host's pause, a joining player's character editor): the looter cannot walk, the
        // order waits. Stopped: it walked, then stood still for a second (a big ragdoll blocks it).
        kc::Vec3 dest;
        bool moving = true;
        float speed = 0;
        if (!kenshi::GetMovement(me, dest, moving, speed)) moving = true;
        const bool paused = kenshi::GetPaused();
        if (paused && it->clock.pausedFor == 0) Log("loot order waits: the game is paused");
        kc::loot::Tick(it->clock, now, paused, moving);
        const bool stopped = kc::loot::Stopped(it->clock, now);
        kc::Vec3 hp;
        const kc::Vec3* host = havePos && HostBodyPos(it->target, hp) ? &hp : nullptr;
        if (havePos && (kc::loot::InReachEither(lp, tp, host, it->body) ||
                        (stopped && kc::loot::InReachEither(lp, tp, host, it->body, kc::loot::kStoppedSlack)))) {
            kenshi::OpenLootWindow(me, target);
            ++lootOpened;
            Log("loot window opened at %.0f units (%s, reach %.0f%s)", kc::loot::GroundDist(lp, tp), it->race.c_str(), kc::loot::Reach(it->body),
                kc::loot::InReachEither(lp, tp, host, it->body) ? "" : ", stopped by the body");
            it = pendingLoot_.erase(it);
        } else if (kc::loot::Expired(it->clock, now)) {
            Log("loot order dropped: the looter is still %.0f units from the body (%s, reach %.0f, %s, paused %.0f s)",
                havePos ? kc::loot::GroundDist(lp, tp) : -1.0f, it->race.c_str(), kc::loot::Reach(it->body),
                it->clock.walked ? "it walked" : "it never walked", it->clock.pausedFor);
            ++lootDropped;
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
    if (logged < 20 && e.kind != kc::AnimKind::State && e.kind != kc::AnimKind::WeaponState && e.kind != kc::AnimKind::CombatMode) {
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
            ++floatersShown;
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
    if (h.type == 0) return kenshi::ResolveObject(h);   // type 0: a building
    if (h.type == 1 || h.type == 0x5B) return nullptr;   // a character not here
    // an item with an inventory of its own: a worn backpack (RootObject::getInventory, vt 0x160)
    void* item = kenshi::ResolveItem(h);
    return item && kenshi::HasInventory(item) ? item : nullptr;
}

bool KenshiWorld::ReadInventory(const kc::Handle& h, std::vector<kc::ItemState>& out) {
    void* c = InventoryHolder(h);
    return c && kenshi::ReadInventory(c, out);
}

bool KenshiWorld::ExecuteInvOp(const kc::Handle& from, const kc::Handle& to, const kc::InvOp& op) {
    void* a = InventoryHolder(from);
    void* b = InventoryHolder(to);
    if (!a || !b) {   // 0.3.4: said (a request that did nothing used to leave no trace)
        Log("client item %s not done: %s x%d, %s inventory not found here", op.kind == kc::InvOpKind::Drop ? "drop" : "move", op.item.templateSid.c_str(),
            op.item.quantity, !a ? "source" : "destination");
        return false;
    }
    std::string err;
    HostCallScope scope;
    const bool ok = kenshi::MoveInventoryItem(a, b, op, &err);
    if (ok)
        Log("client item %s done: %s x%d %s -> %s %d,%d", op.kind == kc::InvOpKind::Drop ? "drop" : "move", op.item.templateSid.c_str(),
            op.item.quantity, op.item.section.c_str(), op.toSection.c_str(), op.toX, op.toY);
    else
        Log("client item move refused: %s (%s x%d)", err.c_str(), op.item.templateSid.c_str(), op.item.quantity);
    CheckAfterOp(op.kind == kc::InvOpKind::Drop ? "a drop" : "a move", a, b);
    return ok;
}

bool KenshiWorld::ExecuteInvSwap(const kc::Handle& from, const kc::Handle& to, const kc::InvOp& a, const kc::InvOp& b) {
    void* x = InventoryHolder(from);
    void* y = InventoryHolder(to);
    if (!x || !y) return false;
    std::string err;
    HostCallScope scope;
    const bool ok = kenshi::SwapInventoryItems(x, y, a, b, &err);
    Log("client item swap %s: %s <-> %s (%s %d,%d)%s%s", ok ? "done" : "not possible", a.item.templateSid.c_str(), b.item.templateSid.c_str(),
        a.toSection.c_str(), a.toX, a.toY, ok ? "" : ": ", err.c_str());
    CheckAfterOp("a swap", x, y);
    return ok;
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
    local.clear();
    if (kenshi::ReadInventory(c, local) && local == items) return true;
    // Say which item the game put elsewhere (the session retries 3 times, then leaves it): an item in
    // the bag that the host has worn was invisible in the logs until a test compared inventories.
    for (const kc::ItemState& s : items) {
        if (std::find(local.begin(), local.end(), s) != local.end()) continue;
        auto same = std::find_if(local.begin(), local.end(), [&](const kc::ItemState& l) { return l.sameKind(s); });
        Log("inventory of %s not as on the host: %s x%d at %s %d,%d here %s", KeyOf(h).c_str(), s.templateSid.c_str(), s.quantity,
            s.section.c_str(), s.x, s.y,
            same == local.end() ? "missing" : ("at " + same->section + " " + std::to_string(same->x) + "," + std::to_string(same->y)).c_str());
        break;
    }
    return false;
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

// Clients start from the host's save and run at the host's speed, paused when it is; ClockSync
// trims that speed a little while the clocks are slightly apart, and sets the hours outright when
// they are far apart (SetGameHours).
void KenshiWorld::SetTime(const kc::TimeState& t) {
    hostTime_ = t;
    haveHostTime_ = true;
}

// A client far off the host's clock (it stalled loading a zone, a pause reached it late): the day
// and the hour of day the clock is recomputed from are set, the game's own way (kenshi::SetGameHours).
bool KenshiWorld::SetGameHours(double hours) {
    if (!active_ || !client_ || !live_) return false;
    HostCallScope scope;
    return kenshi::SetGameHours(hours);
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
    const std::string slot = ImportSlot();
    const fs::path dir = FromGamePath(folder) / slot;
    std::error_code ec;
    if (dir.filename() != slot) { if (err) *err = "bad save folder"; return false; }
    fs::remove_all(dir, ec);   // only ever our own slot
    for (const auto& f : files) {
        if (!kc::ValidWorldPath(f.path)) { if (err) *err = "refused file path " + f.path; return false; }
        const fs::path target = dir / FromUtf8(f.path);
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
        if (!out) { if (err) *err = "cannot write " + ToUtf8(target); return false; }
    }
    if (!kenshi::RequestLoad(slot)) { if (err) *err = "the game refused to load the save"; return false; }
    Log("host world written to save slot %s, loading it", slot.c_str());
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
    // a living member of the squad (a remembered character that died, or is not in this world,
    // is not theirs to come back to: they get a new one, made in the character editor)
    auto inSquad = [&](const kc::Handle& h) {
        for (kenshi::Character* c : squad) { kc::Handle sh; if (kenshi::GetHandle(c, sh) && sh == h && !kenshi::IsDead(c)) return c; }
        return static_cast<kenshi::Character*>(nullptr);
    };
    // 1. the character this Steam account played last time (whatever name the player uses now)
    bool remembered = false;
    if (steamId)
        for (const auto& e : ledger) {
            if (e.steamId != steamId) continue;
            remembered = true;
            if (!inSquad(e.handle)) continue;
            out = e.handle;
            Log("%s is back with their character", name.c_str());
            return true;
        }
    if (remembered) Log("%s's remembered character is not a living member of the squad in this world: a new one for them", name.c_str());
    // 2. a squad member with the player's name that no other account owns
    auto ownedByOther = [&](const kc::Handle& h) {
        for (const auto& e : ledger) if (e.handle == h && e.steamId != steamId) return true;
        return false;
    };
    for (kenshi::Character* c : squad) {
        std::string n;
        kc::Handle h;
        if (kenshi::CharacterName(c, n) && n == name && !kenshi::IsDead(c) && kenshi::GetHandle(c, h) && !(steamId && ownedByOther(h))) {
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
    hostBounties_.clear();
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
    }
    ResetGround();
    if (!active) controllable_.clear();
}

void KenshiWorld::SetControllable(const std::vector<kc::Handle>& handles) {
    controllable_.clear();
    controllable_.insert(handles.begin(), handles.end());
}

void KenshiWorld::SetSquadOwners(const std::vector<std::pair<kc::Handle, uint8_t>>& owners) {
    squadOwners_.clear();
    for (const auto& [h, id] : owners) {
        auto a = alias_.find(h);
        squadOwners_[a != alias_.end() ? a->second : h] = id;
    }
}

// Client (0.3.2): our copy of a host squad character is in one of our player squads (it has a
// portrait in the squad bar and can be put in a squad). Not here, dead or down: nothing to do.
bool KenshiWorld::InPlayerFactionHere(const kc::Handle& h) {
    if (FindSquad(h)) return true;
    kenshi::Character* c = Find(h);
    if (!c || kenshi::IsDead(c)) return true;
    return kenshi::InPlayerSquad(c);
}

// Host (0.3.2): a character outside the player's squads, removed (a character made for a player's
// editor and left behind). kenshi::DestroyObject never deletes a squad member (its portrait).
bool KenshiWorld::RemoveStrayCharacter(const kc::Handle& h) {
    kenshi::Character* c = Find(h);
    if (!c || !kenshi::IsCharacter(c) || kenshi::InPlayerSquad(c)) return false;
    std::string name;
    kenshi::CharacterName(c, name);
    bool ok = false;
    {
        HostCallScope scope;
        if (kenshi::InventoryWindowShows(c)) kenshi::CloseInventoryWindows();
        ok = kenshi::DestroyObject(c);
    }
    resolved_.erase(h);
    Log("character '%s' %s: it is in no player squad (made for a character editor never finished)", name.c_str(), ok ? "removed" : "could not be removed");
    return ok;
}

void KenshiWorld::Toast(const std::string& msg) {
    std::lock_guard<std::mutex> lk(toastMutex_);
    if (toasts_.size() < 16) toasts_.push_back(msg);
    recentToasts_.push_back(msg);
    if (recentToasts_.size() > 8) recentToasts_.erase(recentToasts_.begin());
}

std::vector<std::string> KenshiWorld::RecentToasts() {
    std::lock_guard<std::mutex> lk(toastMutex_);
    return recentToasts_;
}

std::vector<std::string> KenshiWorld::TakeToasts() {
    std::lock_guard<std::mutex> lk(toastMutex_);
    return std::exchange(toasts_, {});
}

} // namespace kcp
