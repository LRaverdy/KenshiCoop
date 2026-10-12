#pragma once
// Talking to NPCs in co-op (0.3.4). Pure rules (no game calls), shared by the plugin (the client's
// right click on a character, the host's conversations) and unit tested in tests/test_main.cpp.
//
// 1. A client's right click on an NPC. The game decides what the click does
//    (PlayerInterface::<character under the cursor>, 0x7FB410): talk only when its own check says the
//    NPC has a "talk to me" line for us (Character::<can be talked to>, 0x67AC00), which depends on
//    the client's copy of the NPC (its dialogue, its state here). On 12 October two players could not
//    talk to a merchant for minutes, until a resync: the host received no talk order at all. When a
//    right click on an NPC gives no order at all, the client says why in its log and, for a standing
//    NPC, sends the talk order anyway: the host's world decides (its game, TaskTargetAllowed,
//    TalkTargetBusy, UpdatePendingTalks).
// 2. A conversation the host's game holds with another player's character (shown on that player's
//    screen, never in the host's window). Dialogue::update (0x685090) counts down the line timers
//    (+0x230, +0x234) while a speech label exists and the game is not paused, and when they ran out
//    it ends the conversation or moves it on by itself. In single player the window pauses the game,
//    so this never happens while a line waits for the player's answer. On the host the game is not
//    paused: a guard's contraband check (its first line said aloud) closed 0.15 s and 1 s after it
//    opened on rob1's and nass4's screens (12 October). The host keeps the line timer up while the
//    player has answers to pick, as the pause would.
#include <cstddef>

namespace kc::talk {

// ---------------------------------------------------------------- 1. a click that gave no order
struct ClickFacts {
    bool rightClick = false;     // the click itself, not the cursor passing over the character
    bool otherMode = false;      // the game's PlayerInterface+0x268: the click gives no order at all
    bool character = false;      // what was clicked is a character
    bool animal = false;
    bool playerSide = false;     // ours, another player's, or in the player faction here
    bool deadOrDown = false;     // dead, knocked out, lying (loot and carry are other orders)
    int inSomething = 0;         // here: 1 in a bed, 2 in a cage (the game gives bed or cage orders)
    bool ownSelected = false;    // one of our characters is selected (it would talk)
};

enum class ClickVerdict {
    Ignore,     // nothing to say: not a click on an NPC
    LogOnly,    // logged (why no talk order), nothing sent
    SendTalk,   // logged, and the talk order goes to the host
};

struct ClickDecision {
    ClickVerdict verdict = ClickVerdict::Ignore;
    const char* why = "";
};

inline ClickDecision DecideClick(const ClickFacts& f) {
    if (!f.rightClick || !f.character) return {ClickVerdict::Ignore, "not a right click on a character"};
    if (f.playerSide) return {ClickVerdict::Ignore, "a player's character"};
    if (f.otherMode) return {ClickVerdict::LogOnly, "the game's interface is in another mode (its click gives no order)"};
    if (f.animal) return {ClickVerdict::LogOnly, "an animal (nothing to say)"};
    if (f.deadOrDown) return {ClickVerdict::LogOnly, "dead, knocked out or lying here"};
    if (!f.ownSelected) return {ClickVerdict::LogOnly, "none of our characters is selected"};
    if (f.inSomething == 1) return {ClickVerdict::LogOnly, "in a bed here"};
    if (f.inSomething == 2) return {ClickVerdict::LogOnly, "in a cage here"};
    return {ClickVerdict::SendTalk, "a standing NPC: the host decides"};
}

// The same NPC clicked again within this time: not sent twice (the cursor function may run twice in
// the frame of a click).
constexpr double kResendAfter = 1.0;

// ---------------------------------------------------------------- 1b. the host finds the NPC
// The client names the NPC by its handle (the host's, when its copy is linked) and by its kind and
// where it saw it: a copy not linked to the host's NPC (a local character or a stand-in without its
// alias) has a handle the host does not know, or one naming another character.
constexpr float kTalkFindRadius = 300.0f;   // the client's copy may be off the host's NPC (48 units on 12 October)
inline bool SameNpc(bool sameKind, float distance) { return sameKind && distance <= kTalkFindRadius; }

// ---------------------------------------------------------------- 2. the host keeps the line
constexpr float kHoldSeconds = 3.0f;    // the line timer is kept at least this high (a long frame at x3 never empties it)
constexpr double kMaxHold = 300.0;      // after five minutes without an answer the game's timers run again
// Kept while that player's window shows answers to pick: a line without answers (an NPC saying two
// lines in a row) moves on with its timer, as the game would (its window ends a conversation that
// offers nothing).
inline bool HoldLine(bool shownToPlayer, size_t replies, double waitingSeconds) {
    return shownToPlayer && replies > 0 && waitingSeconds >= 0 && waitingSeconds < kMaxHold;
}

} // namespace kc::talk
