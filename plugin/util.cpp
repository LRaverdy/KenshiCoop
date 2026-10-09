#include "util.h"

#include <windows.h>
#include <bcrypt.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

#include "kc/protocol.h"

namespace kcp {

namespace {
std::mutex g_logMutex;
FILE* g_log = nullptr;

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

void LogOpen(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_log) return;
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
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_log) return;
    fprintf(g_log, "%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fflush(g_log);
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
    c.overlay = num(L"ui", L"overlay", 1) != 0;
    c.debugCommands = num(L"debug", L"commands", 0) != 0;
    c.characterPerPlayer = num(L"coop", L"own_character", 1) != 0;
    return c;
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

double NowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

} // namespace kcp
