#include "overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "util.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace kcp {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn o_present = nullptr;
ResizeFn o_resize = nullptr;
void* g_presentTarget = nullptr;
void* g_resizeTarget = nullptr;

std::mutex g_modelMutex;
OverlayModel g_model;
std::mutex g_actionMutex;
std::vector<OverlayAction> g_actions;

IDXGISwapChain* g_swap = nullptr;   // the swap chain we initialised on (Kenshi's)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_ready = false;
bool g_failed = false;

// ImGui is fed by the window procedure and drawn by Present: both normally run on the game's
// main thread, the lock covers the case where they do not.
std::recursive_mutex g_imguiMutex;
HWND g_hwnd = nullptr;
WNDPROC g_oldWndProc = nullptr;

std::atomic<bool> g_mpOpen{false}, g_consoleOpen{false};
std::atomic<bool> g_mpOpened{false};            // reset the window's fields from the settings
std::atomic<bool> g_wantKeyboard{false}, g_wantMouse{false};

// ---- the windows' own state (render thread)
char g_nameBuf[64] = {};
char g_addrBuf[128] = {};
int g_portBuf = 27960;
char g_cmdBuf[256] = {};
uint64_t g_logSeen = 0;
std::atomic<bool> g_refocusCommand{false};

void PushAction(OverlayAction a) {
    std::lock_guard<std::mutex> lk(g_actionMutex);
    g_actions.push_back(std::move(a));
}

std::atomic<bool> g_dialogOpen{false};
bool Interactive() { return g_mpOpen.load() || g_consoleOpen.load() || g_dialogOpen.load(); }

void ReleaseRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_ready && Interactive()) {
        std::lock_guard<std::recursive_mutex> lk(g_imguiMutex);
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        const bool mouseMsg = (msg >= WM_LBUTTONDOWN && msg <= WM_MOUSEHWHEEL) && msg != WM_MOUSEMOVE;
        const bool keyMsg = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
        if ((mouseMsg && g_wantMouse.load()) || (keyMsg && g_wantKeyboard.load())) return 0;
    }
    return CallWindowProcW(g_oldWndProc, hwnd, msg, wp, lp);
}

bool EnsureInit(IDXGISwapChain* swap) {
    if (g_ready) return swap == g_swap;
    if (g_failed) return false;
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap->GetDesc(&desc)) || !desc.OutputWindow) { g_failed = true; return false; }
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) { g_failed = true; return false; }
    g_device->GetImmediateContext(&g_ctx);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui::GetStyle().Alpha = 0.95f;
    if (!ImGui_ImplWin32_Init(desc.OutputWindow) || !ImGui_ImplDX11_Init(g_device, g_ctx)) { g_failed = true; return false; }
    g_hwnd = desc.OutputWindow;
    g_oldWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
    g_swap = swap;
    g_ready = true;
    Log("overlay initialised");
    return true;
}

void DrawStatus(const OverlayModel& m, float w, float h) {
    if (m.visible) {
        ImGui::SetNextWindowPos(ImVec2(w - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGui::Begin("KenshiCoop", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", m.title.c_str());
        ImGui::Separator();
        for (const auto& l : m.lines) ImGui::TextUnformatted(l.c_str());
        if (!m.chat.empty()) {
            ImGui::Separator();
            for (const auto& l : m.chat) ImGui::TextUnformatted(l.c_str());
        }
        ImGui::End();
    }
    if (!m.toasts.empty()) {
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.12f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("KenshiCoopToast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
        for (const auto& t : m.toasts) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", t.c_str());
        ImGui::End();
    }
}

void CopyTo(char* buf, size_t n, const std::string& s) {
    strncpy_s(buf, n, s.c_str(), _TRUNCATE);
}

// A conversation in the host's world: what the other one says, and the answers to pick.
void DrawDialog(const OverlayModel& m, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.62f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(std::min(620.0f, w - 32.0f), 0.0f), ImGuiCond_Always);
    const std::string title = (m.dialogName.empty() ? std::string("Conversation") : m.dialogName) + "###kcdialog";
    if (!ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(m.dialogText.empty() ? "..." : m.dialogText.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::BeginDisabled(m.dialogWaiting);
    for (size_t i = 0; i < m.dialogReplies.size(); ++i) {
        const std::string label = std::to_string(i + 1) + ". " + m.dialogReplies[i] + "##r" + std::to_string(i);
        if (ImGui::Selectable(label.c_str())) {
            OverlayAction a;
            a.kind = OverlayAction::Kind::DialogAnswer;
            a.index = int(i);
            PushAction(std::move(a));
        }
    }
    ImGui::EndDisabled();
    if (m.dialogReplies.empty()) ImGui::TextDisabled("(la conversation continue...)");
    ImGui::End();
}

void DrawMultiplayer(const OverlayModel& m, float w, float h) {
    if (g_mpOpened.exchange(false)) {
        CopyTo(g_nameBuf, sizeof(g_nameBuf), m.name);
        CopyTo(g_addrBuf, sizeof(g_addrBuf), m.address);
        g_portBuf = m.port ? m.port : 27960;
    }
    bool open = true;
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.3f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Multijoueur (Ctrl+Shift+M)", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!open) g_mpOpen = false;
        return;
    }
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", m.stateText.c_str());
    if (m.download >= 0) ImGui::ProgressBar(m.download, ImVec2(-1.0f, 0.0f));
    if (!m.errorText.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m.errorText.c_str());
    if (m.leftHostWorld) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Tu n'es plus dans la partie de l'hôte : ce que tu vois n'en est qu'une copie, en pause.");
        ImGui::TextUnformatted("Pour reprendre une de tes parties : Échap, puis Charger. Ou rejoins à nouveau ci-dessous.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Quitter le jeu")) PushAction({OverlayAction::Kind::QuitGame, {}, {}, {}, 0});
    }
    ImGui::Separator();
    if (!m.active) {
        ImGui::InputText("Ton nom", g_nameBuf, sizeof(g_nameBuf));
        ImGui::InputText("Adresse ou code Steam", g_addrBuf, sizeof(g_addrBuf));
        ImGui::InputInt("Port", &g_portBuf, 0, 0);
        g_portBuf = std::clamp(g_portBuf, 1, 65535);
        auto action = [](OverlayAction::Kind k) {
            OverlayAction a;
            a.kind = k;
            a.name = g_nameBuf;
            a.address = g_addrBuf;
            a.port = uint16_t(g_portBuf);
            PushAction(std::move(a));
        };
        ImGui::BeginDisabled(!m.worldLoaded);
        if (ImGui::Button("Héberger la partie")) action(OverlayAction::Kind::Host);
        ImGui::EndDisabled();
        if (!m.worldLoaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Charge d'abord la partie à partager.");
        ImGui::SameLine();
        if (ImGui::Button("Rejoindre")) action(OverlayAction::Kind::Join);
        ImGui::TextDisabled("Héberger : la partie chargée est partagée.");
        ImGui::TextDisabled("Rejoindre : marche aussi depuis le menu principal.");
        if (!m.steamId.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Amis Steam qui hébergent :");
            if (m.steamFriends.empty()) ImGui::TextDisabled("  personne pour l'instant");
            for (const auto& [name, id] : m.steamFriends) {
                ImGui::PushID(id.c_str());
                if (ImGui::Button("Rejoindre")) {
                    OverlayAction a;
                    a.kind = OverlayAction::Kind::Join;
                    a.name = g_nameBuf;
                    a.address = "steam:" + id;
                    a.port = uint16_t(g_portBuf);
                    PushAction(std::move(a));
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(name.c_str());
                ImGui::PopID();
            }
            ImGui::TextDisabled("Via Steam : aucun port à ouvrir. Sinon, une IP (port UDP %d).", g_portBuf);
        } else {
            ImGui::TextDisabled("L'hôte doit ouvrir le port UDP %d (ou utilisez un VPN de jeu).", g_portBuf);
        }
    } else {
        if (!m.hosting) {
            if (ImGui::Button("Modifier mon personnage")) {
                OverlayAction a;
                a.kind = OverlayAction::Kind::EditCharacter;
                PushAction(std::move(a));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(l'éditeur de Kenshi ; tout le monde verra le résultat)");
            ImGui::Separator();
        }
        if (m.hosting && !m.steamId.empty()) {
            ImGui::Text("Code Steam : %s", m.steamId.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copier")) ImGui::SetClipboardText(m.steamId.c_str());
            ImGui::TextDisabled("Tes amis Steam : clic droit sur ton nom > Rejoindre la partie,");
            ImGui::TextDisabled("ou ce code dans leur fenêtre Multijoueur.");
            ImGui::Separator();
        }
        if (ImGui::BeginTable("players", m.hosting ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Joueur");
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Persos", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            if (m.hosting) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 160.0f);
            ImGui::TableHeadersRow();
            for (const auto& p : m.players) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u. %s%s", unsigned(p.id), p.name.c_str(), p.you ? " (toi)" : "");
                ImGui::TableNextColumn();
                if (p.you && m.hosting) ImGui::TextUnformatted("hôte");
                else ImGui::Text("%u ms", p.pingMs);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", p.characters);
                if (m.hosting) {
                    ImGui::TableNextColumn();
                    if (!p.you && p.characters > 0) {
                        ImGui::PushID(int(p.id));
                        if (ImGui::SmallButton("TP vers moi")) PushAction({OverlayAction::Kind::Command, {}, {}, "tp " + std::to_string(p.id), 0});
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Amène ses personnages près de ton personnage sélectionné (pour le débloquer).");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Resync")) PushAction({OverlayAction::Kind::Command, {}, {}, "resync " + std::to_string(p.id), 0});
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Il recharge ton monde tel qu'il est maintenant (en cas de désynchro).");
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndTable();
        }
        if (m.hosting && m.players.size() > 1) {
            if (ImGui::Button("Resynchroniser tout le monde")) PushAction({OverlayAction::Kind::Command, {}, {}, "resync", 0});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Chaque joueur recharge ton monde tel qu'il est maintenant (quelques secondes, la partie attend).");
            ImGui::SameLine();
        }
        // the host's leave closes the game for everyone: a second click confirms it
        static double leaveArmedAt = -1e9;
        const bool armed = m.hosting && ImGui::GetTime() - leaveArmedAt < 4.0;
        if (ImGui::Button(armed ? "Confirmer : fermer la partie pour tous" : "Quitter la session")) {
            if (m.hosting && !armed) leaveArmedAt = ImGui::GetTime();
            else { leaveArmedAt = -1e9; PushAction({OverlayAction::Kind::Leave, {}, {}, {}, 0}); }
        }
    }
    ImGui::End();
    if (!open) g_mpOpen = false;
}

void DrawConsole(const OverlayModel& m, float w, float h) {
    bool open = true;
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h - 24.0f), ImGuiCond_Appearing, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, w - 40.0f), 340.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Console KenshiCoop (Ctrl+Shift+K)", &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!open) g_consoleOpen = false;
        return;
    }
    uint64_t seq = 0;
    const std::vector<std::string> lines = RecentLog(300, &seq);
    const float inputHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("log", ImVec2(0.0f, -inputHeight), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& l : lines) {
        const bool cmd = l.find("> ") != std::string::npos && l.find("> ") < 12;
        if (cmd) ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", l.c_str());
        else ImGui::TextUnformatted(l.c_str());
    }
    if (seq != g_logSeen) {   // something new: follow it
        g_logSeen = seq;
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::TextDisabled(m.fullConsole ? "Tape help pour la liste des commandes." : "Tape help pour la liste des commandes (client : consultation seulement).");
    ImGui::SetNextItemWidth(-1.0f);
    if (g_refocusCommand) { ImGui::SetKeyboardFocusHere(); g_refocusCommand = false; }
    if (ImGui::InputText("##cmd", g_cmdBuf, sizeof(g_cmdBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (g_cmdBuf[0]) {
            OverlayAction a;
            a.kind = OverlayAction::Kind::Command;
            a.text = g_cmdBuf;
            PushAction(std::move(a));
        }
        g_cmdBuf[0] = 0;
        g_refocusCommand = true;
    }
    ImGui::End();
    if (!open) g_consoleOpen = false;
}

void Render(IDXGISwapChain* swap) {
    if (!EnsureInit(swap)) return;
    OverlayModel m;
    {
        std::lock_guard<std::mutex> lk(g_modelMutex);
        m = g_model;
    }
    const bool interactive = Interactive();
    if (!m.visible && m.toasts.empty() && !interactive) {
        g_wantKeyboard = false;
        g_wantMouse = false;
        return;
    }

    if (!g_rtv) {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
        const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
        if (FAILED(hr)) return;
    }
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap->GetDesc(&desc)) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0) return;

    std::lock_guard<std::recursive_mutex> lk(g_imguiMutex);
    ImGuiIO& io = ImGui::GetIO();
    // our windows take the mouse and keyboard only while one of them is open
    if (interactive) io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard);
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2(float(desc.BufferDesc.Width), float(desc.BufferDesc.Height));
    ImGui::NewFrame();
    DrawStatus(m, io.DisplaySize.x, io.DisplaySize.y);
    if (g_mpOpen) DrawMultiplayer(m, io.DisplaySize.x, io.DisplaySize.y);
    if (g_consoleOpen) DrawConsole(m, io.DisplaySize.x, io.DisplaySize.y);
    if (m.dialogOpen) DrawDialog(m, io.DisplaySize.x, io.DisplaySize.y);
    io.MouseDrawCursor = interactive && io.WantCaptureMouse;   // the game may hide the system cursor
    ImGui::Render();
    g_wantKeyboard = interactive && io.WantCaptureKeyboard;
    g_wantMouse = interactive && io.WantCaptureMouse;

    // The DX11 backend restores pipeline state but not render targets: do that ourselves.
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
}

void RenderSEH(IDXGISwapChain* swap) {
    __try {
        Render(swap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;   // never risk the game's frame twice
        g_wantKeyboard = false;
        g_wantMouse = false;
        Log("overlay: exception during rendering, overlay disabled");
    }
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) RenderSEH(swap);
    return o_present(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (swap == g_swap) ReleaseRTV();   // the back buffer is about to be recreated
    return o_resize(swap, count, w, h, fmt, flags);
}

// ---- DirectInput (the game's keyboard and mouse, read through OIS): held back while our windows
// have them. Presses are dropped, releases still go through, so no key stays stuck down.
using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(void* dev, DWORD size, void* data);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(void* dev, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags);
using GetCapsFn = HRESULT(STDMETHODCALLTYPE*)(void* dev, DIDEVCAPS* caps);
constexpr size_t kSlotGetCaps = 3, kSlotGetState = 9, kSlotGetData = 10;

struct DiHooks {
    void* stateTarget = nullptr;
    void* dataTarget = nullptr;
    GetStateFn o_state = nullptr;
    GetDataFn o_data = nullptr;
};
DiHooks g_di[2];   // DirectInput's ANSI and wide device implementations
std::mutex g_devMutex;
std::unordered_map<void*, BYTE> g_devType;   // device -> DI8DEVTYPE_*

BYTE DeviceType(void* dev) {
    {
        std::lock_guard<std::mutex> lk(g_devMutex);
        auto it = g_devType.find(dev);
        if (it != g_devType.end()) return it->second;
    }
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    void** vt = *reinterpret_cast<void***>(dev);
    const BYTE type = SUCCEEDED(reinterpret_cast<GetCapsFn>(vt[kSlotGetCaps])(dev, &caps)) ? BYTE(caps.dwDevType & 0xFF) : 0;
    std::lock_guard<std::mutex> lk(g_devMutex);
    g_devType[dev] = type;
    return type;
}

void FilterState(void* dev, DWORD size, void* data) {
    if (!data) return;
    const BYTE type = DeviceType(dev);
    if (type == DI8DEVTYPE_KEYBOARD && g_wantKeyboard) {
        std::memset(data, 0, size);
    } else if (type == DI8DEVTYPE_MOUSE && g_wantMouse && size >= sizeof(DIMOUSESTATE)) {
        auto* s = static_cast<DIMOUSESTATE*>(data);
        s->lZ = 0;
        std::memset(static_cast<BYTE*>(data) + offsetof(DIMOUSESTATE, rgbButtons), 0, size - offsetof(DIMOUSESTATE, rgbButtons));
    }
}

void FilterData(void* dev, DIDEVICEOBJECTDATA* data, DWORD* count) {
    if (!data || !count || *count == 0) return;
    const BYTE type = DeviceType(dev);
    const bool keys = type == DI8DEVTYPE_KEYBOARD && g_wantKeyboard;
    const bool mouse = type == DI8DEVTYPE_MOUSE && g_wantMouse;
    if (!keys && !mouse) return;
    DWORD kept = 0;
    for (DWORD i = 0; i < *count; ++i) {
        const DIDEVICEOBJECTDATA& e = data[i];
        bool drop = false;
        if (keys) drop = (e.dwData & 0x80) != 0;   // key presses
        if (mouse) drop = e.dwOfs == DIMOFS_Z || (e.dwOfs >= DIMOFS_BUTTON0 && e.dwOfs <= DIMOFS_BUTTON7 && (e.dwData & 0x80));
        if (!drop) data[kept++] = e;
    }
    *count = kept;
}

HRESULT STDMETHODCALLTYPE hk_getStateA(void* dev, DWORD size, void* data) {
    const HRESULT hr = g_di[0].o_state(dev, size, data);
    if (SUCCEEDED(hr)) FilterState(dev, size, data);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_getStateW(void* dev, DWORD size, void* data) {
    const HRESULT hr = g_di[1].o_state(dev, size, data);
    if (SUCCEEDED(hr)) FilterState(dev, size, data);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_getDataA(void* dev, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) {
    const HRESULT hr = g_di[0].o_data(dev, size, data, count, flags);
    if (SUCCEEDED(hr) && !(flags & DIGDD_PEEK)) FilterData(dev, data, count);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_getDataW(void* dev, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) {
    const HRESULT hr = g_di[1].o_data(dev, size, data, count, flags);
    if (SUCCEEDED(hr) && !(flags & DIGDD_PEEK)) FilterData(dev, data, count);
    return hr;
}

// Find the device methods through a throwaway keyboard device, and hook them (both flavours share
// one implementation on some systems: the second hook is then simply the same one).
bool HookDirectInput(std::string* err) {
    const IID* iids[2] = {&IID_IDirectInput8A, &IID_IDirectInput8W};
    void* stateHooks[2] = {reinterpret_cast<void*>(&hk_getStateA), reinterpret_cast<void*>(&hk_getStateW)};
    void* dataHooks[2] = {reinterpret_cast<void*>(&hk_getDataA), reinterpret_cast<void*>(&hk_getDataW)};
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        IUnknown* di = nullptr;
        if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[i], reinterpret_cast<void**>(&di), nullptr)) || !di)
            continue;
        void* dev = nullptr;
        HRESULT hr;
        if (i == 0) hr = static_cast<IDirectInput8A*>(static_cast<void*>(di))->CreateDevice(GUID_SysKeyboard, reinterpret_cast<IDirectInputDevice8A**>(&dev), nullptr);
        else hr = static_cast<IDirectInput8W*>(static_cast<void*>(di))->CreateDevice(GUID_SysKeyboard, reinterpret_cast<IDirectInputDevice8W**>(&dev), nullptr);
        if (SUCCEEDED(hr) && dev) {
            void** vt = *reinterpret_cast<void***>(dev);
            g_di[i].stateTarget = vt[kSlotGetState];
            g_di[i].dataTarget = vt[kSlotGetData];
            const bool same = i == 1 && g_di[1].stateTarget == g_di[0].stateTarget;
            if (same) {
                g_di[1] = g_di[0];
                any = true;
            } else if (MH_CreateHook(g_di[i].stateTarget, stateHooks[i], reinterpret_cast<void**>(&g_di[i].o_state)) == MH_OK &&
                       MH_CreateHook(g_di[i].dataTarget, dataHooks[i], reinterpret_cast<void**>(&g_di[i].o_data)) == MH_OK &&
                       MH_EnableHook(g_di[i].stateTarget) == MH_OK && MH_EnableHook(g_di[i].dataTarget) == MH_OK) {
                any = true;
            } else {
                g_di[i] = DiHooks{};
            }
            static_cast<IUnknown*>(dev)->Release();
        }
        di->Release();
    }
    if (!any && err) *err = "cannot hook DirectInput (typing in KenshiCoop windows also reaches the game)";
    return any;
}

// Find IDXGISwapChain::Present/ResizeBuffers through a throwaway device on a hidden window.
bool FindSwapChainFunctions(void** present, void** resize, std::string* err) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KenshiCoopDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { if (err) *err = "cannot create dummy window"; return false; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, &fl, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, &fl, &ctx);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        void** vt = *reinterpret_cast<void***>(swap);
        *present = vt[8];
        *resize = vt[13];
    } else if (err) {
        *err = "cannot create a D3D11 device for the overlay";
    }
    if (swap) swap->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

bool OverlayInstall(std::string* err) {
    if (!FindSwapChainFunctions(&g_presentTarget, &g_resizeTarget, err)) return false;
    if (MH_CreateHook(g_presentTarget, reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&o_present)) != MH_OK ||
        MH_CreateHook(g_resizeTarget, reinterpret_cast<void*>(&hk_resize), reinterpret_cast<void**>(&o_resize)) != MH_OK ||
        MH_EnableHook(g_presentTarget) != MH_OK || MH_EnableHook(g_resizeTarget) != MH_OK) {
        if (err) *err = "cannot hook the swap chain";
        return false;
    }
    std::string diErr;
    if (!HookDirectInput(&diErr)) Log("overlay: %s", diErr.c_str());
    return true;
}

void OverlayPublish(OverlayModel model) {
    std::lock_guard<std::mutex> lk(g_modelMutex);
    g_dialogOpen = model.dialogOpen;
    g_model = std::move(model);
}

std::vector<OverlayAction> OverlayTakeActions() {
    std::lock_guard<std::mutex> lk(g_actionMutex);
    std::vector<OverlayAction> out;
    out.swap(g_actions);
    return out;
}

void OverlayToggleMultiplayer() {
    const bool open = !g_mpOpen.load();
    if (open) g_mpOpened = true;
    g_mpOpen = open;
}

void OverlayToggleConsole() {
    const bool open = !g_consoleOpen.load();
    if (open) g_refocusCommand = true;
    g_consoleOpen = open;
}

void OverlayOpenMultiplayer() {
    if (g_mpOpen.load()) return;
    g_mpOpened = true;
    g_mpOpen = true;
}

bool OverlayTyping() { return g_wantKeyboard.load(); }

void OverlayShutdown() {
    g_mpOpen = false;
    g_consoleOpen = false;
    g_wantKeyboard = false;
    g_wantMouse = false;
    if (g_presentTarget) MH_DisableHook(g_presentTarget);
    if (g_resizeTarget) MH_DisableHook(g_resizeTarget);
    for (auto& d : g_di) {
        if (d.stateTarget && d.o_state) MH_DisableHook(d.stateTarget);
        if (d.dataTarget && d.o_data) MH_DisableHook(d.dataTarget);
    }
    if (g_hwnd && g_oldWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldWndProc));
}

} // namespace kcp
