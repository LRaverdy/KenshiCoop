// Player statistics, kept by the host for every player of its world and saved next to the game
// (KenshiCoop_stats/<save>.txt) so they carry over from one session to the next.
//
// The host is the only one to count: it runs every character, it executes the clients' trades,
// loot moves, placements and research requests. What is counted, and how (docs/FONCTIONNALITES.md,
// "Statistiques des joueurs"):
//  - play time (seconds in the world, the host while it hosts, a client from the end of its load
//    to its departure), sessions, first and last session (Unix time);
//  - distance walked by the player's characters (sum of position steps, DistanceStep below: steps
//    longer than a character could run, teleports, resync jumps, are not counted; nor those of a
//    character lying down, carried or dead);
//  - knock-outs and deaths of the player's characters (the health flags going on);
//  - characters (the player's squad characters now), recruits (clients: NPCs recruited in their
//    conversations, animals bought);
//  - cats earned and spent in trade windows, items taken from chests, bodies and animals (clients:
//    the host executes these; the host's own trades and loot run in its game and are not seen);
//  - buildings placed, research queued (the host: its placements; its own research is not seen);
//  - best skill (its French name and level) of the player's characters, the highest ever seen.
// Pure logic, tested by tests/test_main.cpp (TestPlayerStats).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "kc/protocol.h"

namespace kc {

struct PlayerStats {
    std::string key;          // the same person across connections: "steam:<id>" or "name:<name>" (Session::PlayerKey)
    std::string name;         // last name seen
    bool host = false;        // the host's own line (its own trades, loot, research and recruits are not seen)
    double playSeconds = 0;
    uint32_t sessions = 0;
    int64_t firstSeen = 0;    // Unix time of the first session (0: never)
    int64_t lastSeen = 0;     // Unix time last seen in the world
    double distance = 0;      // world units (about 10 cm each)
    uint32_t knockouts = 0;   // their characters knocked out
    uint32_t deaths = 0;      // their characters dead
    uint32_t characters = 0;  // squad characters they have (last count)
    uint32_t recruits = 0;
    int64_t cashEarned = 0;   // cats from sales
    int64_t cashSpent = 0;    // cats paid for purchases
    uint32_t itemsLooted = 0; // items taken (quantity) from chests, bodies, animals
    uint32_t buildings = 0;   // placements built
    uint32_t research = 0;    // techs queued
    float bestSkill = 0;      // the highest skill level of their characters ever seen
    std::string bestSkillName;   // its French name
    // session state, not saved
    bool online = false;
    uint8_t playerId = 0;     // their number in this session (colour), 0: offline

    bool operator==(const PlayerStats& o) const;
};

// Every player of one world, by key.
class PlayerStatsBook {
public:
    // The line of that player (made when new); its name is updated.
    PlayerStats& Get(const std::string& key, const std::string& name);
    const PlayerStats* Find(const std::string& key) const;
    PlayerStats* FindMutable(const std::string& key);
    const std::map<std::string, PlayerStats>& all() const { return players_; }
    size_t size() const { return players_.size(); }
    void Clear() { players_.clear(); }
    // Adds another book's totals (a file read while counting already went on): counts add up, the
    // first session is the earliest, the last the latest, the best skill the highest.
    void Merge(const PlayerStatsBook& other);
    // Text file (UTF-8): "kc-player-stats 1", then a block per player ("player <key>", "<field> <value>"
    // lines, "end"). Unknown fields are skipped; a bad value leaves the field at 0.
    std::string Serialize() const;
    // Reads a file's text (replaces the book). False: not a stats file (the book is then empty).
    bool Parse(const std::string& text, std::string* err = nullptr);
    // The lines to show / send: online players first (by player number), then the others, most recent first.
    std::vector<PlayerStats> Sorted() const;

private:
    std::map<std::string, PlayerStats> players_;
};

// A character's walk: the distance to count for one step from `from` to `to` taking `realSeconds`
// of wall clock at game speed `speed` (0 when paused). A step longer than a running character can go
// (kMaxUnitsPerGameSecond, plus kStepSlack) is a teleport or a resync: not counted; nor jitter under
// kMinStep.
constexpr float kMaxUnitsPerGameSecond = 200.0f;
constexpr float kStepSlack = 10.0f;
constexpr float kMinStep = 0.5f;
float DistanceStep(const Vec3& from, const Vec3& to, double realSeconds, float speed);

// "1 h 05 min", "12 min", "45 s"
std::string FormatPlayTime(double seconds);
// "1,2 km" / "350 m" (a world unit is about 10 cm)
std::string FormatDistance(double units);
// "13/10/2026" (local date), "—" for 0
std::string FormatDate(int64_t unixTime);
// The text of the stats tab of the game's window (MyGUI colour codes "#RRGGBB", "##" for a "#"):
// a block per player, in their colour, online or not. Without accents (FoldAccents).
std::string PlayerStatsPageText(const std::vector<PlayerStats>& players, bool inSession);
// Kenshi's fonts have the codes 32-126 and 8127-8217 only: Latin-1 letters lose their accent.
std::string FoldAccents(const std::string& s);

// Writes `text` to `path` atomically: a temporary file next to it, flushed, then renamed over the
// old one (a crash mid-write leaves the old file). The folder is made if needed.
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* err = nullptr);
bool ReadTextFile(const std::filesystem::path& path, std::string& out);
// A save name made safe as a file name (letters, digits, - _ . and space; others become '_').
std::string SafeFileStem(const std::string& name);

// ---- network: PlayerStats (S->C, reliable), every player's statistics
constexpr uint32_t kMaxStatsPlayers = 64;
struct PlayerStatsMsg {
    std::vector<PlayerStats> players;
};
void Encode(Writer& w, const PlayerStatsMsg& m);
bool Decode(Reader& r, PlayerStatsMsg& m);

}  // namespace kc
