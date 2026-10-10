// Logging, configuration and small helpers for the plugin.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kcp {

// Log lines go to KenshiCoop.log next to kenshi_x64.exe. Thread-safe; flushed on every line so
// the log survives a game crash.
void LogOpen(const std::wstring& path);
void LogClose();
void Log(const char* fmt, ...);
// The newest log lines (oldest first, at most `max`), for the in-game console. `seq` counts every
// line ever logged, so a reader can tell whether anything new arrived.
std::vector<std::string> RecentLog(size_t max, uint64_t* seq = nullptr);

struct Config {
    std::string name = "Player";
    std::string joinAddress = "127.0.0.1";
    uint16_t port = 27960;
    float snapDistance = 15.0f;     // client: teleport a character when further than this from host state
    float destEpsilon = 2.0f;       // client: re-issue a destination when it moved more than this
    float interestRadius = 0.0f;    // host: NPCs this close to the squad are replicated (0 = every active one)
    bool overlay = true;
    bool debugCommands = false;     // [debug] commands=1: enable the scripted test channel (debug.h)
    bool hostConsole = true;        // [ui] host_console=1: the console window opens by itself when hosting
    bool steamLoopback = false;     // [debug] steam_loopback=1: "Steam" links over local UDP (tests, two instances on one PC)
    bool characterPerPlayer = true; // host: [coop] own_character=1 gives every joining player a character
    // [ui] map layer (each player's own choice, changed in the Multijoueur window's "Affichage")
    bool mapMarkers = true;         // map_markers: players and enemies on the game's map screen
    bool headMarkers = true;        // head_markers: a marker above each player's character
    bool portraitColours = true;    // portrait_colours: player colour frames in the squad bar
    bool minimap = true;            // minimap (also Ctrl+Shift+N)
    bool minimapRotate = false;     // minimap_rotate: turns with the camera (else north up)
    int minimapCorner = 1;          // minimap_corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right
    float minimapZoom = 1500.0f;    // minimap_zoom: world units from the centre to the rim
    bool pings = true;              // pings: place and show pings
};
Config LoadConfig(const std::wstring& iniPath);   // creates the file with defaults if missing
bool SaveConnection(const std::wstring& iniPath, const std::string& name, const std::string& address, uint16_t port);
// One [ui] map layer setting (key as in KenshiCoop.ini): applied to `cfg` and written to the file.
// False: no such key.
bool SetUiOption(Config& cfg, const std::wstring& iniPath, const std::string& key, float value);

std::wstring GameDir();                            // folder of kenshi_x64.exe, with trailing '\'
bool Sha256File(const std::wstring& path, std::string& hexOut);
uint64_t HashFile(const std::wstring& path);      // FNV-1a of file contents (0 if missing)
double NowSeconds();                               // monotonic

} // namespace kcp
