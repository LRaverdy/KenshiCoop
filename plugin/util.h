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
};
Config LoadConfig(const std::wstring& iniPath);   // creates the file with defaults if missing
bool SaveConnection(const std::wstring& iniPath, const std::string& name, const std::string& address, uint16_t port);

std::wstring GameDir();                            // folder of kenshi_x64.exe, with trailing '\'
bool Sha256File(const std::wstring& path, std::string& hexOut);
uint64_t HashFile(const std::wstring& path);      // FNV-1a of file contents (0 if missing)
double NowSeconds();                               // monotonic

// What the game thread is doing (a string literal: "game frame", "tick: debug commands"...), for
// the crash report: tells a crash inside the game's own frame from one inside a KenshiCoop call.
void SetCrashPhase(const char* phase);
const char* CrashPhase();

} // namespace kcp
