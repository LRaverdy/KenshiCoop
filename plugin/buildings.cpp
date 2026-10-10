// Lot E: buildings and furniture placed in build mode, bought, built and dismantled, for every
// player (the session side is common/src/session_buildings.cpp).
//
// Build mode ends in <PreviewGroup>::createBuildings, which calls RootObjectFactory::createBuilding
// for each placed preview with the final values (template, position relative to the terrain or the
// parent building, rotation, town, the layout of the building it is furniture of, the building it
// stands inside, floor). The hooks (plugin/hooks.cpp) catch those values: a client builds nothing
// and asks the host; the host builds and tells everyone. Every machine then builds with the same
// values through the same factory call, followed by what build mode does to a fresh building.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hooks.h"
#include "util.h"
#include "world.h"

namespace kcp {

namespace {
// Building
constexpr uintptr_t BU_interior = 0x1F0;           // BuildingInterior*
constexpr uintptr_t IN_layout = 0x30;               // BuildingInterior: its Layout (embedded)
constexpr uintptr_t LA_building = 0x90;             // Layout: the building it belongs to
constexpr uintptr_t CS_complete = 0x0, CS_paused = 0x1, CS_dismantled = 0x2, CS_progress = 0x4, CS_total = 0x28;   // ConstructionState
// Building vtable
constexpr uintptr_t BV_getFaction = 0x58, BV_getBuildState = 0x228, BV_setConstructionProgress = 0x238,
                    BV_notifyConstructionComplete = 0x240, BV_isForSale = 0x2C0, BV_setupMiningResourceLevel = 0x2D8,
                    BV_setFaction = 0xA0;
constexpr uintptr_t LV_floorLayout = 0x40;          // Layout vtable: the layout build mode uses for an upper floor (see createBuildings)
constexpr uintptr_t PI_faction = 0x2A0;             // PlayerInterface: the player faction
constexpr uintptr_t kSnapCallbackVt = 0x16DFB00;    // build mode's callback for a building snapped to another one: {vt, PlayerInterface*, Building* snappedTo}
constexpr uintptr_t kGameAlloc = 0xED650A, kGameFree = 0xED64F8;   // the allocator build mode uses for that callback
constexpr int kAnswerYes = 2;                       // a confirmation dialog's "yes"

using FnCreateSig = void* (*)(void* factory, void* data, const float* pos, void* town, void* faction, const float* rot, void* cb, void* layout,
                              void* doorOf, void* save, void* indoors, bool invisible, bool completed, bool foliage, int floor, bool outside);
using FnVoidSig = void (*)(void*);
using FnPtrSig = void* (*)(void*);
using FnBoolSig = bool (*)(void*);
using FnIntSig = int (*)(void*);
using FnFloatArgSig = void (*)(void*, float);
using FnIntArgSig = void (*)(void*, int);
using FnAllocSig = void* (*)(size_t);

template <class T>
bool Rd(const void* p, uintptr_t off, T& out) { return kenshi::ReadRaw(p, off, &out, sizeof(T)); }
template <class T>
bool Wr(void* p, uintptr_t off, const T& v) { return kenshi::WriteRaw(p, off, &v, sizeof(T)); }

// A virtual function of a game object: only when its vtable and the function are the game's own
// (a stale or wrong pointer must not lead to a call into anything).
void* Slot(const void* obj, uintptr_t slot) {
    void* vt = nullptr;
    void* fn = nullptr;
    return obj && Rd(obj, 0, vt) && kenshi::InGameImage(vt) && Rd(vt, slot, fn) && kenshi::InGameImage(fn) ? fn : nullptr;
}

void* CreateSeh(void* fn, void* factory, void* data, const float* pos, void* town, void* faction, const float* rot, void* cb, void* layout,
                void* indoors, int floor, bool outside) {
    __try { return reinterpret_cast<FnCreateSig>(fn)(factory, data, pos, town, faction, rot, cb, layout, nullptr, nullptr, indoors, false, false, false, floor, outside); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool VoidSeh(void* fn, void* self) {
    __try { reinterpret_cast<FnVoidSig>(fn)(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* PtrSeh(void* fn, void* self) {
    __try { return reinterpret_cast<FnPtrSig>(fn)(self); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool BoolSeh(void* fn, void* self, bool& out) {
    __try { out = reinterpret_cast<FnBoolSig>(fn)(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool IntSeh(void* fn, void* self, int& out) {
    __try { out = reinterpret_cast<FnIntSig>(fn)(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FloatArgSeh(void* fn, void* self, float v) {
    __try { reinterpret_cast<FnFloatArgSig>(fn)(self, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool IntArgSeh(void* fn, void* self, int v) {
    __try { reinterpret_cast<FnIntArgSig>(fn)(self, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* AllocSeh(size_t n) {
    __try { return reinterpret_cast<FnAllocSig>(kenshi::Addr(kGameAlloc))(n); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool SetFactionSeh(void* fn, void* b, void* faction) {
    __try { reinterpret_cast<void (*)(void*, void*, void*)>(fn)(b, faction, nullptr); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void FreeSeh(void* p) {
    __try { reinterpret_cast<FnVoidSig>(kenshi::Addr(kGameFree))(p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

float Dist3(const kc::Vec3& a, const kc::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

void* PlayerFaction() {
    void* f = nullptr;
    kenshi::PlayerInterface* pi = kenshi::Player();
    return pi && Rd(pi, PI_faction, f) ? f : nullptr;
}

bool IsBuilding(const void* obj) {
    kc::Handle h;
    return obj && !kenshi::IsCharacter(obj) && kenshi::ObjectHandle(obj, h) && h.type == 0;
}

void* BuildState(void* b) {
    void* fn = IsBuilding(b) ? Slot(b, BV_getBuildState) : nullptr;
    return fn ? PtrSeh(fn, b) : nullptr;
}

bool IsOurs(void* b) {
    void* fn = Slot(b, BV_getFaction);
    void* ours = PlayerFaction();
    return fn && ours && PtrSeh(fn, b) == ours;
}

bool ForSale(void* b) {
    void* fn = IsBuilding(b) ? Slot(b, BV_isForSale) : nullptr;
    bool v = false;
    return fn && BoolSeh(fn, b, v) && v;
}

bool KindAndPlace(void* b, std::string& sid, kc::Vec3& pos) {
    return b && kenshi::ObjectTemplate(b, sid) && kenshi::ObjectPosition(b, pos);
}

// ---- placement validity: the values build mode uses (docs/MOTEUR.md section 10, "Validité d'une pose")
constexpr float kNoTerrain = -99.0f;          // UtilityT::getTerrainHeightFast where no terrain is known (0x168BDA0)
constexpr float kGroundValidMin = 98.0f;      // PreviewBuilding::Footprint::isGroundValid: a LAND footprint lower than this is in the water (0x16DFAF0)
constexpr float kMinNormalY = 0.2f;           // PreviewBuilding::buildingPlacementUpdate: terrain normal.y below this = slopeOK false (0x16885D8)
constexpr int kGroundLand = 1;                // BuildingPlacementGroundType: 0 any, 1 land (the default), 2 water
constexpr float kTooClose = 10.0f;            // mod: another building's origin this close (1 m) overlaps for sure
constexpr uintptr_t kTownList = 0x2134100;    // TownList* (what PreviewBuilding::placementVerification reads)
constexpr uintptr_t TB_x = 0x48, TB_z = 0x50, TB_type = 0xD8;   // TownBase: position x / z, TownType
constexpr uintptr_t TBV_getPosition = 0x40, TBV_getFaction = 0x58, TBV_isTown = 0x268, TBV_getRadius = 0x2A0;
constexpr uintptr_t FA_isPlayer = 0x250;      // Faction: PlayerInterface* (the player's own faction)
constexpr int kTownTypeAny = 10;              // what placementVerification asks getNearestTown for

using FnHeightSig = float (*)(float, float);
using FnPointSig = void* (*)(const float*);
using FnNearestTownSig = void* (*)(void* list, const float* pos, void* owner, void* except, void* mine, int type);
using FnBordersSig = bool (*)(void* town, const float* pos, float mult);
using FnWithinRadiusSig = void* (*)(void* list, const float* pos, bool skipPlayerTowns);
using FnPosOutSig = const float* (*)(void* self, float* out);
using FnFloatSig = float (*)(void* self);

bool HeightSeh(kenshi::Fn f, float x, float z, float& out) {
    __try { out = reinterpret_cast<FnHeightSig>(kenshi::FnAddr(f))(x, z); return std::isfinite(out); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool IndoorsSeh(const float* p, void*& out) {
    __try { out = reinterpret_cast<FnPointSig>(kenshi::FnAddr(kenshi::FnIsIndoors))(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* NearestTownSeh(void* list, const float* p) {
    __try { return reinterpret_cast<FnNearestTownSig>(kenshi::FnAddr(kenshi::FnGetNearestTown))(list, p, nullptr, nullptr, nullptr, kTownTypeAny); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool BordersSeh(void* town, const float* p, float mult) {
    __try { return reinterpret_cast<FnBordersSig>(kenshi::FnAddr(kenshi::FnWithinBordersRange))(town, p, mult); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* WithinRadiusSeh(void* list, const float* p) {
    __try { return reinterpret_cast<FnWithinRadiusSig>(kenshi::FnAddr(kenshi::FnGetNearestWithinItsRadius))(list, p, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool PosOutSeh(void* fn, void* self, float* out) {
    __try {
        const float* r = reinterpret_cast<FnPosOutSig>(fn)(self, out);
        if (r && r != out) std::memcpy(out, r, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FloatSeh(void* fn, void* self, float& out) {
    __try { out = reinterpret_cast<FnFloatSig>(fn)(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// PreviewBuilding::placementVerification's town (0x4DBA40 to 0x4DBD86, PreviewBuilding+0x90): the
// town a building at p would be "too close to". Build mode's click (0x4E27E0) refuses the spot when
// there is one: "Can't build too close to another town." Player towns never count.
void* TownTooClose(const float p[3], bool createsPlayerTown) {
    void* list = nullptr;
    if (!Rd(reinterpret_cast<void*>(kenshi::Addr(kTownList)), 0, list) || !list) return nullptr;
    void* town = NearestTownSeh(list, p);
    if (!town || !Slot(town, TBV_isTown)) return nullptr;   // a game object only
    int type = 0;
    Rd(town, TB_type, type);
    const float mult = (type == 4 || type == 9) ? 1.0f : 2.5f;
    bool inRange = BordersSeh(town, p, mult);
    float tx = 0, tz = 0;
    if (Rd(town, TB_x, tx) && Rd(town, TB_z, tz) && std::sqrt((tx - p[0]) * (tx - p[0]) + (tz - p[2]) * (tz - p[2])) < 2000.0f) inRange = true;
    // a closer town whose own radius holds the spot replaces it
    if (void* other = WithinRadiusSeh(list, p)) {
        void* isTownFn = Slot(other, TBV_isTown);
        void* otherTown = isTownFn ? PtrSeh(isTownFn, other) : nullptr;
        float op[3] = {0, 0, 0}, radius = 0;
        void* posFn = Slot(other, TBV_getPosition);
        void* radFn = Slot(other, TBV_getRadius);
        if (otherTown && posFn && radFn && PosOutSeh(posFn, other, op) && FloatSeh(radFn, other, radius)) {
            const float r = static_cast<float>(radius * (mult == 1.0f ? 0.6 : 0.9));
            const float d2 = (p[0] - op[0]) * (p[0] - op[0]) + (p[2] - op[2]) * (p[2] - op[2]);
            if (r * r > d2) town = otherTown;
        }
    }
    if (!inRange || !town) return nullptr;
    void* facFn = Slot(town, TBV_getFaction);
    void* fac = facFn ? PtrSeh(facFn, town) : nullptr;
    void* isPlayer = nullptr;
    if (fac && Rd(fac, FA_isPlayer, isPlayer) && isPlayer) return nullptr;   // our own town
    if (!createsPlayerTown && !BordersSeh(town, p, 1.0f)) return nullptr;
    return town;
}
} // namespace

// ---------------------------------------------------------------- finding buildings

void* KenshiWorld::BuildingAt(const std::string& sid, const kc::Vec3& pos) {
    if (sid.empty()) return nullptr;
    std::vector<void*> around;
    kenshi::ObjectsNear(pos, 40.0f, around);
    void* best = nullptr;
    float bestD = 5.0f;   // every machine builds it from the same values: it stands at the same place
    for (void* o : around) {
        std::string s;
        kc::Vec3 p;
        if (!IsBuilding(o) || !KindAndPlace(o, s, p) || s != sid) continue;
        const float d = Dist3(p, pos);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}

void* KenshiWorld::NearestBuilding(const kc::Vec3& from, const std::string& part, float radius, int want) {
    std::vector<void*> around;
    kenshi::ObjectsNear(from, radius, around);
    void* best = nullptr;
    float bestD = radius;
    for (void* o : around) {
        std::string sid, name;
        kc::Vec3 p;
        if (!IsBuilding(o) || !KindAndPlace(o, sid, p)) continue;
        if (!part.empty() && part != "any") {
            if (!kenshi::TemplateDisplayName(sid, name)) name = sid;
            if (name.find(part) == std::string::npos && sid.find(part) == std::string::npos) continue;
        }
        if (want == 1 && !ForSale(o)) continue;
        if (want >= 2 && !IsOurs(o)) continue;
        if (want == 3) {
            void* st = BuildState(o);
            uint8_t complete = 1;
            if (!st || !Rd(st, CS_complete, complete) || complete) continue;
        }
        const float d = Dist3(p, from);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}

bool KenshiWorld::FindBuilding(const std::string& sid, const kc::Vec3& pos, kc::Handle& out) {
    void* b = BuildingAt(sid, pos);
    return b && kenshi::ObjectHandle(b, out);
}

bool KenshiWorld::BuildingIdentity(const kc::Handle& h, std::string& sid, kc::Vec3& pos) {
    void* b = kenshi::ResolveObject(h);
    return IsBuilding(b) && KindAndPlace(b, sid, pos);
}

// ---------------------------------------------------------------- placements

bool KenshiWorld::DescribeBuildArgs(const BuildArgs& a, kc::BuildPlace& out) {
    out = kc::BuildPlace{};
    if (!kenshi::GameDataSidOf(a.data, out.sid)) return false;
    out.pos = a.pos;
    out.rot = a.rot;
    out.floor = a.floor;
    out.flags = a.outside ? kc::kBuildOutside : 0;
    if (a.layout) {
        // furniture: the building whose interior holds that layout
        void* parent = nullptr;
        void* interior = nullptr;
        if (Rd(a.layout, LA_building, parent) && parent && KindAndPlace(parent, out.parentSid, out.parentPos)) {
            const bool direct = Rd(parent, BU_interior, interior) && interior && reinterpret_cast<uint8_t*>(interior) + IN_layout == a.layout;
            if (!direct) out.flags |= kc::kBuildFloorLayout;
        }
    }
    if (a.indoors) KindAndPlace(a.indoors, out.indoorsSid, out.indoorsPos);
    if (a.callback && a.callback != kenshi::Player()) {
        void* snapped = nullptr;
        if (Rd(a.callback, 0x10, snapped) && snapped) KindAndPlace(snapped, out.snapSid, out.snapPos);
    }
    if (a.town) kenshi::ObjectHandle(a.town, out.town);
    return true;
}

void KenshiWorld::NoteBuildCapture(const BuildArgs& a, void* created) {
    LocalPlacement lp;
    if (!DescribeBuildArgs(a, lp.place)) {
        Log("build mode placement not understood (no template)");
        return;
    }
    if (created) kenshi::ObjectHandle(created, lp.created);
    Log("build mode placement: %s at %.1f,%.1f,%.1f floor %d%s%s", lp.place.sid.c_str(), lp.place.pos.x, lp.place.pos.y, lp.place.pos.z,
        lp.place.floor, lp.place.parentSid.empty() ? "" : " (furniture)", created ? "" : " - asked of the host");
    std::lock_guard<std::mutex> lk(buildMutex_);
    if (localPlacements_.size() < 64) localPlacements_.push_back(std::move(lp));
}

void KenshiWorld::TakeLocalPlacements(std::vector<LocalPlacement>& out) {
    std::lock_guard<std::mutex> lk(buildMutex_);
    out.swap(localPlacements_);
    localPlacements_.clear();
}

bool KenshiWorld::ExecutePlacement(const kc::BuildPlace& p, kc::Handle& created, kc::Vec3& worldPos) {
    created = kc::Handle{};
    kenshi::GameWorld* w = kenshi::World();
    kenshi::PlayerInterface* pi = kenshi::Player();
    void* data = kenshi::GameDataBySid(p.sid);
    void* factory = nullptr;
    if (!w || !pi || !data || !Rd(w, kenshi::off::GW_factory, factory) || !factory) {
        Log("building placement: %s cannot be built (template %s)", p.sid.c_str(), data ? "found" : "unknown");
        return false;
    }
    // the layout of the building it is furniture of, the building it is inside, the one it snaps to
    void* layout = nullptr;
    if (!p.parentSid.empty()) {
        void* parent = BuildingAt(p.parentSid, p.parentPos);
        void* interior = nullptr;
        if (!parent || !Rd(parent, BU_interior, interior) || !interior) {
            Log("building placement: the building %s it is furniture of is not here", p.parentSid.c_str());
            return false;
        }
        layout = reinterpret_cast<uint8_t*>(interior) + IN_layout;
        if (p.flags & kc::kBuildFloorLayout) {
            void* fn = Slot(layout, LV_floorLayout);
            void* l2 = fn ? PtrSeh(fn, layout) : nullptr;
            if (l2) layout = l2;
        }
    }
    void* indoors = p.indoorsSid.empty() ? nullptr : BuildingAt(p.indoorsSid, p.indoorsPos);
    void* town = p.town.valid() ? kenshi::ResolveObject(p.town) : nullptr;
    void* callback = pi;
    void* snapCb = nullptr;
    if (!p.snapSid.empty()) {
        if (void* snapped = BuildingAt(p.snapSid, p.snapPos)) {
            snapCb = AllocSeh(0x18);
            if (snapCb) {
                const uintptr_t vt = kenshi::Addr(kSnapCallbackVt);
                Wr(snapCb, 0, vt);
                Wr(snapCb, 8, static_cast<void*>(pi));
                Wr(snapCb, 0x10, snapped);
                callback = snapCb;
            }
        }
    }
    const float pos[3] = {p.pos.x, p.pos.y, p.pos.z};
    const float rot[4] = {p.rot.w, p.rot.x, p.rot.y, p.rot.z};   // Ogre::Quaternion
    void* b = nullptr;
    {
        HostCallScope scope;
        b = CreateSeh(kenshi::FnAddr(kenshi::FnCreateBuilding), factory, data, pos, town, PlayerFaction(), rot, callback, layout, indoors, p.floor,
                      (p.flags & kc::kBuildOutside) != 0);
        if (b) {
            // what build mode does to a fresh building
            if (void* fn = Slot(b, BV_setupMiningResourceLevel)) VoidSeh(fn, b);
            VoidSeh(kenshi::FnAddr(kenshi::FnClearUsageNodes), b);
        }
    }
    if (snapCb) FreeSeh(snapCb);
    if (!b || !kenshi::ObjectHandle(b, created) || !kenshi::ObjectPosition(b, worldPos)) {
        Log("building placement: the game's factory refused %s", p.sid.c_str());
        return false;
    }
    Log("built %s at %.1f,%.1f,%.1f (placement %u)", p.sid.c_str(), worldPos.x, worldPos.y, worldPos.z, unsigned(p.netId));
    return true;
}

// ---------------------------------------------------------------- placement validity

bool KenshiWorld::GroundAt(float x, float z, float& ground, float& withWater) {
    if (!kenshi::World()) return false;
    return HeightSeh(kenshi::FnTerrainHeight, x, z, ground) && HeightSeh(kenshi::FnTerrainWithWaterHeight, x, z, withWater);
}

// Build mode accepts a spot in its click handler (0x4E27E0) from what PreviewBuilding::
// placementVerification (vt 0xC8, 0x4DD240) and buildingPlacementUpdate (vt 0x50) computed on the
// preview under the cursor. A placement that comes from elsewhere (a client's request, a debug
// command) has no preview: the same rules are applied here with the functions those checks call,
// in the click handler's order, on the building's own spot (the game checks each footprint).
// Furniture is left to the client's build mode (its layout and nodes need the preview).
bool KenshiWorld::CheckPlacement(const kc::BuildPlace& p, std::string& why, std::string& whyFr) {
    why.clear();
    whyFr.clear();
    void* data = kenshi::GameDataBySid(p.sid);
    if (!data) {
        why = "unknown template";
        whyFr = "modèle de bâtiment inconnu.";
        return false;
    }
    if (!p.parentSid.empty()) return true;   // furniture
    float ground = 0, withWater = 0;
    // a loaded world only (towns, terrain); the debug channel and the session call this from the
    // game thread's tick, never from another thread
    if (!kenshi::Player() || !GroundAt(p.pos.x, p.pos.z, ground, withWater)) {
        why = "no game loaded";
        whyFr = "partie pas chargée.";
        return false;
    }
    if (ground == kNoTerrain) {
        why = "the ground there is not loaded on the host";
        whyFr = "le terrain n'est pas chargé chez l'hôte (réessaie dans un moment).";
        return false;
    }
    // the factory's height is relative to getTerrainWithWaterHeight (createBuildings, 0x4D72A0)
    const float at[3] = {p.pos.x, withWater + p.pos.y, p.pos.z};
    void* indoors = p.indoorsSid.empty() ? nullptr : BuildingAt(p.indoorsSid, p.indoorsPos);
    void* snapped = p.snapSid.empty() ? nullptr : BuildingAt(p.snapSid, p.snapPos);
    // 1. town ("Can't build too close to another town.")
    bool createsTown = false;
    kenshi::GameDataBoolField(data, "creates player town", createsTown);
    if (void* town = TownTooClose(at, createsTown)) {
        std::string tsid;
        why = "too close to a town";
        if (kenshi::ObjectTemplate(town, tsid)) why += " (" + tsid + ")";
        whyFr = "trop près d'une ville.";
        return false;
    }
    // 2. inside a building ("Must be placed outside or on a roof.": isIndoorsOK, through isIndoors)
    if (!indoors) {
        void* in = nullptr;
        if (IndoorsSeh(at, in) && in) {
            why = "inside another building";
            whyFr = "il doit être posé dehors ou sur un toit.";
            return false;
        }
    }
    // 3. on top of another building ("Too close to another building.": the footprints' collision
    //    test needs the preview; here only a building whose origin is within 1 m)
    if (!snapped) {
        std::vector<void*> around;
        kenshi::ObjectsNear(kc::Vec3{at[0], at[1], at[2]}, 40.0f, around);
        for (void* o : around) {
            std::string sid;
            kc::Vec3 op;
            if (o == indoors || !IsBuilding(o) || !KindAndPlace(o, sid, op)) continue;
            const float dx = op.x - at[0], dz = op.z - at[2];
            if (std::sqrt(dx * dx + dz * dz) < kTooClose && std::fabs(op.y - at[1]) < 30.0f) {
                why = "on top of another building (" + sid + ")";
                whyFr = "trop près d'un autre bâtiment.";
                return false;
            }
        }
    }
    if (indoors) return true;   // on a floor or a roof: the ground below does not matter
    // 4. ground ("The building has one or more parts on invalid ground."): Footprint::isGroundValid
    int groundType = kGroundLand;
    kenshi::GameDataIntField(data, "ground type", groundType);
    if (groundType == kGroundLand && ground < kGroundValidMin) {
        char b[96];
        snprintf(b, sizeof(b), "in water or acid (ground %.1f, surface %.1f)", double(ground), double(withWater));
        why = b;
        whyFr = "dans l'eau ou l'acide.";
        return false;
    }
    // 5. slope ("Ground needs to be more level."): the terrain's normal under the spot
    constexpr float s = 2.0f;
    float hx0 = 0, hx1 = 0, hz0 = 0, hz1 = 0;
    if (!snapped && HeightSeh(kenshi::FnTerrainHeight, p.pos.x - s, p.pos.z, hx0) && HeightSeh(kenshi::FnTerrainHeight, p.pos.x + s, p.pos.z, hx1) &&
        HeightSeh(kenshi::FnTerrainHeight, p.pos.x, p.pos.z - s, hz0) && HeightSeh(kenshi::FnTerrainHeight, p.pos.x, p.pos.z + s, hz1) &&
        hx0 != kNoTerrain && hx1 != kNoTerrain && hz0 != kNoTerrain && hz1 != kNoTerrain) {
        const float gx = (hx1 - hx0) / (2 * s), gz = (hz1 - hz0) / (2 * s);
        const float ny = 1.0f / std::sqrt(1.0f + gx * gx + gz * gz);
        if (ny < kMinNormalY) {
            char b[64];
            snprintf(b, sizeof(b), "too steep (normal.y %.2f)", double(ny));
            why = b;
            whyFr = "le sol doit être plus plat.";
            return false;
        }
    }
    return true;
}

bool KenshiWorld::DebugPlace(const kc::BuildPlace& p) {
    LocalPlacement lp;
    lp.place = p;
    if (!client_) {
        kc::Vec3 at;
        if (!ExecutePlacement(p, lp.created, at)) return false;
    }
    std::lock_guard<std::mutex> lk(buildMutex_);
    localPlacements_.push_back(std::move(lp));
    return true;
}

// ---------------------------------------------------------------- construction state

bool KenshiWorld::ReadBuildState(const kc::Handle& h, float& progress, uint8_t& flags) {
    return ReadBuildStateOf(kenshi::ResolveObject(h), progress, flags);
}

bool KenshiWorld::ReadBuildStateOf(void* b, float& progress, uint8_t& flags) {
    void* st = BuildState(b);
    uint8_t complete = 0, paused = 0, dismantled = 0;
    if (!st || !Rd(st, CS_progress, progress) || !Rd(st, CS_complete, complete) || !Rd(st, CS_paused, paused) || !Rd(st, CS_dismantled, dismantled))
        return false;
    if (!std::isfinite(progress)) progress = 0;
    flags = uint8_t((complete ? kc::kSiteComplete : 0) | (paused ? kc::kSitePaused : 0) | (dismantled ? kc::kSiteDismantling : 0));
    return true;
}

void KenshiWorld::ApplyBuildState(const kc::Handle& h, float progress, uint8_t flags) {
    void* b = kenshi::ResolveObject(h);
    void* st = BuildState(b);
    float cur = 0;
    uint8_t complete = 0;
    if (!st || !Rd(st, CS_progress, cur) || !Rd(st, CS_complete, complete)) return;
    HostCallScope scope;
    if (std::fabs(cur - progress) > 0.01f) {
        if (void* fn = Slot(b, BV_setConstructionProgress)) FloatArgSeh(fn, b, progress);   // completes it when it reaches the total
    }
    Rd(st, CS_complete, complete);
    if (!(flags & kc::kSiteComplete) && complete) {
        // finished here, a site to work on at the host (a bought building to repair): the same here
        const uint8_t no = 0;
        Wr(st, CS_complete, no);
    } else if ((flags & kc::kSiteComplete) && !complete) {
        if (void* fn = Slot(b, BV_notifyConstructionComplete)) VoidSeh(fn, b);
    }
    const uint8_t paused = (flags & kc::kSitePaused) ? 1 : 0, dismantled = (flags & kc::kSiteDismantling) ? 1 : 0;
    Wr(st, CS_paused, paused);
    Wr(st, CS_dismantled, dismantled);
}

void KenshiWorld::ConstructionSitesNear(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::Handle>& out) {
    out.clear();
    std::vector<void*> around;
    for (const auto& c : centers) {
        kenshi::ObjectsNear(c, radius, around);
        for (void* o : around) {
            void* st = BuildState(o);
            uint8_t complete = 1, dismantled = 0;
            if (!st || !Rd(st, CS_complete, complete) || !Rd(st, CS_dismantled, dismantled) || (complete && !dismantled)) continue;   // any faction: bought, towns, NPC sites
            kc::Handle h;
            if (kenshi::ObjectHandle(o, h) && std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
        }
        if (out.size() > 512) break;
    }
}

// The client's own game may build only what no player owns (a town it loads alone, NPC sites):
// the players' buildings are built, repaired and finished by the host.
bool KenshiWorld::IsPlayerBuilding(void* b) {
    return IsBuilding(b) && IsOurs(b);
}

// Building::setFaction(Faction*, ActivePlatoon*) (vtable +0xA0, 0x557950), as Task_OperateMachine
// calls it (platoon null): the player faction, or nobody's (null: the game's default faction). Only
// a mine or a natural node goes back to nobody (the others keep their owner).
bool KenshiWorld::SetBuildingOurs(void* b, bool ours) {
    void* fn = IsBuilding(b) ? Slot(b, BV_setFaction) : nullptr;
    void* f = ours ? PlayerFaction() : nullptr;
    if (!fn || (ours && !f)) return false;
    if (!ours) {
        const int kind = kenshi::BuildingFunctionOf(b);
        if (kind != 1 && kind != 27) return false;   // 1 a mine, 27 a natural deposit
    }
    const bool ok = SetFactionSeh(fn, b, f);
    std::string sid;
    kenshi::ObjectTemplate(b, sid);
    Log("building %s: %s as on the host (%s)", TemplateName(sid).c_str(), ours ? "the player faction's" : "nobody's", ok ? "ok" : "faulted");
    return ok && IsOurs(b) == ours;
}

bool KenshiWorld::IsBuildingForSale(void* b) { return ForSale(b); }

// ---------------------------------------------------------------- removal

void KenshiWorld::TrackBuilding(const kc::Handle& h) {
    void* b = kenshi::ResolveObject(h);
    if (!b) return;
    std::lock_guard<std::mutex> lk(buildMutex_);
    trackedBuildings_[b] = h;
}

void KenshiWorld::NoteObjectDestroyed(void* obj) {
    std::lock_guard<std::mutex> lk(buildMutex_);
    auto it = trackedBuildings_.find(obj);
    if (it == trackedBuildings_.end()) return;
    removedBuildings_.push_back(it->second);
    trackedBuildings_.erase(it);
}

void KenshiWorld::TakeBuildingRemovals(std::vector<kc::Handle>& out) {
    std::lock_guard<std::mutex> lk(buildMutex_);
    out.swap(removedBuildings_);
    removedBuildings_.clear();
}

bool KenshiWorld::RemoveBuilding(const kc::Handle& h) {
    void* b = kenshi::ResolveObject(h);
    if (!IsBuilding(b)) return false;
    HostCallScope scope;
    return kenshi::DestroyAnyObject(b);
}

// ---------------------------------------------------------------- buying, dismantling

void KenshiWorld::NoteLocalBuildAction(void* building, kc::BuildActionKind kind, int answer) {
    kc::BuildAction a;
    a.kind = kind;
    a.arg = answer;
    if (!KindAndPlace(building, a.sid, a.pos)) return;
    Log("%s of %s confirmed here: asked of the host", kind == kc::BuildActionKind::Buy ? "purchase" : "dismantling", a.sid.c_str());
    std::lock_guard<std::mutex> lk(buildMutex_);
    if (localBuildActions_.size() < 32) localBuildActions_.push_back(std::move(a));
}

void KenshiWorld::NoteHostBought(void* building) {
    if (ForSale(building)) return;   // the purchase did not go through (not enough cats)
    NoteLocalBuildAction(building, kc::BuildActionKind::Buy, kAnswerYes);
}

void KenshiWorld::TakeLocalBuildActions(std::vector<kc::BuildAction>& out) {
    std::lock_guard<std::mutex> lk(buildMutex_);
    out.swap(localBuildActions_);
    localBuildActions_.clear();
}

bool KenshiWorld::ExecuteBuildAction(const kc::BuildAction& a, std::string& refused) {
    refused.clear();
    void* b = BuildingAt(a.sid, a.pos);
    if (!b) return false;
    if (a.kind == kc::BuildActionKind::Buy) {
        if (!ForSale(b)) {
            if (client_ && IsOurs(b)) return true;   // already ours here
            refused = "Ce bâtiment n'est plus à vendre.";
            return false;
        }
        int price = 0;
        int32_t cats = 0;
        if (!client_ && IntSeh(kenshi::FnAddr(kenshi::FnCalculateSaleValue), b, price) && kenshi::ReadPlayerMoney(cats) && cats < price) {
            refused = "Pas assez d'argent pour acheter ce bâtiment (" + std::to_string(price) + " cats).";
            return false;
        }
        HostCallScope scope;
        IntArgSeh(kenshi::FnAddr(kenshi::FnBuyMeCallback), b, a.arg ? a.arg : kAnswerYes);
        if (ForSale(b)) {
            refused = "L'achat n'a pas abouti.";
            return false;
        }
        return true;
    }
    if (!IsOurs(b)) {
        refused = "Ce bâtiment n'est pas à nous.";
        return false;
    }
    HostCallScope scope;
    return IntArgSeh(kenshi::FnAddr(kenshi::FnConfirmDismantle), b, a.arg ? a.arg : kAnswerYes);
}

} // namespace kcp
