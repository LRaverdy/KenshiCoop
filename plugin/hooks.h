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

// Our own calls into the game pass through the order-blocking hooks while this is alive.
struct HostCallScope {
    HostCallScope();
    ~HostCallScope();
    HostCallScope(const HostCallScope&) = delete;
    HostCallScope& operator=(const HostCallScope&) = delete;
};
// Inside it, the animation hooks let a client play one of the host's animations (every other
// animation call on the host's characters is refused on clients, ours included).
std::string AnimHookStats();   // tests

struct AnimReplayScope {
    AnimReplayScope();
    ~AnimReplayScope();
    AnimReplayScope(const AnimReplayScope&) = delete;
    AnimReplayScope& operator=(const AnimReplayScope&) = delete;
};

// Character::playerMoveOrderDefault(nullptr, nullptr, pos) through the original function.
bool CallPlayerMoveOrder(kenshi::Character* c, const kc::Vec3& pos);

} // namespace kcp
