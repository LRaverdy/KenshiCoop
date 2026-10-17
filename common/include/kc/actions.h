// 0.3.5, lot 5 (branch night-actions): what a client player's characters do in the host's game that
// the player must see, or answer, on their own screen. No protocol change: message types 135-139 are
// new, and a peer that does not know a type ignores it (as PlayerStats).
//
//  - slave shops: a slaver's "I'd like to buy slaves" / "sell slaves" (dialogue actions 0x2A / 0x29)
//    makes the game's slave window, which the host's game would open on the host's screen. The offer
//    goes to the player whose character talked (SlaveShop), their choice comes back (SlaveTrade) and
//    the host's game buys or sells (the animal shops' design, protocol 36);
//  - action progress: the bar the host's game draws above a character doing a timed task (picking,
//    cutting or forcing a lock, picking shackles...: ProgressBarTask) goes to that character's player
//    (ActionProgress), whose game draws the same bar above its copy;
//  - disguise: the faction a character passes for, worked out by the host's game from the uniform it
//    wears (StateBroadcastData +0x100 / +0x108 / +0x110), goes to that character's player (Disguise),
//    whose copy takes it (the game's own "In Disguise" indicator then shows it);
//  - inventories a client gave up rebuilding (three failed attempts, e.g. while a far zone loaded
//    after an admin teleport): the client asks for them again (InvResend). Without them, nothing it
//    sells or moves is seen, so nothing reaches the host (A_FAIRE 16/10 point 15).
// Pure logic and the messages: tested by tests/test_main.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kc/protocol.h"
#include "kc/wire.h"

namespace kc {

// ---- slave shops (SlaveShop S->C, SlaveTrade C->S)
enum SlaveShopKind : uint8_t {
    kSlaveBuy = 0,    // the slaver's slaves (dialogue action 0x2A, "Je voudrais acheter des esclaves")
    kSlaveSell = 1,   // the player's own characters, sold as slaves (0x29, "Je voudrais vendre des esclaves")
};
constexpr uint32_t kMaxShopSlaves = 32;
struct SlaveShopMsg {
    uint32_t offerId = 0;
    uint8_t kind = kSlaveBuy;
    uint32_t sellerNetId = 0;          // the slaver (the dialogue's NPC)
    uint32_t buyerNetId = 0;           // the player's character in the conversation (it pays or is paid)
    int32_t basePrice = 0;             // the dialogue's price (0: the game's own for each)
    std::vector<uint32_t> slaves;      // the characters offered, in the window's order
    std::vector<int32_t> prices;       // the host's price of each (same order)
    std::string note;                  // nothing to offer: why (shown to the player)
};
struct SlaveTradeMsg {
    uint32_t offerId = 0;
    uint8_t kind = kSlaveBuy;
    uint32_t buyerNetId = 0;           // the player's character (OwnCharacter rule)
    std::vector<uint32_t> slaves;      // ticked in the window (none: closed without a deal)
    int32_t shownPrice = 0;            // the total the player's window showed (the host's prices are paid)
};
void Encode(Writer& w, const SlaveShopMsg& m);
bool Decode(Reader& r, SlaveShopMsg& m);
void Encode(Writer& w, const SlaveTradeMsg& m);
bool Decode(Reader& r, SlaveTradeMsg& m);

// ---- action progress (ActionProgress S->C)
constexpr uint16_t kProgressEnded = 0xFFFF;   // the task is over: the bar goes
constexpr uint32_t kMaxProgressEntries = 64;
struct ActionProgressEntry {
    uint32_t netId = 0;
    uint16_t permille = 0;             // 0..1000, or kProgressEnded
};
struct ActionProgressMsg {
    std::vector<ActionProgressEntry> entries;
};
void Encode(Writer& w, const ActionProgressMsg& m);
bool Decode(Reader& r, ActionProgressMsg& m);

// ---- disguise (Disguise S->C)
struct DisguiseInfo {
    std::string factionSid;            // the faction it passes for ("" none)
    std::string factionName;           // its name, for the player's indicator
    float level = 0;                   // how complete the uniform is (the game: > 0.6 disguised)
    float blown = 0;                   // seconds left of "disguise blown" (0: not blown)
    bool operator==(const DisguiseInfo& o) const = default;
};
struct DisguiseEntry {
    uint32_t netId = 0;
    DisguiseInfo d;
};
constexpr uint32_t kMaxDisguiseEntries = 64;
struct DisguiseMsg {
    std::vector<DisguiseEntry> entries;
};
void Encode(Writer& w, const DisguiseMsg& m);
bool Decode(Reader& r, DisguiseMsg& m);

// ---- inventories asked again (InvResend C->S)
constexpr uint32_t kMaxInvResend = 8;
struct InvResendMsg {
    std::vector<uint32_t> netIds;      // the asking player's own characters or their worn backpacks
};
void Encode(Writer& w, const InvResendMsg& m);
bool Decode(Reader& r, InvResendMsg& m);

namespace actions {

// The game's own rule (AI vt 0x48, 0x5A51E0, then StateBroadcastData::getDisguise 0x7C0520): the
// character passes for that faction when the uniform is complete enough and not seen through.
constexpr float kDisguiseThreshold = 0.6f;
inline bool Disguised(const DisguiseInfo& d) { return !d.factionSid.empty() && d.level > kDisguiseThreshold && d.blown <= 0.0f; }
// What changed enough to tell the player (the blown timer counts down every frame: only on/off).
bool DisguiseChanged(const DisguiseInfo& before, const DisguiseInfo& now);

// Host: one character's timed task, as its player was last told.
struct ProgressTrack {
    float value = 0;                   // as last seen in the host's game
    double seenAt = -1e9;              // when the game last moved it
    float sentValue = -1;              // as last sent (-1: never, or ended)
    double sentAt = -1e9;
};
constexpr double kProgressSendInterval = 0.25;   // at most 4 times a second per character
constexpr double kProgressStale = 1.0;           // not moved for that long: the task is over
// The value to send now, if any: -1 nothing, kEnded (as float) the bar goes, else 0..1.
constexpr float kSendEnded = -2.0f;
float ProgressToSend(ProgressTrack& t, double now);
inline uint16_t ToPermille(float v) { return uint16_t(v <= 0 ? 0 : v >= 1 ? 1000 : int(v * 1000.0f + 0.5f)); }

// Jobs (A_FAIRE 16/10 point 2). The panel's cross (Character::removePermajob 0x5C9000 ->
// AITaskSystem 0x507300) only takes the job out of the list and deletes it: what the character is
// doing for that job (its current order, AITaskSystem +0x80) goes on, and a "follow" never ends by
// itself. It ends with its job when the job removed is the one being done (same kind, same subject)
// and no job left in the list is that same one.
struct JobRef {
    int type = -1;
    Handle subject;
};
bool EndsCurrentTask(const JobRef& removed, const std::vector<JobRef>& remaining, const JobRef& current);

} // namespace actions
} // namespace kc
