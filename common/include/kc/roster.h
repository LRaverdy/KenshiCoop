// Which characters each player had (night-join, 0.3.5): the host's record of every character that
// entered a player's squads, kept next to the game in KenshiCoop-players.txt so that a player who comes
// back (a resync, a crash, the host restarting its session or loading a save) gets all of them again,
// not only the one character the file used to remember.
//
// One line per character, in the order they were recorded (newest last):
//   <steamId> <handle> <name>          the player's own character (the format of 0.3.4 and earlier;
//                                      older lines of a player stay as its history, never overwritten)
//   <steamId> squad <handle> <name>    another character of theirs (recruit, freed slave, given by the host)
//   <steamId> editor <handle> <name>   their own character, made for them, its character editor not
//                                      finished yet (the editor opens again when they come back)
// A 0.3.4 host reads the first kind only (the others do not parse as a handle and are skipped).
// Handles are the save's object ids: Kenshi gives a character a new one when it changes squad (or is
// captured), so the session keeps the file's handles up to date (Rehandle) while it runs.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "kc/protocol.h"

namespace kc {

enum class RosterKind : uint8_t { Own = 0, Member = 1, Editor = 2 };

struct RosterChar {
    Handle handle;
    std::string name;                     // the character's name when last recorded (a fallback to find it)
    RosterKind kind = RosterKind::Member;
};

struct RosterPlayer {
    uint64_t steamId = 0;                 // 0: a player without Steam (kept in memory only)
    std::string playerName;               // what they were called when last seen (memory only)
    std::vector<RosterChar> chars;        // newest last
    const RosterChar* LatestOwn() const;  // their own character as last recorded (Own or Editor)
};

class Roster {
public:
    static constexpr size_t kMaxPerPlayer = 64;   // characters kept per player
    static constexpr size_t kMaxOwnHistory = 4;   // own characters kept per player (the newest first)

    void Clear() { players_.clear(); dirty_ = false; }
    // The file's text (lines it cannot read are skipped). Keys are "steam:<id>", as Session::PlayerKey.
    void Parse(const std::string& text);
    std::string Serialize() const;   // players without a Steam id are not written

    // Record `h` as one of that player's characters (added, or its name and kind updated). A Member
    // never downgrades the player's Own/Editor record of the same character; Own after Editor ends the
    // editor. Another player's Member record of the same character is dropped (it changed hands); an
    // Own or Editor record of another player is kept. True: the roster changed.
    bool Note(const std::string& key, uint64_t steamId, const std::string& playerName, const Handle& h, const std::string& name, RosterKind kind);
    // Kenshi gave a recorded character a new handle: every record of it follows. True: changed.
    bool Rehandle(const Handle& from, const Handle& to, const std::string& name = {});
    // The host took that character back (or gave it to someone else): no longer that player's Member.
    // Own and Editor records stay. True: changed.
    bool ForgetMember(const std::string& key, const Handle& h);
    // The character's editor was finished: its Editor record becomes Own.
    bool EditorDone(const std::string& key, const Handle& h);

    const RosterPlayer* Find(const std::string& key) const;
    RosterPlayer* Find(const std::string& key);
    // The player whose Own or Editor record names `h` (empty: none); `kind` set when found.
    std::string ReservedFor(const Handle& h, RosterKind* kind = nullptr) const;
    // Any player's record of `h` (empty: none).
    std::string AnyRecordOf(const Handle& h) const;
    const std::map<std::string, RosterPlayer>& players() const { return players_; }

    bool dirty() const { return dirty_; }
    void Saved() { dirty_ = false; }

private:
    void Trim(RosterPlayer& p);
    std::map<std::string, RosterPlayer> players_;
    bool dirty_ = false;
};

std::string RosterHandleText(const Handle& h);                 // "type:container:containerSerial:index:serial"
bool ParseRosterHandle(const std::string& text, Handle& out);

} // namespace kc
