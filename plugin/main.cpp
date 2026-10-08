// KenshiCoop plugin entry point. Kenshi loads it as an Ogre plugin (Plugins_x64.cfg) and calls
// dllStartPlugin while the engine starts, before any save is loaded.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>

#include "debug.h"
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
    case kc::SessionState::Downloading: return "receiving the host's world...";
    case kc::SessionState::Loading: return "loading the host's world...";
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
    for (auto& h : sel) if (g_world->FindSquad(h)) mine.push_back(h);
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
    if (!g_world->Ready()) { Log("---- diagnostics (%s): no world loaded", why); return; }
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
    for (const auto& h : sel) Log("    hand{type=%u idx=%u ser=%u} known=%d", h.type, h.index, h.serial, int(g_world->FindSquad(h) != nullptr));
    std::vector<kenshi::Character*> active;
    kenshi::ActiveCharacters(active);
    double hours = -1;
    kenshi::GetGameHours(hours);
    Log("  active characters: %zu, game clock: %.3f h", active.size(), hours);
    for (const auto& h : hs) {
        kc::EntityVitals v;
        if (g_world->ReadVitals(h, v))
            Log("  vitals idx=%u blood=%.1f ko=%.1f flags=%u parts=%zu first=(%.1f, %.1f, %.1f)", h.index, v.blood, v.koTimer, unsigned(v.flags),
                v.parts.size(), v.parts.empty() ? 0.f : v.parts[0].flesh, v.parts.empty() ? 0.f : v.parts[0].stun, v.parts.empty() ? 0.f : v.parts[0].bandage);
    }
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
    if (st == kc::SessionState::Downloading) {
        m.lines.push_back("Receiving the host's world: " + std::to_string(int(g_session->downloadProgress() * 100)) + "%");
    } else if (st == kc::SessionState::Loading || st == kc::SessionState::Connecting || st == kc::SessionState::Handshake) {
        m.lines.push_back("Please wait...");
    } else if (st == kc::SessionState::Idle || st == kc::SessionState::Failed) {
        if (st == kc::SessionState::Failed && !g_session->lastError().empty()) m.lines.push_back(g_session->lastError());
        m.lines.push_back("Ctrl+Shift+H  host this game");
        m.lines.push_back("Ctrl+Shift+J  join " + g_cfg.joinAddress + ":" + std::to_string(g_cfg.port) + " (works from the main menu)");
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
        m.lines.push_back("World: " + std::to_string(g_session->npcCount()) + " NPCs synced" +
                          (g_session->isClient() && g_session->missingNpcs() ? " (" + std::to_string(g_session->missingNpcs()) + " not spawned here yet)" : ""));
        if (g_session->missingSquad())
            m.lines.push_back("WARNING: " + std::to_string(g_session->missingSquad()) + " squad members missing here - load the host's save!");
        if (g_session->isHost() && g_session->joiningPlayers())
            m.lines.push_back(std::to_string(g_session->joiningPlayers()) + " player(s) joining - game paused");
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

void Tick(bool live) {
    g_world->BeginFrame(live);
    for (auto& t : g_world->TakeToasts()) Toast(t);

    const bool ready = g_world->Ready();
    if (live && ready != g_wasReady) {
        g_wasReady = ready;
        Log(ready ? "world ready: %zu player characters" : "world unloaded", g_world->CharacterCount());
        if (ready) DumpDiagnostics("world loaded");
    } else if (!live && g_wasReady && NowSeconds() - LastLiveTick() > 1.0) {
        g_wasReady = false;
        Log("world unloaded (menu or loading)");
    }
    HandleHotkeys();
    if (g_cfg.debugCommands) DebugPoll(*g_session, *g_world, live);
    g_session->Tick(live);
    if (live) g_world->EndFrame();
    PublishOverlay();
}

void TickEntry(bool live) { Tick(live); }

// ---- Ogre frame listener: keeps KenshiCoop running in menus and loading screens, where the
// game's main loop (and therefore our main-loop tick) does not run. Its vtable must match the
// FrameListener of Kenshi's Ogre build, verified in OgreMain_x64.dll (Root::_fireFrame*):
// [0] frameStarted, [1] frameRenderingQueued, [2] (extra slot, unused), [3] frameEnded, [4] dtor.
struct FrameEvent {
    float timeSinceLastEvent;
    float timeSinceLastFrame;
};
class CoopFrameListener {
public:
    virtual bool frameStarted(const FrameEvent&) {
        if (NowSeconds() - LastLiveTick() > 0.2) RunTick(false);
        return true;
    }
    virtual bool frameRenderingQueued(const FrameEvent&) { return true; }
    virtual bool extraSlot(const void*) { return true; }
    virtual bool frameEnded(const FrameEvent&) { return true; }
    virtual ~CoopFrameListener() = default;
};
CoopFrameListener g_frameListener;

bool AddFrameListenerSEH(void* getSingleton, void* addListener) {
    using GetRootFn = void* (*)();
    using AddFn = void (*)(void* root, void* listener);
    __try {
        void* root = reinterpret_cast<GetRootFn>(getSingleton)();
        if (!root) return false;
        reinterpret_cast<AddFn>(addListener)(root, &g_frameListener);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InstallFrameListener() {
    HMODULE ogre = GetModuleHandleW(L"OgreMain_x64.dll");
    if (!ogre) return false;
    void* get = reinterpret_cast<void*>(GetProcAddress(ogre, "?getSingleton@Root@Ogre@@SAAEAV12@XZ"));
    void* add = reinterpret_cast<void*>(GetProcAddress(ogre, "?addFrameListener@Root@Ogre@@QEAAXPEAVFrameListener@2@@Z"));
    return get && add && AddFrameListenerSEH(get, add);
}

// Last-chance crash report: where the game died, written to KenshiCoop.log before Windows takes over.
std::string ModuleOf(uintptr_t addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) || !mod)
        return "?";
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring w(path, n);
    const size_t slash = w.find_last_of(L"\\/");
    char buf[160];
    snprintf(buf, sizeof(buf), "%ls+0x%llx", slash == std::wstring::npos ? w.c_str() : w.c_str() + slash + 1,
             (unsigned long long)(addr - reinterpret_cast<uintptr_t>(mod)));
    return buf;
}

LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> once{0};
    if (once.fetch_add(1) == 0 && ep && ep->ExceptionRecord && ep->ContextRecord) {
        const auto* er = ep->ExceptionRecord;
        const auto* cx = ep->ContextRecord;
        Log("CRASH code=%08lx at %p (%s) thread=%lu access=%llu addr=%p", er->ExceptionCode, er->ExceptionAddress,
            ModuleOf(reinterpret_cast<uintptr_t>(er->ExceptionAddress)).c_str(), GetCurrentThreadId(),
            er->NumberParameters > 0 ? (unsigned long long)er->ExceptionInformation[0] : 0ull,
            er->NumberParameters > 1 ? reinterpret_cast<void*>(er->ExceptionInformation[1]) : nullptr);
        // Return addresses on the stack that land in a loaded module: enough to see who jumped where.
        const auto* sp = reinterpret_cast<const uintptr_t*>(cx->Rsp);
        int shown = 0;
        for (int i = 0; i < 256 && shown < 24; ++i) {
            uintptr_t v = 0;
            if (IsBadReadPtr(sp + i, sizeof(v))) break;
            v = sp[i];
            const std::string m = ModuleOf(v);
            if (m != "?") { Log("  stack[%d] %s", i, m.c_str()); ++shown; }
        }
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

bool Start() {
    const std::wstring dir = GameDir();
    LogOpen(dir + L"KenshiCoop.log");
    Log("KenshiCoop %s starting", kVersion);
    g_prevFilter = SetUnhandledExceptionFilter(&CrashFilter);

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
    sc.interestRadius = g_cfg.interestRadius;
    g_session = std::make_unique<kc::Session>(*g_world, sc, NowSeconds, [](const std::string& s) { Log("%s", s.c_str()); });

    if (!InstallHooks(&TickEntry, &err)) { Log("disabled: %s", err.c_str()); g_session.reset(); g_world.reset(); return false; }
    if (!OverlayInstall(&err)) Log("overlay unavailable: %s (multiplayer still works, see this log)", err.c_str());
    if (!InstallFrameListener()) Log("frame listener unavailable: joining from the main menu will not work");
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
