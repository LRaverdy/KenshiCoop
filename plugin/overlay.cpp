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
#include <cstdio>
#include <string>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "kc/admin.h"
#include "map_view.h"
#include "overlay_map.h"
#include "util.h"

#define GET_X_LPARAM_(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM_(lp) ((int)(short)HIWORD(lp))

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
std::mutex g_sceneMutex;
MapScene g_scene;                               // the map layer (map_view.h), from the game thread
std::atomic<float> g_screenW{0}, g_screenH{0};

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

std::atomic<bool> g_mpOpen{false}, g_consoleOpen{false}, g_diploOpen{false};
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
bool Interactive() { return g_mpOpen.load() || g_consoleOpen.load() || g_dialogOpen.load() || g_diploOpen.load(); }

void ReleaseRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// The cursor of a mouse message, in back buffer pixels.
bool CursorOf(HWND hwnd, UINT msg, LPARAM lp, float& x, float& y) {
    POINT pt{GET_X_LPARAM_(lp), GET_Y_LPARAM_(lp)};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) ScreenToClient(hwnd, &pt);
    RECT rc;
    if (!GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    const float bw = g_screenW.load(), bh = g_screenH.load();
    x = float(pt.x) * (bw > 0 ? bw / float(rc.right) : 1.0f);
    y = float(pt.y) * (bh > 0 ? bh / float(rc.bottom) : 1.0f);
    return true;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // the map layer: pings, the minimap's wheel and buttons (not while one of our windows has the mouse)
    if (g_ready && !(Interactive() && g_wantMouse.load()) && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        float x = 0, y = 0;
        if (CursorOf(hwnd, msg, lp, x, y) && MapOverlayMessage(msg, wp, x, y)) return 0;
    }
    if (g_ready && Interactive()) {
        std::lock_guard<std::recursive_mutex> lk(g_imguiMutex);
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        const bool mouseMsg = (msg >= WM_LBUTTONDOWN && msg <= WM_MOUSEHWHEEL) && msg != WM_MOUSEMOVE;
        const bool keyMsg = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
        if ((mouseMsg && g_wantMouse.load()) || (keyMsg && g_wantKeyboard.load())) return 0;
    }
    return CallWindowProcW(g_oldWndProc, hwnd, msg, wp, lp);
}

// The window's client area and DPI (the mouse arrives in client pixels; we draw in back buffer pixels).
int WindowDpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return fn && hwnd ? int(fn(hwnd)) : 96;
}
std::atomic<int> g_clientW{0}, g_clientH{0}, g_dpi{96};
// Logs the sizes the overlay converts between, when they change (at most every second).
void NoteDisplaySizes(float bw, float bh) {
    RECT rc{};
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc)) return;
    const int cw = rc.right, ch = rc.bottom, dpi = WindowDpi(g_hwnd);
    g_clientW = cw; g_clientH = ch; g_dpi = dpi;
    static std::string last;
    static double at = -1e9;
    char k[96];
    snprintf(k, sizeof(k), "%.0fx%.0f %dx%d %d", double(bw), double(bh), cw, ch, dpi);
    if (last == k || NowSeconds() - at < 1.0) return;
    last = k;
    at = NowSeconds();
    Log("overlay: back buffer %.0fx%.0f (ImGui display), window client %dx%d, dpi %d, mouse scale %.3f,%.3f", double(bw), double(bh), cw, ch, dpi,
        cw > 0 ? double(bw) / cw : 1.0, ch > 0 ? double(bh) / ch : 1.0);
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
    else if (m.dialogWaiting) ImGui::TextDisabled("(l'hôte fait avancer la conversation...)");
    ImGui::Separator();
    if (ImGui::SmallButton("Partir##kcleave")) {   // our character walks away: the host ends the conversation
        OverlayAction a;
        a.kind = OverlayAction::Kind::DialogAnswer;
        a.index = -1;   // kc::kDialogLeave
        PushAction(std::move(a));
    }
    ImGui::End();
}

// ---- the host's Administration section (render thread state)
int g_xpSkill = -1;           // -1: every skill
float g_xpAmount = 100.0f;
int g_xpLevels = 0;           // 0: experience points, 1: levels
int g_moneyAmount = 1000;
std::string g_armedKey;       // a risky button clicked once: the second click within 4 s confirms
double g_armedAt = -1e9;

void Admin(const std::string& args) { PushAction({OverlayAction::Kind::Command, {}, {}, "admin " + args, 0}); }

// A small button; risky: the first click turns it into "Confirmer ?", the second one acts.
bool ConfirmButton(const char* label, const std::string& key, bool risky, const char* tip) {
    const bool armed = g_armedKey == key && ImGui::GetTime() - g_armedAt < 4.0;
    const std::string text = std::string(armed ? "Confirmer ?" : label) + "###" + key;
    if (armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.30f, 0.20f, 1.0f));
    const bool clicked = ImGui::SmallButton(text.c_str());
    if (armed) ImGui::PopStyleColor();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", armed ? "Clique encore pour confirmer." : tip);
    if (!clicked) return false;
    if (!risky || armed) { g_armedKey.clear(); return true; }
    g_armedKey = key;
    g_armedAt = ImGui::GetTime();
    return false;
}

std::string XpArgs() {
    char amount[32];
    snprintf(amount, sizeof(amount), "%g", double(g_xpAmount));
    return std::string(g_xpSkill < 0 ? "all" : kc::admin::kSkills[g_xpSkill].key) + " " + amount + (g_xpLevels ? " levels" : "");
}

bool XpRisky() { return g_xpLevels ? g_xpAmount > kc::admin::kConfirmLevels : g_xpAmount > kc::admin::kConfirmXp; }

// "TP à..." : near another player, or on the host's marked point. who: "<id>", "host" or "all".
void TpPopup(const OverlayModel& m, const std::string& who, int selfId, bool risky) {
    if (!ImGui::BeginPopup("tpto")) return;
    if (risky) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Des persos sont à terre : ils seront relevés pour le TP.");
    for (const auto& p : m.players) {
        if (int(p.id) == selfId || (who == "all" && p.you)) continue;
        const std::string label = "près de " + p.name;
        if (ConfirmButton(label.c_str(), who + "-to-" + std::to_string(p.id), risky, nullptr)) {
            Admin("tp " + who + " " + std::to_string(p.id));
            ImGui::CloseCurrentPopup();
        }
    }
    if (!m.movePoint.empty()) {
        const std::string label = "au point marqué (" + m.movePoint + ")";
        if (ConfirmButton(label.c_str(), who + "-to-point", risky, "Le dernier endroit où tu as ordonné un déplacement (clic droit au sol).")) {
            Admin("tp " + who + " point");
            ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::TextDisabled("Point marqué : fais un clic droit au sol (ordre de déplacement).");
    }
    ImGui::EndPopup();
}

// ---- the item spawner (render thread state; the list comes from the game thread)
std::mutex g_itemsMutex;
bool g_itemsFresh = false;
std::vector<OverlayItem> g_itemsIn;
std::vector<OverlayMaker> g_makersIn;
std::vector<OverlayItem> g_items;     // render thread's copy
std::vector<OverlayMaker> g_makers;
char g_spawnSearch[64] = "";
int g_spawnCat = -1;                  // -1: every category
int g_spawnSel = -1;                  // index into g_items
int g_spawnCount = 20;
int g_spawnTarget = -1;               // -1: near the host, else a player id
int g_spawnMaker = 0, g_spawnModel = 0;

void TakeItems() {
    std::lock_guard<std::mutex> lk(g_itemsMutex);
    if (!g_itemsFresh) return;
    g_itemsFresh = false;
    std::string sel = g_spawnSel >= 0 && g_spawnSel < int(g_items.size()) ? g_items[g_spawnSel].sid : std::string();
    g_items.swap(g_itemsIn);
    g_makers.swap(g_makersIn);
    g_itemsIn.clear();
    g_makersIn.clear();
    g_spawnSel = -1;
    for (size_t i = 0; i < g_items.size(); ++i)
        if (g_items[i].sid == sel) g_spawnSel = int(i);
}

bool IsWeaponCat(int cat) { return cat == int(kc::admin::ItemCat::Weapon) || cat == int(kc::admin::ItemCat::Crossbow); }

void SelectItem(int i) {
    if (i == g_spawnSel) return;
    g_spawnSel = i;
    g_spawnMaker = 0;
    g_spawnModel = 0;
}

// The manufacturers that make this weapon.
std::vector<const OverlayMaker*> MakersOf(const std::string& sid) {
    std::vector<const OverlayMaker*> out;
    for (const auto& m : g_makers)
        if (std::find(m.weapons.begin(), m.weapons.end(), sid) != m.weapons.end()) out.push_back(&m);
    return out;
}

void DrawSpawner(const OverlayModel& m) {
    TakeItems();
    if (!ImGui::CollapsingHeader("Faire apparaître des objets")) return;
    if (g_items.empty()) {
        ImGui::TextDisabled("Liste des objets pas encore prête (une partie doit être chargée).");
        return;
    }
    namespace adm = kc::admin;
    // favourites
    ImGui::TextUnformatted("Favoris :");
    for (size_t f = 0; f < adm::kFavouriteCount; ++f) {
        int idx = -1;
        for (size_t i = 0; i < g_items.size() && idx < 0; ++i)
            if (g_items[i].sid == adm::kFavourites[f].sid) idx = int(i);
        if (idx < 0) continue;
        ImGui::SameLine();
        ImGui::PushID(int(f));
        if (ImGui::SmallButton(adm::kFavourites[f].fr)) {
            SelectItem(idx);
            g_spawnCount = adm::kFavourites[f].count;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", g_items[idx].name.c_str(), g_items[idx].sid.c_str());
        ImGui::PopID();
    }
    // search and category
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##spawnsearch", "Rechercher (nom anglais ou sid)", g_spawnSearch, sizeof(g_spawnSearch));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::BeginCombo("##spawncat", g_spawnCat < 0 ? "Toutes les catégories" : adm::CategoryFr(adm::ItemCat(g_spawnCat)))) {
        if (ImGui::Selectable("Toutes les catégories", g_spawnCat < 0)) g_spawnCat = -1;
        for (int c = 0; c < adm::kItemCatCount; ++c)
            if (ImGui::Selectable(adm::CategoryFr(adm::ItemCat(c)), g_spawnCat == c)) g_spawnCat = c;
        ImGui::EndCombo();
    }
    std::vector<int> shown;
    for (size_t i = 0; i < g_items.size(); ++i) {
        const auto& it = g_items[i];
        if (g_spawnCat >= 0 && it.category != g_spawnCat) continue;
        if (g_spawnSearch[0] && !adm::SearchMatches(it.name, it.sid, g_spawnSearch)) continue;
        shown.push_back(int(i));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu objet(s)", shown.size());
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("spawnlist", 3, tf, ImVec2(0.0f, 170.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Objet");
        ImGui::TableSetupColumn("Catégorie", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Pile", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(shown.size()));
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const int i = shown[size_t(r)];
                const auto& it = g_items[size_t(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(it.name.c_str(), g_spawnSel == i, ImGuiSelectableFlags_SpanAllColumns)) SelectItem(i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.sid.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(adm::CategoryFr(adm::ItemCat(it.category)));
                ImGui::TableNextColumn();
                ImGui::Text("%d", it.stack);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    if (g_spawnSel < 0 || g_spawnSel >= int(g_items.size())) {
        ImGui::TextDisabled("Choisis un objet dans la liste ou un favori.");
        return;
    }
    const OverlayItem& it = g_items[size_t(g_spawnSel)];
    ImGui::Text("%s", it.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s, piles de %d au plus)", adm::CategoryFr(adm::ItemCat(it.category)), it.stack);
    // quantity
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("quantité##spawncount", &g_spawnCount, 1, 10);
    g_spawnCount = std::clamp(g_spawnCount, 1, adm::kMaxSpawn);
    // weapons: who made it, which model
    std::string gear;
    if (IsWeaponCat(it.category)) {
        const auto makers = MakersOf(it.sid);
        if (makers.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(fabricant par défaut)");
        } else {
            g_spawnMaker = std::clamp(g_spawnMaker, 0, int(makers.size()) - 1);
            const OverlayMaker& mk = *makers[size_t(g_spawnMaker)];
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo("##spawnmaker", mk.name.c_str())) {
                for (int k = 0; k < int(makers.size()); ++k)
                    if (ImGui::Selectable(makers[size_t(k)]->name.c_str(), k == g_spawnMaker)) { g_spawnMaker = k; g_spawnModel = 0; }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fabricant");
            g_spawnModel = std::clamp(g_spawnModel, 0, int(mk.models.size()) - 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo("##spawnmodel", mk.models[size_t(g_spawnModel)].second.c_str())) {
                for (int k = 0; k < int(mk.models.size()); ++k)
                    if (ImGui::Selectable(mk.models[size_t(k)].second.c_str(), k == g_spawnModel)) g_spawnModel = k;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Modèle (qualité), le meilleur en premier");
            gear = " " + mk.sid + " " + mk.models[size_t(g_spawnModel)].first;
        }
    }
    // where
    std::string targetName = "près de moi";
    bool targetKnown = g_spawnTarget < 0;
    for (const auto& p : m.players)
        if (!p.you && int(p.id) == g_spawnTarget) { targetName = "près de " + p.name; targetKnown = true; }
    if (!targetKnown) { g_spawnTarget = -1; targetName = "près de moi"; }
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##spawntarget", targetName.c_str())) {
        if (ImGui::Selectable("près de moi", g_spawnTarget < 0)) g_spawnTarget = -1;
        for (const auto& p : m.players) {
            if (p.you) continue;
            const std::string label = "près de " + p.name + "##" + std::to_string(p.id);
            if (ImGui::Selectable(label.c_str(), g_spawnTarget == int(p.id))) g_spawnTarget = int(p.id);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const std::string where = g_spawnTarget < 0 ? "here" : std::to_string(g_spawnTarget);
    const std::string label = "Faire apparaître " + std::to_string(g_spawnCount);
    const std::string tip = "Au sol, autour du perso " + std::string(g_spawnTarget < 0 ? "sélectionné" : "du joueur") +
                            ", en piles de " + std::to_string(it.stack) + " au plus. Tout le monde les voit.";
    if (ConfirmButton(label.c_str(), "spawn", g_spawnCount > adm::kConfirmSpawn, tip.c_str()))
        Admin("spawn " + it.sid + " " + std::to_string(g_spawnCount) + " " + where + gear);
    if (g_spawnCount > adm::kConfirmSpawn) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "plus de %d : deux clics", adm::kConfirmSpawn);
    }
}

// The host's player list with an admin row per player and one for everyone (host included).
void DrawAdmin(const OverlayModel& m) {
    size_t othersDown = 0, hostDown = 0, allDown = 0;
    for (const auto& p : m.players) {
        allDown += p.down;
        (p.you ? hostDown : othersDown) += p.down;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("admin", 5, flags)) {
        ImGui::TableSetupColumn("Joueur");
        ImGui::TableSetupColumn("Ping");
        ImGui::TableSetupColumn("Persos");
        ImGui::TableSetupColumn("Dieu");
        ImGui::TableSetupColumn("Actions");
        ImGui::TableHeadersRow();
        for (const auto& p : m.players) {
            const std::string id = std::to_string(p.id);
            ImGui::PushID(int(p.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u. %s%s", unsigned(p.id), p.name.c_str(), p.you ? " (toi)" : "");
            if (p.down) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%zu à terre", p.down);
            }
            ImGui::TableNextColumn();
            if (p.you) ImGui::TextUnformatted("hôte");
            else ImGui::Text("%u ms", p.pingMs);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", p.characters);
            ImGui::TableNextColumn();
            bool god = p.god;
            if (ImGui::Checkbox("##god", &god)) Admin("god " + id + (god ? " on" : " off"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mode dieu : plus aucun dégât ni K.-O. Reste actif aux changements de zone et reconnexions.");
            ImGui::TableNextColumn();
            if (!p.you) {
                if (ConfirmButton("TP moi", "tpme", p.down > 0, "Ses personnages viennent près de ton perso sélectionné."))
                    Admin("tp " + id + " host");
                ImGui::SameLine();
                if (ConfirmButton("Aller", "goto", hostDown > 0, "Tes personnages vont près des siens.")) Admin("tp host " + id);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("TP à...")) ImGui::OpenPopup("tpto");
            TpPopup(m, p.you ? "host" : id, p.id, p.down > 0);
            ImGui::SameLine();
            if (ImGui::SmallButton("Soigner")) Admin("heal " + id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Soigne toutes les blessures et réveille un perso K.-O.");
            ImGui::SameLine();
            if (ConfirmButton("XP", "xp", XpRisky(), "Donne l'expérience réglée plus bas (compétence, quantité).")) Admin("xp " + id + " " + XpArgs());
            if (!p.you) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Resync")) PushAction({OverlayAction::Kind::Command, {}, {}, "resync " + id, 0});
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Il recharge ton monde tel qu'il est maintenant (en cas de désynchro).");
            }
            ImGui::PopID();
        }
        // everyone, the host included
        ImGui::PushID("all");
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Tout le monde");
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        bool godAll = m.godAll;
        if (ImGui::Checkbox("##god", &godAll)) Admin(std::string("god all ") + (godAll ? "on" : "off"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mode dieu pour tous les persos de l'escouade, nouveaux venus compris.");
        ImGui::TableNextColumn();
        if (m.players.size() > 1) {
            if (ConfirmButton("TP moi", "tpme", othersDown > 0, "Les personnages de tous les joueurs viennent près de toi.")) Admin("tp all host");
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("TP à...")) ImGui::OpenPopup("tpto");
        TpPopup(m, "all", -1, allDown > 0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Soigner")) Admin("heal all");
        ImGui::SameLine();
        if (ConfirmButton("XP", "xp", XpRisky(), "Donne l'expérience réglée plus bas à tous les persos de l'escouade.")) Admin("xp all " + XpArgs());
        ImGui::PopID();
        ImGui::EndTable();
    }
    // the experience to give, and the shared money
    ImGui::SetNextItemWidth(170.0f);
    const char* preview = g_xpSkill < 0 ? "Toutes les compétences" : kc::admin::kSkills[g_xpSkill].fr;
    if (ImGui::BeginCombo("##skill", preview)) {
        if (ImGui::Selectable("Toutes les compétences", g_xpSkill < 0)) g_xpSkill = -1;
        for (int i = 0; i < int(kc::kStatCount); ++i)
            if (ImGui::Selectable(kc::admin::kSkills[i].fr, g_xpSkill == i)) g_xpSkill = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputFloat("##xpamount", &g_xpAmount, 0.0f, 0.0f, "%g");
    g_xpAmount = std::clamp(g_xpAmount, 0.0f, float(g_xpLevels ? kc::admin::kMaxLevels : kc::admin::kMaxXp));
    ImGui::SameLine();
    ImGui::RadioButton("points d'XP", &g_xpLevels, 0);
    ImGui::SameLine();
    ImGui::RadioButton("niveaux", &g_xpLevels, 1);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("##money", &g_moneyAmount, 0, 0);
    g_moneyAmount = std::clamp(g_moneyAmount, -2000000000, 2000000000);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_moneyAmount == 0);
    if (ImGui::SmallButton("cats à l'argent commun")) Admin("money " + std::to_string(g_moneyAmount));
    ImGui::EndDisabled();
    if (m.movePoint.empty()) ImGui::TextDisabled("Point marqué : aucun (clic droit au sol pour en marquer un).");
    else ImGui::TextDisabled("Point marqué : %s (dernier clic droit au sol).", m.movePoint.c_str());
    DrawSpawner(m);
}

// The host's diplomacy: the player faction's relations, the squad's bounties, the world's changes.
void DrawDiplomacy(const OverlayModel& m, float w, float h) {
    bool open = true;
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.15f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, w - 32.0f), std::min(620.0f, h - 64.0f)), ImGuiCond_Appearing);
    if (!ImGui::Begin("Diplomatie (Ctrl+Shift+F)", &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!open) g_diploOpen = false;
        return;
    }
    if (!m.diploHave) {
        ImGui::TextDisabled("Pas encore de valeurs de l'hôte (rejoins ou héberge une partie).");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", m.diploHeader.c_str());
        if (ImGui::CollapsingHeader("Relations de ta faction", ImGuiTreeNodeFlags_DefaultOpen) &&
            ImGui::BeginTable("kcrel", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0.0f, 260.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Faction");
            ImGui::TableSetupColumn("Relation", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            for (const auto& r : m.diploRelations) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", r.relation);
                ImGui::TableSetColumnIndex(2);
                if (r.war) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "en guerre");
                else if (r.standing == 2) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "ennemi");
                else if (r.standing == 1) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "allié");
                else ImGui::TextUnformatted("neutre");
            }
            ImGui::EndTable();
        }
        if (ImGui::CollapsingHeader("Primes de l'escouade", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (m.diploBounties.empty()) ImGui::TextDisabled("Personne n'est recherché.");
            for (const auto& l : m.diploBounties) ImGui::BulletText("%s", l.c_str());
        }
        if (ImGui::CollapsingHeader("Monde (chefs, guerres, villes)")) {
            if (m.diploWorld.empty()) ImGui::TextDisabled("Rien n'a changé.");
            for (const auto& l : m.diploWorld) ImGui::BulletText("%s", l.c_str());
        }
    }
    ImGui::End();
    if (!open) g_diploOpen = false;
}

// "Affichage": what the map layer shows. Each change is kept in KenshiCoop.ini ([ui]).
void DrawDisplaySettings(const OverlayModel& m) {
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Affichage (carte, minicarte, repères)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto option = [](const char* key, float v) {
        OverlayAction a;
        a.kind = OverlayAction::Kind::SetOption;
        a.text = key;
        a.value = v;
        PushAction(std::move(a));
    };
    auto box = [&](const char* label, const char* key, bool cur, const char* tip) {
        bool v = cur;
        if (ImGui::Checkbox(label, &v)) option(key, v ? 1.0f : 0.0f);
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    box("Joueurs et ennemis sur la carte (bouton CARTE)", "map_markers", m.optMap, "Tous les persos des joueurs, à leur couleur, et en rouge les escouades hostiles qui nous visent.");
    box("Curseur de couleur au-dessus des joueurs", "head_markers", m.optHeads, "Un petit curseur à la couleur du joueur, avec son nom.");
    box("Contour de couleur des cartes des joueurs (barre d'escouade)", "portrait_colours", m.optPortraits, "Le cadre du portrait des persos des joueurs ; les recrues gardent le cadre normal.");
    box("Minicarte (Ctrl+Shift+N)", "minimap", m.optMinimap, nullptr);
    box("Minicarte tournante (suit la caméra)", "minimap_rotate", m.optMinimapRotate, "Sinon le nord reste en haut.");
    static const char* corners[] = {"en haut à gauche", "en haut à droite", "en bas à gauche", "en bas à droite"};
    int corner = std::clamp(m.optMinimapCorner, 0, 3);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("Coin de la minicarte", &corner, corners, 4)) option("minimap_corner", float(corner));
    box("Pings", "pings", m.optPings, "Clic molette ou Alt+clic sur la carte, la minicarte ou le sol.\nMaj : danger, Ctrl : butin, Maj+Ctrl : à l'aide.");
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
    for (const auto& q : m.queueLines) ImGui::TextUnformatted(q.c_str());
    if (m.download >= 0 && m.queueLines.empty()) ImGui::ProgressBar(m.download, ImVec2(-1.0f, 0.0f));
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
        if (m.hosting) {
            DrawAdmin(m);
        } else if (ImGui::BeginTable("players", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Joueur");
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Persos", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();
            for (const auto& p : m.players) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u. %s%s", unsigned(p.id), p.name.c_str(), p.you ? " (toi)" : "");
                ImGui::TableNextColumn();
                ImGui::Text("%u ms", p.pingMs);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", p.characters);
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
    DrawDisplaySettings(m);
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
    MapScene scene;
    {
        std::lock_guard<std::mutex> lk(g_sceneMutex);
        scene = g_scene;
    }
    const bool interactive = Interactive();
    if (!m.visible && m.toasts.empty() && !interactive && !scene.live) {
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
    g_screenW = io.DisplaySize.x;
    g_screenH = io.DisplaySize.y;
    NoteDisplaySizes(io.DisplaySize.x, io.DisplaySize.y);
    ImGui::NewFrame();
    MapOverlayDraw(scene, io.DisplaySize.x, io.DisplaySize.y, g_device);
    DrawStatus(m, io.DisplaySize.x, io.DisplaySize.y);
    if (g_mpOpen) DrawMultiplayer(m, io.DisplaySize.x, io.DisplaySize.y);
    if (g_consoleOpen) DrawConsole(m, io.DisplaySize.x, io.DisplaySize.y);
    if (g_diploOpen) DrawDiplomacy(m, io.DisplaySize.x, io.DisplaySize.y);
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
    } else if (type == DI8DEVTYPE_MOUSE && (g_wantMouse || MapOverlayHoldsMouse()) && size >= sizeof(DIMOUSESTATE)) {
        auto* s = static_cast<DIMOUSESTATE*>(data);
        s->lZ = 0;
        std::memset(static_cast<BYTE*>(data) + offsetof(DIMOUSESTATE, rgbButtons), 0, size - offsetof(DIMOUSESTATE, rgbButtons));
    }
}

void FilterData(void* dev, DIDEVICEOBJECTDATA* data, DWORD* count) {
    if (!data || !count || *count == 0) return;
    const BYTE type = DeviceType(dev);
    const bool keys = type == DI8DEVTYPE_KEYBOARD && g_wantKeyboard;
    const bool mouse = type == DI8DEVTYPE_MOUSE && (g_wantMouse || MapOverlayHoldsMouse());
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

void OverlayToggleDiplomacy() { g_diploOpen = !g_diploOpen.load(); }
bool OverlayDiplomacyOpen() { return g_diploOpen.load(); }

void OverlayOpenMultiplayer() {
    if (g_mpOpen.load()) return;
    g_mpOpened = true;
    g_mpOpen = true;
}

bool OverlayTyping() { return g_wantKeyboard.load(); }

void OverlayPublishItems(std::vector<OverlayItem> items, std::vector<OverlayMaker> makers) {
    std::lock_guard<std::mutex> lk(g_itemsMutex);
    g_itemsIn = std::move(items);
    g_makersIn = std::move(makers);
    g_itemsFresh = true;
}

void OverlayPublishScene(MapScene scene) {
    std::lock_guard<std::mutex> lk(g_sceneMutex);
    g_scene = std::move(scene);
}

void OverlayScreenSize(float& w, float& h) {
    w = g_screenW.load();
    h = g_screenH.load();
}

void OverlayWindowInfo(int& clientW, int& clientH, int& dpi) {
    clientW = g_clientW.load();
    clientH = g_clientH.load();
    dpi = g_dpi.load();
}

void OverlayPushAction(OverlayAction a) { PushAction(std::move(a)); }

void OverlayShutdown() {
    g_mpOpen = false;
    g_consoleOpen = false;
    g_diploOpen = false;
    g_wantKeyboard = false;
    g_wantMouse = false;
    if (g_presentTarget) MH_DisableHook(g_presentTarget);
    if (g_resizeTarget) MH_DisableHook(g_resizeTarget);
    for (auto& d : g_di) {
        if (d.stateTarget && d.o_state) MH_DisableHook(d.stateTarget);
        if (d.dataTarget && d.o_data) MH_DisableHook(d.dataTarget);
    }
    MapOverlayRelease();
    if (g_hwnd && g_oldWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldWndProc));
}

} // namespace kcp
