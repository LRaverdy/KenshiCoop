#include "util.h"

#include <windows.h>
#include <bcrypt.h>

#include <share.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "kc/admin.h"
#include "kc/bug_report.h"
#include "kc/protocol.h"

namespace kcp {

namespace {
std::mutex g_logMutex;
FILE* g_log = nullptr;
std::deque<std::string> g_recent;   // newest last
uint64_t g_logSeq = 0;
constexpr size_t kRecentLines = 500;
std::vector<std::pair<std::string, uint64_t>> g_watches;   // tests: lines containing that text, counted from LogWatch on
kc::bug::LogRing g_tail;   // bug reports (0.3.5): the last 10 minutes, as written to the file

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
} // namespace

namespace {
// The previous session's log is kept: moved into the KenshiCoop-logs folder under the time it was
// last written (the 30 most recent are kept).
void ArchivePreviousLog(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
    FILETIME local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &st);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"KenshiCoop-logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\KenshiCoop-%04d-%02d-%02d_%02d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (!MoveFileExW(path.c_str(), (dir + name).c_str(), MOVEFILE_REPLACE_EXISTING)) return;   // in use: another instance
    // prune: the oldest beyond 30 go (names sort by date)
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\KenshiCoop-*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do files.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + 30 < files.size(); ++i) DeleteFileW((dir + L"\\" + files[i]).c_str());
}
} // namespace

void LogOpen(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_log) return;
    ArchivePreviousLog(path);
    // A second Kenshi instance (local testing) gets its own log instead of interleaving lines.
    g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    if (!g_log) {
        std::wstring alt = path;
        const size_t dot = alt.rfind(L'.');
        alt.insert(dot == std::wstring::npos ? alt.size() : dot, L"-" + std::to_wstring(GetCurrentProcessId()));
        g_log = _wfsopen(alt.c_str(), L"w", _SH_DENYWR);
    }
}

void LogClose() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lk(g_logMutex);
    char stamp[16];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    g_recent.push_back(stamp + std::string(msg));
    if (g_recent.size() > kRecentLines) g_recent.pop_front();
    ++g_logSeq;
    for (auto& [text, n] : g_watches)
        if (std::strstr(msg, text.c_str())) ++n;
    char full[24];
    snprintf(full, sizeof(full), "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    g_tail.Push(now, full + std::string(msg));
    if (!g_log) return;
    fprintf(g_log, "%s%s\n", full, msg);
    fflush(g_log);
}

std::vector<std::string> LogTail(double seconds) {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_tail.Trim(now);
    return g_tail.Since(now - seconds);
}

uint64_t LogWatch(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    for (const auto& [t, n] : g_watches)
        if (t == text) return n;
    if (text.empty() || g_watches.size() >= 16) return 0;
    g_watches.emplace_back(text, 0);
    return 0;
}

std::vector<std::string> RecentLog(size_t max, uint64_t* seq) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (seq) *seq = g_logSeq;
    const size_t n = std::min(max, g_recent.size());
    return std::vector<std::string>(g_recent.end() - ptrdiff_t(n), g_recent.end());
}

Config LoadConfig(const std::wstring& ini) {
    Config c;
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // A first name that differs between friends' PCs: the Windows account name when it fits.
        std::string user = "Player";
        wchar_t wuser[64];
        DWORD n = 64;
        if (GetUserNameW(wuser, &n)) {
            const std::string u = Narrow(wuser);
            if (kc::ValidName(u) && u.size() <= 15) user = u;
        }
        const std::string text = std::string() +
            "; KenshiCoop configuration\r\n"
            "; Hotkeys (in game): Ctrl+Shift+H host | Ctrl+Shift+J join | Ctrl+Shift+L leave\r\n"
            ";                    Ctrl+Shift+G give selected characters to the next player | Ctrl+Shift+O overlay\r\n"
            "[player]\r\n"
            "; your name in the session, and the name of your own character in the host's world\r\n"
            "name=" + user + "\r\n"
            "\r\n"
            "[network]\r\n"
            "; address of the host to join (IP or hostname)\r\n"
            "join_address=127.0.0.1\r\n"
            "; UDP port used to host and to join\r\n"
            "port=27960\r\n"
            "\r\n"
            "[sync]\r\n"
            "; a remote character further than this from the host position is teleported\r\n"
            "snap_distance=15\r\n"
            "destination_epsilon=2\r\n"
            "; NPCs within this distance of the squad are replicated to clients (0 = every active one)\r\n"
            "interest_radius=0\r\n"
            "\r\n"
            "[coop]\r\n"
            "; host: every player who joins gets a new character of their own in the squad (1), or none (0)\r\n"
            "own_character=1\r\n"
            "\r\n"
            "[ui]\r\n"
            "overlay=1\r\n";
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) { fputs(text.c_str(), f); fclose(f); }
    }
    wchar_t buf[256];
    auto str = [&](const wchar_t* sec, const wchar_t* key, const std::string& def) {
        GetPrivateProfileStringW(sec, key, Widen(def).c_str(), buf, 256, ini.c_str());
        return Narrow(buf);
    };
    auto num = [&](const wchar_t* sec, const wchar_t* key, double def) {
        const std::string s = str(sec, key, std::to_string(def));
        char* end = nullptr;
        const double v = strtod(s.c_str(), &end);
        return (end && end != s.c_str()) ? v : def;
    };
    c.name = str(L"player", L"name", c.name);
    if (c.name.size() > kc::kMaxNameLen) c.name.resize(kc::kMaxNameLen);
    if (!kc::ValidName(c.name)) c.name = "Player";
    c.joinAddress = str(L"network", L"join_address", c.joinAddress);
    const double port = num(L"network", L"port", c.port);
    if (port >= 1 && port <= 65535) c.port = uint16_t(port);
    c.snapDistance = float(num(L"sync", L"snap_distance", c.snapDistance));
    c.destEpsilon = float(num(L"sync", L"destination_epsilon", c.destEpsilon));
    c.interestRadius = float(num(L"sync", L"interest_radius", c.interestRadius));
    c.streamRadius = std::max(0.0f, float(num(L"sync", L"stream_radius", c.streamRadius)));
    c.standInRadius = std::max(0.0f, float(num(L"sync", L"standin_radius", c.standInRadius)));
    c.spawnsPerFrame = std::clamp(int(num(L"sync", L"spawns_per_frame", c.spawnsPerFrame)), 1, 50);
    c.autoZoneResync = num(L"sync", L"auto_zone_resync", 1) != 0;
    c.overlay = num(L"ui", L"overlay", 1) != 0;
    c.debugCommands = num(L"debug", L"commands", 0) != 0;
    c.steamLoopback = num(L"debug", L"steam_loopback", 0) != 0;
    c.hostConsole = num(L"ui", L"host_console", 1) != 0;
    c.characterPerPlayer = num(L"coop", L"own_character", 1) != 0;
    c.mapMarkers = num(L"ui", L"map_markers", 1) != 0;
    c.headMarkers = num(L"ui", L"head_markers", 1) != 0;
    c.portraitColours = num(L"ui", L"portrait_colours", 1) != 0;
    c.minimap = num(L"ui", L"minimap", 1) != 0;
    c.minimapRotate = num(L"ui", L"minimap_rotate", 0) != 0;
    c.minimapCorner = std::clamp(int(num(L"ui", L"minimap_corner", 1)), 0, 3);
    c.minimapZoom = std::clamp(float(num(L"ui", L"minimap_zoom", 1500)), 300.0f, 30000.0f);
    c.minimapX = float(num(L"ui", L"minimap_x", -1));
    c.minimapY = float(num(L"ui", L"minimap_y", -1));
    if (c.minimapX < 0 || c.minimapY < 0 || c.minimapX > 1 || c.minimapY > 1) c.minimapX = c.minimapY = -1;
    c.minimapSize = std::clamp(float(num(L"ui", L"minimap_size", 1)), 0.5f, 2.5f);
    c.pings = num(L"ui", L"pings", 1) != 0;
    // strtod would follow the C locale: read with the admin parser (decimal point or comma)
    float rate = 1.0f;
    c.healRate = kc::admin::ParseRate(str(L"host", L"heal_rate", "1"), rate) ? rate : 1.0f;
    c.xpRate = kc::admin::ParseRate(str(L"host", L"xp_rate", "1"), rate) ? rate : 1.0f;
    return c;
}

bool SaveIniValue(const std::wstring& ini, const char* section, const char* key, const std::string& value) {
    return WritePrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), Widen(value).c_str(), ini.c_str()) != 0;
}

bool SaveConnection(const std::wstring& ini, const std::string& name, const std::string& address, uint16_t port) {
    return WritePrivateProfileStringW(L"player", L"name", Widen(name).c_str(), ini.c_str()) &&
           WritePrivateProfileStringW(L"network", L"join_address", Widen(address).c_str(), ini.c_str()) &&
           WritePrivateProfileStringW(L"network", L"port", std::to_wstring(port).c_str(), ini.c_str());
}

bool SetUiOption(Config& c, const std::wstring& ini, const std::string& key, float v) {
    const bool on = v != 0.0f;
    if (key == "map_markers") c.mapMarkers = on;
    else if (key == "head_markers") c.headMarkers = on;
    else if (key == "portrait_colours") c.portraitColours = on;
    else if (key == "minimap") c.minimap = on;
    else if (key == "minimap_rotate") c.minimapRotate = on;
    else if (key == "pings") c.pings = on;
    else if (key == "minimap_corner") c.minimapCorner = std::clamp(int(v), 0, 3);
    else if (key == "minimap_zoom") c.minimapZoom = std::clamp(v, 300.0f, 30000.0f);
    else if (key == "minimap_x") c.minimapX = (v < 0 || v > 1) ? -1.0f : v;
    else if (key == "minimap_y") c.minimapY = (v < 0 || v > 1) ? -1.0f : v;
    else if (key == "minimap_size") c.minimapSize = std::clamp(v, 0.5f, 2.5f);
    else if (key != "minimap_reset") return false;
    auto put = [&](const char* k, const std::string& text) { WritePrivateProfileStringW(L"ui", Widen(k).c_str(), Widen(text).c_str(), ini.c_str()); };
    auto fraction = [](float f) {
        char b[32];
        snprintf(b, sizeof(b), "%.4f", double(f));
        return std::string(f < 0 ? "-1" : b);
    };
    // the corner (or a reset) drops the free position
    if (key == "minimap_corner" || key == "minimap_reset") {
        c.minimapX = c.minimapY = -1;
        put("minimap_x", "-1");
        put("minimap_y", "-1");
    }
    if (key == "minimap_reset") {
        c.minimapSize = 1.0f;
        put("minimap_size", "1");
        return true;
    }
    if (key == "minimap_zoom") put("minimap_zoom", std::to_string(int(c.minimapZoom)));
    else if (key == "minimap_corner") put("minimap_corner", std::to_string(c.minimapCorner));
    else if (key == "minimap_x") put("minimap_x", fraction(c.minimapX));
    else if (key == "minimap_y") put("minimap_y", fraction(c.minimapY));
    else if (key == "minimap_size") put("minimap_size", fraction(c.minimapSize));
    else put(key.c_str(), on ? "1" : "0");
    return true;
}

std::wstring GameDir() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L".\\" : s.substr(0, slash + 1);
}

bool Sha256File(const std::wstring& path, std::string& hexOut) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got > 0)
        ok = BCryptHashData(h, buf.data(), got, 0) == 0;
    uint8_t digest[32];
    ok = ok && BCryptFinishHash(h, digest, sizeof(digest), 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    static const char* hex = "0123456789ABCDEF";
    hexOut.clear();
    for (uint8_t b : digest) { hexOut.push_back(hex[b >> 4]); hexOut.push_back(hex[b & 15]); }
    return true;
}

uint64_t HashFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) h = kc::Fnv1a64(buf, n, h);
    fclose(f);
    return h;
}

namespace {
std::atomic<const char*> g_crashPhase{"start"};
} // namespace

void SetCrashPhase(const char* phase) { g_crashPhase.store(phase, std::memory_order_relaxed); }
const char* CrashPhase() { return g_crashPhase.load(std::memory_order_relaxed); }

double NowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

} // namespace kcp
