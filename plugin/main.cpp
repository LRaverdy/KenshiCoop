// KenshiCoop plugin entry point. Kenshi loads it as an Ogre plugin (Plugins_x64.cfg) and calls
// dllStartPlugin while the engine starts, before any save is loaded.
#include <windows.h>

#include <cstdio>
#include <deque>
#include <memory>
#include <string>

#include "hooks.h"
#include "kc/session.h"
#include "kenshi.h"
#include "overlay.h"
#include "util.h"
#include "world.h"

namespace kcp {

namespace {

constexpr const char* kVersion = "0.1.0";

Config g_cfg;
std::unique_ptr<KenshiWorld> g_world;
std::unique_ptr<kc::Session> g_session;
bool g_overlayVisible = true;
std::deque<std::pair<std::string, double>> g_toasts;   // text, expiry time
bool g_wasReady = false;

const char* StateName(kc::SessionState s) {
    switch (s) {
    case kc::SessionState::Idle: return "offline";
    case kc::SessionState::Hosting: return "hosting";
    case kc::SessionState::Connecting: return "connecting...";
    case kc::SessionState::Handshake: return "handshaking...";
    case kc::SessionState::Connected: return "connected";
    case kc::SessionState::Failed: return "disconnected";
    }
    return "?";
}

void Toast(const std::string& msg, double seconds = 4.0) {
    const double now = NowSeconds();
    for (auto& t : g_toasts) if (t.first == msg) { t.second = now + seconds; return; }
    g_toasts.emplace_back(msg, now + seconds);
    while (g_toasts.size() > 4) g_toasts.pop_front();
    Log("toast: %s", msg.c_str());
}

// ---- hotkeys: Ctrl+Shift+<key>, edge-triggered, only while Kenshi has focus
bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool OurWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

struct Hotkey {
    int vk;
    bool down = false;
};
Hotkey g_hkHost{'H'}, g_hkJoin{'J'}, g_hkLeave{'L'}, g_hkGive{'G'}, g_hkOverlay{'O'}, g_hkDiag{'D'};

bool Pressed(Hotkey& k, bool modifiers) {
    const bool now = modifiers && KeyDown(k.vk);
    const bool edge = now && !k.down;
    k.down = now;
    return edge;
}

void GiveSelectedToNextPlayer() {
    if (!g_session->isHost()) { Toast("Only the host can hand characters to players."); return; }
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    std::vector<kc::Handle> mine;
    for (auto& h : sel) if (g_world->Find(h)) mine.push_back(h);
    if (mine.empty()) { Toast("Select one of your squad members first."); return; }
    // cycle owner: host -> each connected player -> host
    std::vector<uint8_t> order{g_session->localId()};
    for (auto& [id, p] : g_session->players()) order.push_back(id);
    const uint8_t cur = g_session->ownerOf(mine.front());
    size_t idx = 0;
    for (size_t i = 0; i < order.size(); ++i) if (order[i] == cur) idx = i;
    const uint8_t next = order[(idx + 1) % order.size()];
    for (auto& h : mine) g_session->Assign(h, next);
    const std::string who = next == g_session->localId() ? "you" : g_session->players().at(next).name;
    Toast(std::to_string(mine.size()) + " character(s) now controlled by " + who + ".");
}

// Writes everything KenshiCoop reads from the game to the log, to validate the memory layout.
void DumpDiagnostics(const char* why) {
    std::vector<kc::Handle> hs;
    g_world->PlayerCharacters(hs);
    Log("---- diagnostics (%s): %zu player characters, speed=%.2f paused=%d", why, hs.size(), kenshi::GetFrameSpeed(), int(kenshi::GetPaused()));
    for (const auto& h : hs) {
        kc::EntityState st;
        const bool ok = g_world->Read(h, st);
        Log("  hand{type=%u cont=%u cser=%u idx=%u ser=%u} read=%d pos=(%.1f, %.1f, %.1f) rot=(%.3f, %.3f, %.3f, %.3f) dest=(%.1f, %.1f, %.1f) flags=%u",
            h.type, h.container, h.containerSerial, h.index, h.serial, int(ok), st.pos.x, st.pos.y, st.pos.z,
            st.rot.w, st.rot.x, st.rot.y, st.rot.z, st.dest.x, st.dest.y, st.dest.z, unsigned(st.flags));
    }
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    Log("  selected: %zu", sel.size());
    for (const auto& h : sel) Log("    hand{type=%u idx=%u ser=%u} known=%d", h.type, h.index, h.serial, int(g_world->Find(h) != nullptr));
    Log("  world fingerprint %016llx, mods hash %016llx", (unsigned long long)g_world->Fingerprint(), (unsigned long long)g_world->ModsHash());
}

void HandleHotkeys() {
    const bool mods = OurWindowFocused() && KeyDown(VK_CONTROL) && KeyDown(VK_SHIFT);
    std::string err;
    if (Pressed(g_hkOverlay, mods)) g_overlayVisible = !g_overlayVisible;
    if (Pressed(g_hkHost, mods)) {
        if (g_session->isHost()) Toast("Already hosting.");
        else if (g_session->Host(&err)) Toast("Hosting on port " + std::to_string(g_cfg.port) + ".");
        else Toast("Cannot host: " + err);
    }
    if (Pressed(g_hkJoin, mods)) {
        if (g_session->isClient()) Toast("Already connected.");
        else if (g_session->Join(g_cfg.joinAddress, g_cfg.port, &err)) Toast("Joining " + g_cfg.joinAddress + "...");
        else Toast("Cannot join: " + err);
    }
    if (Pressed(g_hkLeave, mods)) {
        g_session->Leave();
        Toast("Left the session.");
    }
    if (Pressed(g_hkGive, mods)) GiveSelectedToNextPlayer();
    if (Pressed(g_hkDiag, mods)) { DumpDiagnostics("hotkey"); Toast("Diagnostics written to KenshiCoop.log"); }
}

void PublishOverlay() {
    OverlayModel m;
    m.visible = g_overlayVisible;
    m.title = std::string("KenshiCoop ") + kVersion + "  -  " + StateName(g_session->state());
    const auto st = g_session->state();
    if (st == kc::SessionState::Idle || st == kc::SessionState::Failed) {
        if (st == kc::SessionState::Failed && !g_session->lastError().empty()) m.lines.push_back(g_session->lastError());
        m.lines.push_back("Ctrl+Shift+H  host this game");
        m.lines.push_back("Ctrl+Shift+J  join " + g_cfg.joinAddress + ":" + std::to_string(g_cfg.port));
        m.lines.push_back("Ctrl+Shift+O  hide this panel");
    } else {
        m.lines.push_back("You: " + g_cfg.name + " (player " + std::to_string(g_session->localId()) + ")" +
                          (g_session->isHost() ? "" : "   ping " + std::to_string(g_session->pingMs()) + " ms"));
        for (auto& [id, p] : g_session->players())
            m.lines.push_back("  player " + std::to_string(id) + ": " + p.name + (g_session->isHost() ? "   ping " + std::to_string(p.rttMs) + " ms" : ""));
        size_t mine = 0;
        std::vector<kc::Handle> hs;
        g_world->PlayerCharacters(hs);
        for (auto& h : hs) if (g_session->ownerOf(h) == g_session->localId()) ++mine;
        m.lines.push_back("Squad: " + std::to_string(hs.size()) + " characters, " + std::to_string(mine) + " yours");
        if (g_session->missingEntities())
            m.lines.push_back("WARNING: " + std::to_string(g_session->missingEntities()) + " host characters missing here - load the host's save!");
        if (g_session->isHost()) m.lines.push_back("Ctrl+Shift+G  give selected to next player");
        m.lines.push_back("Ctrl+Shift+L  leave");
    }
    const auto& chat = g_session->chatLog();
    for (size_t i = chat.size() > 6 ? chat.size() - 6 : 0; i < chat.size(); ++i) m.chat.push_back(chat[i]);
    const double now = NowSeconds();
    while (!g_toasts.empty() && g_toasts.front().second < now) g_toasts.pop_front();
    for (auto& t : g_toasts) m.toasts.push_back(t.first);
    OverlayPublish(std::move(m));
}

void Tick() {
    g_world->BeginFrame();
    for (auto& t : g_world->TakeToasts()) Toast(t);

    const bool ready = g_world->Ready();
    if (ready != g_wasReady) {
        g_wasReady = ready;
        Log(ready ? "world ready: %zu player characters" : "world unloaded", g_world->CharacterCount());
        if (ready) DumpDiagnostics("world loaded");
    }
    HandleHotkeys();
    g_session->Tick();
    g_world->EndFrame();
    PublishOverlay();
}

void TickEntry() { Tick(); }

bool Start() {
    const std::wstring dir = GameDir();
    LogOpen(dir + L"KenshiCoop.log");
    Log("KenshiCoop %s starting", kVersion);

    // Never patch a game build we have not analysed.
    std::string sha;
    if (!Sha256File(dir + L"kenshi_x64.exe", sha)) { Log("cannot read kenshi_x64.exe, disabled"); return false; }
    if (sha != kenshi::kSupportedExeSha256) {
        Log("unsupported kenshi_x64.exe (sha256 %s), KenshiCoop disabled. Supported: Kenshi 1.0.68 Steam.", sha.c_str());
        return false;
    }
    std::string err;
    if (!kenshi::Init(&err)) { Log("disabled: %s", err.c_str()); return false; }

    g_cfg = LoadConfig(dir + L"KenshiCoop.ini");
    g_overlayVisible = g_cfg.overlay;
    g_world = std::make_unique<KenshiWorld>(g_cfg);
    g_world->SetGameBuild(std::stoull(sha.substr(0, 16), nullptr, 16));

    kc::SessionConfig sc;
    sc.name = g_cfg.name;
    sc.port = g_cfg.port;
    sc.snapDistance = g_cfg.snapDistance;
    g_session = std::make_unique<kc::Session>(*g_world, sc, NowSeconds, [](const std::string& s) { Log("%s", s.c_str()); });

    if (!InstallHooks(&TickEntry, &err)) { Log("disabled: %s", err.c_str()); g_session.reset(); g_world.reset(); return false; }
    if (!OverlayInstall(&err)) Log("overlay unavailable: %s (multiplayer still works, see this log)", err.c_str());
    Log("ready. name='%s' port=%u join=%s", g_cfg.name.c_str(), g_cfg.port, g_cfg.joinAddress.c_str());
    return true;
}

} // namespace

KenshiWorld* TheWorld() { return g_world.get(); }

} // namespace kcp

extern "C" __declspec(dllexport) void dllStartPlugin() {
    // Keep the DLL mapped for the life of the process: hooks may still be on a thread's stack
    // while Ogre shuts plugins down.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&dllStartPlugin), &self);
    try {
        kcp::Start();
    } catch (const std::exception& e) {
        kcp::Log("startup exception: %s", e.what());
    } catch (...) {
        kcp::Log("startup exception");
    }
}

extern "C" __declspec(dllexport) void dllStopPlugin() {
    if (kcp::g_session) kcp::g_session->Leave();
    kcp::OverlayShutdown();
    kcp::RemoveHooks();
    kcp::Log("stopped");
    kcp::LogClose();
}
