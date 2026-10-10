// Host administration, carried out on the host's game thread: god mode (kept by player identity and
// re-applied every tick), teleports through the safe admin TP (KenshiWorld::TeleportCharacters),
// experience through the game's own increaseStat, healing, money. Each action is logged in English
// and the affected player gets a French notice. Clients have none of it: every entry point refuses
// unless this game hosts the session.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kc/admin.h"
#include "kc/session.h"
#include "world.h"

namespace kcp {

struct AdminPlayer {
    uint8_t id = 0;
    std::string name;
    uint32_t pingMs = 0;
    size_t characters = 0;
    size_t down = 0;      // knocked out, lying or dead: a teleport of them asks for a confirmation
    bool god = false;
    bool host = false;
};

// The words after "admin" (see kc::admin::Usage). Returns the French answer for the host (one or
// more lines); ok: the action was carried out.
std::string AdminRun(const std::string& line, kc::Session& s, KenshiWorld& w, bool* ok = nullptr);
// Every tick: god mode on exactly the characters of the players who have it (zone changes and
// rejoins re-resolve them); when hosting ends, everything is switched off and forgotten.
void AdminUpkeep(kc::Session& s, KenshiWorld& w);
// Host: everyone in the session (the host first), for the window.
std::vector<AdminPlayer> AdminPlayers(kc::Session& s, KenshiWorld& w, const std::string& hostName);
bool AdminGodAll();

} // namespace kcp
