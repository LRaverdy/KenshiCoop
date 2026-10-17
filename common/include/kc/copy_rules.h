// Rules for what the local game may still do to a copy the host drives (0.3.5, branch night-posture,
// A_FAIRE 16/10 points 7, 8 and 12). Pure functions, tested in tests/test_main.cpp; the hooks and
// KenshiWorld (plugin/) apply them.
#pragma once

#include <cstdint>

namespace kc::copy {

// ---- beds (point 12)
// Client: the local game asks to put a character into a bed (Character::setInBed(true)). On a copy
// the host drives it is refused: the local tasks that do it (Task_UseBed, Task_OperateMachine, from
// the save) fought the host's state every 2 s (~2650 lines in the 16/10 session: "put in its bed as
// on the host", "still in its bed here while the host has it free: taken out", "out of its bed
// first"). The mod's own calls (the host's bed, under HostCallScope) and the save's loading (a
// sleeper loaded in its bed; the host's state follows within 5 s anyway) go through.
inline bool RefuseLocalBed(bool client, bool modCall, bool on, bool hostDriven, bool fromLoad) {
    return client && !modCall && on && hostDriven && !fromLoad;
}

// ---- stealth (point 8)
// Character+0xE0 (getStealthMode / setStealthMode, vt 0x368 / 0x370): 0 not sneaking, 1 sneaking; 2,
// 3, 4 are the game's other states (down, can't sneak, can't act). The sneaking walk and the stealth
// icon follow it. On the host the AI's actions set 1; on a client the AI does not run, and the copy's
// own Character::update (every frame) kept it at 0 while the host's sneaked. The state our copy may
// take: 1 when the host's sneaks (instead of 0), never 1 when it does not; the other states pass.
inline int CopyStealthMode(int requested, bool hostSneaks) {
    if (hostSneaks) return requested == 0 ? 1 : requested;
    return requested == 1 ? 0 : requested;
}

// ---- a gone player's character (point 7)
// HaltCharacter: the game's stop (halt, orders cleared, current task ended: a training on a dummy and
// its progress bar, a machine, a chair), then off every machine that still lists it. A bed is left
// only awake (a sleeper stays where it sleeps); a cage never (its captivity is the host's).
inline bool GoneLeavesBed(int inSomething, bool unconscious) { return inSomething == 1 && !unconscious; }

// ---- leftover tasks of a copy (point 7, the rejoiner's side)
// Client: a copy keeps the tasks and orders it had in the save (a training, a sleep...). They are
// dropped the game's way (reThinkCurrentAIAction, every 2 s). That call does nothing while the current
// task holds the body (a training on a dummy kept its progress bar over a rejoiner's character after
// a resync, 16/10 21:51); after kRethinks such tries the orders are cleared like the game's stop button
// does, at most every kClearGap seconds, never on a body being carried or carrying one (the carry is
// the host's, replayed by ApplyCarry); kMaxClears times per body at most (a copy whose tasks come
// back every time is not cleared every 10 s for ever).
constexpr int kRethinks = 2;
constexpr double kClearGap = 10.0;
constexpr int kMaxClears = 3;
inline bool ShouldClearOrders(int rethinks, double sinceCleared, bool carrying, int clears = 0) {
    return !carrying && rethinks >= kRethinks && sinceCleared >= kClearGap && clears < kMaxClears;
}

} // namespace kc::copy
