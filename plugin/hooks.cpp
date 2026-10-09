#include "hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <intrin.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "util.h"
#include "world.h"

namespace kcp {

namespace {

TickFn g_tick = nullptr;
std::atomic<double> g_lastLiveTick{-1e9};
// Ogre may fire frame events on another thread than the game's main loop: ticks are serialized,
// and a tick that would overlap another one (or re-enter it) is simply skipped.
std::mutex g_tickMutex;
std::atomic<DWORD> g_liveThread{0}, g_menuThread{0};
thread_local int g_hostCall = 0;   // >0 while KenshiCoop itself is calling into the game

// ---- originals
using MainLoopFn = void (*)(void* gw, float t);
using PlayerMoveFn = void (*)(void* pi, const float* pos, void* building);
using AddOrderFn = void (*)(void* pi, void* building, int task, void* subject, bool shift, bool addDontClear, const float* loc);
using NewTaskFn = void (*)(void* pi, int task, const void* targetHand, void* building, const float* clickPos, bool addDontClear);
using AddTaskNearestFn = void (*)(void* pi, void* building, int task, void* subject, bool shift, const float* loc, bool noAnimals);
using AddJobFn = void (*)(void* pi, int task, void* subject, bool shift, bool add, const float* loc);
using SetOrderFn = void (*)(void* pi, int order);
using StopMoveFn = void (*)(void* pi);
using MoveOrderFn = void (*)(void* chr, void* building, void* subject, const float* loc);
using AIUpdateFn = void (*)(void* ai, float t);
using MedDamageFn = void (*)(void* med, void* part, const void* damage, bool loadingSavestate, bool canSever, const float* force);
using MedKnockoutFn = void (*)(void* med, float skill01);
using DeclareDeadFn = void (*)(void* chr);
using VoidFn = void (*)(void* self);

MainLoopFn o_mainLoop = nullptr;
PlayerMoveFn o_playerMove = nullptr;
AddOrderFn o_addOrder = nullptr;
AddTaskNearestFn o_addTaskNearest = nullptr;
AddJobFn o_addJob = nullptr;
NewTaskFn o_newTask = nullptr;
SetOrderFn o_setOrder = nullptr;
StopMoveFn o_stopMove = nullptr;
MoveOrderFn o_moveOrder = nullptr;
AIUpdateFn o_aiUpdate4 = nullptr;
AIUpdateFn o_aiPeriodic = nullptr;
MedDamageFn o_medDamage = nullptr;
MedKnockoutFn o_medKnockout = nullptr;
DeclareDeadFn o_declareDead = nullptr;
using RagdollModeFn = void (*)(void*, bool, int);
RagdollModeFn o_ragdollMode = nullptr;
using CreateCharFn = void* (*)(void* factory, void* faction, const float* pos, void* owner, void* data, void* home, float age);
CreateCharFn o_createChar = nullptr;
VoidFn o_regionUpdateBT = nullptr;
using EffectCtorFn = void* (*)(void* self, void* effect, void* biome, const float* pos);
EffectCtorFn o_effectCtor = nullptr;
VoidFn o_effectAffect = nullptr;
VoidFn o_effectStop = nullptr;
VoidFn o_regionUpdateEffects = nullptr;
VoidFn o_seasonGetNewWeather = nullptr;

void SafeTick(bool live) {
    // C++ exceptions must never unwind into game code.
    try {
        g_tick(live);
    } catch (const std::exception& e) {
        Log("tick exception: %s", e.what());
    } catch (...) {
        Log("tick exception (unknown)");
    }
}

void TickSEH(bool live) {
    __try {
        SafeTick(live);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int reported = 0;
        if (reported++ < 5) Log("tick: access violation caught (code %08lx)", GetExceptionCode());
    }
}

void hk_mainLoop(void* gw, float t) {
    o_mainLoop(gw, t);
    g_lastLiveTick.store(NowSeconds());
    RunTick(true);
}

// Classifies the current selection: are all selected characters ours to command?
struct SelectionInfo {
    std::vector<kc::Handle> mine;
    size_t foreign = 0;
};
SelectionInfo ClassifySelection(const HookView& v) {
    SelectionInfo s;
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    for (const auto& h : sel) {
        if (v.controllable.count(h)) s.mine.push_back(h);
        else if (h.type == kenshi::kItemTypeCharacter || h.type == kenshi::kItemTypeAnimalCharacter) ++s.foreign;
    }
    return s;
}

void ToastForeign() {
    if (KenshiWorld* w = TheWorld()) w->Toast("This character belongs to another player.");
}

void hk_playerMove(void* pi, const float* pos, void* building) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active) return o_playerMove(pi, pos, building);
    const SelectionInfo s = ClassifySelection(*v);
    if (v->client) {
        // Clients never move characters locally: the order goes to the host.
        if (KenshiWorld* w = TheWorld()) {
            for (const auto& h : s.mine) {
                kc::Command c;
                c.kind = kc::CommandKind::MoveTo;
                c.pos = {pos[0], pos[1], pos[2]};
                w->QueueLocalOrder(h, c);
            }
        }
        if (s.foreign) ToastForeign();
        return;
    }
    if (s.foreign) { ToastForeign(); return; }
    o_playerMove(pi, pos, building);
}

// Orders that are not synchronized yet. On a client they are refused (executing them locally
// would make the client's world diverge); on the host they are refused only when the selection
// contains another player's character.
bool AllowUnsyncedOrder(int task) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active) return true;
    if (v->client) {
        Log("client order refused (task %d)", task);
        if (KenshiWorld* w = TheWorld()) w->Toast("Only movement and loot orders are synchronized in this version.");
        return false;
    }
    if (ClassifySelection(*v).foreign) { ToastForeign(); return false; }
    return true;
}

// Looting a knocked-out or dead character from a client: the looter walks there through the host
// and the game's loot window opens locally once it is close. Items moved in that window are
// replayed by the host (Session::ClientInventoryDiff).
constexpr int kTaskLootTarget = 26;   // TaskType::LOOT_TARGET
bool ClientLoot(int task, kenshi::Character* target) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active || !v->client || task != kTaskLootTarget || !target) return false;
    if (!kenshi::IsDown(target) && !kenshi::IsDead(target) && !kenshi::IsRagdoll(target)) return false;
    KenshiWorld* w = TheWorld();
    if (!w) return false;
    const SelectionInfo s = ClassifySelection(*v);
    if (s.mine.empty()) {
        if (s.foreign) ToastForeign();
        return true;
    }
    kc::Handle looter = s.mine.front();
    kc::Vec3 tp, p;
    float best = -1;
    if (kenshi::GetPosition(target, tp))
        for (const auto& h : s.mine)
            if (kenshi::Character* c = w->FindSquad(h); c && kenshi::GetPosition(c, p)) {
                const float d = (p.x - tp.x) * (p.x - tp.x) + (p.z - tp.z) * (p.z - tp.z);
                if (best < 0 || d < best) { best = d; looter = h; }
            }
    w->RequestLoot(looter, target);
    return true;
}

void hk_addOrder(void* pi, void* building, int task, void* subject, bool shift, bool addDontClear, const float* loc) {
    if (ClientLoot(task, kenshi::IsCharacter(subject) ? static_cast<kenshi::Character*>(subject) : nullptr)) return;
    if (AllowUnsyncedOrder(task)) o_addOrder(pi, building, task, subject, shift, addDontClear, loc);
}
void hk_newTask(void* pi, int task, const void* targetHand, void* building, const float* clickPos, bool addDontClear) {
    kc::Handle th;
    if (ClientLoot(task, kenshi::HandleFromHand(targetHand, th) ? kenshi::Resolve(th) : nullptr)) return;
    if (AllowUnsyncedOrder(task)) o_newTask(pi, task, targetHand, building, clickPos, addDontClear);
}
// The right-click "loot" on a body goes through this one (the nearest selected character acts).
void hk_addTaskNearest(void* pi, void* building, int task, void* subject, bool shift, const float* loc, bool noAnimals) {
    if (ClientLoot(task, kenshi::IsCharacter(subject) ? static_cast<kenshi::Character*>(subject) : nullptr)) return;
    if (AllowUnsyncedOrder(task)) o_addTaskNearest(pi, building, task, subject, shift, loc, noAnimals);
}
void hk_addJob(void* pi, int task, void* subject, bool shift, bool add, const float* loc) {
    if (AllowUnsyncedOrder(task)) o_addJob(pi, task, subject, shift, add, loc);
}
void hk_setOrder(void* pi, int order) {
    if (AllowUnsyncedOrder(-order)) o_setOrder(pi, order);
}
void hk_stopMove(void* pi) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active) return o_stopMove(pi);
    const SelectionInfo s = ClassifySelection(*v);
    if (v->client) {
        if (KenshiWorld* w = TheWorld()) {
            for (const auto& h : s.mine) {
                kc::Command c;
                c.kind = kc::CommandKind::Stop;
                w->QueueLocalOrder(h, c);
            }
        }
        return;
    }
    if (s.foreign) { ToastForeign(); return; }
    o_stopMove(pi);
}

// Safety net below the UI layer: nothing but KenshiCoop may order a character this machine does
// not control.
void hk_moveOrder(void* chr, void* building, void* subject, const float* loc) {
    if (!g_hostCall) {
        auto v = KenshiWorld::View();
        if (v->active && (v->client || v->squadForeign.count(chr))) return;
    }
    o_moveOrder(chr, building, subject, loc);
}

// Clients have no brains at all: the host's world decides what every character does.
void hk_aiUpdate4(void* ai, float t) {
    if (!KenshiWorld::ClientActive()) o_aiUpdate4(ai, t);
}
void hk_aiPeriodic(void* ai, float t) {
    if (!KenshiWorld::ClientActive()) o_aiPeriodic(ai, t);
}

// Clients never decide damage, knockouts or deaths: only the host's values (applied by
// KenshiCoop inside a HostCallScope) and save-game loading may change health.
void hk_medDamage(void* med, void* part, const void* damage, bool loadingSavestate, bool canSever, const float* force) {
    if (KenshiWorld::ClientActive() && !g_hostCall && !loadingSavestate) return;
    o_medDamage(med, part, damage, loadingSavestate, canSever, force);
}
void hk_medKnockout(void* med, float skill01) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_medKnockout(med, skill01);
}
void hk_declareDead(void* chr) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_declareDead(chr);
}
// Nor do the characters the host drives fall over on their own (local knockout from the host's
// health values, a hit that would knock down locally...): they go down when, and where, the
// host's do. Others (bodies of a zone that just streamed in, far from everyone) lie down as usual.
// Clients never populate the world by themselves (wandering squads, bar patrons...): every
// character comes from the host's world. Every caller of the factory copes with a null result
// (it is how the game refuses a second copy of a unique character).
void* hk_createRandomCharacter(void* factory, void* faction, const float* pos, void* owner, void* data, void* home, float age) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return nullptr;
    return o_createChar(factory, faction, pos, owner, data, home, age);
}

void hk_ragdollMode(void* chr, bool on, int part) {
    if (on && KenshiWorld::ClientActive() && !g_hostCall && KenshiWorld::View()->replicated.count(chr)) return;
    o_ragdollMode(chr, on, part);
}

// Weather: the host reports each region after the game advances it; clients impose the host's
// weather first and never roll a new one themselves.
void hk_regionUpdateBT(void* region) {
    KenshiWorld* w = TheWorld();
    if (w) w->WeatherRegionTick(region, false);
    o_regionUpdateBT(region);
    if (w) w->WeatherRegionTick(region, true);
}
// Weather effects: a client places the host's effects (KenshiWorld::WeatherRegionTick) where the
// host's game put them; the game's spawn code still runs, only the place it chose is replaced.
void* hk_effectCtor(void* self, void* effect, void* biome, const float* pos) {
    const float* forced = kenshi::EffectSpawnPosition();
    return o_effectCtor(self, effect, biome, forced ? forced : pos);
}
// What a lightning bolt or a gas cloud does to the characters it reaches is the host's call.
void hk_effectAffect(void* handler) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_effectAffect(handler);
}
// A client's weather never ends by itself (the host's does, and is then imposed): when the game
// thinks the current weather is over it still asks to rebuild the effect groups, which would wipe
// every effect placed for the host. Only a weather switch the host made rebuilds them.
void hk_regionUpdateEffects(void* region) {
    if (KenshiWorld::ClientActive())
        if (KenshiWorld* w = TheWorld(); w && !w->TakeEffectsRebuild(region)) return;
    o_regionUpdateEffects(region);
}
// Diagnostics: who stops the effects a client placed for the host.
void hk_effectStop(void* handler) {
    if (KenshiWorld::ClientActive() && !g_hostCall)
        if (KenshiWorld* w = TheWorld()) w->NoteEffectStop(handler, reinterpret_cast<uintptr_t>(_ReturnAddress()) - kenshi::Base());
    o_effectStop(handler);
}
void hk_seasonGetNewWeather(void* season) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_seasonGetNewWeather(season);
}

bool CallMoveOrderSEH(void* chr, const float* pos) {
    __try {
        o_moveOrder(chr, nullptr, nullptr, pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct HookDef {
    kenshi::Fn fn;
    void* detour;
    void** original;
};

std::vector<void*> g_installed;

} // namespace

void RunTick(bool live) {
    if (!g_tick) return;
    const DWORD tid = GetCurrentThreadId();
    std::atomic<DWORD>& seen = live ? g_liveThread : g_menuThread;
    if (seen.exchange(tid) != tid) Log("%s tick runs on thread %lu", live ? "main-loop" : "frame-listener", tid);
    std::unique_lock<std::mutex> lk(g_tickMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;   // another tick is running (other thread, or re-entered by the game)
    TickSEH(live);
}

double LastLiveTick() { return g_lastLiveTick.load(); }

HostCallScope::HostCallScope() { ++g_hostCall; }
HostCallScope::~HostCallScope() { --g_hostCall; }

bool CallPlayerMoveOrder(kenshi::Character* c, const kc::Vec3& pos) {
    if (!o_moveOrder || !kenshi::IsCharacter(c)) return false;
    const float p[3] = {pos.x, pos.y, pos.z};
    HostCallScope scope;
    return CallMoveOrderSEH(c, p);
}

bool InstallHooks(TickFn tick, std::string* err) {
    g_tick = tick;
    const HookDef defs[] = {
        {kenshi::FnMainLoop, reinterpret_cast<void*>(&hk_mainLoop), reinterpret_cast<void**>(&o_mainLoop)},
        {kenshi::FnPlayerMove, reinterpret_cast<void*>(&hk_playerMove), reinterpret_cast<void**>(&o_playerMove)},
        {kenshi::FnAddOrderSelected, reinterpret_cast<void*>(&hk_addOrder), reinterpret_cast<void**>(&o_addOrder)},
        {kenshi::FnAddTaskNearest, reinterpret_cast<void*>(&hk_addTaskNearest), reinterpret_cast<void**>(&o_addTaskNearest)},
        {kenshi::FnAddJobSelected, reinterpret_cast<void*>(&hk_addJob), reinterpret_cast<void**>(&o_addJob)},
        {kenshi::FnNewPlayerTaskSelected, reinterpret_cast<void*>(&hk_newTask), reinterpret_cast<void**>(&o_newTask)},
        {kenshi::FnSetOrderSelected, reinterpret_cast<void*>(&hk_setOrder), reinterpret_cast<void**>(&o_setOrder)},
        {kenshi::FnStopCharactersMovement, reinterpret_cast<void*>(&hk_stopMove), reinterpret_cast<void**>(&o_stopMove)},
        {kenshi::FnPlayerMoveOrderDefault, reinterpret_cast<void*>(&hk_moveOrder), reinterpret_cast<void**>(&o_moveOrder)},
        {kenshi::FnAIUpdate4Frame, reinterpret_cast<void*>(&hk_aiUpdate4), reinterpret_cast<void**>(&o_aiUpdate4)},
        {kenshi::FnAIPeriodicUpdate, reinterpret_cast<void*>(&hk_aiPeriodic), reinterpret_cast<void**>(&o_aiPeriodic)},
        {kenshi::FnMedApplyDamage, reinterpret_cast<void*>(&hk_medDamage), reinterpret_cast<void**>(&o_medDamage)},
        {kenshi::FnMedKnockout, reinterpret_cast<void*>(&hk_medKnockout), reinterpret_cast<void**>(&o_medKnockout)},
        {kenshi::FnDeclareDead, reinterpret_cast<void*>(&hk_declareDead), reinterpret_cast<void**>(&o_declareDead)},
        {kenshi::FnRagdollMode, reinterpret_cast<void*>(&hk_ragdollMode), reinterpret_cast<void**>(&o_ragdollMode)},
        {kenshi::FnCreateRandomCharacter, reinterpret_cast<void*>(&hk_createRandomCharacter), reinterpret_cast<void**>(&o_createChar)},
        {kenshi::FnRegionUpdateBT, reinterpret_cast<void*>(&hk_regionUpdateBT), reinterpret_cast<void**>(&o_regionUpdateBT)},
        {kenshi::FnSeasonGetNewWeather, reinterpret_cast<void*>(&hk_seasonGetNewWeather), reinterpret_cast<void**>(&o_seasonGetNewWeather)},
        {kenshi::FnEffectHandlerCtor, reinterpret_cast<void*>(&hk_effectCtor), reinterpret_cast<void**>(&o_effectCtor)},
        {kenshi::FnEffectAffectObjects, reinterpret_cast<void*>(&hk_effectAffect), reinterpret_cast<void**>(&o_effectAffect)},
        {kenshi::FnEffectStop, reinterpret_cast<void*>(&hk_effectStop), reinterpret_cast<void**>(&o_effectStop)},
        {kenshi::FnRegionUpdateEffects, reinterpret_cast<void*>(&hk_regionUpdateEffects), reinterpret_cast<void**>(&o_regionUpdateEffects)},
    };
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        if (err) *err = std::string("MinHook init failed: ") + MH_StatusToString(init);
        return false;
    }
    for (const auto& d : defs) {
        void* target = kenshi::FnAddr(d.fn);
        const MH_STATUS st = MH_CreateHook(target, d.detour, d.original);
        if (st != MH_OK) {
            if (err) *err = std::string("cannot hook ") + kenshi::kFunctions[d.fn].name + ": " + MH_StatusToString(st);
            RemoveHooks();
            return false;
        }
        g_installed.push_back(target);
    }
    // Enable all at once: MinHook suspends other threads while patching.
    const MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    if (en != MH_OK) {
        if (err) *err = std::string("cannot enable hooks: ") + MH_StatusToString(en);
        RemoveHooks();
        return false;
    }
    return true;
}

void RemoveHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    for (void* t : g_installed) MH_RemoveHook(t);
    g_installed.clear();
    g_tick = nullptr;
}

} // namespace kcp
