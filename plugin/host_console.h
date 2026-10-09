// A window of its own, outside the game (a second screen, or alt-tab): the session at a glance and
// the log as it is written, with a command line. Meant for the host: who is there, how well each
// player's game follows the host's, what each of them does.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kcp {

struct ConsolePlayer {
    uint8_t id = 0;
    std::string name;
    uint32_t pingMs = 0;
    size_t characters = 0;
    std::string state;   // en jeu, arrive, crée son perso...
    std::string sync;    // how well their game follows (from their reports)
    bool warn = false;   // the sync needs a look
};

struct ConsoleModel {
    std::string status;                 // one line: session, entities, speed, pause
    std::vector<ConsolePlayer> players;
};

void HostConsoleShow(bool show);        // the first show creates the window (its own thread)
bool HostConsoleVisible();
void HostConsolePublish(ConsoleModel model);          // game thread -> window
std::vector<std::string> HostConsoleTakeCommands();   // typed in the window, for the game thread
void HostConsoleShutdown();

} // namespace kcp
