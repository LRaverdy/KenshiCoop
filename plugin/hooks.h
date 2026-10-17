// MinHook detours on Kenshi functions.
#pragma once
#include <functional>
#include <string>

#include "kc/call_scopes.h"
#include "kc/protocol.h"
#include "kenshi.h"

namespace kcp {

// Called on the game thread: `live` = right after GameWorld's main loop (the world may be touched);
// otherwise from the frame listener while no world is running (menus, loading screens).
using TickFn = void (*)(bool live);

bool InstallHooks(TickFn tick, std::string* err);
void RemoveHooks();
// Runs the tick with exception barriers; never lets an exception reach game code.
void RunTick(bool live);
double LastLiveTick();   // NowSeconds() of the last main-loop tick
// Logs a crash once per process (main.cpp): `exceptionPointers` is an EXCEPTION_POINTERS*.
void ReportCrash(void* exceptionPointers, const char* source);

bool InHostCall();   // a HostCallScope is alive on this thread (hooks in other files)

// Our own calls into the game pass through the order-blocking hooks while this is alive.
struct HostCallScope {
    HostCallScope();
    ~HostCallScope();
    HostCallScope(const HostCallScope&) = delete;
    HostCallScope& operator=(const HostCallScope&) = delete;
};
bool InHostCall();   // a HostCallScope is alive on this thread (lot A: doors, other files' hooks)
// An access violation caught by an __except skips the destructors of the scopes opened between the
// fault and the handler (kc/call_scopes.h): one left counted made a client run every later order
// itself. Mark before a guarded call into the game; repair after it returned or its exception was
// caught (logged when scopes were left open; returns how many).
kc::ScopeCounts MarkCallScopes();
int RepairCallScopes(const kc::ScopeCounts& mark, const char* where);
// The same around a block: declared before the block's HostCallScope, it repairs on the way out
// whatever an exception caught by a __try wrapper inside the block left open.
struct CallScopeGuard {
    explicit CallScopeGuard(const char* where) : mark(MarkCallScopes()), where(where) {}
    ~CallScopeGuard() { RepairCallScopes(mark, where); }
    CallScopeGuard(const CallScopeGuard&) = delete;
    CallScopeGuard& operator=(const CallScopeGuard&) = delete;
    kc::ScopeCounts mark;
    const char* where;
};
// Host (0.3.4): the game's character editor does not open while this is alive (another player's
// animal purchase: the shop's confirm opens it on the bought animals; on the host it paused the game
// and kept them out of the squad list until closed). See hk_showEditor.
struct NoEditorScope {
    NoEditorScope();
    ~NoEditorScope();
    NoEditorScope(const NoEditorScope&) = delete;
    NoEditorScope& operator=(const NoEditorScope&) = delete;
};
int EditorsSkipped();   // editors not opened because of a NoEditorScope (logs, tests)
// Inside it, the animation hooks let a client play one of the host's animations (every other
// animation call on the host's characters is refused on clients, ours included).
std::string AnimHookStats();   // tests

struct AnimReplayScope {
    AnimReplayScope();
    ~AnimReplayScope();
    AnimReplayScope(const AnimReplayScope&) = delete;
    AnimReplayScope& operator=(const AnimReplayScope&) = delete;
};

// Host: a client's player order (Command kind Task), given to `c` alone exactly as the game's UI
// gives it to a selection.
// Host: a client's order run by the game's "selected characters" function on exactly that character
// (the host's selection is set to it alone and put back within the call). Refused (false, with a
// French reason in `refused`) when the selection cannot be made exactly that character.
bool RunPlayerTask(kenshi::Character* c, const kc::Command& cmd, void* subject, void* building, std::string* refused = nullptr);
int ActorLeaks();   // host: host characters seen getting a task from a client's order (should stay 0)

// Client: `c` says this line (a speech bubble, as the host's game showed it).
bool ReplaySay(kenshi::Character* c, const std::string& text, bool shout);
// Host: the player answered in a conversation shown on their screen.
bool CallReplyClicked(void* dialogue, int index);

// Admin panel: the last ground point the local player ordered a move to (false: none since the
// world was loaded).
bool LastMoveOrderPoint(kc::Vec3& out);
void ForgetMoveOrderPoint();

// Character::playerMoveOrderDefault(nullptr, nullptr, pos) through the original function.
bool CallPlayerMoveOrder(kenshi::Character* c, const kc::Vec3& pos);

// 0.3.5 lot 5 (plugin/actions.cpp). Host: a job (kind, subject; invalid subject: any) was just removed from
// that character's job list: the order it was carrying out for it ends too (A_FAIRE 16/10 point 2).
void JobRemoved(void* chr, int type, const kc::Handle& subject, const char* how);
std::string ProgressHookStats();   // tests: what the progress bar hooks saw (actions.cpp)

} // namespace kcp
