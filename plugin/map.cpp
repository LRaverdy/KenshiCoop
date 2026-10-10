// Map markers, minimap, markers above the heads, squad bar frames and pings: the game side.
//
// Everything a player sees comes from the session's map feed (the host's view of every squad
// character and of the hostile squads, see common/src/session_map.cpp), placed with the game's
// own geometry, read here every frame on the game thread:
//  - the map screen (ManagementScreen::mapScreen): its map image's rectangle on screen (it pans and
//    zooms with the map) and the world rectangle the image covers (the projection of
//    MapScreen::worldToMapCoords);
//  - the 3D camera (PlayerInterface::camera -> Ogre::Camera): projection * view;
//  - the squad bar's portraits (PortraitMainCellView, followed through their update/destructor).
// Every read is guarded (SEH, vtable checks): a map opened while loading, a zone change or speed 3
// only ever give an empty scene for a frame.
#include "map.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "kc/colors.h"
#include "kenshi.h"
#include "overlay.h"
#include "overlay_map.h"

namespace kcp {

const char* PingKindName(int kind) {
    switch (kind) {
    case 0: return "Aller ici";
    case 1: return "Danger / ennemis";
    case 2: return "Butin";
    case 3: return "A l'aide";
    }
    return "Ping";
}

const char* ThreatKindName(int kind) {
    switch (kind) {
    case 1: return "hostiles proches";
    case 2: return "attaquent";
    case 3: return "raid en route";
    }
    return "?";
}

namespace mapmarks {
PortraitUpdateFn o_portraitUpdate = nullptr;
PortraitDtorFn o_portraitDtor = nullptr;
} // namespace mapmarks

namespace {

// ---- offsets (docs/MOTEUR.md, "Carte")
constexpr uintptr_t kMgmtMainWidget = 0x8;      // ManagementScreen: MyGUI::Window* ("<prefix>_Root" of Kenshi_OverviewWindow.layout)
constexpr uintptr_t kMgmtMapScreen = 0xA8;      // ManagementScreen: MapScreen*
constexpr uintptr_t kMapTab = 0x10;             // MapScreen: MyGUI::TabItem* ("<prefix>_MapTab")
constexpr uintptr_t kMapScrollView = 0x18;      // MapScreen: MyGUI::ScrollView* ("<prefix>_MapScrollView", the visible part)
constexpr uintptr_t kMapImage = 0x20;           // MapScreen: MyGUI::ImageBox* ("<prefix>_MapImage", GUI_Map.dds, sized by the zoom)
constexpr uintptr_t kMapWorldBounds = 0x194;    // MapScreen: float minX, minZ (Vector4 worldBounds .x .y)
constexpr uintptr_t kMapWorldSize = 0x1A4;      // MapScreen: float sizeX, sizeZ
constexpr uintptr_t kPiCamera = 0x30;           // PlayerInterface: CameraClass*
constexpr uintptr_t kCamInitialised = 0x20;     // CameraClass: bool
constexpr uintptr_t kCamOgre = 0x68;            // CameraClass: Ogre::Camera*
constexpr uintptr_t kWidgetCropped = 0x8;       // MyGUI::Widget: its ICroppedRectangle base
constexpr uintptr_t kWidgetChildren = 0x3D8;    // MyGUI::Widget: vector<Widget*> mWidgetChild (begin, end)
constexpr uintptr_t kWidgetSkinChildren = 0x3F8;// MyGUI::Widget: vector<Widget*> mWidgetChildSkin ("Client"...)
constexpr uintptr_t kGuiRootWidgets = 0x28;     // MyGUI::Gui: vector<Widget*> of the root widgets (Gui::getEnumerator)
constexpr size_t kRenderMgrViewSize = 6;        // MyGUI::RenderManager vtable slot: const IntSize& getViewSize() const
constexpr uintptr_t kCellMainWidget = 0x8;      // PortraitMainCellView (wraps::BaseCellView::mMainWidget)
constexpr uintptr_t kCellHandle = 0xA8;         // PortraitMainCellView: hand characterHandle
constexpr uintptr_t kActivePlatoonMe = 0x78;    // ActivePlatoon: Platoon*
constexpr uintptr_t kFactionWarMgr = 0x88;      // Faction: FactionWarMgr*
constexpr uintptr_t kCampaignData = 0x10;       // CampaignInstance: CampaignData*
constexpr uintptr_t kCampaignEnemy = 0x78;      // CampaignInstance: Faction* enemy (whom it attacks)
constexpr uintptr_t kCampaignDataHostile = 0x12;// CampaignData: bool _isHostile
constexpr uintptr_t kRoOwner = 0x10;            // RootObjectBase: Faction*

template <typename T>
bool Rd(const void* p, uintptr_t off, T& out) { return p && kenshi::ReadRaw(p, off, &out, sizeof(T)); }

// Rate-limited state logs: a line when the state changes, at most one per `gap` seconds (a state
// that changed too soon is logged once the gap has passed, if it still holds).
struct StateLog {
    std::string key;
    double at = -1e9;
    bool Due(const std::string& k, double gap) {
        if (k == key || NowSeconds() - at < gap) return false;
        key = k;
        at = NowSeconds();
        return true;
    }
};

// ---- MyGUI and Ogre, through their DLL exports
struct Coord { int left, top, width, height; };
using WidgetBoolFn = bool (*)(const void* widget);
using WidgetPtrFn = void* (*)(const void* widget);
using AbsCoordFn = Coord* (*)(const void* cropped, Coord* out);
using MatrixFn = const float* (*)(const void* frustum);
using FromPointFn = void* (*)(void* layerManager, int x, int y);
using MouseMoveFn = bool (*)(void* input, int x, int y, int z);
using MouseButtonFn = bool (*)(void* input, int x, int y, int button);
using MousePosFn = const int* (*)(const void* input);
using TabIndexFn = void (*)(void* tabControl, size_t index);
struct Exports {
    bool tried = false;
    WidgetBoolFn inheritedVisible = nullptr, visible = nullptr;
    WidgetPtrFn parent = nullptr, name = nullptr;
    AbsCoordFn absCoord = nullptr;
    void** renderMgr = nullptr;     // &Singleton<RenderManager>::msInstance
    void** layerMgr = nullptr;
    void** gui = nullptr;
    void** input = nullptr;
    FromPointFn widgetFromPoint = nullptr;
    MouseMoveFn mouseMove = nullptr;
    MouseButtonFn mousePress = nullptr, mouseRelease = nullptr;
    MousePosFn mousePos = nullptr;
    TabIndexFn tabSelect = nullptr;
    MatrixFn viewMatrix = nullptr, projMatrix = nullptr;
    WidgetPtrFn camSceneMgr = nullptr;                       // Ogre::Camera::getSceneManager
    float* (*relativeOrigin)(const void* sm, float* out) = nullptr;   // Ogre::SceneManager::getRelativeOrigin (Kenshi's)
};
Exports g_ex;
void ResolveExports() {
    if (g_ex.tried) return;
    g_ex.tried = true;
    if (HMODULE gui = GetModuleHandleW(L"MyGUIEngine_x64.dll")) {
        auto fn = [gui](const char* n) { return reinterpret_cast<void*>(GetProcAddress(gui, n)); };
        g_ex.inheritedVisible = reinterpret_cast<WidgetBoolFn>(fn("?getInheritedVisible@Widget@MyGUI@@QEBA_NXZ"));
        g_ex.visible = reinterpret_cast<WidgetBoolFn>(fn("?getVisible@Widget@MyGUI@@QEBA_NXZ"));
        g_ex.parent = reinterpret_cast<WidgetPtrFn>(fn("?getParent@Widget@MyGUI@@QEBAPEAV12@XZ"));
        g_ex.name = reinterpret_cast<WidgetPtrFn>(fn("?getName@Widget@MyGUI@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ"));
        g_ex.absCoord = reinterpret_cast<AbsCoordFn>(fn("?getAbsoluteCoord@ICroppedRectangle@MyGUI@@QEBA?AU?$TCoord@H@types@2@XZ"));
        g_ex.renderMgr = reinterpret_cast<void**>(fn("?msInstance@?$Singleton@VRenderManager@MyGUI@@@MyGUI@@0PEAVRenderManager@2@EA"));
        g_ex.layerMgr = reinterpret_cast<void**>(fn("?msInstance@?$Singleton@VLayerManager@MyGUI@@@MyGUI@@0PEAVLayerManager@2@EA"));
        g_ex.gui = reinterpret_cast<void**>(fn("?msInstance@?$Singleton@VGui@MyGUI@@@MyGUI@@0PEAVGui@2@EA"));
        g_ex.input = reinterpret_cast<void**>(fn("?msInstance@?$Singleton@VInputManager@MyGUI@@@MyGUI@@0PEAVInputManager@2@EA"));
        g_ex.widgetFromPoint = reinterpret_cast<FromPointFn>(fn("?getWidgetFromPoint@LayerManager@MyGUI@@QEAAPEAVWidget@2@HH@Z"));
        g_ex.mouseMove = reinterpret_cast<MouseMoveFn>(fn("?injectMouseMove@InputManager@MyGUI@@QEAA_NHHH@Z"));
        g_ex.mousePress = reinterpret_cast<MouseButtonFn>(fn("?injectMousePress@InputManager@MyGUI@@QEAA_NHHUMouseButton@2@@Z"));
        g_ex.mouseRelease = reinterpret_cast<MouseButtonFn>(fn("?injectMouseRelease@InputManager@MyGUI@@QEAA_NHHUMouseButton@2@@Z"));
        g_ex.mousePos = reinterpret_cast<MousePosFn>(fn("?getMousePosition@InputManager@MyGUI@@QEBAAEBU?$TPoint@H@types@2@XZ"));
        g_ex.tabSelect = reinterpret_cast<TabIndexFn>(fn("?setIndexSelected@TabControl@MyGUI@@QEAAX_K@Z"));
    }
    if (HMODULE ogre = GetModuleHandleW(L"OgreMain_x64.dll")) {
        g_ex.viewMatrix = reinterpret_cast<MatrixFn>(GetProcAddress(ogre, "?getViewMatrix@Camera@Ogre@@UEBAAEBVMatrix4@2@XZ"));
        g_ex.projMatrix = reinterpret_cast<MatrixFn>(GetProcAddress(ogre, "?getProjectionMatrix@Frustum@Ogre@@UEBAAEBVMatrix4@2@XZ"));
        g_ex.camSceneMgr = reinterpret_cast<WidgetPtrFn>(GetProcAddress(ogre, "?getSceneManager@Camera@Ogre@@QEBAPEAVSceneManager@2@XZ"));
        g_ex.relativeOrigin = reinterpret_cast<float* (*)(const void*, float*)>(
            GetProcAddress(ogre, "?getRelativeOrigin@SceneManager@Ogre@@QEBA?AVVector3@2@XZ"));
    }
    Log("map: exports MyGUI visible=%d/%d coord=%d parent=%d name=%d view=%d layers=%d gui=%d input=%d, Ogre view=%d proj=%d origin=%d",
        g_ex.visible != nullptr, g_ex.inheritedVisible != nullptr, g_ex.absCoord != nullptr, g_ex.parent != nullptr, g_ex.name != nullptr,
        g_ex.renderMgr != nullptr, g_ex.layerMgr != nullptr && g_ex.widgetFromPoint != nullptr, g_ex.gui != nullptr,
        g_ex.input != nullptr && g_ex.mousePress != nullptr, g_ex.viewMatrix != nullptr, g_ex.projMatrix != nullptr,
        g_ex.camSceneMgr != nullptr && g_ex.relativeOrigin != nullptr);
}

bool CallBoolSEH(WidgetBoolFn f, const void* w, bool& out) {
    __try {
        out = f(w);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* CallPtrSEH(WidgetPtrFn f, const void* w) {
    __try {
        return f(w);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool WidgetCoordSEH(const void* w, Coord& out) {
    __try {
        g_ex.absCoord(static_cast<const uint8_t*>(w) + kWidgetCropped, &out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* FromPointSEH(void* lm, int x, int y, bool& ok) {
    ok = false;
    __try {
        void* w = g_ex.widgetFromPoint(lm, x, y);
        ok = true;
        return w;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
const int* ViewSizeSEH(void* rm) {
    __try {
        using Fn = const int* (*)(void*);
        void** vt = *static_cast<void***>(rm);
        return reinterpret_cast<Fn>(vt[kRenderMgrViewSize])(rm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Shown on screen: the widget itself visible and every parent too. MyGUI's getInheritedVisible is
// only the parents' part (disassembled: it returns mInheritsVisible, set from the parent's
// visibility; the widget's own mVisible is separate): a root window hidden with setVisible(false)
// still answers true there. That was why the management screen counted as always up.
bool WidgetShown(const void* w) {
    if (!w || !g_ex.visible || !g_ex.inheritedVisible) return false;
    bool own = false, parents = false;
    return CallBoolSEH(g_ex.visible, w, own) && own && CallBoolSEH(g_ex.inheritedVisible, w, parents) && parents;
}
void* WidgetParent(const void* w) { return w && g_ex.parent ? CallPtrSEH(g_ex.parent, w) : nullptr; }
// MyGUI's name (a std::string of the game's MSVC 2010 runtime: 16-byte buffer or pointer, size at
// +0x10, capacity at +0x18).
std::string WidgetName(const void* w) {
    if (!w || !g_ex.name) return {};
    const void* s = CallPtrSEH(g_ex.name, w);
    uint64_t size = 0, cap = 0;
    if (!s || !Rd(s, 0x10, size) || !Rd(s, 0x18, cap) || size > 256 || cap < size) return {};
    std::string out(size_t(size), '\0');
    if (cap < 16) {
        if (!kenshi::ReadRaw(s, 0, out.data(), size_t(size))) return {};
    } else {
        const void* p = nullptr;
        if (!Rd(s, 0, p) || !p || !kenshi::ReadRaw(p, 0, out.data(), size_t(size))) return {};
    }
    return out;
}
bool NameEndsWith(const std::string& n, const char* suffix) {
    const size_t k = std::strlen(suffix);
    return n.size() >= k && n.compare(n.size() - k, k, suffix) == 0 && (n.size() == k || n[n.size() - k - 1] == '_');
}
bool AbsCoord(const void* w, Coord& c) {
    return w && g_ex.absCoord && WidgetCoordSEH(w, c) && c.width >= 0 && c.height >= 0 && c.width < 100000 && c.height < 100000;
}
// A widget shown on screen, and where (its whole rectangle, MyGUI view pixels).
bool WidgetOnScreen(const void* w, Coord& c) { return WidgetShown(w) && AbsCoord(w, c) && c.width > 0 && c.height > 0; }
// The part of a shown widget its parents let through (scrolled lists, scroll views and windows crop
// their children): its rectangle cut by every parent's.
// Also cut by MyGUI's view (viewW x viewH, when known): Kenshi's bars keep their layout when the
// window shrinks, and a widget can lie partly or wholly below the screen.
bool VisiblePart(const void* w, Coord& vis, float viewW = 0, float viewH = 0) {
    Coord c{};
    if (!WidgetOnScreen(w, c)) return false;
    int x0 = c.left, y0 = c.top, x1 = c.left + c.width, y1 = c.top + c.height;
    if (viewW > 0 && viewH > 0) {
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, int(viewW)); y1 = std::min(y1, int(viewH));
    }
    const void* p = WidgetParent(w);
    for (int depth = 0; p && depth < 48; ++depth, p = WidgetParent(p)) {
        Coord pc{};
        if (!AbsCoord(p, pc)) break;
        x0 = std::max(x0, pc.left); y0 = std::max(y0, pc.top);
        x1 = std::min(x1, pc.left + pc.width); y1 = std::min(y1, pc.top + pc.height);
    }
    if (x1 <= x0 || y1 <= y0) return false;
    vis = {x0, y0, x1 - x0, y1 - y0};
    return true;
}
bool IsAncestorOrSelf(const void* anc, const void* w) {
    for (int depth = 0; w && depth < 64; ++depth, w = WidgetParent(w))
        if (w == anc) return true;
    return false;
}
// Something of the game's GUI (another window, a menu) on top of `w` at (x, y): MyGUI's own picking
// (LayerManager::getWidgetFromPoint, top layer first). Only widgets that take the mouse are seen.
bool CoveredAt(const void* w, int x, int y) {
    if (!g_ex.layerMgr || !g_ex.widgetFromPoint) return false;
    void* lm = nullptr;
    if (!Rd(g_ex.layerMgr, 0, lm) || !lm) return false;
    bool ok = false;
    void* top = FromPointSEH(lm, x, y, ok);
    if (!ok || !top) return false;
    return !IsAncestorOrSelf(w, top) && !IsAncestorOrSelf(top, w);
}
// MyGUI's view (the size its coordinates are in).
bool GuiViewSize(float& w, float& h) {
    void* rm = nullptr;
    if (!g_ex.renderMgr || !Rd(g_ex.renderMgr, 0, rm) || !rm) return false;
    const int* sz = ViewSizeSEH(rm);
    int v[2] = {};
    if (!sz || !kenshi::ReadRaw(sz, 0, v, sizeof(v)) || v[0] < 16 || v[1] < 16 || v[0] > 65536 || v[1] > 65536) return false;
    w = float(v[0]);
    h = float(v[1]);
    return true;
}
// A widget under `root` whose name ends with "_<suffix>" (or is `suffix`), depth first.
void* FindWidget(const void* root, const char* suffix, int depth = 0, int maxDepth = 24) {
    if (!root || depth > maxDepth) return nullptr;
    if (NameEndsWith(WidgetName(root), suffix)) return const_cast<void*>(root);
    for (uintptr_t vec : {kWidgetChildren, kWidgetSkinChildren}) {
        void** b = nullptr;
        void** e = nullptr;
        if (!Rd(root, vec, b) || !Rd(root, vec + 8, e) || !b || e < b || e - b > 4096) continue;
        for (void** it = b; it < e; ++it) {
            void* c = nullptr;
            if (!Rd(it, 0, c) || !c) continue;
            if (void* f = FindWidget(c, suffix, depth + 1, maxDepth)) return f;
        }
    }
    return nullptr;
}
// Same, from every root widget of the GUI.
void* FindWidgetAnywhere(const char* suffix) {
    void* gui = nullptr;
    void** b = nullptr;
    void** e = nullptr;
    if (!g_ex.gui || !Rd(g_ex.gui, 0, gui) || !gui || !Rd(gui, kGuiRootWidgets, b) || !Rd(gui, kGuiRootWidgets + 8, e) || !b || e < b || e - b > 4096)
        return nullptr;
    for (void** it = b; it < e; ++it) {
        void* c = nullptr;
        if (Rd(it, 0, c) && c)
            if (void* f = FindWidget(c, suffix)) return f;
    }
    return nullptr;
}
bool RelativeOriginSEH(const void* cam, float out[3]) {
    if (!g_ex.camSceneMgr || !g_ex.relativeOrigin) return false;
    __try {
        const void* sm = g_ex.camSceneMgr(cam);
        if (!sm) return false;
        float tmp[4] = {};
        g_ex.relativeOrigin(sm, tmp);
        out[0] = tmp[0]; out[1] = tmp[1]; out[2] = tmp[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool MatricesSEH(const void* cam, float view[16], float proj[16]) {
    __try {
        const float* v = g_ex.viewMatrix(cam);
        const float* p = g_ex.projMatrix(cam);
        if (!v || !p) return false;
        std::memcpy(view, v, 64);
        std::memcpy(proj, p, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- game functions (kFunctions)
using WorldToMapFn = int* (*)(void* mapScreen, int* out, const float* pos);
using MarkerColourFn = const void* (*)(void* who);
using CampaignFn = void* (*)(void* warMgr, void* platoon);
bool WorldToMapSEH(void* ms, const float* pos, int out[2]) {
    __try {
        reinterpret_cast<WorldToMapFn>(kenshi::FnAddr(kenshi::FnMapWorldToCoords))(ms, out, pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
const void* MarkerColourSEH(void* who) {
    __try {
        return reinterpret_cast<MarkerColourFn>(kenshi::FnAddr(kenshi::FnMapMarkerColor))(who);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
void* CampaignSEH(void* warMgr, void* platoon) {
    __try {
        return reinterpret_cast<CampaignFn>(kenshi::FnAddr(kenshi::FnWarCurrentCampaign))(warMgr, platoon);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* ManagementScreen() {
    void* m = nullptr;
    if (!Rd(reinterpret_cast<void*>(kenshi::Addr(kenshi::rva::ManagementScreenSingleton)), 0, m) || !m) return nullptr;
    uintptr_t vt = 0;
    return Rd(m, 0, vt) && vt == kenshi::Addr(kenshi::rva::VtManagementScreen) ? m : nullptr;
}
void* MapScreen() {
    void* ms = nullptr;
    void* m = ManagementScreen();
    return m && Rd(m, kMgmtMapScreen, ms) ? ms : nullptr;
}

// ---- the squad bar's portraits (main thread: GUI updates and our tick)
std::mutex g_cellsMutex;
std::unordered_set<void*> g_cells;

// The map screen's widgets: MapScreen's fields, checked by their layout names (they are
// "<prefix>_MapImage"... in Kenshi_OverviewWindow.layout, the prefix being the screen's address);
// if a field does not hold the named widget, the widget is looked up by name in the window.
struct MapWidgets {
    void* mgmt = nullptr;
    void* window = nullptr;   // "_Root"
    void* tab = nullptr;      // "_MapTab"
    void* scroll = nullptr;   // "_MapScrollView"
    void* image = nullptr;    // "_MapImage"
    bool byName = false;      // found by name, not through MapScreen's fields
};
MapWidgets g_mw;
bool MapWidgetsFor(void* mgmt, void* ms, MapWidgets& out) {
    if (g_mw.mgmt == mgmt && g_mw.image) { out = g_mw; return true; }
    MapWidgets w;
    w.mgmt = mgmt;
    Rd(mgmt, kMgmtMainWidget, w.window);
    Rd(ms, kMapTab, w.tab);
    Rd(ms, kMapScrollView, w.scroll);
    Rd(ms, kMapImage, w.image);
    const std::string nImage = WidgetName(w.image), nScroll = WidgetName(w.scroll), nTab = WidgetName(w.tab);
    if (!NameEndsWith(nImage, "MapImage") || !NameEndsWith(nScroll, "MapScrollView") || !NameEndsWith(nTab, "MapTab")) {
        Log("map: MapScreen fields hold '%s' / '%s' / '%s', looking the map's widgets up by name", nImage.c_str(), nScroll.c_str(), nTab.c_str());
        w.image = FindWidget(w.window, "MapImage");
        w.scroll = FindWidget(w.window, "MapScrollView");
        w.tab = FindWidget(w.window, "MapTab");
        w.byName = true;
    }
    if (!w.image) return false;
    Log("map: map screen widgets: window '%s', tab '%s', scroll view '%s', image '%s'%s", WidgetName(w.window).c_str(), WidgetName(w.tab).c_str(),
        WidgetName(w.scroll).c_str(), WidgetName(w.image).c_str(), w.byName ? " (by name)" : "");
    g_mw = w;
    out = w;
    return true;
}

// The GUI part of the scene: covered, the map image and the squad bar frames, in MyGUI view pixels.
// Read on the game thread by the tick, and again by the overlay in its own frame when it draws on
// that same thread (MapRefreshGui), so what is drawn is what the game's GUI shows in that frame.
struct GuiStats {
    int cells = 0, hidden = 0, clipped = 0, covered = 0;
};
void ReadGui(MapScene& s, GuiStats* st) {
    s.inDisplay = false;
    if (!GuiViewSize(s.guiW, s.guiH)) s.guiW = s.guiH = 0;
    // the management screen (map, squads, research...) or the character editor covers the 3D view
    void* mgmt = ManagementScreen();
    void* ms = MapScreen();
    MapWidgets mw;
    const bool widgets = mgmt && ms && MapWidgetsFor(mgmt, ms, mw);
    const bool mgmtUp = widgets && WidgetShown(mw.window);
    s.covered = mgmtUp || kenshi::CharacterEditorOpen();
    s.mapOpen = false;
    if (!s.showMap) s.mapWhy = "markers off (setting)";
    else if (!mgmt) s.mapWhy = "no management screen yet";
    else if (!ms) s.mapWhy = "no map screen in the management screen";
    else if (!widgets) s.mapWhy = "map widgets not found";
    else if (!s.boundsOk) s.mapWhy = "map world bounds unreadable";
    else if (!mgmtUp) s.mapWhy = "management screen closed";
    else if (mw.tab && !WidgetShown(mw.tab)) s.mapWhy = "another tab of the management screen";
    else {
        Coord ic{}, vis{};
        if (!WidgetOnScreen(mw.image, ic)) s.mapWhy = "map image hidden";
        else if (!VisiblePart(mw.image, vis, s.guiW, s.guiH)) s.mapWhy = "map image cropped out";
        else {
            s.mapOpen = true;
            s.mapWhy.clear();
            s.imgX = float(ic.left); s.imgY = float(ic.top); s.imgW = float(ic.width); s.imgH = float(ic.height);
            s.clipX0 = float(vis.left); s.clipY0 = float(vis.top);
            s.clipX1 = float(vis.left + vis.width); s.clipY1 = float(vis.top + vis.height);
        }
    }
    // squad bar frames: the visible part of each cell's widget, unless a game window is on top
    s.portraits.clear();
    for (const auto& [cell, owner] : s.portraitCells) {
        {
            std::lock_guard<std::mutex> lk(g_cellsMutex);
            if (!g_cells.count(cell)) continue;   // destroyed since
        }
        uintptr_t vt = 0;
        void* main = nullptr;
        if (!Rd(cell, 0, vt) || vt != kenshi::Addr(kenshi::rva::VtPortraitCell) || !Rd(cell, kCellMainWidget, main) || !main) continue;
        if (st) ++st->cells;
        Coord vis{};
        if (!WidgetShown(main)) { if (st) ++st->hidden; continue; }
        if (!VisiblePart(main, vis, s.guiW, s.guiH)) { if (st) ++st->clipped; continue; }
        if (CoveredAt(main, vis.left + vis.width / 2, vis.top + vis.height / 2)) { if (st) ++st->covered; continue; }
        s.portraits.push_back({float(vis.left), float(vis.top), float(vis.width), float(vis.height), owner});
    }
}

DWORD g_sceneThread = 0;   // the thread the game's GUI is read on (the tick's)


MapScene g_scene;

float Dist2D(const kc::Vec3& a, const kc::Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

} // namespace

namespace mapmarks {
unsigned long long g_portraitsRefused = 0;
// The squad bar draws a portrait (the game's GUI, on the game thread). Only the cell is remembered:
// its PortraitData is never read here (the 20-minute soak's client crash was a corrupt PortraitData*
// in this very call, 0x415150 -> 0x412D96), and only a cell of the right class, which the destructor
// hook below forgets (its deleting destructor, vtable slot 0, goes through 0x426450).
// The cell's PortraitData is first checked to be one the game has (its pointer looked up among the
// PortraitManager's, never read): a cell whose data is not is left as it is for this draw instead of
// letting the game read a hand from freed memory (the 20:20 soak crash went through here).
void hk_portraitUpdate(void* cell, const void* info, void* data) {
    if (!kenshi::IsGamePortrait(data)) {
        static unsigned long long nextLog = 0;
        ++g_portraitsRefused;
        if (GetTickCount64() >= nextLog) {
            nextLog = GetTickCount64() + 2000;
            Log("squad bar: a portrait cell's data %p is not one of the game's portraits: not drawn (%llu so far)", data, g_portraitsRefused);
        }
        return;
    }
    o_portraitUpdate(cell, info, data);
    uintptr_t vt = 0;
    if (!cell || !Rd(cell, 0, vt) || vt != kenshi::Addr(kenshi::rva::VtPortraitCell)) return;
    std::lock_guard<std::mutex> lk(g_cellsMutex);
    if (g_cells.size() < 512) g_cells.insert(cell);
}
void hk_portraitDtor(void* cell) {
    {
        std::lock_guard<std::mutex> lk(g_cellsMutex);
        g_cells.erase(cell);
    }
    o_portraitDtor(cell);
}
} // namespace mapmarks

// ---- host: hostile squads near the players
void KenshiWorld::ReadMapThreats(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::MapThreat>& out) {
    out.clear();
    std::vector<kenshi::Character*> squad, all;
    kenshi::PlayerCharacters(squad);
    if (squad.empty()) return;
    std::unordered_set<kenshi::Character*> mine(squad.begin(), squad.end());
    std::unordered_set<kc::Handle, kc::HandleHash> mineHandles;
    for (auto* c : squad) if (kc::Handle h; kenshi::GetHandle(c, h)) mineHandles.insert(h);
    void* playerFaction = nullptr;
    Rd(squad.front(), kRoOwner, playerFaction);
    const void* enemyColour = reinterpret_cast<const void*>(kenshi::Addr(kenshi::rva::MarkerColourEnemy));
    kenshi::ActiveCharacters(all);
    struct Group { kc::Vec3 sum; int n = 0; int kind = 0; void* faction = nullptr; float nearest = 1e30f; };
    std::unordered_map<void*, Group> groups;
    std::unordered_map<void*, bool> raidSquads;   // per squad: its campaign attacks the player faction
    for (kenshi::Character* c : all) {
        if (mine.count(c) || kenshi::IsDead(c)) continue;
        kc::Vec3 p;
        if (!kenshi::GetPosition(c, p)) continue;
        float nearest = 1e30f;
        for (const auto& ctr : centers) nearest = std::min(nearest, Dist2D(p, ctr));
        void* sq = kenshi::SquadOf(c);
        int kind = 0;
        kc::Handle target;
        if (kenshi::ReadCombat(c, target) && mineHandles.count(target)) kind = int(kc::ThreatKind::Attacking);
        if (!kind && sq && playerFaction) {
            auto it = raidSquads.find(sq);
            if (it == raidSquads.end()) {
                bool raid = false;
                void *platoon = nullptr, *faction = nullptr, *war = nullptr;
                if (Rd(sq, kActivePlatoonMe, platoon) && platoon && Rd(c, kRoOwner, faction) && faction && Rd(faction, kFactionWarMgr, war) && war) {
                    if (void* camp = CampaignSEH(war, platoon)) {
                        void *enemy = nullptr, *data = nullptr;
                        bool hostile = false;
                        raid = Rd(camp, kCampaignEnemy, enemy) && enemy == playerFaction && Rd(camp, kCampaignData, data) && data &&
                               Rd(data, kCampaignDataHostile, hostile) && hostile;
                    }
                }
                it = raidSquads.emplace(sq, raid).first;
            }
            if (it->second) kind = int(kc::ThreatKind::Raid);
        }
        if (!kind && nearest <= radius && MarkerColourSEH(c) == enemyColour) kind = int(kc::ThreatKind::Near);
        if (!kind) continue;
        Group& g = groups[sq ? sq : static_cast<void*>(c)];
        g.sum.x += p.x; g.sum.y += p.y; g.sum.z += p.z;
        ++g.n;
        g.kind = std::max(g.kind, kind);
        g.nearest = std::min(g.nearest, nearest);
        if (!g.faction) Rd(c, kRoOwner, g.faction);
    }
    std::vector<std::pair<float, kc::MapThreat>> sorted;
    for (auto& [key, g] : groups) {
        kc::MapThreat t;
        t.pos = {g.sum.x / g.n, g.sum.y / g.n, g.sum.z / g.n};
        t.count = uint8_t(std::min(g.n, 255));
        t.kind = kc::ThreatKind(g.kind);
        const std::string sid = kenshi::FactionSidOf(g.faction);
        if (!kenshi::TemplateDisplayName(sid, t.label)) t.label = sid;
        if (t.label.size() > kc::kMaxMapLabelLen) t.label.resize(kc::kMaxMapLabelLen);
        // raids and fights first, then the closest
        sorted.push_back({float(3 - g.kind) * 1e7f + g.nearest, std::move(t)});
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [k, t] : sorted) {
        if (out.size() >= kc::kMaxMapThreats) break;
        out.push_back(std::move(t));
    }
}

// ---- what the GUI part of the scene shows, in the log: on change, rate-limited
namespace {
StateLog g_logMap, g_logMapRect, g_logFrames, g_logGuiSize;
void LogGuiState(const MapScene& s, const GuiStats& st) {
    char b[320];
    if (g_logGuiSize.Due(std::to_string(int(s.guiW)) + "x" + std::to_string(int(s.guiH)), 1.0))
        Log("map: MyGUI view %.0fx%.0f%s", double(s.guiW), double(s.guiH), s.guiW > 0 ? "" : " (unreadable: taken as the back buffer)");
    if (s.mapOpen) {
        if (g_logMap.Due("open", 0.5)) Log("map screen: open (map tab shown)");
        snprintf(b, sizeof(b), "map screen: image at (%.0f,%.0f) %.0fx%.0f, visible part (%.0f,%.0f)-(%.0f,%.0f), MyGUI view %.0fx%.0f, %zu chars, %zu hostile squads",
                 double(s.imgX), double(s.imgY), double(s.imgW), double(s.imgH), double(s.clipX0), double(s.clipY0), double(s.clipX1), double(s.clipY1),
                 double(s.guiW), double(s.guiH), s.chars.size(), s.threats.size());
        char k[96];
        snprintf(k, sizeof(k), "%d,%d,%d,%d,%d,%d", int(s.imgX) / 8, int(s.imgY) / 8, int(s.imgW) / 8, int(s.clipX0), int(s.clipY0), int(s.clipX1));
        if (g_logMapRect.Due(k, 2.0)) Log("%s", b);
    } else {
        if (g_logMap.Due("closed:" + s.mapWhy, 0.5)) Log("map screen: no markers drawn (%s)", s.mapWhy.c_str());
        g_logMapRect.key.clear();
    }
    if (s.showPortraits) {
        snprintf(b, sizeof(b), "%zu/%d/%d/%d/%d", s.portraits.size(), st.cells, st.hidden, st.clipped, st.covered);
        if (g_logFrames.Due(b, 2.0))
            Log("squad bar: %zu frame(s) (player portraits %d: %d hidden, %d cropped out, %d under a game window)", s.portraits.size(), st.cells, st.hidden,
                st.clipped, st.covered);
    }
}
} // namespace

// ---- the scene, every frame
void UpdateMapScene(kc::Session& session, KenshiWorld& world, const Config& cfg, bool live) {
    ResolveExports();
    MapScene s;
    s.showMap = cfg.mapMarkers;
    s.showHeads = cfg.headMarkers;
    s.showPortraits = cfg.portraitColours;
    s.showMinimap = cfg.minimap;
    s.minimapRotate = cfg.minimapRotate;
    s.minimapCorner = cfg.minimapCorner;
    s.minimapZoom = cfg.minimapZoom;
    s.showPings = cfg.pings;
    const bool inSession = session.isHost() || session.state() == kc::SessionState::Connected;
    s.me = inSession ? session.localId() : 1;
    s.live = live && world.Ready() && kenshi::World() && kenshi::Player();
    if (!s.live) {
        s.mapWhy = "no world running";
        g_scene = s;
        OverlayPublishScene(s);
        return;
    }
    g_sceneThread = GetCurrentThreadId();
    s.builtAt = NowSeconds();
    // the world rectangle the map image covers (valid from the start, map never opened)
    if (void* ms = MapScreen()) {
        float b[2] = {}, sz[2] = {};
        if (kenshi::ReadRaw(ms, kMapWorldBounds, b, sizeof(b)) && kenshi::ReadRaw(ms, kMapWorldSize, sz, sizeof(sz)) && std::isfinite(b[0]) &&
            std::isfinite(b[1]) && sz[0] > 1.0f && sz[1] > 1.0f && sz[0] < 1e8f && sz[1] < 1e8f) {
            s.boundsOk = true;
            s.minX = b[0]; s.minZ = b[1]; s.sizeX = sz[0]; s.sizeZ = sz[1];
        }
    }
    // the 3D camera
    void* cc = nullptr;
    void* cam = nullptr;
    bool init = false;
    if (g_ex.viewMatrix && g_ex.projMatrix && Rd(kenshi::Player(), kPiCamera, cc) && cc && Rd(cc, kCamInitialised, init) && init &&
        Rd(cc, kCamOgre, cam) && cam) {
        float v[16], p[16];
        if (MatricesSEH(cam, v, p)) {
            // Kenshi's Ogre renders around a moving origin (SceneManager::getRelativeOrigin, a
            // Kenshi addition: CameraClass::getCameraPos adds it to the camera node's position).
            // The view matrix is in that render space: render = world - origin. Folded in here
            // (in double), so the projections take world positions.
            double o[3] = {0, 0, 0};
            float of[3] = {};
            if (RelativeOriginSEH(cam, of) && std::isfinite(of[0]) && std::isfinite(of[1]) && std::isfinite(of[2])) {
                o[0] = of[0]; o[1] = of[1]; o[2] = of[2];
                s.originOk = true;
                s.origin = {of[0], of[1], of[2]};
            }
            bool finite = true;
            for (int i = 0; i < 4; ++i) {
                double row[4];
                for (int j = 0; j < 4; ++j) {
                    double acc = 0;
                    for (int k = 0; k < 4; ++k) acc += double(p[i * 4 + k]) * double(v[k * 4 + j]);
                    row[j] = acc;
                }
                row[3] -= row[0] * o[0] + row[1] * o[1] + row[2] * o[2];
                for (int j = 0; j < 4; ++j) {
                    s.viewProj[i * 4 + j] = float(row[j]);
                    finite = finite && std::isfinite(s.viewProj[i * 4 + j]);
                }
            }
            s.camOk = finite;
            s.camFwdX = -v[8];
            s.camFwdZ = -v[10];
        }
    }
    // the players and their characters: the host's feed (in a session), else our squad
    const kc::MapMarkersMsg& feed = session.mapMarkers();
    std::map<uint8_t, std::string> names;
    if (inSession) {
        for (const auto& p : feed.players) names[p.id] = p.name;
        names[s.me] = cfg.name;
        std::unordered_map<kenshi::Character*, uint8_t> avatarOwner;
        for (const auto& c : feed.chars) {
            SceneChar sc;
            sc.netId = c.netId;
            sc.owner = c.owner;
            sc.name = c.name;
            sc.pos = c.pos;
            sc.avatar = (c.flags & kc::kMapAvatar) != 0;
            sc.down = (c.flags & kc::kMapDown) != 0;
            sc.dead = (c.flags & kc::kMapDead) != 0;
            kc::Handle h;
            kenshi::Character* local = session.netIdHandle(c.netId, h) ? world.Find(h) : nullptr;
            kc::Vec3 lp;
            if (local && kenshi::GetPosition(local, lp)) {
                sc.pos = lp;   // where it stands here, this frame (smooth)
                sc.head = sc.avatar && !sc.dead;
                if (sc.avatar) avatarOwner[local] = c.owner;
            }
            s.chars.push_back(std::move(sc));
        }
        for (const auto& t : feed.threats) s.threats.push_back({t.pos, int(t.count), int(t.kind), t.label});
        for (const auto& p : session.pings()) {
            ScenePing sp;
            sp.id = p.ping.id;
            sp.owner = p.ping.owner;
            sp.who = names.count(p.ping.owner) ? names[p.ping.owner] : "?";
            sp.kind = int(p.ping.kind);
            sp.pos = p.ping.pos;
            sp.age = float(session.pingAge(p));
            s.pings.push_back(std::move(sp));
        }
        // the squad bar: frames of the players' own characters
        if (s.showPortraits) {
            std::vector<void*> cells;
            {
                std::lock_guard<std::mutex> lk(g_cellsMutex);
                cells.assign(g_cells.begin(), g_cells.end());
            }
            for (void* cell : cells) {
                uintptr_t vt = 0;
                void* main = nullptr;
                kc::Handle h;
                if (!Rd(cell, 0, vt) || vt != kenshi::Addr(kenshi::rva::VtPortraitCell) || !Rd(cell, kCellMainWidget, main) || !main ||
                    !kenshi::HandleFromHand(static_cast<uint8_t*>(cell) + kCellHandle, h))
                    continue;
                kenshi::Character* c = kenshi::Resolve(h);
                auto it = c ? avatarOwner.find(c) : avatarOwner.end();
                if (it == avatarOwner.end()) continue;
                s.portraitCells.push_back({cell, it->second});   // its rectangle: ReadGui, below and in the overlay's frame
            }
        }
    } else {
        names[s.me] = cfg.name;
        std::vector<kenshi::Character*> squad;
        kenshi::PlayerCharacters(squad);
        for (auto* c : squad) {
            SceneChar sc;
            sc.owner = s.me;
            kenshi::CharacterName(c, sc.name);
            if (!kenshi::GetPosition(c, sc.pos)) continue;
            sc.dead = kenshi::IsDead(c);
            s.chars.push_back(std::move(sc));
        }
    }
    for (const auto& [id, n] : names) s.players.push_back({id, n});
    // the game's GUI: covered or not, the map screen, the squad bar frames (MyGUI view pixels)
    GuiStats st;
    ReadGui(s, &st);
    LogGuiState(s, st);
    // the minimap's centre: the selected character, else our own
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    for (const auto& h : sel)
        if (kenshi::Character* c = world.FindSquad(h); c && kenshi::GetPosition(c, s.centre)) { s.centreOk = true; break; }
    for (int pass = 0; pass < 2 && !s.centreOk; ++pass)
        for (const auto& c : s.chars)
            if (c.owner == s.me && (pass == 1 || c.avatar) && !c.dead) { s.centre = c.pos; s.centreOk = true; break; }
    g_scene = s;
    OverlayPublishScene(std::move(s));
}

const MapScene& LastMapScene() { return g_scene; }

std::string CheckMapProjection(float x, float z) {
    void* ms = MapScreen();
    if (!ms) return "err no map screen";
    const float pos[3] = {x, 0, z};
    int pt[2] = {};
    if (!WorldToMapSEH(ms, pos, pt)) return "err call failed";
    // ours, inside the image (the game's point is relative to the map image too), with the image's
    // size read now (the map screen need not be open)
    float b[2] = {}, sz[2] = {};
    void* image = nullptr;
    Coord ic{};
    if (!kenshi::ReadRaw(ms, kMapWorldBounds, b, sizeof(b)) || !kenshi::ReadRaw(ms, kMapWorldSize, sz, sizeof(sz)) || sz[0] <= 0 || sz[1] <= 0 ||
        !Rd(ms, kMapImage, image) || !image || !g_ex.absCoord || !WidgetCoordSEH(image, ic))
        return "err map screen not set up";
    const float ox = (x - b[0]) / sz[0] * float(ic.width), oy = (z - b[1]) / sz[1] * float(ic.height);
    char out[200];
    snprintf(out, sizeof(out), "ok game=%d,%d ours=%.1f,%.1f image=%dx%d open=%d", pt[0], pt[1], double(ox), double(oy), ic.width, ic.height,
             g_scene.mapOpen ? 1 : 0);
    return out;
}

std::string DescribeMapScene(const std::string& what) {
    const MapScene& s = g_scene;
    float w = 0, h = 0;
    OverlayScreenSize(w, h);
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    auto clean = [](std::string n) {
        for (char& c : n) if (c == ' ' || c == '=' || c == ';' || (unsigned char)c < 32) c = '_';
        return n.empty() ? std::string("-") : n;
    };
    auto colour = [](uint8_t id) { return std::string(kc::PlayerColor(id).name); };
    o << "ok live=" << s.live << " me=" << int(s.me);
    if (what == "carte") {
        o << " open=" << s.mapOpen << " bounds=" << s.boundsOk << " enabled=" << s.showMap << " chars=" << s.chars.size() << " threats=" << s.threats.size()
          << " why=" << clean(s.mapWhy) << " ;";
        for (const auto& c : s.chars) {
            float sx = 0, sy = 0;
            const bool on = WorldToMapScreen(s, c.pos, sx, sy);
            o << ' ' << clean(c.name) << ":owner=" << int(c.owner) << ":col=" << colour(c.owner) << ":av=" << c.avatar << ":x=" << c.pos.x << ":z=" << c.pos.z;
            if (on && s.mapOpen) o << ":sx=" << sx << ":sy=" << sy << ":vis=" << InMapClip(s, sx, sy);
        }
        o << " ;";
        for (const auto& t : s.threats) o << " threat:kind=" << t.kind << ":n=" << t.count << ":x=" << t.pos.x << ":z=" << t.pos.z << ":" << clean(t.label);
    } else if (what == "minicarte") {
        o << " enabled=" << s.showMinimap << " covered=" << s.covered << " centre=" << s.centreOk << ":" << s.centre.x << "," << s.centre.z
          << " zoom=" << s.minimapZoom << " rotate=" << s.minimapRotate << " corner=" << s.minimapCorner << " ;";
        // dots inside the circle: same rule as the overlay (distance <= zoom)
        for (const auto& c : s.chars)
            if (s.centreOk && Dist2D(c.pos, s.centre) <= s.minimapZoom) o << ' ' << clean(c.name) << ":owner=" << int(c.owner) << ":col=" << colour(c.owner);
        for (const auto& t : s.threats)
            if (s.centreOk && Dist2D(t.pos, s.centre) <= s.minimapZoom) o << " threat:kind=" << t.kind;
    } else if (what == "tetes") {
        o << " enabled=" << s.showHeads << " cam=" << s.camOk << " covered=" << s.covered << " screen=" << w << "x" << h << " origin=" << s.originOk << ":"
          << s.origin.x << "," << s.origin.y << "," << s.origin.z << " ;";
        for (const auto& c : s.chars) {
            if (!c.head) continue;
            float sx = 0, sy = 0;
            const bool proj = WorldToScreen(s, {c.pos.x, c.pos.y + 22.0f, c.pos.z}, w, h, sx, sy);
            const bool on = proj && sx >= 0 && sy >= 0 && sx <= w && sy <= h;
            o << ' ' << clean(c.name) << ":owner=" << int(c.owner) << ":col=" << colour(c.owner) << ":onscreen=" << on;
            if (proj) o << ":sx=" << sx << ":sy=" << sy;   // off screen too: where it projects
        }
    } else if (what == "barre") {
        o << " enabled=" << s.showPortraits << " frames=" << s.portraits.size();
        {
            std::lock_guard<std::mutex> lk(g_cellsMutex);
            o << " cells=" << g_cells.size();
        }
        o << " ;";
        for (const auto& r : s.portraits) o << " frame:owner=" << int(r.owner) << ":col=" << colour(r.owner) << ":x=" << r.x << ":y=" << r.y << ":w=" << r.w;
    } else if (what == "pings") {
        o << " enabled=" << s.showPings << " n=" << s.pings.size() << " ;";
        for (const auto& p : s.pings)
            o << " id=" << p.id << ":owner=" << int(p.owner) << ":who=" << clean(p.who) << ":kind=" << p.kind << ":x=" << p.pos.x << ":z=" << p.pos.z
              << ":age=" << p.age;
    } else {
        return "err carte|minicarte|tetes|barre|pings";
    }
    return o.str();
}

// ---- the overlay's own frame: the GUI rectangles read again where they are drawn
bool MapRefreshGui(MapScene& s) {
    if (!s.live || !g_sceneThread || GetCurrentThreadId() != g_sceneThread) return false;   // only on the game's GUI thread
    ReadGui(s, nullptr);
    return true;
}

// ---- tests: the conversions, fresh from the game's GUI, and what the overlay last drew
std::string DescribeMapConversion() {
    ResolveExports();
    MapScene s = g_scene;
    if (!s.live) return "err no world running";
    ReadGui(s, nullptr);   // now, on the game thread: the portrait widgets as they are
    float bw = 0, bh = 0;
    OverlayScreenSize(bw, bh);
    int cw = 0, ch = 0, dpi = 0;
    OverlayWindowInfo(cw, ch, dpi);
    const GuiToDisplay g = MakeGuiToDisplay(s.guiW, s.guiH, bw, bh);
    const MapDrawn d = MapOverlayLastDrawn();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "ok bb=" << int(bw) << "x" << int(bh) << " client=" << cw << "x" << ch << " dpi=" << dpi << " gui=" << int(s.guiW) << "x" << int(s.guiH)
      << " fx=" << g.fx << " fy=" << g.fy << " known=" << g.known << " covered=" << s.covered << " drawnAge=" << std::min(99.0, NowSeconds() - d.at)
      << " drawnBb=" << int(d.w) << "x" << int(d.h) << " refreshed=" << d.refreshed << " heads=" << d.heads << " minimap=" << d.minimap
      << " mapOpen=" << s.mapOpen << " mapDrawn=" << d.mapOpen << " markers=" << d.mapMarkers << " ;";
    o.precision(1);
    for (const auto& r : s.portraits) {
        const SceneRect c = GuiRectToDisplay(r, g);
        o << " frame:owner=" << int(r.owner) << ":raw=" << r.x << "," << r.y << "," << r.w << "," << r.h << ":conv=" << c.x << "," << c.y << "," << c.w << ","
          << c.h;
    }
    o << " ;";
    for (const auto& r : d.frames) o << " drawn:owner=" << int(r.owner) << ":x=" << r.x << ":y=" << r.y << ":w=" << r.w << ":h=" << r.h;
    o << " ;";
    if (s.mapOpen) {
        o << " map:raw=" << s.imgX << "," << s.imgY << "," << s.imgW << "," << s.imgH << ":conv=" << s.imgX * g.fx << "," << s.imgY * g.fy << ","
          << s.imgW * g.fx << "," << s.imgH * g.fy;
        if (d.mapOpen) o << ":drawn=" << d.imgX << "," << d.imgY << "," << d.imgW << "," << d.imgH;
    } else {
        o << " map:why=" << [](std::string n) { for (char& c : n) if (c == ' ') c = '_'; return n; }(s.mapWhy);
    }
    return o.str();
}

// ---- tests: the game's own way to the map, its "MAP" shortcut button (ShortcutMapButton of
// Kenshi_MainPanel.layout, next to the squad bar), clicked through MyGUI's input; the game's key
// for it is a setting of the player (not assumed here).
namespace {
bool InjectClickSEH(void* input, int x, int y) {
    __try {
        const int* cur = g_ex.mousePos ? g_ex.mousePos(input) : nullptr;
        const int ox = cur ? cur[0] : x, oy = cur ? cur[1] : y;
        g_ex.mouseMove(input, x, y, 0);
        g_ex.mousePress(input, x, y, 0);     // MyGUI::MouseButton::Left
        g_ex.mouseRelease(input, x, y, 0);
        g_ex.mouseMove(input, ox, oy, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool TabSelectSEH(void* tabs, size_t i) {
    __try {
        g_ex.tabSelect(tabs, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

std::string MapUi(const std::string& what) {
    ResolveExports();
    MapScene s = g_scene;
    if (!s.live) return "err no world running";
    ReadGui(s, nullptr);
    void* mgmt = ManagementScreen();
    void* window = nullptr;
    const bool up = mgmt && Rd(mgmt, kMgmtMainWidget, window) && WidgetShown(window);
    std::string why = s.mapWhy;
    for (char& c : why) if (c == ' ') c = '_';
    const std::string state = "open=" + std::to_string(s.mapOpen) + " window=" + std::to_string(up) + " why=" + (why.empty() ? "-" : why);
    if (what == "state") return "ok " + state;
    if (what != "open" && what != "close" && what != "maptab") return "err mapui open|close|maptab|state";
    if (what == "maptab") {   // the window is up on another tab: select the map's
        void* tabs = up ? FindWidget(window, "TabsMain") : nullptr;
        if (!tabs || !g_ex.tabSelect) return "err no tab control (" + state + ")";
        return TabSelectSEH(tabs, 0) ? "ok tab selected" : "err tab selection failed";
    }
    if ((what == "open" && s.mapOpen) || (what == "close" && !up)) return "ok already " + state;
    void* input = nullptr;
    if (!g_ex.input || !Rd(g_ex.input, 0, input) || !input || !g_ex.mouseMove || !g_ex.mousePress || !g_ex.mouseRelease) return "err no MyGUI input";
    // close: the window's own close button (Kenshi_WindowCX skin: header -> "Button", Event=close);
    // the MAP button is often under the window by then (a click there lands on the window)
    void* target = nullptr;
    if (what == "close") {
        void** b = nullptr;
        void** e = nullptr;
        if (Rd(window, kWidgetSkinChildren, b) && Rd(window, kWidgetSkinChildren + 8, e) && b && e >= b && e - b < 64)
            for (void** it = b; it < e && !target; ++it) {
                void* c = nullptr;
                if (Rd(it, 0, c) && c) target = FindWidget(c, "Button", 0, 1);   // the header's children only
            }
        if (!target) return "err close button not found (" + state + ")";
    } else {
        static void* button = nullptr;
        if (!button || !NameEndsWith(WidgetName(button), "ShortcutMapButton")) button = FindWidgetAnywhere("ShortcutMapButton");
        if (!button) return "err ShortcutMapButton not found";
        target = button;
    }
    Coord c{};
    if (!VisiblePart(target, c, s.guiW, s.guiH)) return "err the button is not on screen (" + WidgetName(target) + ")";
    const int x = c.left + c.width / 2, y = c.top + c.height / 2;
    if (CoveredAt(target, x, y)) return "err the button is under another window (" + WidgetName(target) + ")";
    if (!InjectClickSEH(input, x, y)) return "err click failed";
    Log("map: test clicked '%s' at (%d,%d) to %s the map", WidgetName(target).c_str(), x, y, what.c_str());
    return "ok clicked " + std::to_string(x) + "," + std::to_string(y);
}

} // namespace kcp
