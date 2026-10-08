#include "hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <vector>

#include "util.h"
#include "world.h"

namespace kcp {

namespace {

TickFn g_tick = nullptr;
thread_local int g_hostCall = 0;   // >0 while KenshiCoop itself is calling into the game

// ---- originals
using MainLoopFn = void (*)(void* gw, float t);
using PlayerMoveFn = void (*)(void* pi, const float* pos, void* building);
using AddOrderFn = void (*)(void* pi, void* building, int task, void* subject, bool shift, bool addDontClear, const float* loc);
using NewTaskFn = void (*)(void* pi, int task, const void* targetHand, void* building, const float* clickPos, bool addDontClear);
using SetOrderFn = void (*)(void* pi, int order);
using StopMoveFn = void (*)(void* pi);
using MoveOrderFn = void (*)(void* chr, void* building, void* subject, const float* loc);
using AIUpdateFn = void (*)(void* ai, float t);

MainLoopFn o_mainLoop = nullptr;
PlayerMoveFn o_playerMove = nullptr;
AddOrderFn o_addOrder = nullptr;
NewTaskFn o_newTask = nullptr;
SetOrderFn o_setOrder = nullptr;
StopMoveFn o_stopMove = nullptr;
MoveOrderFn o_moveOrder = nullptr;
AIUpdateFn o_aiUpdate4 = nullptr;
AIUpdateFn o_aiPeriodic = nullptr;

void SafeTick() {
    // C++ exceptions must never unwind into game code.
    try {
        g_tick();
    } catch (const std::exception& e) {
        Log("tick exception: %s", e.what());
    } catch (...) {
        Log("tick exception (unknown)");
    }
}

void TickSEH() {
    __try {
        SafeTick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int reported = 0;
        if (reported++ < 5) Log("tick: access violation caught (code %08lx)", GetExceptionCode());
    }
}

void hk_mainLoop(void* gw, float t) {
    o_mainLoop(gw, t);
    if (g_tick) TickSEH();
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
bool AllowUnsyncedOrder() {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active) return true;
    if (v->client) {
        if (KenshiWorld* w = TheWorld()) w->Toast("Only movement orders are synchronized in this version.");
        return false;
    }
    if (ClassifySelection(*v).foreign) { ToastForeign(); return false; }
    return true;
}

void hk_addOrder(void* pi, void* building, int task, void* subject, bool shift, bool addDontClear, const float* loc) {
    if (AllowUnsyncedOrder()) o_addOrder(pi, building, task, subject, shift, addDontClear, loc);
}
void hk_newTask(void* pi, int task, const void* targetHand, void* building, const float* clickPos, bool addDontClear) {
    if (AllowUnsyncedOrder()) o_newTask(pi, task, targetHand, building, clickPos, addDontClear);
}
void hk_setOrder(void* pi, int order) {
    if (AllowUnsyncedOrder()) o_setOrder(pi, order);
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
        if (v->active && (v->foreign.count(chr) || (v->client && v->replicated.count(chr)))) return;
    }
    o_moveOrder(chr, building, subject, loc);
}

// Clients: replicated characters have no local brain; the host decides everything they do.
bool SkipAI(void* ai) {
    if (!KenshiWorld::ClientActive()) return false;
    auto v = KenshiWorld::View();
    if (!v->active || !v->client || v->replicated.empty()) return false;
    const void* c = kenshi::AICharacter(static_cast<kenshi::AI*>(ai));
    return c && v->replicated.count(c);
}
void hk_aiUpdate4(void* ai, float t) {
    if (!SkipAI(ai)) o_aiUpdate4(ai, t);
}
void hk_aiPeriodic(void* ai, float t) {
    if (!SkipAI(ai)) o_aiPeriodic(ai, t);
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
        {kenshi::FnNewPlayerTaskSelected, reinterpret_cast<void*>(&hk_newTask), reinterpret_cast<void**>(&o_newTask)},
        {kenshi::FnSetOrderSelected, reinterpret_cast<void*>(&hk_setOrder), reinterpret_cast<void**>(&o_setOrder)},
        {kenshi::FnStopCharactersMovement, reinterpret_cast<void*>(&hk_stopMove), reinterpret_cast<void**>(&o_stopMove)},
        {kenshi::FnPlayerMoveOrderDefault, reinterpret_cast<void*>(&hk_moveOrder), reinterpret_cast<void**>(&o_moveOrder)},
        {kenshi::FnAIUpdate4Frame, reinterpret_cast<void*>(&hk_aiUpdate4), reinterpret_cast<void**>(&o_aiUpdate4)},
        {kenshi::FnAIPeriodicUpdate, reinterpret_cast<void*>(&hk_aiPeriodic), reinterpret_cast<void**>(&o_aiPeriodic)},
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
