// MinHook detours on Kenshi functions.
#pragma once
#include <functional>
#include <string>

#include "kc/protocol.h"
#include "kenshi.h"

namespace kcp {

// Called once per game frame, on the game thread, right after GameWorld's main loop.
using TickFn = void (*)();

bool InstallHooks(TickFn tick, std::string* err);
void RemoveHooks();

// Our own calls into the game pass through the order-blocking hooks while this is alive.
struct HostCallScope {
    HostCallScope();
    ~HostCallScope();
    HostCallScope(const HostCallScope&) = delete;
    HostCallScope& operator=(const HostCallScope&) = delete;
};

// Character::playerMoveOrderDefault(nullptr, nullptr, pos) through the original function.
bool CallPlayerMoveOrder(kenshi::Character* c, const kc::Vec3& pos);

} // namespace kcp
