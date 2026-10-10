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
constexpr uintptr_t kMgmtMainWidget = 0x8;      // ManagementScreen: MyGUI::Window* (inherited-visible = the screen is up)
constexpr uintptr_t kMgmtMapScreen = 0xA8;      // ManagementScreen: MapScreen*
constexpr uintptr_t kMapScrollView = 0x18;      // MapScreen: MyGUI::ScrollView* (the visible part)
constexpr uintptr_t kMapImage = 0x20;           // MapScreen: MyGUI::ImageBox* (GUI_Map.dds, sized by the zoom)
constexpr uintptr_t kMapWorldBounds = 0x194;    // MapScreen: float minX, minZ (Vector4 worldBounds .x .y)
constexpr uintptr_t kMapWorldSize = 0x1A4;      // MapScreen: float sizeX, sizeZ
constexpr uintptr_t kPiCamera = 0x30;           // PlayerInterface: CameraClass*
constexpr uintptr_t kCamInitialised = 0x20;     // CameraClass: bool
constexpr uintptr_t kCamOgre = 0x68;            // CameraClass: Ogre::Camera*
constexpr uintptr_t kWidgetCropped = 0x8;       // MyGUI::Widget: its ICroppedRectangle base
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

// ---- MyGUI and Ogre, through their DLL exports
struct Coord { int left, top, width, height; };
using WidgetBoolFn = bool (*)(const void* widget);
using AbsCoordFn = Coord* (*)(const void* cropped, Coord* out);
using MatrixFn = const float* (*)(const void* frustum);
struct Exports {
    bool tried = false;
    WidgetBoolFn inheritedVisible = nullptr;
    AbsCoordFn absCoord = nullptr;
    MatrixFn viewMatrix = nullptr, projMatrix = nullptr;
};
Exports g_ex;
void ResolveExports() {
    if (g_ex.tried) return;
    g_ex.tried = true;
    if (HMODULE gui = GetModuleHandleW(L"MyGUIEngine_x64.dll")) {
        g_ex.inheritedVisible = reinterpret_cast<WidgetBoolFn>(GetProcAddress(gui, "?getInheritedVisible@Widget@MyGUI@@QEBA_NXZ"));
        g_ex.absCoord = reinterpret_cast<AbsCoordFn>(GetProcAddress(gui, "?getAbsoluteCoord@ICroppedRectangle@MyGUI@@QEBA?AU?$TCoord@H@types@2@XZ"));
    }
    if (HMODULE ogre = GetModuleHandleW(L"OgreMain_x64.dll")) {
        g_ex.viewMatrix = reinterpret_cast<MatrixFn>(GetProcAddress(ogre, "?getViewMatrix@Camera@Ogre@@UEBAAEBVMatrix4@2@XZ"));
        g_ex.projMatrix = reinterpret_cast<MatrixFn>(GetProcAddress(ogre, "?getProjectionMatrix@Frustum@Ogre@@UEBAAEBVMatrix4@2@XZ"));
    }
    Log("map: exports MyGUI visible=%d coord=%d, Ogre view=%d proj=%d", g_ex.inheritedVisible != nullptr, g_ex.absCoord != nullptr,
        g_ex.viewMatrix != nullptr, g_ex.projMatrix != nullptr);
}

bool WidgetVisibleSEH(const void* w) {
    __try {
        return g_ex.inheritedVisible(w);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
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
// A widget shown on screen (it and all its parents visible), and where.
bool WidgetOnScreen(const void* w, Coord& c) {
    if (!w || !g_ex.inheritedVisible || !g_ex.absCoord) return false;
    if (!WidgetVisibleSEH(w) || !WidgetCoordSEH(w, c)) return false;
    return c.width > 0 && c.height > 0 && c.width < 100000 && c.height < 100000;
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

MapScene g_scene;

float Dist2D(const kc::Vec3& a, const kc::Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

} // namespace

namespace mapmarks {
void hk_portraitUpdate(void* cell, const void* info, void* data) {
    o_portraitUpdate(cell, info, data);
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
        g_scene = s;
        OverlayPublishScene(s);
        return;
    }
    // the management screen (map, squads, research...) or the character editor covers the 3D view
    void* mgmt = ManagementScreen();
    void* mgmtWindow = nullptr;
    Coord mc{};
    const bool mgmtUp = mgmt && Rd(mgmt, kMgmtMainWidget, mgmtWindow) && WidgetOnScreen(mgmtWindow, mc);
    s.covered = mgmtUp || kenshi::CharacterEditorOpen();
    // the map screen and its projection
    if (void* ms = MapScreen()) {
        float b[2] = {}, sz[2] = {};
        if (kenshi::ReadRaw(ms, kMapWorldBounds, b, sizeof(b)) && kenshi::ReadRaw(ms, kMapWorldSize, sz, sizeof(sz)) && std::isfinite(b[0]) &&
            std::isfinite(b[1]) && sz[0] > 1.0f && sz[1] > 1.0f && sz[0] < 1e8f && sz[1] < 1e8f) {
            s.boundsOk = true;
            s.minX = b[0]; s.minZ = b[1]; s.sizeX = sz[0]; s.sizeZ = sz[1];
        }
        void *image = nullptr, *scroll = nullptr;
        Coord ic{}, sc{};
        if (mgmtUp && s.boundsOk && Rd(ms, kMapImage, image) && WidgetOnScreen(image, ic)) {
            s.mapOpen = true;
            s.imgX = float(ic.left); s.imgY = float(ic.top); s.imgW = float(ic.width); s.imgH = float(ic.height);
            s.clipX0 = s.imgX; s.clipY0 = s.imgY; s.clipX1 = s.imgX + s.imgW; s.clipY1 = s.imgY + s.imgH;
            if (Rd(ms, kMapScrollView, scroll) && WidgetOnScreen(scroll, sc)) {
                s.clipX0 = std::max(s.clipX0, float(sc.left)); s.clipY0 = std::max(s.clipY0, float(sc.top));
                s.clipX1 = std::min(s.clipX1, float(sc.left + sc.width)); s.clipY1 = std::min(s.clipY1, float(sc.top + sc.height));
            }
            if (s.clipX1 <= s.clipX0 || s.clipY1 <= s.clipY0) s.mapOpen = false;
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
            bool finite = true;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) {
                    float acc = 0;
                    for (int k = 0; k < 4; ++k) acc += p[i * 4 + k] * v[k * 4 + j];
                    s.viewProj[i * 4 + j] = acc;
                    finite = finite && std::isfinite(acc);
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
                Coord rc{};
                if (!Rd(cell, 0, vt) || vt != kenshi::Addr(kenshi::rva::VtPortraitCell) || !Rd(cell, kCellMainWidget, main) ||
                    !kenshi::HandleFromHand(static_cast<uint8_t*>(cell) + kCellHandle, h) || !WidgetOnScreen(main, rc))
                    continue;
                kenshi::Character* c = kenshi::Resolve(h);
                auto it = c ? avatarOwner.find(c) : avatarOwner.end();
                if (it == avatarOwner.end()) continue;
                s.portraits.push_back({float(rc.left), float(rc.top), float(rc.width), float(rc.height), it->second});
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
          << " ;";
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
        o << " enabled=" << s.showHeads << " cam=" << s.camOk << " covered=" << s.covered << " screen=" << w << "x" << h << " ;";
        for (const auto& c : s.chars) {
            if (!c.head) continue;
            float sx = 0, sy = 0;
            const bool on = WorldToScreen(s, {c.pos.x, c.pos.y + 22.0f, c.pos.z}, w, h, sx, sy) && sx >= 0 && sy >= 0 && sx <= w && sy <= h;
            o << ' ' << clean(c.name) << ":owner=" << int(c.owner) << ":col=" << colour(c.owner) << ":onscreen=" << on;
            if (on) o << ":sx=" << sx << ":sy=" << sy;
        }
    } else if (what == "barre") {
        o << " enabled=" << s.showPortraits << " frames=" << s.portraits.size();
        {
            std::lock_guard<std::mutex> lk(g_cellsMutex);
            o << " cells=" << g_cells.size();
        }
        o << " ;";
        for (const auto& r : s.portraits) o << " owner=" << int(r.owner) << ":col=" << colour(r.owner) << ":x=" << r.x << ":y=" << r.y << ":w=" << r.w;
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

} // namespace kcp
