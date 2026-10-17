// 0.3.5, lot 5 (branch night-actions): the game's side of action progress bars, disguise and jobs
// removed (common/src/session_actions.cpp is the session's; slave shops are in plugin/shops.cpp).
//
// Progress bars (A_FAIRE 16/10 point 5) [D]. Every timed task of the game is a ProgressBarTask
// (Task_PickLock, Task_CutLock, Task_BruteForceLock, Task_PickLock_Shackles, Task_CutShackles,
// Task_LockDoor, Task_AddMaterialsToBuilding...): its progress is a float at +0x80 (0..1), moved by
// <add progress> (0x336E60, += ; the bar of materials and shackles) or <set progress> (0x3375D0, = ;
// picking a lock: the lock's +0x18 / +0x1C), both given the character doing it. When it begins
// (vt 8, 0x331CA0) and the character is of the player faction, it makes a FloatingProgressBar
// (0x73FAB0 on the GUI 0x21337B0, kept at +0x78) that those two functions move above the character
// and fill; its end (vt 0x18, 0x331D50) hands it to the GUI to delete (0x6E9F10). On a client the
// task never runs (the host's game drives our characters): no bar. Host: both functions are hooked
// and report the progress of other players' characters (TakeActionProgress). Client: the same game
// bar is made, moved and filled above our copy (ShowActionProgress); it only shows when the game's
// "floating numbers" option is on (+8 visible and [0x2133AE0], as for the host's own bars).
//
// Disguise (point 13) [D]. AI vt 0x48 (0x5A51E0, run by AI::periodicUpdate 0x5112B0 for every
// character of the player faction but animals) works it out from the worn uniform: hat (inventory
// section 3) and body armour (5) give 0.4 each, legs (6) 0.2 if of the same faction, boots (9) none;
// the faction (Item +0x1E8) of those pieces goes to StateBroadcastData (Character +0x1A0) +0x108 and
// the sum to +0x100, cut to a tenth when pieces of two factions are mixed. The disguise holds
// (StateBroadcastData::getDisguise 0x7C0520, what NPCs and the stealth panel 0x88DF26 read) while
// +0x100 > 0.6 and the "disguise blown" timer +0x110 (seconds, saved as "disguiseblown") is not
// running. Clients run no AI: their copies kept no disguise (no indicator). Host: worked out again at
// once after a client's item move touching a character (AfterClientItemMove), read for the session
// (ReadDisguise). Client: written into our copy (ApplyDisguise).
//
// Jobs removed (point 2) [D]. The Tâches panel's cross (Character::removePermajob 0x5C9000 ->
// AITaskSystem 0x507300) takes the job out of the list (+0x98, count +0x90) and deletes it; the order
// the character is carrying out for it (AITaskSystem +0x80, count +0x78) is not touched: a "follow"
// goes on until another order, in solo too. Host: when the job removed is the one being done and
// none like it is left, the character drops it the game's own way (reThinkCurrentAIAction 0x5C8330,
// what DropLocalTasks calls), then takes its next job.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "hooks.h"
#include "util.h"
#include "world.h"

namespace kenshi {

namespace {
constexpr uintptr_t kRvaTheGui = 0x21337B0;                 // ForgottenGUI (an object, not a pointer)
constexpr uintptr_t kFbSetVisible = 0x10, kFbSetPosition = 0x18;   // FloatingProgressBar vtable: setVisible(bool), setPosition(const Vector3&)
constexpr uintptr_t CH_stateBroadcast = 0x1A0, CH_ai = 0x650;
constexpr uintptr_t SBD_disguiseLevel = 0x100, SBD_disguiseFaction = 0x108, SBD_disguiseBlown = 0x110;
constexpr uintptr_t AI_owner = 0x10, AI_tasks = 0x20;       // AI: +0x10 checked by periodicUpdate before vt 0x48; its AITaskSystem
constexpr uintptr_t TS_orderCount = 0x78, TS_orders = 0x80; // AITaskSystem: the orders being carried out
constexpr uintptr_t TK_subject = 0x10, TK_data = 0x70, TD_type = 0x44;   // Tasker: subject hand, its task data, the TaskType

bool Copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool Get(const void* p, uintptr_t off, T& out) {
    return p && Copy(&out, static_cast<const uint8_t*>(p) + off, sizeof(T));
}
template <class T>
bool Put(void* p, uintptr_t off, const T& v) {
    return p && Copy(static_cast<uint8_t*>(p) + off, &v, sizeof(T));
}
void* VtSlot(const void* obj, uintptr_t slot) {
    void* vt = nullptr;
    void* fn = nullptr;
    return Get(obj, 0, vt) && vt && Get(vt, slot, fn) ? fn : nullptr;
}

void* NewBarSeh(void* fn, void* gui) {
    __try { return reinterpret_cast<void* (*)(void*)>(fn)(gui); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool BarFloatSeh(void* fn, void* bar, float v) {
    __try { reinterpret_cast<void (*)(void*, float)>(fn)(bar, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BarPosSeh(void* fn, void* bar, const float* pos) {
    __try { reinterpret_cast<void (*)(void*, const float*)>(fn)(bar, pos); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BarBoolSeh(void* fn, void* bar, bool on) {
    __try { reinterpret_cast<void (*)(void*, bool)>(fn)(bar, on); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GuiPtrSeh(void* fn, void* gui, void* p) {
    __try { reinterpret_cast<void (*)(void*, void*)>(fn)(gui, p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AiFloatSeh(void* fn, void* ai, float dt) {
    __try { reinterpret_cast<void (*)(void*, float)>(fn)(ai, dt); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* StateBroadcast(Character* c) {
    void* sbd = nullptr;
    return IsCharacter(c) && Get(c, CH_stateBroadcast, sbd) ? sbd : nullptr;
}
} // namespace

void* NewFloatingBar() {
    void* gui = reinterpret_cast<void*>(Addr(kRvaTheGui));
    return NewBarSeh(FnAddr(FnFloatBarCreate), gui);
}

bool SetFloatingBar(void* bar, const kc::Vec3& pos, float progress) {
    if (!bar) return false;
    const float p[3] = {pos.x, pos.y, pos.z};
    void* setPos = VtSlot(bar, kFbSetPosition);
    void* setVis = VtSlot(bar, kFbSetVisible);
    return setPos && setVis && BarPosSeh(setPos, bar, p) && BarFloatSeh(FnAddr(FnFloatBarSetProgress), bar, std::clamp(progress, 0.0f, 1.0f)) &&
           BarBoolSeh(setVis, bar, true);
}

void DeleteFloatingBar(void* bar) {
    if (bar) GuiPtrSeh(FnAddr(FnFloatBarDestroy), reinterpret_cast<void*>(Addr(kRvaTheGui)), bar);
}

bool ReadDisguiseState(Character* c, float& level, void*& faction, float& blown) {
    level = blown = 0;
    faction = nullptr;
    void* sbd = StateBroadcast(c);
    if (!sbd || !Get(sbd, SBD_disguiseLevel, level) || !Get(sbd, SBD_disguiseFaction, faction) || !Get(sbd, SBD_disguiseBlown, blown)) return false;
    if (!std::isfinite(level)) level = 0;
    if (!std::isfinite(blown) || blown < 0) blown = 0;
    return true;
}

bool WriteDisguiseState(Character* c, float level, void* faction, float blown) {
    void* sbd = StateBroadcast(c);
    return sbd && Put(sbd, SBD_disguiseLevel, level) && Put(sbd, SBD_disguiseFaction, faction) && Put(sbd, SBD_disguiseBlown, blown);
}

bool RecomputeDisguise(Character* c) {
    void* ai = nullptr;
    void* owner = nullptr;
    if (!IsCharacter(c) || !Get(c, CH_ai, ai) || !ai || !Get(ai, AI_owner, owner) || !owner) return false;   // as periodicUpdate checks
    return AiFloatSeh(FnAddr(FnAiDisguiseUpdate), ai, 0.0f);
}

bool CurrentOrder(Character* c, int& type, kc::Handle& subject) {
    type = -1;
    subject = kc::Handle{};
    void* ai = nullptr;
    void* ts = nullptr;
    uint32_t n = 0;
    void** orders = nullptr;
    void* first = nullptr;
    void* data = nullptr;
    if (!IsCharacter(c) || !Get(c, CH_ai, ai) || !ai || !Get(ai, AI_tasks, ts) || !ts || !Get(ts, TS_orderCount, n) || n == 0 || n > 256 ||
        !Get(ts, TS_orders, orders) || !orders || !Get(orders, 0, first) || !first || !Get(first, TK_data, data) || !data || !Get(data, TD_type, type))
        return false;
    HandleFromHand(static_cast<uint8_t*>(first) + TK_subject, subject);
    return type >= 0;
}

} // namespace kenshi

namespace kcp {

namespace {
// ---- host: progress of the timed tasks of other players' characters, since the last TakeActionProgress
std::mutex g_progressMutex;
std::vector<kc::IWorld::ActionProgress> g_progress;
uint64_t g_progressSeen = 0, g_progressNotChar = 0, g_progressCalls = 0, g_progressOwn = 0;

void NoteProgress(void* task, void* who) {
    ++g_progressCalls;
    auto v = KenshiWorld::View();
    if (!v->active || v->client || !who) return;
    // In game 17/10: the second argument is the worker's CharBody (Character+0x648, vtable 0x16F8A68
    // under Task_PickLock), not the Character: found back among the other players' characters
    kenshi::Character* c = kenshi::IsCharacter(who) ? static_cast<kenshi::Character*>(who) : nullptr;
    if (!c)
        for (const void* f : v->squadForeign) {
            auto* fc = static_cast<kenshi::Character*>(const_cast<void*>(f));
            if (kenshi::BodyOf(fc) == who) { c = fc; break; }
        }
    if (!c) {
        if (g_progressNotChar++ < 3)
            Log("action progress: neither a character nor one of the other players' bodies (vtable rva %#llx, task vtable rva %#llx)",
                (unsigned long long)(kenshi::VtableRva(who)), (unsigned long long)(kenshi::VtableRva(task)));
        return;
    }
    if (!v->squadForeign.count(c)) { ++g_progressOwn; return; }   // the host's own: its game draws the bar here
    float p = 0;
    kc::Handle h;
    if (!kenshi::Get(task, 0x80, p) || !std::isfinite(p) || !kenshi::GetHandle(c, h)) return;
    std::lock_guard<std::mutex> lk(g_progressMutex);
    ++g_progressSeen;
    for (auto& e : g_progress)
        if (e.who == h) { e.progress = p; return; }
    if (g_progress.size() < 64) g_progress.push_back({h, p});
}

} // namespace

// tests ("progressstate"): what the two progress hooks saw on this machine
std::string ProgressHookStats() {
    return "calls=" + std::to_string(g_progressCalls) + " foreign=" + std::to_string(g_progressSeen) + " own=" + std::to_string(g_progressOwn) +
           " notchar=" + std::to_string(g_progressNotChar);
}

namespace {
// ---- client: the bars drawn above our copies
struct ShownBar { kc::Handle who; void* bar = nullptr; };
std::vector<ShownBar> g_bars;
uint32_t g_barsGeneration = 0;

std::string HKey(const kc::Handle& h) {
    char b[80];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}

// ---- host: disguise worked out again after a client's item move (logged when it changed)
kc::LogLimiter g_disguiseLog{6, 10.0};
} // namespace

namespace actionhooks {
using ProgressFn = void (*)(void* task, void* who, float v);
ProgressFn o_progressAdd = nullptr;
ProgressFn o_progressSet = nullptr;
void hk_progressAdd(void* task, void* who, float v) {
    o_progressAdd(task, who, v);
    NoteProgress(task, who);
}
void hk_progressSet(void* task, void* who, float v) {
    o_progressSet(task, who, v);
    NoteProgress(task, who);
}
} // namespace actionhooks

// Host: a job was just removed from `c`'s list (the panel's cross, a client's request through it, or
// every job of a kind): what it is doing for that job ends with it (point 2).
void JobRemoved(void* chr, int type, const kc::Handle& subject, const char* how) {
    auto v = KenshiWorld::View();
    kc::actions::JobRef removed;
    removed.type = type;
    removed.subject = subject;
    if (!v->active || v->client || !kenshi::IsCharacter(chr) || removed.type < 0) return;
    auto* c = static_cast<kenshi::Character*>(chr);
    // the player faction's characters only (the game itself removes NPCs' jobs, and a bought animal's or
    // slave's, before they are ours: never touched)
    if (!kenshi::InPlayerSquad(c)) return;
    kc::actions::JobRef current;
    std::string name;
    kenshi::CharacterName(c, name);
    if (!kenshi::CurrentOrder(c, current.type, current.subject)) {
        Log("jobs: %s's job %d removed (%s); no current order", name.c_str(), removed.type, how);
        return;
    }
    std::vector<kc::actions::JobRef> left;
    const int n = kenshi::PermajobCount(c);
    for (int i = 0; i < n; ++i) {
        kc::actions::JobRef j;
        j.type = kenshi::PermajobType(c, i);
        kc::Vec3 at;
        kenshi::PermajobTarget(c, i, j.subject, at);
        left.push_back(j);
    }
    if (!kc::actions::EndsCurrentTask(removed, left, current)) {
        Log("jobs: %s's job %d removed (%s); its current order %d goes on (another job, or one like it left)", name.c_str(), removed.type, how, current.type);
        return;
    }
    bool dropped = false;
    {
        HostCallScope scope;
        dropped = kenshi::DropLocalTasks(c);
    }
    Log("jobs: %s's job %d removed (%s) while it was doing it: its current order %s", name.c_str(), removed.type, how,
        dropped ? "ended too (the game's reThinkCurrentAIAction)" : "could NOT be ended (exception caught)");
}

// ---- KenshiWorld (IWorld): action progress
void KenshiWorld::TakeActionProgress(std::vector<ActionProgress>& out) {
    std::lock_guard<std::mutex> lk(g_progressMutex);
    out.swap(g_progress);
    g_progress.clear();
}

bool KenshiWorld::ShowActionProgress(const kc::Handle& who, float progress) {
    if (g_barsGeneration != generation_) {   // another world: the GUI that held them is gone
        g_bars.clear();
        g_barsGeneration = generation_;
    }
    auto it = std::find_if(g_bars.begin(), g_bars.end(), [&](const ShownBar& b) { return b.who == who; });
    if (progress < 0) {
        if (it != g_bars.end()) {
            kenshi::DeleteFloatingBar(it->bar);
            g_bars.erase(it);
        }
        return true;
    }
    kenshi::Character* c = Find(who);
    kc::Vec3 pos;
    if (!c || !kenshi::GetPosition(c, pos)) return true;   // not here now: kept, moved when it is back
    if (it == g_bars.end()) {
        if (g_bars.size() >= 16) return false;
        void* bar = kenshi::NewFloatingBar();
        if (!bar) return false;
        Log("action progress: a bar made above our copy of %s (the host's game runs the task)", HKey(who).c_str());
        g_bars.push_back({who, bar});
        it = g_bars.end() - 1;
    }
    return kenshi::SetFloatingBar(it->bar, pos, progress);
}

// ---- KenshiWorld (IWorld): disguise
bool KenshiWorld::ReadDisguise(const kc::Handle& h, kc::DisguiseInfo& out) {
    out = kc::DisguiseInfo{};
    kenshi::Character* c = Find(h);
    void* faction = nullptr;
    if (!c || !kenshi::ReadDisguiseState(c, out.level, faction, out.blown)) return false;
    if (faction) {
        out.factionSid = kenshi::FactionSidOf(faction);
        void* gd = nullptr;
        if (kenshi::Get(faction, kenshi::off::FAC_data, gd) && gd) kenshi::ReadStdString(static_cast<uint8_t*>(gd) + kenshi::off::GD_name, out.factionName);
        if (out.factionName.empty()) out.factionName = out.factionSid;
    }
    return true;
}

bool KenshiWorld::ApplyDisguise(const kc::Handle& h, const kc::DisguiseInfo& d) {
    kenshi::Character* c = Find(h);
    if (!c) return false;
    void* faction = d.factionSid.empty() ? nullptr : kenshi::FactionBySid(d.factionSid);
    // a faction unknown here: no disguise (a null faction with a level would read as one)
    return kenshi::WriteDisguiseState(c, faction ? d.level : 0.0f, faction, d.blown);
}

// Host: a client's item move (or swap) just ran on these inventories: a character among them works its
// disguise out again now, as the game does every AI periodic update (a uniform put on through a
// client's request is a disguise at once, not seconds later).
void KenshiWorld::AfterClientItemMove(const kc::Handle& a, const kc::Handle& b) {
    for (const kc::Handle* h : {&a, &b}) {
        if (h == &b && b == a) break;
        kenshi::Character* c = Find(*h);
        if (!c) continue;
        float before = 0, after = 0, blown = 0;
        void* fBefore = nullptr;
        void* fAfter = nullptr;
        kenshi::ReadDisguiseState(c, before, fBefore, blown);
        bool ran = false;
        {
            HostCallScope scope;
            ran = kenshi::RecomputeDisguise(c);
        }
        kenshi::ReadDisguiseState(c, after, fAfter, blown);
        if ((fBefore != fAfter || std::fabs(before - after) > 0.01f || !ran) && g_disguiseLog.Allow(NowSeconds()))
            Log("disguise of %s worked out again after a client item move: uniform %.2f -> %.2f, faction %s%s", HKey(*h).c_str(), double(before), double(after),
                fAfter ? kenshi::FactionSidOf(fAfter).c_str() : "none", ran ? "" : " (the game's update could NOT run)");
    }
}

} // namespace kcp
