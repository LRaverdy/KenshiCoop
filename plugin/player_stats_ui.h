// Player statistics in the game's own window (0.3.5): a "JOUEURS" tab added to the management
// window (the one with the MAP, SQUADS, DIALOGUE... tabs: ManagementScreen, Kenshi_OverviewWindow.layout),
// made with the game's MyGUI (MyGUIEngine_x64.dll exports) and its skins, filled with every player's
// statistics as the session has them (kc/player_stats.h).
#pragma once
#include <string>

#include "kc/session.h"
#include "world.h"

namespace kcp {

// Game thread, every live tick (a look every 0.5 s): makes the tab once the window exists (again if
// the game rebuilt it), refreshes its text when it is shown and the statistics changed. Also saves
// the statistics when the host's world was saved under a new name.
void PlayerStatsUiUpkeep(kc::Session& session, KenshiWorld& world, bool live);
// Tests ("pstatsui [state|select|text]"): the tab's state; select it in the window; its text as set.
std::string PlayerStatsUi(const std::string& what);

} // namespace kcp
