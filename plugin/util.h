// Logging, configuration and small helpers for the plugin.
#pragma once
#include <cstdint>
#include <string>

namespace kcp {

// Log lines go to KenshiCoop.log next to kenshi_x64.exe. Thread-safe; flushed on every line so
// the log survives a game crash.
void LogOpen(const std::wstring& path);
void LogClose();
void Log(const char* fmt, ...);

struct Config {
    std::string name = "Player";
    std::string joinAddress = "127.0.0.1";
    uint16_t port = 27960;
    float snapDistance = 300.0f;    // client: teleport a puppet when further than this from host state
    float destEpsilon = 5.0f;       // client: re-issue a destination when it moved more than this
    bool overlay = true;
};
Config LoadConfig(const std::wstring& iniPath);   // creates the file with defaults if missing

std::wstring GameDir();                            // folder of kenshi_x64.exe, with trailing '\'
bool Sha256File(const std::wstring& path, std::string& hexOut);
uint64_t HashFile(const std::wstring& path);      // FNV-1a of file contents (0 if missing)
double NowSeconds();                               // monotonic

} // namespace kcp
