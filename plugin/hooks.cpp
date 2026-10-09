#include "hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
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
// Nor do they decide a collapse (pain, crippled limbs): it would leave a character the host has
// standing lying on the ground, flickering between hurt and unconscious.
void hk_collapse(void* med, bool medic, bool agony) {
    if (KenshiWorld::ClientActive() && !g_hostCall) return;
    o_collapse(med, medic, agony);
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
                    const float t = kenshi::SyncedAnimTime(single, mine, want, match->looped);
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

HostCallScope::HostCallScope() { ++g_hostCall; }
HostCallScope::~HostCallScope() { --g_hostCall; }
AnimReplayScope::AnimReplayScope() { ++g_animReplay; ++g_hostCall; }
AnimReplayScope::~AnimReplayScope() { --g_animReplay; --g_hostCall; }

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
        {kenshi::FnReassessCollapse, reinterpret_cast<void*>(&hk_collapse), reinterpret_cast<void**>(&o_collapse)},
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
