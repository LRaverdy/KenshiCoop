// MinHook detours on Kenshi functions.
#pragma once
#include <functional>
#include <string>

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

bool InHostCall();   // a HostCallScope is alive on this thread (hooks in other files)

// Our own calls into the game pass through the order-blocking hooks while this is alive.
struct HostCallScope {
    HostCallScope();
    ~HostCallScope();
    HostCallScope(const HostCallScope&) = delete;
    HostCallScope& operator=(const HostCallScope&) = delete;
};
bool InHostCall();   // a HostCallScope is alive on this thread (lot A: doors, other files' hooks)
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
bool RunPlayerTask(kenshi::Character* c, const kc::Command& cmd, void* subject, void* building);

// Client: `c` says this line (a speech bubble, as the host's game showed it).
bool ReplaySay(kenshi::Character* c, const std::string& text, bool shout);
// Host: the player answered in a conversation shown on their screen.
bool CallReplyClicked(void* dialogue, int index);

// Character::playerMoveOrderDefault(nullptr, nullptr, pos) through the original function.
bool CallPlayerMoveOrder(kenshi::Character* c, const kc::Vec3& pos);

} // namespace kcp
