#include "hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "ranged.h"
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
thread_local int g_animReplay = 0; // >0 while KenshiCoop replays one of the host's animations

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
using CollapseFn = void (*)(void* med, bool medic, bool agony);
CollapseFn o_collapse = nullptr;
using CreateLabelFn = void* (*)(void* gui, const void* text, const float* colour, int size, int speed);
using LabelTrackFn = void (*)(void* label, const void* hand, const float* offset);
using LabelColorFn = void (*)(void* label, const float* colour);
CreateLabelFn o_createLabel = nullptr;
using GiveItemFn = bool (*)(void* chr, void* item, bool dropOnFail, bool destroyOnFail);
using DropItemFn = void (*)(void* chr, void* item);
GiveItemFn o_giveItem = nullptr;
using PickupFn = void (*)(void* pi, void* item);
PickupFn o_pickup = nullptr;
DropItemFn o_dropItem = nullptr;
LabelTrackFn o_labelTrack = nullptr;
LabelColorFn o_labelColor = nullptr;
// host: the damage number addWound is building (created, then tracked, then coloured)
thread_local void* t_floaterLabel = nullptr;
thread_local kc::AnimEvent t_floater;
thread_local kenshi::Character* t_floaterChar = nullptr;
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
using CombatAnimFn = void (*)(void* ac, void* technique, float speed, void* extra);
using PlayActionFn = void (*)(void* ac, void* anim, float speedMult, float weight, bool stumble);
using AnimBoolFn = bool (*)(void* ac);
using AnimBoolStrFn = bool (*)(void* ac, const void* name);
using AnimPtrFn = void (*)(void* ac, void* anim);
using AnimSetBoolFn = void (*)(void* ac, bool on);
using AnimCarryFn = void (*)(void* ac, bool carried, bool left, bool right);
CombatAnimFn o_animStartCombat = nullptr, o_animRunCombat = nullptr;
VoidFn o_animEndCombat = nullptr, o_animEndStumble = nullptr;
PlayActionFn o_animPlayAction = nullptr;
AnimBoolFn o_animStopAction = nullptr;
AnimBoolStrFn o_animStopActionNamed = nullptr;
AnimPtrFn o_animStartStumble = nullptr;
AnimSetBoolFn o_animSetCombatMode = nullptr;
AnimCarryFn o_animSetCarryMode = nullptr;
AnimSetBoolFn o_animGuardLegs = nullptr, o_animGuardUpper = nullptr;
using DrawWeaponFn = bool (*)(void* chr, void* item, void* section);
DrawWeaponFn o_drawWeapon = nullptr;
using SingleAnimUpdateFn = void (*)(void* single, float masterTime, float frameTime, bool sounds);
SingleAnimUpdateFn o_singleAnimUpdate = nullptr;
using TrackAnimMoveFn = void (*)(void* mov, bool on);
TrackAnimMoveFn o_trackAnimMove = nullptr;
using AnimSelectFn = void (*)(void* ac, float t);
AnimSelectFn o_animSelect = nullptr;
using CombatMoveFn = void (*)(void* mov, float ft, const float* pos, const float* dir, bool moving, float* repulsion, float* facingOut,
                              bool defensive, int state, float raceSpeedMult);
CombatMoveFn o_combatMove = nullptr;
VoidFn o_sheatheWeapon = nullptr;
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
    std::vector<kc::Handle> others;   // other players' characters
    size_t foreign = 0;
};
SelectionInfo ClassifySelection(const HookView& v) {
    SelectionInfo s;
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    for (const auto& h : sel) {
        if (v.controllable.count(h)) s.mine.push_back(h);
        else if (h.type == kenshi::kItemTypeCharacter || h.type == kenshi::kItemTypeAnimalCharacter) { ++s.foreign; s.others.push_back(h); }
    }
    return s;
}

void ToastForeign() {
    if (KenshiWorld* w = TheWorld()) w->Toast("Ce personnage appartient à un autre joueur.");
}

// Host: other players' characters in the host's selection (a click or a box on them, a squad the
// game selected) are taken out of it, so that the host's own order (move, stop, passive...) goes to
// the host's characters only. It used to be refused as a whole: the host could no longer move his
// own character, and a standing order he clicked (passive) showed on the squad bar but was never
// set. False when nothing of the host's is left to order.
bool HostDropForeign(const SelectionInfo& s) {
    if (!s.foreign) return true;
    for (const auto& h : s.others)
        if (void* o = kenshi::Resolve(h)) kenshi::UnselectObject(o);
    Log("host order: %zu character(s) of other players taken out of the host's selection", s.others.size());
    if (KenshiWorld* w = TheWorld()) w->Toast("Les persos des autres joueurs sont retirés de ta sélection : chacun commande les siens.");
    return !s.mine.empty();
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
    if (!HostDropForeign(s)) return;
    o_playerMove(pi, pos, building);
}

// Orders the player gives that only NPC jobs use (shopping trips, storage chores, the job centre):
// not available to client players. Trading goes through the merchant's conversation (the host sends
// the trade window to the player, see hk_showTrade) and containers through ClientLootContainer.
bool OpensWindow(int task) {
    switch (task) {
    case 55:    // RECRUIT_AT_JOBCENTER
    case 118:   // SHOPPING
    case 119:   // BUY_SHIT
    case 124:   // OPERATE_STORAGE
    case 284:   // LOOT_CONTAINER
        return true;
    default:
        return false;
    }
}

// A player order given through the game's UI. The host runs every order: on a client it becomes
// a command for each of the player's selected characters (or the nearest one), executed by the
// host's game for that character alone; the result comes back like everything else. On the host
// an order is refused only when the selection contains another player's character.
// Returns true when the caller must run the original function.
bool RouteOrder(kc::TaskVia via, int task, void* subject, const kc::Handle* subjectHandle, void* building, const float* loc,
                bool shift, bool add) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active) return true;
    const SelectionInfo s = ClassifySelection(*v);
    if (!v->client) {
        if (!HostDropForeign(s)) return false;
        if (via != kc::TaskVia::SetOrder || !s.foreign) return true;
        // A squad bar toggle with another player's character in the selection: the game's
        // setOrderSelectedCharacters takes on/off from the "main" selected character (a global
        // hand, the last one clicked), which can be the other player's character, or nothing once
        // it is unselected; it then turned the host's passive character passive again instead of
        // off. Done here from the host's own first character, on the host's characters only.
        kenshi::Character* first = kenshi::Resolve(s.mine.front());
        const bool on = task >= 10 && first ? !kenshi::GetStandingOrder(first, task) : true;
        HostCallScope scope;
        for (const auto& h : s.mine)
            if (kenshi::Character* c = kenshi::Resolve(h)) kenshi::SetStandingOrder(c, task, on);
        Log("host mode %d %s on %zu own character(s), the other players' left out", task, on ? "on" : "off", s.mine.size());
        return false;
    }
    KenshiWorld* w = TheWorld();
    if (!w) return false;
    if (OpensWindow(task)) {
        Log("client order refused (task %d opens a window)", task);
        w->Toast("Cette action n'est pas encore disponible en multijoueur.");
        return false;
    }
    if (s.mine.empty()) {
        if (s.foreign) ToastForeign();
        return false;
    }
    kc::Command c;
    c.kind = kc::CommandKind::Task;
    c.via = via;
    c.task = task;
    c.shift = shift;
    c.add = add;
    if (loc) c.pos = {loc[0], loc[1], loc[2]};
    if (subjectHandle) c.subject = *subjectHandle;
    else if (subject) kenshi::ObjectHandle(subject, c.subject);
    // what and where the subject is: the host finds it by that when its handle differs there
    void* subj = subject ? subject : (subjectHandle ? kenshi::ResolveObject(*subjectHandle) : nullptr);
    if (subj && !kenshi::IsCharacter(subj)) {
        kenshi::ObjectTemplate(subj, c.itemSid);
        kenshi::ObjectPosition(subj, c.subjectPos);
    }
    if (building) {
        kenshi::ObjectHandle(building, c.building);
        kenshi::ObjectTemplate(building, c.buildingSid);
        kenshi::ObjectPosition(building, c.buildingPos);
    }
    std::vector<kc::Handle> who = s.mine;
    if (via == kc::TaskVia::TaskNearest && loc && who.size() > 1) {   // the game picks the nearest one
        kc::Handle best = who.front();
        float bestD = 1e30f;
        for (const auto& h : who) {
            kc::Vec3 p;
            kenshi::Character* ch = w->FindSquad(h);
            if (!ch || !kenshi::GetPosition(ch, p)) continue;
            const float d = (p.x - loc[0]) * (p.x - loc[0]) + (p.z - loc[2]) * (p.z - loc[2]);
            if (d < bestD) { bestD = d; best = h; }
        }
        who = {best};
    }
    Log("client order task %d (via %d) for %zu character(s) asked of the host", task, int(via), who.size());
    for (const auto& h : who) w->QueueLocalOrder(h, c);
    if (via == kc::TaskVia::AddJob && shift && loc && o_addJob) {
        // a permanent job (Tâches panel): our copy takes it too, so the panel shows it at once; the
        // host's job lists (Session::ClientJobs) then keep it or take it away like any other
        for (const auto& h : who) {
            kenshi::Character* ch = w->FindSquad(h);
            if (!ch) continue;
            const int before = kenshi::PermajobCount(ch);
            kenshi::WithSelection(ch, [&] {
                HostCallScope scope;
                o_addJob(kenshi::Player(), task, subject ? subject : subj, shift, add, loc);
            });
            if (kenshi::PermajobCount(ch) > before) w->NoteLocalJob(h, kenshi::PermajobType(ch, kenshi::PermajobCount(ch) - 1));
        }
    }
    if (s.foreign) ToastForeign();
    return false;
}

// ---- fix G5: the Tâches panel. On a client, removing or moving a job of one of our characters (the
// panel's cross, a drag, the game's "remove job" of the selection) is done by the host's game, which
// runs the jobs; our copy does it too, and the host's job lists keep it in line (Session::ClientJobs).
// Another player's character keeps its jobs.
using PermajobIntFn = void (*)(void* c, int i);
using PermajobMoveFn = void (*)(void* c, int from, int to);
PermajobIntFn o_removePermajob = nullptr;
PermajobIntFn o_removeJob = nullptr;
PermajobMoveFn o_movePermajob = nullptr;
// False when the local game must not do it.
bool ClientJobChange(void* chr, kc::TaskVia via, int task, int a, int b) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active || !v->client || !kenshi::IsCharacter(chr)) return true;
    kc::Handle h;
    KenshiWorld* w = TheWorld();
    if (!w || !kenshi::GetHandle(static_cast<kenshi::Character*>(chr), h)) return true;
    if (!v->controllable.count(h)) {
        if (!v->squadForeign.count(chr)) return true;   // not a player's character
        ToastForeign();
        return false;
    }
    if (task < 0) return true;
    kc::Command c;
    c.kind = kc::CommandKind::Task;
    c.via = via;
    c.task = task;
    c.pos = {float(a), float(b), 0.0f};
    w->QueueLocalOrder(h, c);
    Log("client job change (via %d, task %d, slot %d -> %d) asked of the host", int(via), task, a, b);
    return true;
}
void hk_removePermajob(void* c, int slot) {
    const int task = kenshi::PermajobType(static_cast<kenshi::Character*>(c), slot);
    if (ClientJobChange(c, kc::TaskVia::RemovePermajob, task, slot, 0)) o_removePermajob(c, slot);
}
void hk_movePermajob(void* c, int from, int to) {
    const int task = kenshi::PermajobType(static_cast<kenshi::Character*>(c), from);
    if (ClientJobChange(c, kc::TaskVia::MovePermajob, task, from, to)) o_movePermajob(c, from, to);
}
void hk_removeJob(void* c, int task) {
    if (ClientJobChange(c, kc::TaskVia::RemoveJob, task, 0, 0)) o_removeJob(c, task);
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

// A right click on a container (chest, shelf, safe) on a client: the nearest of our selected
// characters goes there through the host, and the game's loot window opens here once it arrived.
bool ClientLootContainer(int task, void* subject) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active || !v->client || task != kTaskLootTarget || !subject || kenshi::IsCharacter(subject)) return false;
    std::vector<kc::ItemState> items;
    if (!kenshi::ReadInventory(subject, items)) return false;   // not a container
    KenshiWorld* w = TheWorld();
    if (!w) return false;
    const SelectionInfo s = ClassifySelection(*v);
    if (s.mine.empty()) { if (s.foreign) ToastForeign(); return true; }
    kc::Vec3 at, p;
    kenshi::ObjectPosition(subject, at);
    kenshi::Character* best = nullptr;
    float bestD = 1e30f;
    for (const auto& h : s.mine)
        if (kenshi::Character* c = w->FindSquad(h); c && kenshi::GetPosition(c, p)) {
            const float d = (p.x - at.x) * (p.x - at.x) + (p.z - at.z) * (p.z - at.z);
            if (d < bestD) { bestD = d; best = c; }
        }
    if (best) w->QueueContainerRequest(best, subject);
    return true;
}

void hk_addOrder(void* pi, void* building, int task, void* subject, bool shift, bool addDontClear, const float* loc) {
    if (ClientLoot(task, kenshi::IsCharacter(subject) ? static_cast<kenshi::Character*>(subject) : nullptr)) return;
    if (ClientLootContainer(task, subject)) return;
    if (RouteOrder(kc::TaskVia::AddOrder, task, subject, nullptr, building, loc, shift, addDontClear))
        o_addOrder(pi, building, task, subject, shift, addDontClear, loc);
}
void hk_newTask(void* pi, int task, const void* targetHand, void* building, const float* clickPos, bool addDontClear) {
    kc::Handle th;
    const bool haveTarget = kenshi::HandleFromHand(targetHand, th);
    if (ClientLoot(task, haveTarget ? kenshi::Resolve(th) : nullptr)) return;
    if (haveTarget && ClientLootContainer(task, kenshi::ResolveObject(th))) return;
    if (RouteOrder(kc::TaskVia::NewTask, task, nullptr, haveTarget ? &th : nullptr, building, clickPos, false, addDontClear))
        o_newTask(pi, task, targetHand, building, clickPos, addDontClear);
}
// The right-click "loot" on a body goes through this one (the nearest selected character acts).
void hk_addTaskNearest(void* pi, void* building, int task, void* subject, bool shift, const float* loc, bool noAnimals) {
    if (ClientLoot(task, kenshi::IsCharacter(subject) ? static_cast<kenshi::Character*>(subject) : nullptr)) return;
    if (ClientLootContainer(task, subject)) return;
    if (RouteOrder(kc::TaskVia::TaskNearest, task, subject, nullptr, building, loc, shift, noAnimals))
        o_addTaskNearest(pi, building, task, subject, shift, loc, noAnimals);
}
void hk_addJob(void* pi, int task, void* subject, bool shift, bool add, const float* loc) {
    if (RouteOrder(kc::TaskVia::AddJob, task, subject, nullptr, nullptr, loc, shift, add)) o_addJob(pi, task, subject, shift, add, loc);
}
void hk_setOrder(void* pi, int order) {
    if (RouteOrder(kc::TaskVia::SetOrder, order, nullptr, nullptr, nullptr, nullptr, false, false)) o_setOrder(pi, order);
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
    if (!HostDropForeign(s)) return;
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
    if (!loadingSavestate && med && kenshi::GodMode(static_cast<uint8_t*>(med) - 0x458)) return;   // admin god mode
    o_medDamage(med, part, damage, loadingSavestate, canSever, force);
}
void hk_medKnockout(void* med, float skill01) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    if (med && kenshi::GodMode(static_cast<uint8_t*>(med) - 0x458)) return;
    o_medKnockout(med, skill01);
}
// ---- conversations and the AI's decisions
// The host's world decides everything an NPC does: on clients every decision point of the AI is
// refused (squad AI, crimes and bounties, faction relations, raids, conversations), and the host's
// speech bubbles and conversation windows are replayed. The task system itself keeps running: it
// is what walks a character to the destination the host's state gives it (refusing it leaves
// characters sliding, facing nowhere); what a task could decide is refused one hook at a time.
bool ClientRefuses() { return KenshiWorld::ClientActive() && !g_hostCall; }

using SayFn = void (*)(void* d, const void* text, void* line);
SayFn o_say = nullptr;
void hk_say(void* d, const void* text, void* line) {
    if (ClientRefuses()) return;
    if (!g_hostCall && KenshiWorld::View()->active) {
        std::string s;
        if (KenshiWorld* w = TheWorld(); w && kenshi::ReadStdString(text, s) && !s.empty()) w->NoteSay(d, s);
    }
    o_say(d, text, line);
}
using DialogBoolFn = void (*)(void* d, bool on);
DialogBoolFn o_setInDialog = nullptr;
void hk_setInDialog(void* d, bool on) {
    auto v = KenshiWorld::View();
    // Clients never show the game's own conversation window (a conversation from the save, or one
    // the local game starts): conversations are the host's, shown in our window.
    if (v->active && v->client && on && !g_hostCall) return;
    // another player's conversation: it opens on their screen, not on the host's
    if (v->active && !v->client) {   // any thread: the game prepares conversations on worker threads too
        if (KenshiWorld* w = TheWorld(); w && w->NoteDialogWindow(d, on)) return;
    }
    o_setInDialog(d, on);
}
using DialogFn = void (*)(void* d);
DialogFn o_setResponses = nullptr, o_setReplyText = nullptr;
void hk_setResponses(void* d) {
    auto v = KenshiWorld::View();
    if (v->active && !v->client) {
        if (KenshiWorld* w = TheWorld(); w && w->NoteDialogText(d)) return;
    }
    o_setResponses(d);
}
void hk_setReplyText(void* d) {
    auto v = KenshiWorld::View();
    if (v->active && !v->client) {
        if (KenshiWorld* w = TheWorld(); w && w->NoteDialogText(d)) return;
    }
    o_setReplyText(d);
}
using ReplyClickedFn = void (*)(void* d, int index);
ReplyClickedFn o_replyClicked = nullptr;
using SendEventFn = bool (*)(void* d, void* who, int ev);
SendEventFn o_sendEvent = nullptr;
bool hk_sendEvent(void* d, void* who, int ev) { return ClientRefuses() ? false : o_sendEvent(d, who, ev); }
using SendEventOverrideFn = bool (*)(void* d, void* who, int ev, bool force);
SendEventOverrideFn o_sendEventOverride = nullptr;
bool hk_sendEventOverride(void* d, void* who, int ev, bool force) { return ClientRefuses() ? false : o_sendEventOverride(d, who, ev, force); }
using StartConvFn = bool (*)(void* d, void* target, void* line, int ev, bool force);
StartConvFn o_startConv = nullptr;
bool hk_startConv(void* d, void* target, void* line, int ev, bool force) { return ClientRefuses() ? false : o_startConv(d, target, line, ev, force); }
using StartPlayerConvFn = bool (*)(void* d, void* target, void* line);
StartPlayerConvFn o_startPlayerConv = nullptr;
bool hk_startPlayerConv(void* d, void* target, void* line) { return ClientRefuses() ? false : o_startPlayerConv(d, target, line); }
using PtrFn = void (*)(void* self, void* p);
PtrFn o_doActions = nullptr, o_assessCrimes = nullptr, o_assignBounty = nullptr;
void hk_doActions(void* d, void* line) { if (!ClientRefuses()) o_doActions(d, line); }
void hk_assessCrimes(void* s, void* c) { if (!ClientRefuses()) o_assessCrimes(s, c); }
void hk_assignBounty(void* b, void* f) { if (!ClientRefuses()) o_assignBounty(b, f); }
using FloatBoolFn = void (*)(void* self, float t, bool b);
FloatBoolFn o_dialogAssessment = nullptr;
void hk_dialogAssessment(void* s, float t, bool b) { if (!ClientRefuses()) o_dialogAssessment(s, t, b); }
using FloatFn = void (*)(void* self, float t);
FloatFn o_bbUpdate = nullptr, o_bbPeriodic = nullptr, o_uniquePeriodic = nullptr;
void hk_bbUpdate(void* b, float t) { if (!ClientRefuses()) o_bbUpdate(b, t); }
void hk_bbPeriodic(void* b, float t) { if (!ClientRefuses()) o_bbPeriodic(b, t); }
void hk_uniquePeriodic(void* m, float t) { if (!ClientRefuses()) o_uniquePeriodic(m, t); }
VoidFn o_warPeriodic = nullptr;
void hk_warPeriodic(void* m) { if (!ClientRefuses()) o_warPeriodic(m); }
using AffectAmountFn = void (*)(void* r, void* f, float amount, float mult);
AffectAmountFn o_affectAmount = nullptr;
void hk_affectAmount(void* r, void* f, float amount, float mult) { if (!ClientRefuses()) o_affectAmount(r, f, amount, mult); }
using AffectEventFn = void (*)(void* r, void* f, int ev, float mult);
AffectEventFn o_affectEvent = nullptr;
void hk_affectEvent(void* r, void* f, int ev, float mult) { if (!ClientRefuses()) o_affectEvent(r, f, ev, mult); }
using SetRelationFn = void (*)(void* r, void* f, float to);
SetRelationFn o_setRelation = nullptr;
void hk_setRelation(void* r, void* f, float to) { if (!ClientRefuses()) o_setRelation(r, f, to); }
using SetCrimeFn = bool (*)(void* b, int crime, void* against, const void* agnst);
SetCrimeFn o_setCrime = nullptr;
bool hk_setCrime(void* b, int crime, void* against, const void* agnst) { return ClientRefuses() ? false : o_setCrime(b, crime, against, agnst); }

// Squads: on a client, dropping one of our portraits on a squad asks the host (another member of
// that squad tells it which one; none: a new squad); other players' characters stay where they
// are. The host organises everyone's squads.
using AddAtFn = void (*)(void* squad, void* c, int index);
AddAtFn o_addAt = nullptr;
void hk_addAt(void* squad, void* c, int index) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active || !kenshi::IsCharacter(c)) return o_addAt(squad, c, index);
    kc::Handle h;
    kenshi::GetHandle(static_cast<kenshi::Character*>(c), h);
    const bool mine = v->controllable.count(h) != 0;
    if (!v->client) return o_addAt(squad, c, index);   // the host organises everyone's squads
    KenshiWorld* w = TheWorld();
    if (!mine || !w) { if (w) ToastForeign(); return; }
    kc::Command cmd;
    cmd.kind = kc::CommandKind::SquadMove;
    cmd.task = std::max(0, index);
    std::vector<kenshi::Character*> members;
    kenshi::SquadMembers(squad, members);
    for (kenshi::Character* m : members)
        if (m != c && kenshi::GetHandle(m, cmd.subject)) break;
    if (members.empty() || (members.size() == 1 && members[0] == c)) cmd.subject = kc::Handle{};
    Log("client squad change asked of the host");
    w->QueueLocalOrder(h, cmd);
}

// The game's character editor was confirmed: its characters have new looks (and maybe a new name)
// to show everyone. The editor commits them while it closes.
using GuiFn = void (*)(void* gui);
GuiFn o_closeEditor = nullptr;
void hk_closeEditor(void* gui) {
    std::vector<kenshi::Character*> chars;
    kenshi::EditorCharacters(chars);
    o_closeEditor(gui);
    if (KenshiWorld* w = TheWorld(); w && KenshiWorld::View()->active)
        for (kenshi::Character* c : chars) w->NoteEdited(c);
}

// Trade windows: on the host, a merchant's "let's trade" in another player's conversation asks the
// game for a trade window between that player's character and the merchant. It must open on that
// player's screen, not ours: the session sends it there (with the shop's stock).
// Every trade window goes through ForgottenGUI::showTradeWindow (the dialogue's "trade" action, the
// loot/trade task of a right click): this is the one place to catch them. Whoever calls it: the
// client's own order and its dialogue answer run inside a HostCallScope. And either side: the
// dialogue action passes (its target, its owner), so a conversation the player started arrives
// with the merchant first and the player second.
using ShowTradeFn = void (*)(void* gui, const void* a, const void* b, int type);
ShowTradeFn o_showTrade = nullptr;
void hk_showTrade(void* gui, const void* a, const void* b, int type) {
    auto v = KenshiWorld::View();
    KenshiWorld* w = TheWorld();
    if (w && v->active && !v->client) {
        kc::Handle ha, hb;
        const bool haveA = kenshi::HandleFromHand(a, ha), haveB = kenshi::HandleFromHand(b, hb);
        kenshi::Character* ca = haveA ? kenshi::Resolve(ha) : nullptr;
        kenshi::Character* cb = haveB ? kenshi::Resolve(hb) : nullptr;
        // the player second only for the dialogue's trade (type 1): in a loot (type 3) `a` is always the
        // looter, and the host's character looting another player's body must keep its window
        const bool fa = ca && v->squadForeign.count(ca), fb = !fa && type == 1 && cb && v->squadForeign.count(cb);
        if (fa || fb) {
            // TW_MONEY_TRADING (a merchant's "let's trade"), or TW_AUTO on a merchant standing there
            // (a right click on it): a trade window for that player
            const kc::Handle& player = fa ? ha : hb;
            const kc::Handle& trader = fa ? hb : ha;
            kenshi::Character* other = fa ? cb : ca;
            const bool haveOther = fa ? haveB : haveA;
            const bool merchant = other && !kenshi::IsDead(other) && !kenshi::IsDown(other) && !kenshi::IsUnconscious(other);
            if (haveOther && (type == 1 || (type == 3 && merchant))) {
                w->QueueTradeRequest(player, trader);
                Log("trade window type %d for another player's character (%s side%s): sent to that player", type, fa ? "first" : "second",
                    g_hostCall ? ", from its own order" : "");
            } else {
                Log("trade window type %d for another player's character: not opened here", type);
            }
            return;
        }
        if (!g_hostCall && type == 1 && haveB) w->NoteHostTradeWindow(ha, hb);
    }
    o_showTrade(gui, a, b, type);
}

// ---- lot D: prisons. Being put in a cage or taken out, shackled or freed, enslaved: the host's
// game decides for every character; on clients only the host's state is imposed (ApplyCaptive).
using PrisonModeFn = void (*)(void* c, bool on, void* cage);
PrisonModeFn o_prisonMode = nullptr;
void hk_prisonMode(void* c, bool on, void* cage) {
    if (KenshiWorld::ClientActive() && !g_hostCall && KenshiWorld::View()->replicated.count(c)) return;
    o_prisonMode(c, on, cage);
}
using ChainedModeFn = void (*)(void* c, bool on, const void* owner);
ChainedModeFn o_chainedMode = nullptr;
void hk_chainedMode(void* c, bool on, const void* owner) {
    if (KenshiWorld::ClientActive() && !g_hostCall && KenshiWorld::View()->replicated.count(c)) return;
    o_chainedMode(c, on, owner);
}
using SlaveStateFn = void (*)(void* sbd, int state);
SlaveStateFn o_slaveState = nullptr;
void hk_slaveState(void* sbd, int state) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_slaveState(sbd, state);
}

// A building's own inventory panel (click on a mine, a farm, a machine): on a client its local copy
// holds nothing the host produced. The panel still opens, and the player's nearest character goes to
// it through the host, which then sends the real contents (the container path).
using ShowInvBuildingFn = void* (*)(void* gui, const void* owner);
ShowInvBuildingFn o_showInvBuilding = nullptr;
void* hk_showInvBuilding(void* gui, const void* owner) {
    void* r = o_showInvBuilding(gui, owner);
    auto v = KenshiWorld::View();
    kc::Handle h;
    if (g_hostCall || !v->active || !v->client || !owner || !kenshi::HandleFromHand(owner, h)) return r;
    void* obj = kenshi::ResolveObject(h);
    std::vector<kc::ItemState> items;
    if (!obj || kenshi::IsCharacter(obj) || !kenshi::ReadInventory(obj, items)) return r;
    KenshiWorld* w = TheWorld();
    const SelectionInfo s = ClassifySelection(*v);
    kc::Vec3 at, p;
    kenshi::ObjectPosition(obj, at);
    kenshi::Character* best = nullptr;
    float bestD = 1e30f;
    for (const auto& sh : s.mine)
        if (kenshi::Character* c = w ? w->FindSquad(sh) : nullptr; c && kenshi::GetPosition(c, p)) {
            const float d = (p.x - at.x) * (p.x - at.x) + (p.z - at.z) * (p.z - at.z);
            if (d < bestD) { bestD = d; best = c; }
        }
    if (best) w->QueueContainerRequest(best, obj);
    return r;
}

// Picking a body up on a client happens only when the host's character does (see ApplyCarry).
using PickFn = void (*)(void* c, void* who);
PickFn o_pickChar = nullptr;
void hk_pickChar(void* c, void* who) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_pickChar(c, who);
}

// Standing orders (stealth, hold, passive...) of the host's characters are the host's: on clients the
// local game may not flip them (it did, now and then, put a character in stealth by itself).
using StandingFn = void (*)(void* c, int order, bool on);
StandingFn o_standing = nullptr;
void hk_standing(void* c, int order, bool on) {
    if (KenshiWorld::ClientActive() && !g_hostCall && KenshiWorld::View()->replicated.count(c)) return;
    o_standing(c, order, on);
}

// Experience is the host's too: every gain (combat, training, walking, first aid...) ends in
// increaseStat, refused on clients; the host's skill levels arrive in Progress messages.
using IncreaseStatFn = void (*)(float* stat, float amount, float upperLimit);
IncreaseStatFn o_increaseStat = nullptr;
void hk_increaseStat(float* stat, float amount, float upperLimit) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_increaseStat(stat, amount, upperLimit);
}
// Nor do they decide a collapse (pain, crippled limbs): it would leave a character the host has
// standing lying on the ground, flickering between hurt and unconscious.
void hk_collapse(void* med, bool medic, bool agony) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_collapse(med, medic, agony);
}
// A client's "pick up" goes to the host: the nearest selected character of ours walks there and the
// host's game takes it (the pickup then reaches everyone like any other).
void hk_pickup(void* pi, void* item) {
    auto v = KenshiWorld::View();
    if (g_hostCall || !v->active || !v->client) return o_pickup(pi, item);
    kc::Handle ih;
    kc::ItemState st;
    kc::Vec3 at;
    KenshiWorld* w = TheWorld();
    if (!w || !item || !kenshi::DescribeGroundItem(item, ih, st, at)) return;
    const SelectionInfo s = ClassifySelection(*v);
    kc::Handle best;
    float bestD = 1e30f;
    for (const auto& h : s.mine) {
        kc::Vec3 p;
        kenshi::Character* c = w->Find(h);
        if (!c || !kenshi::GetPosition(c, p)) continue;
        const float d = (p.x - at.x) * (p.x - at.x) + (p.z - at.z) * (p.z - at.z);
        if (d < bestD) { bestD = d; best = h; }
    }
    if (!best.valid()) { if (s.foreign) ToastForeign(); return; }
    kc::Command c;
    c.kind = kc::CommandKind::PickUp;
    c.pos = at;
    c.itemSid = st.templateSid;
    w->QueueLocalOrder(best, c);
}
// Items on the ground belong to the host's world: its pickups and drops are replayed on clients,
// whose own (for host-driven characters) are refused.
bool hk_giveItem(void* chr, void* item, bool dropOnFail, bool destroyOnFail) {
    if (!item || !kenshi::ItemLoose(item)) return o_giveItem(chr, item, dropOnFail, destroyOnFail);   // inventory to inventory
    if (KenshiWorld::ClientActive()) {
        if (!g_hostCall && KenshiWorld::View()->replicated.count(chr)) return false;
        return o_giveItem(chr, item, dropOnFail, destroyOnFail);
    }
    kc::GroundEvent e;
    e.kind = kc::GroundKind::PickedUp;
    const bool known = kenshi::DescribeGroundItem(item, e.item, e.state, e.pos);
    const bool ok = o_giveItem(chr, item, dropOnFail, destroyOnFail);
    if (ok && known && KenshiWorld::View()->active)
        if (KenshiWorld* w = TheWorld()) w->NoteGround(e);
    return ok;
}
void hk_dropItem(void* chr, void* item) {
    if (KenshiWorld::ClientActive() && !g_hostCall && KenshiWorld::View()->replicated.count(chr)) {
        // the player dropped it from one of their characters: the host does it (and everyone sees it)
        if (KenshiWorld* w = TheWorld(); w && item && kenshi::IsCharacter(chr)) w->QueueLocalDrop(static_cast<kenshi::Character*>(chr), item);
        return;
    }
    o_dropItem(chr, item);
    if (KenshiWorld::ClientActive() || !KenshiWorld::View()->active || !item || !kenshi::ItemOnGround(item)) return;
    kc::GroundEvent e;
    e.kind = kc::GroundKind::Dropped;
    if (kenshi::DescribeGroundItem(item, e.item, e.state, e.pos))
        if (KenshiWorld* w = TheWorld()) w->NoteGround(e);
}
std::string ColourHex(const float* c) {
    char b[16];
    auto u = [](float f) { return unsigned(std::lround(std::fmin(1.0f, std::fmax(0.0f, f)) * 255)); };
    snprintf(b, sizeof(b), "%02X%02X%02X%02X", u(c[0]), u(c[1]), u(c[2]), u(c[3]));
    return b;
}
// Damage numbers are made by addWound, which clients never run: the host's are shown on clients.
void* hk_createLabel(void* gui, const void* text, const float* colour, int size, int speed) {
    void* label = o_createLabel(gui, text, colour, size, speed);
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - kenshi::Base();
    if (label && colour && from >= kenshi::kAddWoundBegin && from < kenshi::kAddWoundEnd && !KenshiWorld::ClientActive() &&
        KenshiWorld::View()->active) {
        std::string s;
        if (text) kenshi::ReadStdString(text, s);
        t_floaterLabel = label;
        t_floaterChar = nullptr;
        t_floater = kc::AnimEvent{};
        t_floater.kind = kc::AnimKind::Floater;
        t_floater.name = ColourHex(colour) + "|" + s;
        t_floater.flags = uint8_t((size & 0xF) | ((speed & 0xF) << 4));
    }
    return label;
}
void hk_labelTrack(void* label, const void* hand, const float* offset) {
    o_labelTrack(label, hand, offset);
    if (label && label == t_floaterLabel) {
        t_floaterChar = kenshi::CharacterOfHand(hand);
        if (t_floaterChar)
            if (KenshiWorld* w = TheWorld()) w->NoteAnim(t_floaterChar, t_floater);
    }
}
void hk_labelColor(void* label, const float* colour) {
    o_labelColor(label, colour);
    if (label && colour && label == t_floaterLabel && t_floaterChar) {
        kc::AnimEvent e;
        e.kind = kc::AnimKind::FloaterColor;
        e.name = ColourHex(colour);
        if (KenshiWorld* w = TheWorld()) w->NoteAnim(t_floaterChar, e);
    }
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
    // host: a body the admin TP is moving gets up until it got there (fix G7)
    if (on && !g_hostCall)
        if (KenshiWorld* w = TheWorld(); w && w->HoldsUpright(chr)) return;
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
// Animations beyond walking (attacks, blocks, actions, stumbles, combat and carry modes): the host
// reports every one its characters start or stop; a client never starts them by itself on the
// host's characters and plays the host's instead (KenshiWorld::ApplyAnim, inside a HostCallScope).
// Returns true when the call must not run here.
bool AnimHookBlocked(void* ac, kc::AnimEvent* report) {
    kenshi::Character* c = kenshi::AnimOwner(ac);
    if (!c) return false;
    if (KenshiWorld::ClientActive()) {
        const bool blocked = !g_animReplay && KenshiWorld::View()->replicated.count(c) > 0;
        static std::atomic<int> logged{0};
        if (!g_animReplay && !blocked && report && logged.fetch_add(1) < 40)
            Log("anim not blocked here: kind=%d name='%s' (character not driven by the host)", int(report->kind), report->name.c_str());
        return blocked;
    }
    if (report && KenshiWorld::View()->active)
        if (KenshiWorld* w = TheWorld()) w->NoteAnim(c, std::move(*report));
    return false;
}
void hk_animStartCombat(void* ac, void* technique, float speed, void* extra) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::Combat;
    e.name = kenshi::TechniqueName(technique);
    e.a = speed;
    if (!AnimHookBlocked(ac, &e)) o_animStartCombat(ac, technique, speed, extra);
}
void hk_animRunCombat(void* ac, void* technique, float speed, void* extra) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::CombatRun;
    e.name = kenshi::TechniqueName(technique);
    e.a = speed;
    if (!AnimHookBlocked(ac, &e)) o_animRunCombat(ac, technique, speed, extra);
}
void hk_animEndCombat(void* ac) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::EndCombat;
    if (!AnimHookBlocked(ac, &e)) o_animEndCombat(ac);
}
void hk_animPlayAction(void* ac, void* anim, float speedMult, float weight, bool stumble) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::Action;
    e.name = kenshi::AnimDataName(anim);
    e.a = speedMult;
    e.b = weight;
    e.flags = stumble ? 1 : 0;
    if (!AnimHookBlocked(ac, &e)) o_animPlayAction(ac, anim, speedMult, weight, stumble);
}
bool hk_animStopAction(void* ac) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::StopAction;
    return AnimHookBlocked(ac, &e) ? false : o_animStopAction(ac);
}
bool hk_animStopActionNamed(void* ac, const void* name) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::StopAction;
    if (name) kenshi::ReadStdString(name, e.name);
    return AnimHookBlocked(ac, &e) ? false : o_animStopActionNamed(ac, name);
}
void hk_animStartStumble(void* ac, void* anim) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::Stumble;
    e.name = kenshi::AnimDataName(anim);
    if (!AnimHookBlocked(ac, &e)) o_animStartStumble(ac, anim);
}
void hk_animEndStumble(void* ac) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::EndStumble;
    if (!AnimHookBlocked(ac, &e)) o_animEndStumble(ac);
}
// The game sets these every frame: only changes are reported.
void hk_animSetCombatMode(void* ac, bool on) {
    kenshi::Character* c = kenshi::AnimOwner(ac);
    kenshi::AnimModes m;
    kc::AnimEvent e;
    e.kind = kc::AnimKind::CombatMode;
    e.flags = on ? 1 : 0;
    const bool changed = !c || !kenshi::ReadAnimModes(c, m) || m.combat != on;
    if (!AnimHookBlocked(ac, changed ? &e : nullptr)) o_animSetCombatMode(ac, on);
}
void HookGuard(void* ac, bool on, bool legs) {
    kenshi::Character* c = kenshi::AnimOwner(ac);
    kenshi::AnimModes m;
    kc::AnimEvent e;
    e.kind = legs ? kc::AnimKind::GuardLegs : kc::AnimKind::GuardUpper;
    e.flags = on ? 1 : 0;
    const bool changed = !c || !kenshi::ReadAnimModes(c, m) || (legs ? m.guardLegs : m.guardUpper) != on;
    if (!AnimHookBlocked(ac, changed ? &e : nullptr)) (legs ? o_animGuardLegs : o_animGuardUpper)(ac, on);
}
void hk_animGuardLegs(void* ac, bool on) { HookGuard(ac, on, true); }
void hk_animGuardUpper(void* ac, bool on) { HookGuard(ac, on, false); }
void hk_animSetCarryMode(void* ac, bool carried, bool left, bool right) {
    kenshi::Character* c = kenshi::AnimOwner(ac);
    kenshi::AnimModes m;
    kc::AnimEvent e;
    e.kind = kc::AnimKind::Carry;
    e.flags = uint8_t((carried ? 1 : 0) | (left ? 2 : 0) | (right ? 4 : 0));
    const bool changed = !c || !kenshi::ReadAnimModes(c, m) || m.carried != carried || m.carryLeft != left || m.carryRight != right;
    if (!AnimHookBlocked(ac, changed ? &e : nullptr)) o_animSetCarryMode(ac, carried, left, right);
}
// Weapons drawn and put away follow the host too (otherwise a client sees the host's fighter
// swinging bare hands).
bool WeaponHookBlocked(void* chr, kc::AnimEvent* report) {
    kenshi::Character* c = kenshi::IsCharacter(chr) ? static_cast<kenshi::Character*>(chr) : nullptr;
    if (!c) return false;
    if (KenshiWorld::ClientActive()) return !g_animReplay && KenshiWorld::View()->replicated.count(c) > 0;
    if (KenshiWorld::View()->active)
        if (KenshiWorld* w = TheWorld()) w->NoteAnim(c, std::move(*report));
    return false;
}
bool hk_drawWeapon(void* chr, void* item, void* section) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::DrawWeapon;
    std::string sec;
    if (section) kenshi::ReadStdString(section, sec);
    e.name = kenshi::ItemTemplate(item) + "\t" + sec;
    return WeaponHookBlocked(chr, &e) ? false : o_drawWeapon(chr, item, section);
}
void hk_sheatheWeapon(void* chr) {
    kc::AnimEvent e;
    e.kind = kc::AnimKind::Sheathe;
    if (!WeaponHookBlocked(chr, &e)) o_sheatheWeapon(chr);
}
// A host-driven character whose animations we mirror does not pick its own: what the host's plays
// (created by KenshiWorld::ApplyAnimFrame, timed by the update hook) is all it plays.
void hk_animSelect(void* ac, float t) {
    if (KenshiWorld::ClientActive() && KenshiWorld::View()->anims.count(ac)) return;
    o_animSelect(ac, t);
}
// A host-driven character goes where the host's is, never where its own animations would carry it
// (lunges, steps back): both together made it jump.
void hk_trackAnimMove(void* mov, bool on) {
    if (on && KenshiWorld::ClientActive() && KenshiWorld::View()->facing.count(mov)) on = false;
    o_trackAnimMove(mov, on);
}
// What is on screen: each animation of a host-driven character plays at the host's time and
// weight; one the host does not play is silenced. Runs right before the game advances it.
std::atomic<uint64_t> g_animHookCalls{0}, g_animHookTargets{0}, g_animHookMatched{0}, g_animHookSilenced{0};
void hk_singleAnimUpdate(void* single, float masterTime, float frameTime, bool sounds) {
    g_animHookCalls.fetch_add(1, std::memory_order_relaxed);
    if (KenshiWorld::ClientActive()) {
        auto v = KenshiWorld::View();
        if (!v->anims.empty()) {
            auto it = v->anims.find(kenshi::SingleAnimOwner(single));
            std::string name;
            if (it != v->anims.end() && kenshi::SingleAnimName(single, name)) {
                g_animHookTargets.fetch_add(1, std::memory_order_relaxed);
                const auto& target = *it->second;
                const kc::AnimEntry* match = nullptr;
                for (const auto& e : target.anims)
                    if (e.anim == name) { match = &e; break; }
                if (match) {
                    g_animHookMatched.fetch_add(1, std::memory_order_relaxed);
                    const float late = float(NowSeconds() - target.sampledAt) * v->gameSpeed;
                    const float want = match->time + match->speed * late;
                    float mine = want;
                    kenshi::ReadSingleAnimTime(single, mine);
                    // keep our own smooth progress unless it drifted: jumping the time every frame jitters
                    const float t = kenshi::SyncedAnimTime(single, mine, want, match->looped, v->gameSpeed);
                    kenshi::WriteSingleAnim(single, t, match->speed, match->weight, match->desired);
                } else {
                    g_animHookSilenced.fetch_add(1, std::memory_order_relaxed);
                    float t = 0;
                    kenshi::WriteSingleAnim(single, t, 0.0f, 0.0f, 0.0f);
                }
            }
        }
    }
    o_singleAnimUpdate(single, masterTime, frameTime, sounds);
}
// In a fight, a client's game turns each fighter toward its own idea of the target: the host's
// facing replaces it.
void hk_combatMove(void* mov, float ft, const float* pos, const float* dir, bool moving, float* repulsion, float* facingOut, bool defensive,
                   int state, float raceSpeedMult) {
    o_combatMove(mov, ft, pos, dir, moving, repulsion, facingOut, defensive, state, raceSpeedMult);
    if (!facingOut || !KenshiWorld::ClientActive()) return;
    auto v = KenshiWorld::View();
    auto it = v->facing.find(mov);
    if (it == v->facing.end()) return;
    facingOut[0] = it->second.x;
    facingOut[1] = it->second.y;
    facingOut[2] = it->second.z;
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

// ---- lot E: buildings. Build mode builds its placed previews through <PreviewGroup>::
// createBuildings, which calls the factory once per building with the final values: a client builds
// nothing (the host builds it, then everyone); the host's own placements are noted for everyone.
// Buying and dismantling confirmed by a client are asked of the host; clients never build nor
// dismantle by themselves (the host's progress is imposed).
thread_local int g_buildCapture = 0;   // >0 inside build mode's createBuildings (not our own calls)
using CreateFromPreviewsFn = void (*)(void* group);
CreateFromPreviewsFn o_createFromPreviews = nullptr;
void hk_createFromPreviews(void* group) {
    if (g_hostCall || !KenshiWorld::View()->active) return o_createFromPreviews(group);
    ++g_buildCapture;
    o_createFromPreviews(group);
    --g_buildCapture;
}
using CreateBuildingFn = void* (*)(void* factory, void* data, const float* pos, void* town, void* faction, const float* rot, void* cb, void* layout,
                                   void* doorOf, void* save, void* indoors, bool invisible, bool completed, bool foliage, int floor, bool outside);
CreateBuildingFn o_createBuilding = nullptr;
void* hk_createBuilding(void* factory, void* data, const float* pos, void* town, void* faction, const float* rot, void* cb, void* layout,
                        void* doorOf, void* save, void* indoors, bool invisible, bool completed, bool foliage, int floor, bool outside) {
    KenshiWorld* w = TheWorld();
    if (g_buildCapture <= 0 || g_hostCall || !w || !data || !pos || !rot)
        return o_createBuilding(factory, data, pos, town, faction, rot, cb, layout, doorOf, save, indoors, invisible, completed, foliage, floor, outside);
    KenshiWorld::BuildArgs a;
    a.data = data;
    a.pos = {pos[0], pos[1], pos[2]};
    a.rot = {rot[0], rot[1], rot[2], rot[3]};
    a.town = town;
    a.callback = cb;
    a.layout = layout;
    a.indoors = indoors;
    a.floor = floor;
    a.outside = outside;
    if (KenshiWorld::ClientActive()) {   // the host builds it (build mode copes with nothing built)
        w->NoteBuildCapture(a, nullptr);
        return nullptr;
    }
    void* b = o_createBuilding(factory, data, pos, town, faction, rot, cb, layout, doorOf, save, indoors, invisible, completed, foliage, floor, outside);
    if (b) w->NoteBuildCapture(a, b);
    return b;
}
using BuildingAnswerFn = void (*)(void* building, int answer);
BuildingAnswerFn o_buyMe = nullptr;
BuildingAnswerFn o_confirmDismantle = nullptr;
void hk_buyMe(void* building, int answer) {
    auto v = KenshiWorld::View();
    KenshiWorld* w = TheWorld();
    if (g_hostCall || !v->active || !w) return o_buyMe(building, answer);
    if (v->client) {
        if (answer == 2) w->NoteLocalBuildAction(building, kc::BuildActionKind::Buy, answer);
        return;
    }
    o_buyMe(building, answer);
    if (answer == 2) w->NoteHostBought(building);
}
void hk_confirmDismantle(void* building, int answer) {
    auto v = KenshiWorld::View();
    KenshiWorld* w = TheWorld();
    if (g_hostCall || !v->active || !v->client || !w) return o_confirmDismantle(building, answer);
    if (answer == 2) w->NoteLocalBuildAction(building, kc::BuildActionKind::Dismantle, answer);
}
using BuildProgressFn = void (*)(void* building, float amount);
BuildProgressFn o_addConstruction = nullptr;
void hk_addConstruction(void* building, float amount) {
    // A client refused all of it once, so a town only it had loaded stayed in red sticks: the game
    // raises a town's buildings this way when its zone loads. Only the players' own wait for the host.
    if (KenshiWorld::ClientActive() && !g_hostCall && KenshiWorld::IsPlayerBuilding(building)) return;
    o_addConstruction(building, amount);
}
using DismantleProgressFn = bool (*)(void* building, float amount);
DismantleProgressFn o_addDismantle = nullptr;
bool hk_addDismantle(void* building, float amount) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return false;
    return o_addDismantle(building, amount);
}
using WorldDestroyFn = bool (*)(void* world, void* obj, bool justUnloaded, const char* info);
WorldDestroyFn o_worldDestroy = nullptr;
bool hk_worldDestroy(void* world, void* obj, bool justUnloaded, const char* info) {
    if (!justUnloaded && obj)
        if (KenshiWorld* w = TheWorld()) w->NoteObjectDestroyed(obj);
    return o_worldDestroy(world, obj, justUnloaded, info);
}

// A client in a session never saves: its world is the host's, and a save (quick save, the save
// menu, an autosave) would write it over one of the player's own save slots. Refused.
using SaveFn = void (*)(void* sm, void* name, bool autosave);
SaveFn o_saveManagerSave = nullptr;
void hk_saveManagerSave(void* sm, void* name, bool autosave) {
    if (KenshiWorld::ClientActive()) {
        Log("client: the game's %s refused (the world is the host's: it must not overwrite a save of this player)", autosave ? "autosave" : "save");
        return;
    }
    o_saveManagerSave(sm, name, autosave);
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

std::string AnimHookStats() {
    char b[160];
    snprintf(b, sizeof(b), "calls=%llu targets=%llu matched=%llu silenced=%llu", (unsigned long long)g_animHookCalls.load(),
             (unsigned long long)g_animHookTargets.load(), (unsigned long long)g_animHookMatched.load(), (unsigned long long)g_animHookSilenced.load());
    return b;
}

bool InHostCall() { return g_hostCall > 0; }

HostCallScope::HostCallScope() { ++g_hostCall; }
HostCallScope::~HostCallScope() { --g_hostCall; }
AnimReplayScope::AnimReplayScope() { ++g_animReplay; ++g_hostCall; }
AnimReplayScope::~AnimReplayScope() { --g_animReplay; --g_hostCall; }

namespace {
bool SaySeh(void* d, const void* gs) {
    __try {
        o_say(d, gs, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReplyClickedSeh(void* d, int index) {
    __try {
        o_replyClicked(d, index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool ReplaySay(kenshi::Character* c, const std::string& text, bool shout) {
    void* d = kenshi::CharacterDialogue(c);
    if (!d || !o_say) return false;
    alignas(8) uint8_t gs[0x28];
    kenshi::GameStringView(text, gs);
    kenshi::SetDialogueShouting(d, shout);
    HostCallScope scope;
    return SaySeh(d, gs);
}

bool CallReplyClicked(void* dialogue, int index) {
    if (!kenshi::DialogueOwner(dialogue)) return false;
    if (!o_replyClicked) o_replyClicked = reinterpret_cast<ReplyClickedFn>(kenshi::FnAddr(kenshi::FnDialogueReplyClicked));
    HostCallScope scope;
    return ReplyClickedSeh(dialogue, index);
}

namespace {
// The game's own "order the selected characters" functions, with only that character selected: the
// order goes through every check and special case the game has (carrying, beds, shops, speed groups).
bool CallTaskSeh(void* pi, const kc::Command& cmd, void* subject, void* building, const float* loc, const void* hand) {
    __try {
        switch (cmd.via) {
        case kc::TaskVia::AddOrder: o_addOrder(pi, building, cmd.task, subject, cmd.shift, cmd.add, loc); break;
        case kc::TaskVia::NewTask: o_newTask(pi, cmd.task, hand, building, loc, cmd.add); break;
        case kc::TaskVia::TaskNearest: o_addTaskNearest(pi, building, cmd.task, subject, cmd.shift, loc, cmd.add); break;
        case kc::TaskVia::AddJob: o_addJob(pi, cmd.task, subject, cmd.shift, cmd.add, loc); break;
        case kc::TaskVia::SetOrder: o_setOrder(pi, cmd.task); break;
        default: return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

// Host: a client's order, given to that character alone; the host player's selection, squad bar and
// details panel are left exactly as they were.
bool RunPlayerTask(kenshi::Character* c, const kc::Command& cmd, void* subject, void* building) {
    void* pi = kenshi::Player();
    if (!pi || !kenshi::IsCharacter(c)) return false;
    if (cmd.via == kc::TaskVia::RemovePermajob || cmd.via == kc::TaskVia::MovePermajob || cmd.via == kc::TaskVia::RemoveJob) {
        // the Tâches panel: on that character itself. The slot the client saw, if it holds that job
        // here, else the first job of that kind.
        HostCallScope scope;
        if (cmd.via == kc::TaskVia::RemoveJob) return kenshi::RemoveJobKind(c, cmd.task);
        const int hint = int(cmd.pos.x);
        int slot = kenshi::PermajobType(c, hint) == cmd.task ? hint : -1;
        for (int i = 0, n = kenshi::PermajobCount(c); slot < 0 && i < n; ++i)
            if (kenshi::PermajobType(c, i) == cmd.task) slot = i;
        if (slot < 0) return false;
        if (cmd.via == kc::TaskVia::RemovePermajob) return kenshi::RemovePermajob(c, slot);
        return kenshi::MovePermajob(c, slot, std::min(int(cmd.pos.y), kenshi::PermajobCount(c) - 1));
    }
    if (cmd.via == kc::TaskVia::SetOrder) {
        // the squad bar's toggles, set on the character itself: the game's selection function
        // decides from the host's own toggle buttons, not from that character
        HostCallScope scope;
        const int order = cmd.task;
        const bool on = order > 10 ? !kenshi::GetStandingOrder(c, order) : true;
        kenshi::SetStandingOrder(c, order, on);
        return true;
    }
    const float loc[3] = {cmd.pos.x, cmd.pos.y, cmd.pos.z};
    alignas(8) uint8_t hand[kenshi::off::HandSize];
    kc::Handle target = cmd.subject;
    if (subject) kenshi::ObjectHandle(subject, target);   // found by kind and place: its handle here
    kenshi::MakeHand(target, hand);
    bool ok = false;
    kenshi::WithSelection(c, [&] {
        HostCallScope scope;
        ok = CallTaskSeh(pi, cmd, subject, building, loc, hand);
    });
    return ok;
}

bool CallPlayerMoveOrder(kenshi::Character* c, const kc::Vec3& pos) {
    if (!o_moveOrder || !kenshi::IsCharacter(c)) return false;
    const float p[3] = {pos.x, pos.y, pos.z};
    HostCallScope scope;
    return CallMoveOrderSEH(c, p);
}

// ---- lot A: doors (plugin/doors.cpp)
namespace doorhooks {
bool hk_open(void*);
bool hk_close(void*);
void hk_lock(void*);
void hk_unlock(void*);
void hk_openButton(void*, void*);
void hk_lockButton(void*, void*);
extern bool (*o_open)(void*);
extern bool (*o_close)(void*);
extern void (*o_lock)(void*);
extern void (*o_unlock)(void*);
extern void (*o_openButton)(void*, void*);
extern void (*o_lockButton)(void*, void*);
} // namespace doorhooks
// ---- end lot A

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
        {kenshi::FnReassessCollapse, reinterpret_cast<void*>(&hk_collapse), reinterpret_cast<void**>(&o_collapse)},
        {kenshi::FnPickupItem, reinterpret_cast<void*>(&hk_pickup), reinterpret_cast<void**>(&o_pickup)},
        {kenshi::FnIncreaseStat, reinterpret_cast<void*>(&hk_increaseStat), reinterpret_cast<void**>(&o_increaseStat)},
        {kenshi::FnSquadAddCharacterAt, reinterpret_cast<void*>(&hk_addAt), reinterpret_cast<void**>(&o_addAt)},
        {kenshi::FnCloseCharacterEditor, reinterpret_cast<void*>(&hk_closeEditor), reinterpret_cast<void**>(&o_closeEditor)},
        {kenshi::FnSetStandingOrder, reinterpret_cast<void*>(&hk_standing), reinterpret_cast<void**>(&o_standing)},
        {kenshi::FnPickupCharacter, reinterpret_cast<void*>(&hk_pickChar), reinterpret_cast<void**>(&o_pickChar)},
        {kenshi::FnShowTradeWindow, reinterpret_cast<void*>(&hk_showTrade), reinterpret_cast<void**>(&o_showTrade)},
        {kenshi::FnShowInventoryBuilding, reinterpret_cast<void*>(&hk_showInvBuilding), reinterpret_cast<void**>(&o_showInvBuilding)},
        // ---- fix G5
        {kenshi::FnCharRemovePermajob, reinterpret_cast<void*>(&hk_removePermajob), reinterpret_cast<void**>(&o_removePermajob)},
        {kenshi::FnCharMovePermajob, reinterpret_cast<void*>(&hk_movePermajob), reinterpret_cast<void**>(&o_movePermajob)},
        {kenshi::FnCharRemoveJob, reinterpret_cast<void*>(&hk_removeJob), reinterpret_cast<void**>(&o_removeJob)},
        // ---- lot D: prisons
        {kenshi::FnSetPrisonMode, reinterpret_cast<void*>(&hk_prisonMode), reinterpret_cast<void**>(&o_prisonMode)},
        {kenshi::FnSetChainedMode, reinterpret_cast<void*>(&hk_chainedMode), reinterpret_cast<void**>(&o_chainedMode)},
        {kenshi::FnSetSlaveState, reinterpret_cast<void*>(&hk_slaveState), reinterpret_cast<void**>(&o_slaveState)},
        {kenshi::FnDialogueSay, reinterpret_cast<void*>(&hk_say), reinterpret_cast<void**>(&o_say)},
        {kenshi::FnDialogueSetInDialog, reinterpret_cast<void*>(&hk_setInDialog), reinterpret_cast<void**>(&o_setInDialog)},
        {kenshi::FnDialogueSetResponses, reinterpret_cast<void*>(&hk_setResponses), reinterpret_cast<void**>(&o_setResponses)},
        {kenshi::FnDialogueSetReplyText, reinterpret_cast<void*>(&hk_setReplyText), reinterpret_cast<void**>(&o_setReplyText)},
        {kenshi::FnDialogueSendEvent, reinterpret_cast<void*>(&hk_sendEvent), reinterpret_cast<void**>(&o_sendEvent)},
        {kenshi::FnDialogueSendEventOverride, reinterpret_cast<void*>(&hk_sendEventOverride), reinterpret_cast<void**>(&o_sendEventOverride)},
        {kenshi::FnDialogueStartConversation, reinterpret_cast<void*>(&hk_startConv), reinterpret_cast<void**>(&o_startConv)},
        {kenshi::FnDialogueStartPlayerConversation, reinterpret_cast<void*>(&hk_startPlayerConv), reinterpret_cast<void**>(&o_startPlayerConv)},
        {kenshi::FnDialogueDoActions, reinterpret_cast<void*>(&hk_doActions), reinterpret_cast<void**>(&o_doActions)},
        {kenshi::FnSensoryDialogAssessment, reinterpret_cast<void*>(&hk_dialogAssessment), reinterpret_cast<void**>(&o_dialogAssessment)},
        {kenshi::FnSensoryAssessCrimes, reinterpret_cast<void*>(&hk_assessCrimes), reinterpret_cast<void**>(&o_assessCrimes)},
        {kenshi::FnBlackboardUpdate, reinterpret_cast<void*>(&hk_bbUpdate), reinterpret_cast<void**>(&o_bbUpdate)},
        {kenshi::FnBlackboardPeriodic, reinterpret_cast<void*>(&hk_bbPeriodic), reinterpret_cast<void**>(&o_bbPeriodic)},
        {kenshi::FnFactionWarPeriodic, reinterpret_cast<void*>(&hk_warPeriodic), reinterpret_cast<void**>(&o_warPeriodic)},
        {kenshi::FnUniqueSquadPeriodic, reinterpret_cast<void*>(&hk_uniquePeriodic), reinterpret_cast<void**>(&o_uniquePeriodic)},
        {kenshi::FnAffectRelationsAmount, reinterpret_cast<void*>(&hk_affectAmount), reinterpret_cast<void**>(&o_affectAmount)},
        {kenshi::FnAffectRelationsEvent, reinterpret_cast<void*>(&hk_affectEvent), reinterpret_cast<void**>(&o_affectEvent)},
        {kenshi::FnSetRelation, reinterpret_cast<void*>(&hk_setRelation), reinterpret_cast<void**>(&o_setRelation)},
        {kenshi::FnSetCrime, reinterpret_cast<void*>(&hk_setCrime), reinterpret_cast<void**>(&o_setCrime)},
        {kenshi::FnAssignBounty, reinterpret_cast<void*>(&hk_assignBounty), reinterpret_cast<void**>(&o_assignBounty)},
        {kenshi::FnGiveItem, reinterpret_cast<void*>(&hk_giveItem), reinterpret_cast<void**>(&o_giveItem)},
        {kenshi::FnDropItemHuman, reinterpret_cast<void*>(&hk_dropItem), reinterpret_cast<void**>(&o_dropItem)},
        {kenshi::FnCreateScreenLabel, reinterpret_cast<void*>(&hk_createLabel), reinterpret_cast<void**>(&o_createLabel)},
        {kenshi::FnLabelSetTracking, reinterpret_cast<void*>(&hk_labelTrack), reinterpret_cast<void**>(&o_labelTrack)},
        {kenshi::FnLabelSetColor, reinterpret_cast<void*>(&hk_labelColor), reinterpret_cast<void**>(&o_labelColor)},
        {kenshi::FnRagdollMode, reinterpret_cast<void*>(&hk_ragdollMode), reinterpret_cast<void**>(&o_ragdollMode)},
        {kenshi::FnCreateRandomCharacter, reinterpret_cast<void*>(&hk_createRandomCharacter), reinterpret_cast<void**>(&o_createChar)},
        {kenshi::FnRegionUpdateBT, reinterpret_cast<void*>(&hk_regionUpdateBT), reinterpret_cast<void**>(&o_regionUpdateBT)},
        {kenshi::FnSeasonGetNewWeather, reinterpret_cast<void*>(&hk_seasonGetNewWeather), reinterpret_cast<void**>(&o_seasonGetNewWeather)},
        {kenshi::FnEffectHandlerCtor, reinterpret_cast<void*>(&hk_effectCtor), reinterpret_cast<void**>(&o_effectCtor)},
        {kenshi::FnEffectAffectObjects, reinterpret_cast<void*>(&hk_effectAffect), reinterpret_cast<void**>(&o_effectAffect)},
        {kenshi::FnEffectStop, reinterpret_cast<void*>(&hk_effectStop), reinterpret_cast<void**>(&o_effectStop)},
        {kenshi::FnRegionUpdateEffects, reinterpret_cast<void*>(&hk_regionUpdateEffects), reinterpret_cast<void**>(&o_regionUpdateEffects)},
        {kenshi::FnAnimStartCombat, reinterpret_cast<void*>(&hk_animStartCombat), reinterpret_cast<void**>(&o_animStartCombat)},
        {kenshi::FnAnimRunCombat, reinterpret_cast<void*>(&hk_animRunCombat), reinterpret_cast<void**>(&o_animRunCombat)},
        {kenshi::FnAnimEndCombat, reinterpret_cast<void*>(&hk_animEndCombat), reinterpret_cast<void**>(&o_animEndCombat)},
        {kenshi::FnAnimPlayAction, reinterpret_cast<void*>(&hk_animPlayAction), reinterpret_cast<void**>(&o_animPlayAction)},
        {kenshi::FnAnimStopAction, reinterpret_cast<void*>(&hk_animStopAction), reinterpret_cast<void**>(&o_animStopAction)},
        {kenshi::FnAnimStopActionNamed, reinterpret_cast<void*>(&hk_animStopActionNamed), reinterpret_cast<void**>(&o_animStopActionNamed)},
        {kenshi::FnAnimStartStumble, reinterpret_cast<void*>(&hk_animStartStumble), reinterpret_cast<void**>(&o_animStartStumble)},
        {kenshi::FnAnimEndStumble, reinterpret_cast<void*>(&hk_animEndStumble), reinterpret_cast<void**>(&o_animEndStumble)},
        {kenshi::FnAnimSetCombatMode, reinterpret_cast<void*>(&hk_animSetCombatMode), reinterpret_cast<void**>(&o_animSetCombatMode)},
        {kenshi::FnAnimSetCarryMode, reinterpret_cast<void*>(&hk_animSetCarryMode), reinterpret_cast<void**>(&o_animSetCarryMode)},
        {kenshi::FnDrawWeapon, reinterpret_cast<void*>(&hk_drawWeapon), reinterpret_cast<void**>(&o_drawWeapon)},
        {kenshi::FnSheatheWeapon, reinterpret_cast<void*>(&hk_sheatheWeapon), reinterpret_cast<void**>(&o_sheatheWeapon)},
        {kenshi::FnAnimGuardLegs, reinterpret_cast<void*>(&hk_animGuardLegs), reinterpret_cast<void**>(&o_animGuardLegs)},
        {kenshi::FnAnimGuardUpper, reinterpret_cast<void*>(&hk_animGuardUpper), reinterpret_cast<void**>(&o_animGuardUpper)},
        {kenshi::FnAnimationSelection, reinterpret_cast<void*>(&hk_animSelect), reinterpret_cast<void**>(&o_animSelect)},
        {kenshi::FnTrackAnimationMovement, reinterpret_cast<void*>(&hk_trackAnimMove), reinterpret_cast<void**>(&o_trackAnimMove)},
        {kenshi::FnSingleAnimUpdate, reinterpret_cast<void*>(&hk_singleAnimUpdate), reinterpret_cast<void**>(&o_singleAnimUpdate)},
        {kenshi::FnCombatMovementUpdate, reinterpret_cast<void*>(&hk_combatMove), reinterpret_cast<void**>(&o_combatMove)},
        // ---- lot A: doors
        {kenshi::FnDoorOpen, reinterpret_cast<void*>(&doorhooks::hk_open), reinterpret_cast<void**>(&doorhooks::o_open)},
        {kenshi::FnDoorClose, reinterpret_cast<void*>(&doorhooks::hk_close), reinterpret_cast<void**>(&doorhooks::o_close)},
        {kenshi::FnDoorLock, reinterpret_cast<void*>(&doorhooks::hk_lock), reinterpret_cast<void**>(&doorhooks::o_lock)},
        {kenshi::FnDoorUnlock, reinterpret_cast<void*>(&doorhooks::hk_unlock), reinterpret_cast<void**>(&doorhooks::o_unlock)},
        {kenshi::FnDoorOpenButton, reinterpret_cast<void*>(&doorhooks::hk_openButton), reinterpret_cast<void**>(&doorhooks::o_openButton)},
        {kenshi::FnDoorLockButton, reinterpret_cast<void*>(&doorhooks::hk_lockButton), reinterpret_cast<void**>(&doorhooks::o_lockButton)},
        // ---- end lot A
        // ---- lot C: ranged
        {kenshi::FnGunShoot, reinterpret_cast<void*>(&ranged::hk_gunShoot), reinterpret_cast<void**>(&ranged::o_gunShoot)},
        {kenshi::FnProjectileGet, reinterpret_cast<void*>(&ranged::hk_projectileGet), reinterpret_cast<void**>(&ranged::o_projectileGet)},
        // ---- lot E: buildings
        {kenshi::FnCreateFromPreviews, reinterpret_cast<void*>(&hk_createFromPreviews), reinterpret_cast<void**>(&o_createFromPreviews)},
        {kenshi::FnCreateBuilding, reinterpret_cast<void*>(&hk_createBuilding), reinterpret_cast<void**>(&o_createBuilding)},
        {kenshi::FnBuyMeCallback, reinterpret_cast<void*>(&hk_buyMe), reinterpret_cast<void**>(&o_buyMe)},
        {kenshi::FnConfirmDismantle, reinterpret_cast<void*>(&hk_confirmDismantle), reinterpret_cast<void**>(&o_confirmDismantle)},
        {kenshi::FnAddConstructionProgress, reinterpret_cast<void*>(&hk_addConstruction), reinterpret_cast<void**>(&o_addConstruction)},
        {kenshi::FnAddDismantleProgress, reinterpret_cast<void*>(&hk_addDismantle), reinterpret_cast<void**>(&o_addDismantle)},
        {kenshi::FnWorldDestroy, reinterpret_cast<void*>(&hk_worldDestroy), reinterpret_cast<void**>(&o_worldDestroy)},
        {kenshi::FnSaveManagerSave, reinterpret_cast<void*>(&hk_saveManagerSave), reinterpret_cast<void**>(&o_saveManagerSave)},
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
