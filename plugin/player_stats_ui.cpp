// The "JOUEURS" tab of the game's management window (0.3.5): every player's statistics, in the
// game's own interface.
//
// Built with the game's MyGUI (MyGUIEngine_x64.dll, MSVC 2010) through its exports, exactly as the
// game's layout loader builds Kenshi_OverviewWindow.layout:
//  - the window's TabControl ("TabsMain") is ManagementScreen +0x2D0 (the game's own code reads it
//    there: 0x49FA08 at its creation, 0x48BB90 getIndexSelected...);
//  - Widget::createWidgetRealT(tabs, "TabItem", "", ...) makes a tab: TabControl::onWidgetCreated
//    (MyGUI 0x1A6C50) takes any TabItem created under it as a new tab, placed like the others;
//  - Widget::setProperty(tab, "Caption", "JOUEURS") names it (TabItem::setCaption ->
//    TabControl::setItemName), like the layout's <Property key="Caption">;
//  - inside, an EditBox with the DIALOGUE tab's skin (Kenshi_WordWrap: parchment, word wrap, its
//    scroll bar), read-only, filled by setProperty("Caption", text) with MyGUI colour codes;
//  - TabControl::setButtonDefaultWidth(width / tabs), as the game does at creation (0x49FA3B), so
//    the eight buttons share the bar.
// The game's tab handler (0x49BF90) switches on the index for 0 to 6 only (jump table, `cmp rsi, 6;
// ja`): our tab, the 8th, only gets the window title the handler sets from the tab's name.
//
// Never a dangling pointer: our tab is looked for again in the live window every time
// (TabControl::findItemIndex on the TabControl read from the ManagementScreen singleton, checked by
// its vtable), our text box among the tab's children; a rebuilt window or a new world simply makes
// them again. Every call into the game is guarded (SEH).
#include "player_stats_ui.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "kenshi.h"
#include "util.h"

namespace kcp {

namespace {

constexpr uintptr_t kMgmtMainWidget = 0x8;     // ManagementScreen: MyGUI::Window* (the window's root)
constexpr uintptr_t kMgmtTabs = 0x2D0;         // ManagementScreen: MyGUI::TabControl* "TabsMain"
constexpr uintptr_t kWidgetWidth = 0x28;       // MyGUI::Widget: ICroppedRectangle (+0x8) mCoord.width (+0x20)
constexpr uintptr_t kWidgetChildren = 0x3D8;   // MyGUI::Widget: vector<Widget*> mWidgetChild (begin, end)
constexpr size_t kItemNone = ~size_t(0);       // MyGUI::ITEM_NONE
constexpr uint32_t kAlignStretch = 30;         // MyGUI::Align::Stretch (HStretch | VStretch)
constexpr const char* kTabName = "KenshiCoop_PlayersTab";
constexpr const char* kTextName = "KenshiCoop_PlayersText";

template <typename T>
bool Rd(const void* p, uintptr_t off, T& out) { return p && kenshi::ReadRaw(p, off, &out, sizeof(T)); }

// A std::string of MyGUI's runtime (MSVC 2010: buffer or pointer, size +0x10, capacity +0x18, 40
// bytes). The callees only read it (they copy it): a long one points at our own characters, which
// must outlive the call.
struct MyStr {
    alignas(8) uint8_t raw[0x28] = {};
    explicit MyStr(const std::string& s) {
        const uint64_t size = s.size();
        uint64_t cap = 15;
        if (s.size() < 16) {
            std::memcpy(raw, s.data(), s.size());
        } else {
            const char* p = s.c_str();
            std::memcpy(raw, &p, sizeof(p));
            cap = size;
        }
        std::memcpy(raw + 0x10, &size, 8);
        std::memcpy(raw + 0x18, &cap, 8);
    }
};

using CreateRealFn = void* (*)(void* self, const void* type, const void* skin, const float* coord, uint32_t align, const void* name);
using SetPropertyFn = void (*)(void* self, const void* key, const void* value);
using FindIndexFn = size_t (*)(void* tabs, void* item);
using ConstSizeFn = size_t (*)(const void* tabs);
using SetSizeFn = void (*)(void* self, size_t value);
using SetIntFn = void (*)(void* self, int value);
using GetSizeFn = size_t (*)(void* self);
using WidgetBoolFn = bool (*)(const void* widget);
struct Exports {
    bool tried = false;
    CreateRealFn createReal = nullptr;   // Widget::createWidgetRealT(type, skin, FloatCoord, Align, name)
    SetPropertyFn setProperty = nullptr; // Widget::setProperty(key, value)
    FindIndexFn findIndex = nullptr;     // TabControl::findItemIndex(TabItem*)
    ConstSizeFn itemCount = nullptr;     // TabControl::getItemCount
    ConstSizeFn indexSelected = nullptr; // TabControl::getIndexSelected
    SetSizeFn selectIndex = nullptr;     // TabControl::setIndexSelected
    SetIntFn defaultWidth = nullptr;     // TabControl::setButtonDefaultWidth
    GetSizeFn vscroll = nullptr;         // EditBox::getVScrollPosition
    SetSizeFn setVscroll = nullptr;      // EditBox::setVScrollPosition
    WidgetBoolFn visible = nullptr, inheritedVisible = nullptr;
    bool ok() const { return createReal && setProperty && findIndex && itemCount && indexSelected && selectIndex && defaultWidth && visible && inheritedVisible; }
};
Exports g_ex;
void ResolveExports() {
    if (g_ex.tried) return;
    g_ex.tried = true;
    HMODULE gui = GetModuleHandleW(L"MyGUIEngine_x64.dll");
    if (!gui) { Log("player stats tab: MyGUIEngine_x64.dll not loaded: no tab"); return; }
    auto fn = [gui](const char* n) { return reinterpret_cast<void*>(GetProcAddress(gui, n)); };
    g_ex.createReal = reinterpret_cast<CreateRealFn>(fn(
        "?createWidgetRealT@Widget@MyGUI@@QEAAPEAV12@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0AEBU?$TCoord@M@types@2@UAlign@2@0@Z"));
    g_ex.setProperty = reinterpret_cast<SetPropertyFn>(
        fn("?setProperty@Widget@MyGUI@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0@Z"));
    g_ex.findIndex = reinterpret_cast<FindIndexFn>(fn("?findItemIndex@TabControl@MyGUI@@QEAA_KPEAVTabItem@2@@Z"));
    g_ex.itemCount = reinterpret_cast<ConstSizeFn>(fn("?getItemCount@TabControl@MyGUI@@QEBA_KXZ"));
    g_ex.indexSelected = reinterpret_cast<ConstSizeFn>(fn("?getIndexSelected@TabControl@MyGUI@@QEBA_KXZ"));
    g_ex.selectIndex = reinterpret_cast<SetSizeFn>(fn("?setIndexSelected@TabControl@MyGUI@@QEAAX_K@Z"));
    g_ex.defaultWidth = reinterpret_cast<SetIntFn>(fn("?setButtonDefaultWidth@TabControl@MyGUI@@QEAAXH@Z"));
    g_ex.vscroll = reinterpret_cast<GetSizeFn>(fn("?getVScrollPosition@EditBox@MyGUI@@QEAA_KXZ"));
    g_ex.setVscroll = reinterpret_cast<SetSizeFn>(fn("?setVScrollPosition@EditBox@MyGUI@@QEAAX_K@Z"));
    g_ex.visible = reinterpret_cast<WidgetBoolFn>(fn("?getVisible@Widget@MyGUI@@QEBA_NXZ"));
    g_ex.inheritedVisible = reinterpret_cast<WidgetBoolFn>(fn("?getInheritedVisible@Widget@MyGUI@@QEBA_NXZ"));
    Log("player stats tab: MyGUI exports %s (create=%d property=%d tabs=%d/%d/%d/%d/%d scroll=%d/%d visible=%d/%d)", g_ex.ok() ? "found" : "MISSING",
        g_ex.createReal != nullptr, g_ex.setProperty != nullptr, g_ex.findIndex != nullptr, g_ex.itemCount != nullptr, g_ex.indexSelected != nullptr,
        g_ex.selectIndex != nullptr, g_ex.defaultWidth != nullptr, g_ex.vscroll != nullptr, g_ex.setVscroll != nullptr, g_ex.visible != nullptr,
        g_ex.inheritedVisible != nullptr);
}

// ---- guarded calls (no C++ objects with destructors in these frames)
void* CreateSEH(void* parent, const MyStr* type, const MyStr* skin, const float* coord, uint32_t align, const MyStr* name) {
    __try {
        return g_ex.createReal(parent, type->raw, skin->raw, coord, align, name->raw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool SetPropertySEH(void* w, const MyStr* key, const MyStr* value) {
    __try {
        g_ex.setProperty(w, key->raw, value->raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool FindIndexSEH(void* tabs, void* item, size_t& out) {
    __try {
        out = g_ex.findIndex(tabs, item);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ConstSizeSEH(ConstSizeFn f, const void* tabs, size_t& out) {
    __try {
        out = f(tabs);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SetSizeSEH(SetSizeFn f, void* w, size_t v) {
    __try {
        f(w, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SetIntSEH(SetIntFn f, void* w, int v) {
    __try {
        f(w, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool GetSizeSEH(GetSizeFn f, void* w, size_t& out) {
    __try {
        out = f(w);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool BoolSEH(WidgetBoolFn f, const void* w, bool& out) {
    __try {
        out = f(w);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetProperty(void* w, const std::string& key, const std::string& value) {
    const MyStr k(key), v(value);
    return SetPropertySEH(w, &k, &v);
}
void* Create(void* parent, const char* type, const char* skin, float x, float y, float wd, float ht, const char* name) {
    const std::string t(type), s(skin), n(name);
    const MyStr mt(t), ms(s), mn(n);
    const float coord[4] = {x, y, wd, ht};
    return CreateSEH(parent, &mt, &ms, coord, kAlignStretch, &mn);
}
// Shown on screen: its own visibility and its parents' (MyGUI's getInheritedVisible is the parents' part).
bool Shown(const void* w) {
    bool own = false, parents = false;
    return w && BoolSEH(g_ex.visible, w, own) && own && BoolSEH(g_ex.inheritedVisible, w, parents) && parents;
}
bool IsChild(const void* parent, const void* child) {
    void** b = nullptr;
    void** e = nullptr;
    if (!Rd(parent, kWidgetChildren, b) || !Rd(parent, kWidgetChildren + 8, e) || !b || e < b || e - b > 4096) return false;
    for (void** it = b; it < e; ++it) {
        void* c = nullptr;
        if (Rd(it, 0, c) && c == child) return true;
    }
    return false;
}

void* ManagementScreen() {
    void* m = nullptr;
    if (!Rd(reinterpret_cast<void*>(kenshi::Addr(kenshi::rva::ManagementScreenSingleton)), 0, m) || !m) return nullptr;
    uintptr_t vt = 0;
    return Rd(m, 0, vt) && vt == kenshi::Addr(kenshi::rva::VtManagementScreen) ? m : nullptr;
}

// ---- state (game thread only)
struct Ui {
    void* tabs = nullptr;        // the TabControl our tab was made in (compared, never used without the live one)
    void* tab = nullptr;         // our TabItem
    void* text = nullptr;        // our EditBox
    std::string shownText;       // what the text box was last given
    bool wasShown = false;
    double nextLook = 0;
    double failedAt = -1e9;      // making the tab failed then: not again for a while
    std::string statsWorld;      // host: the save name the statistics followed at the last look
    uint32_t refreshes = 0;
    bool enabled = false;        // a session was active once: the tab is wanted
};
Ui g_ui;

// Our tab and text box in the live window (null: not there).
struct Live {
    void* mgmt = nullptr;
    void* window = nullptr;
    void* tabs = nullptr;
    size_t index = kItemNone, count = 0, selected = kItemNone;
    bool tabOk = false, textOk = false, shown = false;
};
Live LookNow() {
    Live l;
    l.mgmt = ManagementScreen();
    if (!l.mgmt || !Rd(l.mgmt, kMgmtTabs, l.tabs) || !l.tabs) return l;
    Rd(l.mgmt, kMgmtMainWidget, l.window);
    if (!ConstSizeSEH(g_ex.itemCount, l.tabs, l.count) || l.count == 0 || l.count > 64) { l.tabs = nullptr; return l; }
    ConstSizeSEH(g_ex.indexSelected, l.tabs, l.selected);
    if (g_ui.tab && g_ui.tabs == l.tabs && FindIndexSEH(l.tabs, g_ui.tab, l.index) && l.index != kItemNone && l.index < l.count) {
        l.tabOk = true;
        l.textOk = g_ui.text && IsChild(g_ui.tab, g_ui.text);
        l.shown = l.window && Shown(l.window) && l.selected == l.index;
    }
    return l;
}

bool MakeText() {
    g_ui.text = Create(g_ui.tab, "EditBox", "Kenshi_WordWrap", 0.0f, 0.003f, 0.989f, 0.983f, kTextName);
    if (!g_ui.text) return false;
    // as the layout sets the DIALOGUE tab's text box
    for (const char* key : {"MultiLine", "Static", "ReadOnly", "WordWrap", "VisibleVScroll"}) SetProperty(g_ui.text, key, "true");
    SetProperty(g_ui.text, "VisibleHScroll", "false");
    SetProperty(g_ui.text, "MaxTextLength", "65536");   // MyGUI's default (2048) would cut a page of 5 players or more
    g_ui.shownText.clear();
    return true;
}

bool MakeTab(Live& l) {
    g_ui.tab = Create(l.tabs, "TabItem", "", 0.0f, 0.05f, 1.0f, 0.948f, kTabName);
    g_ui.tabs = l.tabs;
    g_ui.text = nullptr;
    size_t index = kItemNone;
    if (!g_ui.tab || !FindIndexSEH(l.tabs, g_ui.tab, index) || index == kItemNone) {
        Log("player stats tab: the game's window did not take a new tab (%s)", g_ui.tab ? "not in its list" : "not made");
        g_ui.tab = nullptr;
        return false;
    }
    SetProperty(g_ui.tab, "Caption", "JOUEURS");
    // the buttons share the bar, as the game shares them when it makes the window
    size_t count = 0;
    int width = 0;
    if (ConstSizeSEH(g_ex.itemCount, l.tabs, count) && count && Rd(l.tabs, kWidgetWidth, width) && width > 0 && width < 20000)
        SetIntSEH(g_ex.defaultWidth, l.tabs, width / int(count));
    const bool text = MakeText();
    Log("player stats tab: \"JOUEURS\" added to the management window (tab %zu of %zu, text box %s)", index + 1, count, text ? "made" : "NOT made");
    return true;
}

void SetText(const std::string& text) {
    size_t pos = 0;
    const bool havePos = g_ex.vscroll && GetSizeSEH(g_ex.vscroll, g_ui.text, pos);
    if (!SetProperty(g_ui.text, "Caption", text)) return;
    if (havePos && g_ex.setVscroll) SetSizeSEH(g_ex.setVscroll, g_ui.text, pos);   // where the reader was
    g_ui.shownText = text;
    ++g_ui.refreshes;
}

} // namespace

void PlayerStatsUiUpkeep(kc::Session& session, KenshiWorld& world, bool live) {
    const double now = NowSeconds();
    if (!live || now < g_ui.nextLook) return;
    g_ui.nextLook = now + 0.5;
    // host: the world saved under a new name: its statistics go to that name's file now
    if (session.isHost() && world.StatsWorldName() != g_ui.statsWorld) {
        if (!g_ui.statsWorld.empty() || !world.StatsWorldName().empty()) session.SavePlayerStatsNow("the world was saved under a new name");
        g_ui.statsWorld = world.StatsWorldName();
    }
    const bool inSession = session.isHost() || session.state() == kc::SessionState::Connected;
    g_ui.enabled = g_ui.enabled || inSession;
    if (!g_ui.enabled || !world.Ready()) return;
    ResolveExports();
    if (!g_ex.ok()) return;
    Live l = LookNow();
    if (!l.tabs) return;   // no management window (yet)
    if (!l.tabOk) {
        if (now - g_ui.failedAt < 30.0) return;
        if (g_ui.tab) Log("player stats tab: the management window was made again: the tab too");
        if (!MakeTab(l)) { g_ui.failedAt = now; return; }
        l = LookNow();
        if (!l.tabOk) return;
    }
    if (!l.textOk) {
        if (!MakeText()) return;
        l.textOk = true;
    }
    // the text: when the tab is shown (and on its first showing), if the statistics changed
    if (!l.shown) { g_ui.wasShown = false; return; }
    const std::string text = kc::PlayerStatsPageText(session.playerStats(), inSession);
    if (text != g_ui.shownText || !g_ui.wasShown) SetText(text);
    g_ui.wasShown = true;
}

std::string PlayerStatsUi(const std::string& what) {
    ResolveExports();
    if (!g_ex.ok()) return "err MyGUI exports missing";
    const Live l = LookNow();
    char b[256];
    std::snprintf(b, sizeof(b), "tab=%d text=%d index=%d count=%zu selected=%d shown=%d window=%d refreshes=%u len=%zu", int(l.tabOk), int(l.textOk),
                  l.index == kItemNone ? -1 : int(l.index), l.count, l.selected == kItemNone ? -1 : int(l.selected), int(l.shown),
                  int(l.window && Shown(l.window)), g_ui.refreshes, g_ui.shownText.size());
    if (what.empty() || what == "state") return std::string("ok ") + b;
    if (what == "select") {   // the window must be open (mapui open)
        if (!l.tabOk) return std::string("err no tab (") + b + ")";
        if (!SetSizeSEH(g_ex.selectIndex, l.tabs, l.index)) return "err selection failed";
        g_ui.nextLook = 0;   // its text now
        return "ok selected";
    }
    if (what == "text") {   // the text as given to the box (MyGUI colour codes kept), one line
        std::string t = g_ui.shownText;
        for (char& c : t) if (c == '\n' || c == '\r') c = '|';
        return "ok " + t;
    }
    return "err pstatsui state|select|text";
}

} // namespace kcp
