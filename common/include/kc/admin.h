// Host administration (the "Administration" section of the Multijoueur window, the console's
// "admin" commands): god mode, teleports, experience, healing, money. Game-agnostic parts only:
// the command syntax, the skill names, the experience arithmetic of the game's increaseStat, and
// the god-mode registry kept by player identity. The plugin (plugin/admin.cpp) carries them out
// on the host's game thread.
#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "kc/protocol.h"

namespace kc::admin {

// ---- skills (kStatCount of them, in the plugin's kStatOffsets order)
struct SkillInfo {
    const char* key;   // command name (English, no spaces)
    const char* fr;    // shown to the host
};
extern const SkillInfo kSkills[kStatCount];
constexpr int kAllSkills = -2;
// "melee_attack", "1", "Attaque" (any case, accents included): its index; "all"/"tout"/"toutes":
// kAllSkills; anything else: -1.
int FindSkill(const std::string& s);

// ---- experience, as the game counts it
// The game's increaseStat(float& stat, float amount, float upperLimit) (kenshi_x64 0x8C5DF0) adds
// amount * ((upperLimit - stat) / upperLimit)^2, and refuses any amount above 20 (no change).
constexpr float kMaxXpPerCall = 20.0f;
constexpr float kStatCap = 100.0f;
float IncreaseStatModel(float stat, float amount, float upper = kStatCap);   // the same arithmetic, for tests

using IncreaseFn = std::function<void(float amount)>;   // one call of the game's increaseStat on the skill
using ReadFn = std::function<float()>;                  // the skill's current level

// `amount` experience points, cut into calls the game accepts. Returns the number of calls.
int GiveXp(double amount, const IncreaseFn& inc);
// Up to `target` (capped at kStatCap) through the game's own gains, each call sized to land on the
// target. Near 100 the gains shrink to nothing: after maxCalls the caller writes the rest itself.
// True: reached. calls: how many calls it took.
bool RaiseTo(float target, const ReadFn& read, const IncreaseFn& inc, int maxCalls, int* calls = nullptr);

// ---- commands ("admin <verb> ...", typed in the console or sent by the window's buttons)
enum class Verb { None, God, Xp, Tp, Heal, Money, List, State };
enum class TpTo { Host, Player, Point, Pos };

struct Target {
    bool all = false;    // every player (the host included)
    bool host = false;   // the host (resolved by the caller: its player id)
    int id = -1;         // a player id
};

struct Command {
    Verb verb = Verb::None;
    Target who;
    bool on = true;           // God
    int skill = kAllSkills;   // Xp
    double amount = 0;        // Xp: experience points or levels
    bool levels = false;      // Xp: amount is in levels
    TpTo tp = TpTo::Host;     // Tp
    Target to;                // Tp to a player
    Vec3 pos;                 // Tp to a position
    long long money = 0;      // Money
};

bool ParseTarget(const std::string& s, Target& out);
// The words after "admin". False: err says why (French, shown to the host).
bool Parse(const std::string& line, Command& out, std::string& err);
std::string Usage();   // French, one line per verb

// Limits (beyond them the command is refused): the window also asks for a confirmation sooner.
constexpr double kMaxXp = 20000;
constexpr double kMaxLevels = 100;
constexpr double kConfirmXp = 1000;      // the window asks twice above this
constexpr double kConfirmLevels = 10;

// ---- god mode by player identity: it stays on across zone changes (characters re-resolved every
// tick), reconnections and rejoins (same Steam account or name, whatever player id comes back).
std::string PlayerKey(uint64_t steamId, const std::string& name);   // "steam:<id>", else "name:<name>"
inline const char* kHostKey = "host";

class GodRegistry {
public:
    // on: that player is in god mode. Off while "everyone" is on: everyone else (current) stays on.
    void Set(const std::string& key, bool on, const std::vector<std::string>& currentKeys);
    void SetAll(bool on);                   // on: every character of the squad, newcomers included
    bool On(const std::string& key) const { return all_ || keys_.count(key) != 0; }
    bool all() const { return all_; }
    bool any() const { return all_ || !keys_.empty(); }
    void Clear() { all_ = false; keys_.clear(); }

private:
    bool all_ = false;
    std::set<std::string> keys_;
};

} // namespace kc::admin
