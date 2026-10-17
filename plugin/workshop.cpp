// Workshop: research, crafting benches, machines (operators, power, production) and town power.
//
// The host's game alone researches, crafts, produces and computes power. The host reads:
//  - the player faction's Research (PlayerInterface +0x38): finished techs (blueprints read are
//    finished techs too), the queue and each queued tech's progress, the research bench level;
//  - its machines (UseableStuff) near the players: who operates them (the operator set +0x3D0),
//    power, battery, progress bar, production amount, and a crafting bench's orders and repeat;
//  - the power panel of their towns (Town +0x470..+0x490, computed by Town::updatePowerGrid).
// A client imposes all of it on its copy (the machine found by kind and place). Its own game never
// researches, pays a research, completes one, reads a blueprint, takes or removes a crafting order
// or computes a host-powered town's grid: the hooks below refuse that outside HostCallScope. The
// research window's add / remove, a blueprint's "learn", the crafting window's add / remove /
// repeat and the building panel's power and battery switches become requests to the host.
// Engine facts (addresses, offsets, how each was found): docs/MOTEUR.md, "Atelier".
#include "world.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_set>

#include "hooks.h"

namespace kcp {

namespace {

// PlayerInterface
constexpr uintptr_t PI_research = 0x38;     // Research* technology
// Research
constexpr uintptr_t RS_screen = 0x0;        // ManagementScreen*
constexpr uintptr_t RS_queue = 0x38;        // std::deque<ResearchItem> (one item per block)
constexpr uintptr_t RS_finished = 0xF0;     // boost::unordered_set<GameData*>
constexpr uintptr_t RS_enabled = 0x130;     // boost::unordered_set<GameData*>: what the build menu and crafting lists offer (KenshiLib offset, read only)
constexpr uintptr_t RS_deskLevel = 0x170;   // int
constexpr uintptr_t RI_data = 0x0, RI_progress = 0x8;   // ResearchItem { GameData*; float }
// MSVC 2010 deque: +0 proxy, +8 map, +0x10 map size, +0x18 first offset, +0x20 size
constexpr uintptr_t DQ_map = 0x8, DQ_mapSize = 0x10, DQ_off = 0x18, DQ_size = 0x20;
// boost::unordered_set (as off:: in kenshi.h)
constexpr uintptr_t USet_bucketCount = 0x18, USet_size = 0x20, USet_buckets = 0x38, USetNode_next = 0x0, USetNode_value = 0x10;
// Building
constexpr uintptr_t B_town = 0x1D0;         // hand of its town
constexpr uintptr_t BV_getUseable = 0x300;  // UseableStuff* getUseableStuff()   (this, or null)
constexpr uintptr_t BV_getProduction = 0x3B0;   // ProductionBuilding* getProductionBuilding()
// UseableStuff
constexpr uintptr_t US_progressBar = 0x3A4;     // float
constexpr uintptr_t US_maxOperators = 0x3AC;    // int
constexpr uintptr_t US_battOn = 0x3B4;          // bool: receives battery power
constexpr uintptr_t US_powerOn = 0x3B5;         // bool
constexpr uintptr_t US_powerOutMax = 0x3BC;     // float: > 0 for a generator
constexpr uintptr_t US_power = 0x3C0;           // float: power given this frame
constexpr uintptr_t US_stored = 0x3C4;          // float: battery charge
constexpr uintptr_t US_storeMax = 0x3C8;        // float: > 0 for a battery
constexpr uintptr_t US_operators = 0x3D0;       // std::set<hand>: +8 head node, +0x10 size
// std::set<hand> node: +0 left, +8 parent, +0x10 right, +0x18 the hand (0x20 bytes), +0x39 isNil
constexpr uintptr_t TN_left = 0x0, TN_parent = 0x8, TN_right = 0x10, TN_value = 0x18, TN_isNil = 0x39;
// StorageBuilding / ProductionBuilding
constexpr uintptr_t SB_productionItem = 0x448;  // ConsumptionItem* (+0 float amount)
// CraftingBuilding
constexpr uintptr_t CB_crafts = 0x498;          // std::deque<CraftingItem> (one per block)
constexpr uintptr_t CB_repeat = 0x4CC;          // bool
constexpr uintptr_t CI_item = 0x0, CI_progress = 0xC;   // CraftingItem { Item*; float endTime; float progress01; ... }
constexpr uintptr_t IT_material = 0xC8;         // Item: material GameData*
// CraftingQueue (the crafting window)
constexpr uintptr_t CQ_bench = 0x60;            // hand of the bench it shows
// Town power panel
constexpr uintptr_t TW_onBattery = 0x470, TW_values = 0x474;   // bool; 8 floats
// UseableStuff vt 0x4E8: bool dontNeedWorkRightNow() const (tryOperate refuses every operator while true;
// a farm: crops growing, a research bench: nothing queued)
constexpr uintptr_t USV_dontNeedWork = 0x4E8;
// FarmBuilding (vtable 0x1689948, a ProductionBuilding; 1.0.68, docs/MOTEUR.md "Fermes")
constexpr uintptr_t FB_state = 0x468;           // int ProductionBuilding::productionState
constexpr uintptr_t FB_plantCount = 0x4F0;      // lektor<Plant> plants (+0x4E8): count
constexpr uintptr_t FB_plantData = 0x4F8;       //   ... and data, 0x28 bytes a plant, its age (float) at +0
constexpr uintptr_t FB_plantSize = 0x28;
constexpr uintptr_t FB_grown = 0x518;           // float: 0..1 the youngest growing plant's age, then 1..2 the harvest window
constexpr uintptr_t FB_died = 0x51C;            // float
constexpr uintptr_t FB_cleared = 0x520;         // float
constexpr uintptr_t FB_harvested = 0x528;       // int
// 0.3.5 (lot 4, point 18): the farm's inputs, water first (what its window's "Eau" bar shows). A lektor
// of 0x20-byte entries: +0 the stored amount (float, about 0..1), +4 per-unit multiplier, +0x10 the
// resource's GameData, +0x18 the InventorySection its delivered items wait in. ProductionBuilding::
// _updateInputs (0xDF4C0, called by FarmBuilding::update at 0xE56BE) drinks it every frame on every
// machine (rain on an outdoor farm fills input 0: 0xE5622-0xE56A5); without water, growth is multiplied
// by the drought factor (+0x550): the host's "grown -0.19" of 16 October (no water delivered, no rain).
constexpr uintptr_t PB_inputCount = 0x480, PB_inputData = 0x488, PB_inputSize = 0x20;
constexpr uint32_t kMaxFarmInputsHere = 16;
// vtables
constexpr uintptr_t kVtCraftingBuilding = 0x16B5A58;
constexpr uintptr_t kVtResearchBuilding = 0x16B28C8;
constexpr uintptr_t kVtTown = 0x1735BA8;
constexpr uintptr_t kVtFarmBuilding = 0x1689948;
constexpr int kCritMaybe = 2;                   // YesNoMaybe: let the game roll the critical success

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try { std::memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T>
bool Rd(const void* base, uintptr_t off, T& out) {
    return base && SafeCopy(&out, static_cast<const uint8_t*>(base) + off, sizeof(T));
}
template <class T>
bool Wr(void* base, uintptr_t off, const T& v) {
    return base && SafeCopy(static_cast<uint8_t*>(base) + off, &v, sizeof(T));
}
void* Slot(const void* obj, uintptr_t slot) {
    void* vt = nullptr;
    void* fn = nullptr;
    return Rd(obj, 0, vt) && vt && Rd(vt, slot, fn) ? fn : nullptr;
}
uintptr_t VtOf(const void* obj) {
    uintptr_t vt = 0;
    return Rd(obj, 0, vt) ? vt : 0;
}
using FnPtr0 = void* (*)(void*);
void* CallPtr(void* obj, uintptr_t slot) {
    void* fn = Slot(obj, slot);
    if (!fn) return nullptr;
    __try { return reinterpret_cast<FnPtr0>(fn)(obj); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// Building::select (vtable +0x130, 0x5504A0): what a click on the building does; it opens the
// building's own inventory panel (showInventoryBuilding) only for a finished building of the player
// faction.
bool CallSelect(void* b) {
    void* fn = Slot(b, 0x130);
    if (!fn) return false;
    __try { reinterpret_cast<void (*)(void*)>(fn)(b); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- calls into the game (SEH: a fault gives false, never a crash)
using FnBoolGd = bool (*)(void*, void*);
using FnVoidGd = void (*)(void*, void*);
using FnBoolItem = bool (*)(void*);
using FnAddCraft = void* (*)(void*, void*, void*, float, int);
using FnRemoveCraft = void (*)(void*, int);
using FnHandArg = void (*)(void*, const void*);
using FnSetInsert = void* (*)(void*, void*, const void*, bool);
using FnButton = void (*)(void*, void*);
using FnVoid0 = void (*)(void*);
using FnRemoveItem = bool (*)(void*, void*, int);

bool CallBoolGd(kenshi::Fn f, void* r, void* gd, bool& out) {
    __try { out = reinterpret_cast<FnBoolGd>(kenshi::FnAddr(f))(r, gd); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallVoidGd(kenshi::Fn f, void* r, void* gd) {
    __try { reinterpret_cast<FnVoidGd>(kenshi::FnAddr(f))(r, gd); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallLearn(void* item, bool& out) {
    __try { out = reinterpret_cast<FnBoolItem>(kenshi::FnAddr(kenshi::FnLearnResearch))(item); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallAddCraft(void* bench, void* base, void* mat, float progress, void*& out) {
    __try { out = reinterpret_cast<FnAddCraft>(kenshi::FnAddr(kenshi::FnCraftAdd))(bench, base, mat, progress, kCritMaybe); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallRemoveCraft(void* bench, int index) {
    __try { reinterpret_cast<FnRemoveCraft>(kenshi::FnAddr(kenshi::FnCraftRemove))(bench, index); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallStopOperating(void* us, const void* hand) {
    __try { reinterpret_cast<FnHandArg>(kenshi::FnAddr(kenshi::FnStopOperating))(us, hand); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSetInsert(void* set, const void* hand) {
    alignas(16) uint8_t ret[16] = {};
    __try { reinterpret_cast<FnSetInsert>(kenshi::FnAddr(kenshi::FnOperatorSetInsert))(set, ret, hand, false); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallButton(kenshi::Fn f, void* us) {
    __try { reinterpret_cast<FnButton>(kenshi::FnAddr(f))(us, nullptr); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallDontNeedWork(void* us, bool& out) {
    void* fn = Slot(us, USV_dontNeedWork);
    if (!fn) return false;
    __try { out = reinterpret_cast<bool (*)(void*)>(fn)(us); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallFarmDestroyAPlant(void* farm) {
    __try { return reinterpret_cast<bool (*)(void*)>(kenshi::FnAddr(kenshi::FnFarmDestroyAPlant))(farm); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallFarmReset(void* farm) {
    __try { reinterpret_cast<void (*)(void*)>(kenshi::FnAddr(kenshi::FnFarmReset))(farm); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallRefreshResearchList(void* screen) {
    __try { reinterpret_cast<FnVoid0>(kenshi::FnAddr(kenshi::FnRefreshResearchList))(screen); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallRemoveItem(void* inventory, void* item) {   // Inventory vt 0x30: removeItemAutoDestroy(item, 1), what the blueprint's "learn" does
    void* fn = Slot(inventory, 0x30);
    if (!fn) return false;
    __try { return reinterpret_cast<FnRemoveItem>(fn)(inventory, item, 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- reading the game's containers
bool DequeBlock(const void* dq, uint64_t i, void*& block) {
    void** map = nullptr;
    uint64_t mapSize = 0, off = 0, size = 0;
    if (!Rd(dq, DQ_map, map) || !map || !Rd(dq, DQ_mapSize, mapSize) || !Rd(dq, DQ_off, off) || !Rd(dq, DQ_size, size)) return false;
    if (i >= size || mapSize == 0 || mapSize > (1u << 20)) return false;
    uint64_t idx = off + i;
    if (idx >= mapSize) idx -= mapSize;
    return idx < mapSize && Rd(map, idx * sizeof(void*), block) && block;
}
uint64_t DequeSize(const void* dq) {
    uint64_t size = 0;
    return Rd(dq, DQ_size, size) && size < 4096 ? size : 0;
}
void ReadPointerSet(const void* set, std::vector<void*>& out, size_t cap) {
    out.clear();
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(set, USet_size, size) || size == 0 || size > cap) return;
    if (!Rd(set, USet_bucketCount, bucketCount) || !Rd(set, USet_buckets, buckets) || !buckets || bucketCount > (1u << 24)) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* v = nullptr;
        if (!Rd(node, USetNode_value, v)) return;
        out.push_back(v);
        if (!Rd(node, USetNode_next, node)) return;
    }
}
bool ReadHand(const void* p, kc::Handle& h) {
    uint8_t raw[kenshi::off::HandSize];
    if (!SafeCopy(raw, p, sizeof(raw))) return false;
    std::memcpy(&h.type, raw + kenshi::off::H_type, 4);
    std::memcpy(&h.container, raw + kenshi::off::H_container, 4);
    std::memcpy(&h.containerSerial, raw + kenshi::off::H_containerSerial, 4);
    std::memcpy(&h.index, raw + kenshi::off::H_index, 4);
    std::memcpy(&h.serial, raw + kenshi::off::H_serial, 4);
    return true;
}
// The operator set (std::set<hand>), in order.
void ReadOperators(const void* us, std::vector<kc::Handle>& out) {
    out.clear();
    const auto* set = static_cast<const uint8_t*>(us) + US_operators;
    void* head = nullptr;
    uint64_t size = 0;
    if (!Rd(set, 0x8, head) || !head || !Rd(set, 0x10, size) || size == 0 || size > 64) return;
    void* root = nullptr;
    if (!Rd(head, TN_parent, root) || !root) return;
    std::vector<void*> todo{root};
    for (int guard = 0; !todo.empty() && guard < 256; ++guard) {
        void* n = todo.back();
        todo.pop_back();
        uint8_t nil = 1;
        if (!n || n == head || !Rd(n, TN_isNil, nil) || nil) continue;
        kc::Handle h;
        if (ReadHand(static_cast<uint8_t*>(n) + TN_value, h)) out.push_back(h);
        void* l = nullptr;
        void* r = nullptr;
        if (Rd(n, TN_left, l)) todo.push_back(l);
        if (Rd(n, TN_right, r)) todo.push_back(r);
    }
}

void* ResearchObj() {
    kenshi::PlayerInterface* pi = kenshi::Player();
    void* r = nullptr;
    return pi && Rd(pi, PI_research, r) ? r : nullptr;
}
std::vector<void*> QueueData(void* r) {
    std::vector<void*> out;
    const auto* dq = static_cast<const uint8_t*>(r) + RS_queue;
    const uint64_t n = DequeSize(dq);
    for (uint64_t i = 0; i < n && i < kc::kMaxResearchQueue; ++i) {
        void* block = nullptr;
        void* gd = nullptr;
        if (DequeBlock(dq, i, block) && Rd(block, RI_data, gd)) out.push_back(gd);
    }
    return out;
}
std::string SidOf(const void* gd) {
    std::string s;
    return gd && kenshi::GameDataSidOf(gd, s) ? s : std::string();
}

// A building of the world (handle type 0): only those have the Building virtual slots used here.
bool IsBuilding(void* obj) {
    kc::Handle h;
    return obj && !kenshi::IsCharacter(obj) && kenshi::ObjectHandle(obj, h) && h.type == 0;
}
bool IsUseable(void* b) { return IsBuilding(b) && CallPtr(b, BV_getUseable) == b; }
bool IsCraftingBench(void* b) { return VtOf(b) == kenshi::Addr(kVtCraftingBuilding); }
bool IsFarm(void* b) { return VtOf(b) == kenshi::Addr(kVtFarmBuilding); }
// growth and rot to 1/200: a farm's numbers move every frame, the change that matters is slower
float Quantize(float v) { return std::isfinite(v) ? std::round(v * 200.0f) / 200.0f : 0.0f; }
bool ReadCrop(void* b, kc::CropState& c) {
    c = kc::CropState{};
    int32_t state = 0, harvested = 0;
    uint32_t plants = 0;
    float grown = 0, died = 0, cleared = 0;
    if (!Rd(b, FB_state, state) || !Rd(b, FB_plantCount, plants) || !Rd(b, FB_harvested, harvested) || !Rd(b, FB_grown, grown) ||
        !Rd(b, FB_died, died) || !Rd(b, FB_cleared, cleared))
        return false;
    c.stage = uint8_t(std::clamp(state, 0, 255));
    c.plants = uint16_t(std::min<uint32_t>(plants, kc::kMaxFarmPlants));
    c.harvested = uint16_t(std::clamp<int32_t>(harvested, 0, c.plants));
    c.grown = Quantize(grown);
    c.died = Quantize(died);
    c.cleared = Quantize(cleared);
    return true;
}
bool ReadFarmInputs(void* b, std::vector<float>& out) {
    out.clear();
    uint32_t n = 0;
    void* data = nullptr;
    if (!Rd(b, PB_inputCount, n) || n == 0 || n > kMaxFarmInputsHere || !Rd(b, PB_inputData, data) || !data) return false;
    for (uint32_t i = 0; i < n && i < kc::kMaxFarmInputs; ++i) {
        float v = 0;
        if (!Rd(data, i * PB_inputSize, v)) return false;
        out.push_back(std::isfinite(v) ? std::max(0.0f, v) : 0.0f);
    }
    return true;
}
void* TownOf(void* b) {
    kc::Handle h;
    if (!ReadHand(static_cast<uint8_t*>(b) + B_town, h) || !h.valid()) return nullptr;
    void* t = kenshi::ResolveObject(h);
    return t && VtOf(t) == kenshi::Addr(kVtTown) ? t : nullptr;
}
void* CraftingBenchOfQueue(void* craftingQueue) {
    kc::Handle h;
    if (!ReadHand(static_cast<uint8_t*>(craftingQueue) + CQ_bench, h) || !h.valid()) return nullptr;
    void* b = kenshi::ResolveObject(h);
    return IsCraftingBench(b) ? b : nullptr;
}

float Dist(const kc::Vec3& a, const kc::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
float Finite(float v) { return std::isfinite(v) ? v : 0.0f; }

thread_local int t_craftWindow = 0;   // inside a crafting window button (hooks below)


struct CraftWindowScope {
    CraftWindowScope() { ++t_craftWindow; }
    ~CraftWindowScope() { --t_craftWindow; }
};

} // namespace

// ---------------------------------------------------------------- research

bool KenshiWorld::ReadResearch(kc::ResearchState& out) {
    out = kc::ResearchState{};
    void* r = ResearchObj();
    if (!r) return false;
    int32_t level = 0;
    if (Rd(r, RS_deskLevel, level)) out.deskLevel = std::clamp(level, 0, 1000);
    std::vector<void*> fin;
    ReadPointerSet(static_cast<uint8_t*>(r) + RS_finished, fin, kc::kMaxResearchFinished);
    for (void* gd : fin)
        if (std::string s = SidOf(gd); !s.empty()) out.finished.push_back(std::move(s));
    std::sort(out.finished.begin(), out.finished.end());
    out.finished.erase(std::unique(out.finished.begin(), out.finished.end()), out.finished.end());
    const auto* dq = static_cast<const uint8_t*>(r) + RS_queue;
    const uint64_t n = DequeSize(dq);
    for (uint64_t i = 0; i < n && i < kc::kMaxResearchQueue; ++i) {
        void* block = nullptr;
        void* gd = nullptr;
        float progress = 0;
        if (!DequeBlock(dq, i, block) || !Rd(block, RI_data, gd) || !Rd(block, RI_progress, progress)) break;
        std::string s = SidOf(gd);
        if (!s.empty()) out.queue.push_back({std::move(s), Finite(progress)});
    }
    return true;
}

size_t KenshiWorld::ApplyResearch(const kc::ResearchState& s) {
    void* r = ResearchObj();
    if (!r) return 0;
    size_t changes = 0;
    CallScopeGuard scopeRepair("workshop");
    HostCallScope scope;   // our own calls go through the research hooks
    int32_t level = 0;
    if (Rd(r, RS_deskLevel, level) && level != s.deskLevel) {   // startResearch checks it
        Wr(r, RS_deskLevel, int32_t(s.deskLevel));
        ++changes;
    }
    // 1. techs the host finished (or learnt from a blueprint): completed here too (buildings and
    //    crafts unlocked in the build menu and the crafting lists)
    std::vector<void*> fin;
    ReadPointerSet(static_cast<uint8_t*>(r) + RS_finished, fin, kc::kMaxResearchFinished);
    std::unordered_set<std::string> local;
    for (void* gd : fin) local.insert(SidOf(gd));
    for (const auto& sid : s.finished) {
        if (local.count(sid)) continue;
        void* gd = kenshi::GameDataBySid(sid);
        if (!gd) {
            static std::unordered_set<std::string> said;
            if (said.insert(sid).second) Log("research: tech %s of the host unknown here", sid.c_str());
            continue;
        }
        bool queued = false;
        if (CallBoolGd(kenshi::FnResearchIsInQueue, r, gd, queued) && queued) CallVoidGd(kenshi::FnResearchStop, r, gd);   // completeResearch pops the front
        if (CallVoidGd(kenshi::FnResearchComplete, r, gd)) {
            ++changes;
            Log("research: %s completed as on the host", TemplateName(sid).c_str());
        }
    }
    // 2. the queue: the host's, in its order
    std::vector<std::string> want;
    for (const auto& q : s.queue) want.push_back(q.sid);
    std::vector<std::string> have;
    for (void* gd : QueueData(r)) have.push_back(SidOf(gd));
    if (have != want) {
        for (void* gd : QueueData(r)) CallVoidGd(kenshi::FnResearchStop, r, gd);
        for (const auto& sid : want) {
            void* gd = kenshi::GameDataBySid(sid);
            bool ok = false;
            if (gd && CallBoolGd(kenshi::FnResearchStart, r, gd, ok) && ok) continue;
            static std::unordered_set<std::string> said;
            if (said.insert(sid).second) Log("research: %s could not be queued here as on the host", TemplateName(sid).c_str());
        }
        ++changes;
    }
    // 3. progress of each queued tech
    const auto* dq = static_cast<const uint8_t*>(r) + RS_queue;
    const uint64_t n = DequeSize(dq);
    for (uint64_t i = 0; i < n && i < s.queue.size(); ++i) {
        void* block = nullptr;
        void* gd = nullptr;
        float p = 0;
        if (!DequeBlock(dq, i, block) || !Rd(block, RI_data, gd) || SidOf(gd) != s.queue[i].sid) break;
        if (Rd(block, RI_progress, p) && p != s.queue[i].progress) Wr(block, RI_progress, s.queue[i].progress);
    }
    if (changes) {
        void* screen = nullptr;
        if (Rd(r, RS_screen, screen) && screen) CallRefreshResearchList(screen);
    }
    return changes;
}

bool KenshiWorld::ExecuteResearchRequest(const kc::ResearchRequest& req, const kc::Handle& actor, std::string& why) {
    why.clear();
    void* r = ResearchObj();
    if (!r) { why = "La recherche n'est pas disponible chez l'hôte."; return false; }
    CallScopeGuard scopeRepair("workshop");
    HostCallScope scope;
    if (req.action == kc::ResearchAction::LearnBlueprint) {
        kenshi::Character* c = Find(actor);
        void* item = nullptr;
        std::vector<kc::ItemState> items;
        if (c && kenshi::ReadInventory(c, items)) {
            const kc::ItemState* pick = nullptr;
            for (const auto& it : items) {
                if (it.templateSid != req.sid) continue;
                if (!pick || (it.section == req.item.section && it.x == req.item.x && it.y == req.item.y)) pick = &it;
            }
            if (pick) item = kenshi::FindItemIn(c, *pick);
        }
        if (!item || kenshi::ItemTemplate(item) != req.sid) { why = "Ce plan n'est pas dans l'inventaire de ton personnage chez l'hôte."; return false; }
        bool learnt = false;
        if (!CallLearn(item, learnt) || !learnt) { why = "Plan non appris : technologie déjà connue ou prérequis manquants."; return false; }
        // the blueprint is used up, as the game's own "learn" does, through the inventory that owns it (one in
        // a worn backpack belongs to the backpack's inventory)
        if (void* inv = kenshi::OwningInventory(c, item)) CallRemoveItem(inv, item);
        return true;
    }
    void* gd = kenshi::GameDataBySid(req.sid);
    if (!gd) { why = "Technologie inconnue chez l'hôte."; return false; }
    std::vector<void*> fin;
    ReadPointerSet(static_cast<uint8_t*>(r) + RS_finished, fin, kc::kMaxResearchFinished);
    const bool finished = std::find(fin.begin(), fin.end(), gd) != fin.end();
    bool queued = false;
    CallBoolGd(kenshi::FnResearchIsInQueue, r, gd, queued);
    if (req.action == kc::ResearchAction::Cancel) {
        if (!queued) { why = "Cette recherche n'est pas (ou plus) dans la file."; return false; }
        return CallVoidGd(kenshi::FnResearchStop, r, gd);
    }
    if (finished) { why = "Technologie déjà recherchée."; return false; }
    if (queued) { why = "Cette recherche est déjà dans la file."; return false; }   // never paid twice
    bool ok = false;
    // the game's own checks: bench level, artifacts and books paid once (payCosts)
    if (!CallBoolGd(kenshi::FnResearchStart, r, gd, ok) || !ok) {
        why = "Recherche impossible : banc de recherche trop petit, ou artefacts / livres manquants.";
        return false;
    }
    if (void* screen = nullptr; Rd(r, RS_screen, screen) && screen) CallRefreshResearchList(screen);
    return true;
}

// ---------------------------------------------------------------- machines

bool KenshiWorld::ReadMachineState(void* b, kc::MachineState& s, std::vector<kc::Handle>* operators) {
    s = kc::MachineState{};
    if (!IsBuilding(b) || !IsUseable(b) || !kenshi::ObjectTemplate(b, s.sid) || !kenshi::ObjectPosition(b, s.pos)) return false;
    uint8_t on = 0, batt = 0;
    int32_t maxOps = 0;
    float outMax = 0, storeMax = 0;
    Rd(b, US_powerOn, on);
    Rd(b, US_battOn, batt);
    Rd(b, US_maxOperators, maxOps);
    Rd(b, US_powerOutMax, outMax);
    Rd(b, US_storeMax, storeMax);
    Rd(b, US_power, s.power);
    Rd(b, US_stored, s.stored);
    Rd(b, US_progressBar, s.progress);
    s.power = Finite(s.power);
    s.stored = Finite(s.stored);
    s.progress = Finite(s.progress);
    if (on) s.flags |= kc::kMachPowerOn;
    if (batt) s.flags |= kc::kMachBatteryOn;
    if (outMax > 0) s.flags |= kc::kMachGenerator;
    if (storeMax > 0) s.flags |= kc::kMachBattery;
    if (IsPlayerBuilding(b)) s.flags |= kc::kMachOurs;
    s.maxOperators = uint8_t(std::clamp(maxOps, 0, 255));
    std::vector<kc::Handle> ops;
    ReadOperators(b, ops);
    s.operatorCount = uint8_t(std::min<size_t>(ops.size(), 255));
    if (operators) *operators = ops;
    if (CallPtr(b, BV_getProduction) == b) {
        void* item = nullptr;
        float amount = 0;
        if (Rd(b, SB_productionItem, item) && item && Rd(item, 0, amount)) s.production = Finite(amount);
    }
    if (IsFarm(b) && ReadCrop(b, s.crop)) s.flags |= kc::kMachFarm;
    if (IsCraftingBench(b)) {
        s.flags |= kc::kMachCrafting;
        uint8_t rep = 0;
        if (Rd(b, CB_repeat, rep) && rep) s.flags |= kc::kMachRepeat;
        const auto* dq = static_cast<const uint8_t*>(b) + CB_crafts;
        const uint64_t n = DequeSize(dq);
        std::lock_guard<std::mutex> lk(workshopMutex_);
        for (uint64_t i = 0; i < n && i < kc::kMaxCraftOrders; ++i) {
            void* ci = nullptr;
            void* item = nullptr;
            void* mat = nullptr;
            float p = 0;
            if (!DequeBlock(dq, i, ci) || !Rd(ci, CI_item, item) || !item) break;
            Rd(ci, CI_progress, p);
            Rd(item, IT_material, mat);
            kc::CraftOrder o;
            o.itemSid = kenshi::ItemTemplate(item);
            o.materialSid = SidOf(mat);
            o.progress = Finite(p);
            if (auto m = craftMeta_.find(ci); m != craftMeta_.end()) {
                o.baseSid = m->second.first;
                o.materialSid = m->second.second;
            }
            if (o.baseSid.empty()) o.baseSid = o.itemSid;
            if (o.baseSid.empty()) continue;
            s.crafts.push_back(std::move(o));
        }
    }
    return true;
}

void KenshiWorld::ReadMachines(const std::vector<kc::Vec3>& centers, float radius, std::vector<WorldMachine>& out, std::vector<kc::TownPower>& towns) {
    out.clear();
    towns.clear();
    std::unordered_set<void*> seen;
    std::unordered_set<void*> seenTowns;
    std::vector<kc::Vec3> done;
    std::vector<void*> objs;
    std::vector<kc::Handle> squad;
    PlayerCharacters(squad);
    for (const auto& c : centers) {
        bool covered = false;
        for (const auto& p : done) covered |= Dist(p, c) < radius * 0.25f;
        if (covered) continue;
        done.push_back(c);
        kenshi::ObjectsNear(c, radius, objs);
        for (void* o : objs) {
            if (out.size() >= 512 || !seen.insert(o).second || !IsUseable(o)) continue;
            WorldMachine m;
            if (!ReadMachineState(o, m.state, &m.operators)) continue;
            // the players' machines; and any other (a mine, an iron node, a town's machine) a player
            // character works
            bool ours = IsPlayerBuilding(o);
            for (const auto& h : m.operators) ours = ours || std::find(squad.begin(), squad.end(), h) != squad.end();
            if (!ours) continue;
            // a machine: worked by someone, powered, a generator, a battery, producing or crafting
            const bool interesting = m.state.maxOperators > 0 || m.state.power > 0 || m.state.production > 0 ||
                                     (m.state.flags & (kc::kMachGenerator | kc::kMachBattery | kc::kMachCrafting)) || VtOf(o) == kenshi::Addr(kVtResearchBuilding);
            if (!interesting) continue;
            kenshi::ObjectHandle(o, m.handle);
            if (IsFarm(o)) ReadFarmInputs(o, m.inputs);   // 0.3.5: its water (FarmInputs)
            if (void* t = TownOf(o); t && towns.size() < kc::kMaxTownPower && seenTowns.insert(t).second) {
                kc::TownPower tp;
                tp.sid = m.state.sid;
                tp.pos = m.state.pos;
                uint8_t bat = 0;
                Rd(t, TW_onBattery, bat);
                tp.onBattery = bat != 0;
                for (int i = 0; i < 8; ++i) {
                    float v = 0;
                    Rd(t, TW_values + 4 * uintptr_t(i), v);
                    tp.values[i] = Finite(v);
                }
                towns.push_back(std::move(tp));
            }
            out.push_back(std::move(m));
        }
    }
}

namespace {
// A client's farm follows the host's crops. Its plants grow here by themselves (FarmBuilding::update),
// but only the host's farmers harvest (FarmBuilding::operate -> destroyAPlant) and only the host's game
// replants after a harvest or once dead crops are cleared (resetFarm): without this the client's field
// stayed ripe, then rotted and replanted on its own while the host's was harvested.
void ApplyCrop(void* b, const kc::CropState& host, const std::string& name) {
    uint32_t plants = 0;
    int32_t harvested = 0;
    float grown = 0;
    if (!Rd(b, FB_plantCount, plants) || !Rd(b, FB_harvested, harvested) || !Rd(b, FB_grown, grown)) return;
    if (plants > 0 && plants <= kc::kMaxFarmPlants) {
        // the host replanted: its harvest went back to 0, or its young plants against our ripe ones
        const bool replanted = harvested > int32_t(host.harvested) || (host.grown < 0.2f && grown > host.grown + 0.5f);
        if (replanted && CallFarmReset(b)) {
            Log("machine: %s replanted as on the host (harvested here %d, there %d; grown here %.2f, there %.2f)", name.c_str(), int(harvested),
                int(host.harvested), grown, host.grown);
            Rd(b, FB_harvested, harvested);
            Rd(b, FB_grown, grown);
        }
        // plants the host's farmers harvested: harvested here too (the same plants, in order)
        int32_t want = int32_t(host.harvested);
        if (host.plants && host.plants != plants) want = int32_t((uint64_t(host.harvested) * plants + host.plants / 2) / host.plants);
        want = std::clamp<int32_t>(want, 0, int32_t(plants));
        for (int guard = 0; harvested < want && guard < int(kc::kMaxFarmPlants); ++guard) {
            if (!CallFarmDestroyAPlant(b)) break;
            if (!Rd(b, FB_harvested, harvested)) break;
        }
        // growth: below 1 it is the youngest growing plant's age (the game takes it again every
        // frame), so every growing plant is moved by the difference; from 1 on it is a time of its own
        void* data = nullptr;
        const bool hostRipe = host.grown >= 1.0f;
        const float shift = hostRipe ? (grown < 1.0f ? 1.0f - grown : 0.0f) : host.grown - grown;
        if (std::fabs(shift) > 0.02f && Rd(b, FB_plantData, data) && data) {
            for (uint32_t i = 0; i < plants; ++i) {
                float age = 0;
                auto* p = static_cast<uint8_t*>(data) + i * FB_plantSize;
                if (!Rd(p, 0, age) || age < -1.0f || age >= 1.0f) continue;   // harvested (-10) or ripe
                Wr(p, 0, std::min(age + shift, hostRipe ? 1.0f : 0.999f));
            }
        }
        if (hostRipe) Wr(b, FB_grown, host.grown);
    }
    Wr(b, FB_died, host.died);
    Wr(b, FB_cleared, host.cleared);
    Wr(b, FB_state, int32_t(host.stage));
}
} // namespace

bool KenshiWorld::ApplyMachine(const kc::MachineState& s, const std::vector<kc::Handle>& operators, const kc::Handle& here) {
    // the host's placement built here (the session knows it by netId): it may stand a little off the
    // host's place (ground, snapping), so it comes before kind and place
    void* b = here.valid() ? kenshi::ResolveObject(here) : nullptr;
    std::string sid;
    if (b && (!IsUseable(b) || !kenshi::ObjectTemplate(b, sid) || sid != s.sid)) b = nullptr;
    if (!b) b = BuildingAt(s.sid, s.pos);
    if (!b || !IsUseable(b)) return false;   // the session says why (not loaded here, or not built)
    CallScopeGuard scopeRepair("workshop");
    HostCallScope scope;
    // whose it is: the game gives a mine or a natural node to the faction of whoever works it
    // (Task_OperateMachine calls Building::setFaction); here nobody works it, so the host's owner is
    // imposed. Building::select opens the building's inventory panel (its output) only for a building
    // of the player faction: without this a client never saw a mine's output window.
    if (((s.flags & kc::kMachOurs) != 0) != IsPlayerBuilding(b)) {
        static std::unordered_map<const void*, double> lastOwnerTry;   // one try every 30 s per building (a refusal is logged once each time)
        if (lastOwnerTry.size() > 512) lastOwnerTry.clear();
        double& at = lastOwnerTry[b];
        if (at == 0 || NowSeconds() - at > 30.0) {
            at = NowSeconds();
            SetBuildingOurs(b, (s.flags & kc::kMachOurs) != 0);
        }
    }
    auto setByte = [&](uintptr_t off, bool v) {
        uint8_t cur = 0;
        if (Rd(b, off, cur) && (cur != 0) != v) Wr(b, off, uint8_t(v ? 1 : 0));
    };
    setByte(US_powerOn, (s.flags & kc::kMachPowerOn) != 0);
    setByte(US_battOn, (s.flags & kc::kMachBatteryOn) != 0);
    Wr(b, US_power, s.power);
    Wr(b, US_stored, s.stored);
    Wr(b, US_progressBar, s.progress);
    if (CallPtr(b, BV_getProduction) == b) {
        void* item = nullptr;
        if (Rd(b, SB_productionItem, item) && item) Wr(item, 0, s.production);
    }
    // who works it: the host's operators (the building's window counts and names them)
    std::vector<kc::Handle> want;
    for (const kc::Handle& h : operators) {
        kc::Handle local;
        if (kenshi::Character* c = Find(h); c && kenshi::GetHandle(c, local)) want.push_back(local);
    }
    std::vector<kc::Handle> have;
    ReadOperators(b, have);
    uint8_t hand[kenshi::off::HandSize];
    for (const kc::Handle& h : have)
        if (std::find(want.begin(), want.end(), h) == want.end()) {
            kenshi::MakeHand(h, hand);
            CallStopOperating(b, hand);
        }
    for (const kc::Handle& h : want)
        if (std::find(have.begin(), have.end(), h) == have.end()) {
            kenshi::MakeHand(h, hand);
            CallSetInsert(static_cast<uint8_t*>(b) + US_operators, hand);
        }
    // a farm: the host's crops
    if ((s.flags & kc::kMachFarm) && IsFarm(b)) ApplyCrop(b, s.crop, TemplateName(s.sid));
    // a crafting bench: the host's orders, in its order, with its progress
    if ((s.flags & kc::kMachCrafting) && IsCraftingBench(b)) {
        setByte(CB_repeat, (s.flags & kc::kMachRepeat) != 0);
        const auto* dq = static_cast<const uint8_t*>(b) + CB_crafts;
        auto localOrders = [&]() {
            std::vector<std::pair<std::string, std::string>> v;
            const uint64_t n = DequeSize(dq);
            for (uint64_t i = 0; i < n && i < 64; ++i) {
                void* ci = nullptr;
                void* item = nullptr;
                void* mat = nullptr;
                if (!DequeBlock(dq, i, ci) || !Rd(ci, CI_item, item) || !item) break;
                Rd(item, IT_material, mat);
                v.emplace_back(kenshi::ItemTemplate(item), SidOf(mat));
            }
            return v;
        };
        std::vector<std::pair<std::string, std::string>> hostOrders;
        for (const auto& c : s.crafts) hostOrders.emplace_back(c.itemSid, c.materialSid);
        auto mine = localOrders();
        bool same = mine.size() == hostOrders.size();
        for (size_t i = 0; same && i < mine.size(); ++i) same = mine[i].first == hostOrders[i].first;   // the material may read differently
        static std::unordered_map<const void*, double> lastResync;   // a bench whose orders do not take here: retried now and then
        const double now = NowSeconds();
        if (!same && lastResync.size() > 512) lastResync.clear();
        if (!same && now - lastResync[b] < 15.0) same = true;
        if (!same) {
            lastResync[b] = now;
            for (int guard = 0; guard < 64 && DequeSize(dq) > 0; ++guard)
                if (!CallRemoveCraft(b, 0)) break;
            for (const auto& c : s.crafts) {
                void* base = kenshi::GameDataBySid(c.baseSid);
                void* mat = c.materialSid.empty() ? nullptr : kenshi::GameDataBySid(c.materialSid);
                void* ci = nullptr;
                if (!base || !CallAddCraft(b, base, mat, c.progress, ci) || !ci)
                    Log("machine: crafting order %s of the host could not be made here", TemplateName(c.baseSid).c_str());
            }
            Log("machine: %s crafting orders as on the host (%zu)", TemplateName(s.sid).c_str(), s.crafts.size());
        }
        const uint64_t n = DequeSize(dq);
        for (uint64_t i = 0; i < n && i < s.crafts.size(); ++i) {
            void* ci = nullptr;
            if (DequeBlock(dq, i, ci)) Wr(ci, CI_progress, s.crafts[i].progress);
        }
    }
    return true;
}

// What a work order on that machine meets (v0.3.4: the player is told by kc::WorkOrderNote; before,
// MachineIdleNote said nothing when the character was already at work there, rob's 60 clicks).
bool KenshiWorld::FillMachineFacts(void* b, kenshi::Character* actor, kc::WorkOrderFacts& f) {
    std::string sid;
    if (!IsUseable(b) || !kenshi::ObjectTemplate(b, sid)) return false;
    f.machine = true;
    f.machineName = TemplateName(sid);
    std::vector<kc::Handle> ops;
    ReadOperators(b, ops);
    kc::Handle me;
    f.atWorkThere = actor && kenshi::GetHandle(actor, me) && std::find(ops.begin(), ops.end(), me) != ops.end();
    int32_t maxOps = 0;
    Rd(b, US_maxOperators, maxOps);
    f.operators = int(ops.size());
    f.maxOperators = int(maxOps);
    bool idle = false;
    f.noWorkNow = CallDontNeedWork(b, idle) && idle;
    f.farm = IsFarm(b);
    return true;
}

// 0.3.5 (lot 4, point 18): the host's water in our farm (our own farm update drinks it, rain fills it with
// our weather, which is the host's: the session imposes the host's amounts every machine round).
bool KenshiWorld::ApplyFarmInputs(const kc::FarmInput& f, const kc::Handle& here) {
    void* b = here.valid() ? kenshi::ResolveObject(here) : nullptr;
    std::string sid;
    if (b && (!IsFarm(b) || !kenshi::ObjectTemplate(b, sid) || sid != f.sid)) b = nullptr;
    if (!b) b = BuildingAt(f.sid, f.pos);
    if (!b || !IsFarm(b)) return false;
    uint32_t n = 0;
    void* data = nullptr;
    if (!Rd(b, PB_inputCount, n) || n == 0 || n > kMaxFarmInputsHere || !Rd(b, PB_inputData, data) || !data) return false;
    for (uint32_t i = 0; i < n && i < f.stored.size(); ++i) {
        const float v = std::isfinite(f.stored[i]) ? std::max(0.0f, f.stored[i]) : 0.0f;
        float cur = 0;
        if (Rd(data, i * PB_inputSize, cur) && cur != v) Wr(data, i * PB_inputSize, v);
    }
    return true;
}

bool KenshiWorld::ApplyTownPower(const kc::TownPower& t) {
    void* b = BuildingAt(t.sid, t.pos);
    void* town = b ? TownOf(b) : nullptr;
    if (!town) return false;
    {
        std::lock_guard<std::mutex> lk(workshopMutex_);
        hostPoweredTowns_.insert(town);   // its own grid update stops: the numbers are the host's
    }
    Wr(town, TW_onBattery, uint8_t(t.onBattery ? 1 : 0));
    for (int i = 0; i < 8; ++i) Wr(town, TW_values + 4 * uintptr_t(i), t.values[i]);
    return true;
}

bool KenshiWorld::TownPowerFromHost(void* town) {
    std::lock_guard<std::mutex> lk(workshopMutex_);
    return hostPoweredTowns_.count(town) > 0;
}

bool KenshiWorld::ExecuteMachineRequest(const kc::MachineRequest& req, const kc::Handle& actor, std::string& why) {
    (void)actor;
    why.clear();
    void* b = BuildingAt(req.sid, req.pos);
    if (!b || !IsUseable(b)) { why = "Machine introuvable chez l'hôte."; return false; }
    if (!IsPlayerBuilding(b)) { why = "Cette machine n'appartient pas aux joueurs."; return false; }
    CallScopeGuard scopeRepair("workshop");
    HostCallScope scope;
    const bool bench = IsCraftingBench(b);
    switch (req.action) {
    case kc::MachineAction::AddCraft: {
        if (!bench) { why = "Ce n'est pas un établi de fabrication."; return false; }
        const auto* dq = static_cast<const uint8_t*>(b) + CB_crafts;
        if (DequeSize(dq) >= kc::kMaxCraftOrders) { why = "La file de fabrication de cet établi est pleine."; return false; }
        void* base = kenshi::GameDataBySid(req.baseSid);
        void* mat = req.materialSid.empty() ? nullptr : kenshi::GameDataBySid(req.materialSid);
        void* ci = nullptr;
        if (!base || (!req.materialSid.empty() && !mat)) { why = "Objet à fabriquer inconnu chez l'hôte."; return false; }
        if (!CallAddCraft(b, base, mat, 0.0f, ci) || !ci) { why = "L'établi refuse cet ordre de fabrication."; return false; }
        return true;
    }
    case kc::MachineAction::RemoveCraft: {
        if (!bench) { why = "Ce n'est pas un établi de fabrication."; return false; }
        const auto* dq = static_cast<const uint8_t*>(b) + CB_crafts;
        if (req.index < 0 || uint64_t(req.index) >= DequeSize(dq)) { why = "Cet ordre de fabrication n'existe plus."; return false; }
        return CallRemoveCraft(b, req.index);
    }
    case kc::MachineAction::SetRepeat:
        if (!bench) { why = "Ce n'est pas un établi de fabrication."; return false; }
        return Wr(b, CB_repeat, uint8_t(req.value ? 1 : 0));
    case kc::MachineAction::SetPower: {
        uint8_t on = 0;
        if (!Rd(b, US_powerOn, on)) return false;
        return (on != 0) == req.value || CallButton(kenshi::FnTogglePowerButton, b);
    }
    case kc::MachineAction::SetBattery: {
        uint8_t on = 0;
        if (!Rd(b, US_battOn, on)) return false;
        return (on != 0) == req.value || CallButton(kenshi::FnToggleBattButton, b);
    }
    }
    return false;
}

void* KenshiWorld::NearestMachine(const kc::Vec3& from, const std::string& part, float radius) {
    std::vector<void*> around;
    kenshi::ObjectsNear(from, radius, around);
    void* best = nullptr;
    float bestD = radius;
    for (void* o : around) {
        std::string sid;
        kc::Vec3 p;
        if (!IsBuilding(o) || !IsUseable(o) || !kenshi::ObjectTemplate(o, sid) || !kenshi::ObjectPosition(o, p)) continue;
        if (!part.empty() && part != "any") {
            std::string name = TemplateName(sid), lp = part, ln = name, ls = sid;
            for (auto* s : {&lp, &ln, &ls}) std::transform(s->begin(), s->end(), s->begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (ln.find(lp) == std::string::npos && ls.find(lp) == std::string::npos) continue;
        }
        const float d = Dist(p, from);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}

// ---------------------------------------------------------------- local asks (client)

void KenshiWorld::QueueResearchAsk(kc::ResearchAction a, void* gd) {
    LocalResearchAsk ask;
    ask.req.action = a;
    ask.req.sid = SidOf(gd);
    if (ask.req.sid.empty()) return;
    std::lock_guard<std::mutex> lk(workshopMutex_);
    if (researchAsks_.size() < 16) researchAsks_.push_back(std::move(ask));
}

void KenshiWorld::QueueBlueprintAsk(void* item) {
    LocalResearchAsk ask;
    ask.req.action = kc::ResearchAction::LearnBlueprint;
    ask.req.sid = kenshi::ItemTemplate(item);
    if (ask.req.sid.empty() || !kenshi::DescribeInventoryItem(item, ask.req.item)) return;
    // the character carrying it
    std::vector<kc::Handle> squad;
    PlayerCharacters(squad);
    for (const kc::Handle& h : squad) {
        kenshi::Character* c = Find(h);
        if (c && kenshi::FindItemIn(c, ask.req.item) == item) {
            const kc::Handle host = HostHandleOf(h);
            ask.actor = host.valid() ? host : h;
            break;
        }
    }
    std::lock_guard<std::mutex> lk(workshopMutex_);
    if (researchAsks_.size() < 16) researchAsks_.push_back(std::move(ask));
}

void KenshiWorld::QueueMachineAsk(void* b, kc::MachineAction a, void* base, void* mat, int index, bool value) {
    LocalMachineAsk ask;
    ask.req.action = a;
    if (!kenshi::ObjectTemplate(b, ask.req.sid) || !kenshi::ObjectPosition(b, ask.req.pos)) return;
    ask.req.baseSid = SidOf(base);
    ask.req.materialSid = SidOf(mat);
    ask.req.index = index;
    ask.req.value = value;
    if (a == kc::MachineAction::AddCraft && ask.req.baseSid.empty()) return;
    std::lock_guard<std::mutex> lk(workshopMutex_);
    if (machineAsks_.size() < 16) machineAsks_.push_back(std::move(ask));
}

void KenshiWorld::TakeWorkshopAsks(std::vector<LocalResearchAsk>& research, std::vector<LocalMachineAsk>& machines) {
    std::lock_guard<std::mutex> lk(workshopMutex_);
    research.swap(researchAsks_);
    researchAsks_.clear();
    machines.swap(machineAsks_);
    machineAsks_.clear();
}

void KenshiWorld::NoteCraftAdded(void* ci, void* base, void* mat) {
    std::lock_guard<std::mutex> lk(workshopMutex_);
    if (craftMeta_.size() > 4096) craftMeta_.clear();
    craftMeta_[ci] = {SidOf(base), SidOf(mat)};
}

// ---------------------------------------------------------------- test commands (canal de test)
// research | researchknown <sid> | researchpick [n] | researchreq <queue|cancel> <sid> |
// researchprogress <amount> (host) | blueprintreq <squadIndex> <item part> | craftlist [n] |
// machine <part> | machinereq <part> <addcraft base [material]|removecraft i|repeat 0/1|power 0/1|battery 0/1> |
// machineinv <part> | machines | crops [part] | storage <part> | workjob <squadIndex> <sid|none> [task].
// handled: true when the command is one of these.
namespace {
// "stage,harvested/plants,grown,died" of a farm, read here (tests)
std::string CropText(const kc::CropState& c) {
    char b[96];
    std::snprintf(b, sizeof(b), "%d,%d/%d,%.2f,%.2f", int(c.stage), int(c.harvested), int(c.plants), c.grown, c.died);
    return b;
}
uint32_t Fnv(const std::string& s, uint32_t h = 2166136261u) {
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}
using FnCheckReq = bool (*)(void*, void*, bool, bool);
bool CallCheckReq(void* r, void* gd, bool checkCost, bool& out) {
    __try { out = reinterpret_cast<FnCheckReq>(kenshi::FnAddr(kenshi::FnResearchCheckRequirements))(r, gd, false, checkCost); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using FnProgressCall = void (*)(void*, float);
bool CallProgress(void* r, float amount) {
    __try { reinterpret_cast<FnProgressCall>(kenshi::FnAddr(kenshi::FnResearchProgress))(r, amount); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
// Research::craftableThings (+0x10): std::map<itemType, lektor<GameData*>> (node value: key +0x18, lektor +0x20)
void CraftableThings(void* r, std::vector<void*>& out, size_t max) {
    out.clear();
    const auto* map = static_cast<const uint8_t*>(r) + 0x10;
    void* head = nullptr;
    uint64_t size = 0;
    if (!Rd(map, 0x8, head) || !head || !Rd(map, 0x10, size) || size == 0 || size > 256) return;
    void* root = nullptr;
    if (!Rd(head, TN_parent, root)) return;
    std::vector<void*> todo{root};
    for (int guard = 0; !todo.empty() && guard < 1024 && out.size() < max; ++guard) {
        void* n = todo.back();
        todo.pop_back();
        uint8_t nil = 1;
        if (!n || n == head || !Rd(n, TN_isNil, nil) || nil) continue;
        const auto* lk = static_cast<const uint8_t*>(n) + 0x20;
        uint32_t count = 0;
        void** data = nullptr;
        if (Rd(lk, kenshi::off::LK_count, count) && Rd(lk, kenshi::off::LK_data, data) && data && count < 10000)
            for (uint32_t i = 0; i < count && out.size() < max; ++i) {
                void* gd = nullptr;
                if (Rd(data, i * sizeof(void*), gd) && gd) out.push_back(gd);
            }
        void* l = nullptr;
        void* rr = nullptr;
        if (Rd(n, TN_left, l)) todo.push_back(l);
        if (Rd(n, TN_right, rr)) todo.push_back(rr);
    }
}
} // namespace

std::string WorkshopCommand(kc::Session& s, KenshiWorld& w, std::istringstream& in, const std::string& cmd, bool& handled) {
    handled = true;
    void* r = ResearchObj();
    auto squadAt = [&](size_t idx, kc::Vec3& pos) -> kenshi::Character* {
        std::vector<kc::Handle> sq;
        w.PlayerCharacters(sq);
        std::sort(sq.begin(), sq.end(), [](const kc::Handle& a, const kc::Handle& b) { return a.index != b.index ? a.index < b.index : a.serial < b.serial; });
        if (idx >= sq.size()) return nullptr;
        kenshi::Character* c = w.Find(sq[idx]);
        return c && kenshi::ObjectPosition(c, pos) ? c : nullptr;
    };
    if (cmd == "machines") {
        // machines: every machine this session knows (host: what it sends; client: the host's), each
        // read in this game: "ok <n> key|found|ops=a/b|on=0/1|farm=stage,h/p,grown,died|prod=x ..." (sorted)
        std::vector<std::string> rows;
        s.ForEachKnownMachine([&](const std::string& key, const kc::MachineState& known, bool foundHere) {
            void* b = w.BuildingAt(known.sid, known.pos);
            if (!b) {   // ours may stand a little off the host's place
                void* close = w.NearestMachine(known.pos, known.sid, 40.0f);
                std::string sid;
                if (close && kenshi::ObjectTemplate(close, sid) && sid == known.sid) b = close;
            }
            kc::MachineState here;
            std::ostringstream o;
            o.setf(std::ios::fixed);
            o.precision(2);
            o << key << "|" << (foundHere ? "found" : "MISSING");
            if (b && w.ReadMachineState(b, here, nullptr)) {
                o << "|ops=" << int(here.operatorCount) << "/" << int(here.maxOperators) << "|on=" << ((here.flags & kc::kMachPowerOn) ? 1 : 0)
                  << "|prod=" << here.production;
                if (here.flags & kc::kMachFarm) o << "|farm=" << CropText(here.crop);
                if (here.flags & kc::kMachCrafting) o << "|crafts=" << here.crafts.size();
            } else {
                o << "|not here";
            }
            rows.push_back(o.str());
        });
        std::sort(rows.begin(), rows.end());
        std::string out = "ok " + std::to_string(rows.size());
        for (const auto& r2 : rows) out += " " + r2;
        return out;
    }
    if (cmd == "crops") {   // crops [part]: the farm nearest to squad member 0 (name or sid part, "any"), read here
        std::string part = "any";
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        kc::Vec3 me;
        if (!squadAt(0, me)) return "err no squad";
        std::vector<void*> objs;
        kenshi::ObjectsNear(me, 3000.0f, objs);
        void* best = nullptr;
        float bestD = 1e30f;
        for (void* o : objs) {
            std::string sid;
            kc::Vec3 p;
            if (!IsFarm(o) || !kenshi::ObjectTemplate(o, sid) || !kenshi::ObjectPosition(o, p)) continue;
            if (part != "any" && Lower(w.TemplateName(sid)).find(Lower(part)) == std::string::npos && sid.find(part) == std::string::npos) continue;
            if (Dist(p, me) < bestD) { bestD = Dist(p, me); best = o; }
        }
        kc::MachineState st;
        std::vector<kc::Handle> ops;
        if (!best || !w.ReadMachineState(best, st, &ops)) return "err no farm " + part;
        bool idle = false;
        CallDontNeedWork(best, idle);
        uint8_t hydro = 0;
        Rd(best, 0x558, hydro);   // FarmBuilding::isHydroponic
        const std::string key = kc::Session::MachineKey(st.sid, st.pos);
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "ok " << key << " stage=" << int(st.crop.stage) << " plants=" << st.crop.plants << " harvested=" << st.crop.harvested << " grown=" << st.crop.grown
          << " died=" << st.crop.died << " cleared=" << st.crop.cleared << " hydro=" << int(hydro != 0) << " idle=" << int(idle) << " ops=" << int(st.operatorCount)
          << "/" << int(st.maxOperators) << " prod=" << st.production;
        std::vector<float> water;   // 0.3.5: its inputs (water first), what the "Eau" bar shows
        o << " water=";
        if (ReadFarmInputs(best, water) && !water.empty())
            for (size_t i = 0; i < water.size(); ++i) o << (i ? "," : "") << water[i];
        else
            o << "none";
        if (const kc::MachineState* known = s.knownMachine(key); known && !s.isHost()) o << " host=" << CropText(known->crop);
        return o.str();
    }
    if (cmd == "storage") {
        // storage <part>: the object with an inventory (storage, chest, machine) nearest to squad member
        // 0 whose name or sid holds part: "ok key stacks=n total=q ih=hash items=sid:qty,..." (sorted)
        std::string part;
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        kc::Vec3 me;
        if (!squadAt(0, me)) return "err no squad";
        std::vector<void*> objs;
        kenshi::ObjectsNear(me, 3000.0f, objs);
        void* best = nullptr;
        float bestD = 1e30f;
        std::vector<kc::ItemState> items;
        for (void* o : objs) {
            std::string sid;
            kc::Vec3 p;
            if (kenshi::IsCharacter(o) || !kenshi::ObjectTemplate(o, sid) || !kenshi::ObjectPosition(o, p) || !kenshi::ReadInventory(o, items)) continue;
            if (part != "any" && Lower(w.TemplateName(sid)).find(Lower(part)) == std::string::npos && sid.find(part) == std::string::npos) continue;
            if (Dist(p, me) < bestD) { bestD = Dist(p, me); best = o; }
        }
        std::string sid;
        kc::Vec3 p;
        if (!best || !kenshi::ObjectTemplate(best, sid) || !kenshi::ObjectPosition(best, p) || !kenshi::ReadInventory(best, items)) return "err no storage " + part;
        std::map<std::string, int> bySid;
        int total = 0;
        uint32_t hh = 2166136261u;
        for (const auto& it : items) {
            bySid[it.templateSid] += it.quantity;
            total += it.quantity;
        }
        std::string list;
        for (const auto& [k, q] : bySid) {
            hh = Fnv(k + ":" + std::to_string(q) + ";", hh);
            list += (list.empty() ? "" : ",") + k + ":" + std::to_string(q);
        }
        std::ostringstream o;
        o << "ok " << kc::Session::MachineKey(sid, p) << " stacks=" << items.size() << " total=" << total << " ih=" << std::hex << hh << std::dec << " items=" << list;
        return o.str();
    }
    if (cmd == "workjob") {
        // workjob <squadIndex> <sid|none> [task]: that member alone gets a PERMANENT job (the Tâches
        // panel, addJobSelectedCharacters with shift, as a right click in jobs mode does) on the
        // instance of that template nearest to squad member 0, or with no subject ("none"); default
        // task 87. On a client the order goes to the host. Hauling 88 with no subject makes no job in
        // 1.0.68 (13/10: jobs 1 -> 1 on the host and on the client): the game wants a target for it.
        size_t idx = 0;
        std::string sid;
        int task = 87;
        in >> idx >> sid >> task;
        kc::Vec3 anchor, p;
        if (!squadAt(0, anchor)) return "err no squad";
        kenshi::Character* c = squadAt(idx, p);
        if (!c) return "err no such squad member";
        void* best = nullptr;
        kc::Vec3 bp = p;
        if (sid != "none") {
            std::vector<void*> objs;
            kenshi::ObjectsNear(anchor, 4000.0f, objs);
            float bestD = 1e30f;
            for (void* o : objs) {
                std::string s2;
                kc::Vec3 op;
                if (!IsBuilding(o) || !kenshi::ObjectTemplate(o, s2) || s2 != sid || !kenshi::ObjectPosition(o, op)) continue;
                if (Dist(op, anchor) < bestD) { bestD = Dist(op, anchor); best = o; bp = op; }
            }
            if (!best) return "err no " + sid + " within 4000";
        }
        const float loc[3] = {bp.x, bp.y, bp.z};
        using FnAddJob = void (*)(void*, int, void*, bool, bool, const float*);
        kenshi::WithSelection(c, [&] { reinterpret_cast<FnAddJob>(kenshi::FnAddr(kenshi::FnAddJobSelected))(kenshi::Player(), task, best, true, true, loc); });
        return "ok " + std::to_string(kenshi::PermajobCount(c)) + " job(s)";
    }
    if (cmd == "research") {
        kc::ResearchState st;
        if (!w.ReadResearch(st)) return "err no research";
        uint32_t h = 2166136261u;
        for (const auto& f : st.finished) h = Fnv(f + ";", h);
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "ok desk=" << st.deskLevel << " finished=" << st.finished.size() << " fh=" << std::hex << h << std::dec << " queue=";
        for (size_t i = 0; i < st.queue.size(); ++i) o << (i ? "|" : "") << st.queue[i].sid << ":" << st.queue[i].progress;
        return o.str();
    }
    if (cmd == "researchenabled") {   // researchenabled [type]: what is unlocked (build menu: type 0), sorted sids
        int type = -1;
        in >> type;
        if (!r) return "err no research";
        std::vector<void*> en;
        ReadPointerSet(static_cast<uint8_t*>(r) + RS_enabled, en, 100000);
        std::vector<std::string> sids;
        for (void* gd : en) {
            int t = -2;
            if (type >= 0 && (!Rd(gd, kenshi::off::GD_type, t) || t != type)) continue;
            if (std::string sid = SidOf(gd); !sid.empty()) sids.push_back(sid);
        }
        std::sort(sids.begin(), sids.end());
        uint32_t h = 2166136261u;
        for (const auto& x : sids) h = Fnv(x + ";", h);
        std::ostringstream o;
        o << "ok " << sids.size() << " eh=" << std::hex << h << std::dec;
        for (const auto& x : sids) o << " " << x;
        return o.str();
    }
    if (cmd == "machinegive") {   // machinegive <part> <item sid|name part> <n>: (host, sandbox) n new items into that machine's inventory
        std::string part, what;
        int n = 1;
        in >> part >> what >> n;
        if (!s.isHost()) return "err host only";
        std::replace(part.begin(), part.end(), '_', ' ');
        std::replace(what.begin(), what.end(), '_', ' ');
        kc::Vec3 me;
        kenshi::Character* c = squadAt(0, me);
        void* b = c ? w.NearestMachine(me, part, 3000.0f) : nullptr;
        if (!b) return "err no machine " + part;
        std::vector<std::string> cands;
        if (kenshi::GameDataBySid(what)) cands.push_back(what);
        std::vector<std::pair<std::string, std::string>> found;
        kenshi::ItemTemplates(what, found, 32);
        for (const auto& f : found) cands.push_back(f.first);
        CallScopeGuard scopeRepair("workshop");
        HostCallScope scope;
        std::string why;
        // 0.3.4: every section of the machine, not only the first one (13/10 workstations and
        // machineout runs: "no room" for the stove, whose first section did not take that item)
        const std::vector<std::string> sections = kenshi::SectionNames(b);
        for (const auto& sid : cands) {
            if (!kenshi::GiveNewItem(c, sid, n, &why)) continue;
            std::vector<kc::ItemState> items;
            kenshi::ReadInventory(c, items);
            for (const auto& it : items) {
                if (it.templateSid != sid) continue;
                for (const auto& sec : sections) {
                    kc::InvOp op;
                    op.item = it;
                    op.toSection = sec;
                    op.toX = -1;
                    op.toY = -1;
                    std::string e;
                    if (kenshi::MoveInventoryItem(c, b, op, &e)) return "ok " + sid + " x" + std::to_string(it.quantity) + " in " + sec;
                    why = e + " (" + sid + " in " + sec + ")";
                }
            }
        }
        std::string secs;
        for (const auto& sec : sections) secs += (secs.empty() ? "" : ",") + sec;
        return "err " + why + " [sections " + secs + "]";
    }
    if (cmd == "researchknown") {
        std::string sid;
        in >> sid;
        kc::ResearchState st;
        if (!w.ReadResearch(st)) return "err no research";
        return std::string("ok ") + (std::binary_search(st.finished.begin(), st.finished.end(), sid) ? "1" : "0");
    }
    if (cmd == "researchpick") {   // researchpick [n] [cost]: techs whose requirements are met here ("cost": and whose cost the benches hold)
        size_t n = 5;
        std::string opt;
        in >> n >> opt;
        const bool checkCost = opt == "cost";
        if (!r) return "err no research";
        kc::ResearchState st;
        w.ReadResearch(st);
        std::vector<void*> all;
        kenshi::GameDataOfType(21, all, 100000);
        std::string out = "ok";
        size_t found = 0;
        for (void* gd : all) {
            const std::string sid = SidOf(gd);
            if (sid.empty() || std::binary_search(st.finished.begin(), st.finished.end(), sid)) continue;
            bool queued = false;
            for (auto& q : st.queue) queued |= q.sid == sid;
            bool ok = false;
            if (queued || !CallCheckReq(r, gd, checkCost, ok) || !ok) continue;
            std::string name = w.TemplateName(sid);
            std::replace(name.begin(), name.end(), ' ', '_');
            out += " " + sid + "=" + name;
            if (++found >= n) break;
        }
        return out;
    }
    if (cmd == "workreq") {   // workreq <squadIndex> <template sid> [shift]: that member alone works the instance nearest to squad member 0 (OPERATE_MACHINERY, as a right click; "shift": Shift held, a permanent job)
        size_t idx = 0;
        std::string sid, opt;
        in >> idx >> sid >> opt;
        const bool shift = opt == "shift";
        kc::Vec3 anchor, p;
        if (!squadAt(0, anchor)) return "err no squad";
        kenshi::Character* c = squadAt(idx, p);
        if (!c) return "err no such squad member";
        std::vector<void*> objs;
        kenshi::ObjectsNear(anchor, 4000.0f, objs);
        void* best = nullptr;
        float bestD = 1e30f;
        kc::Vec3 bp;
        for (void* o : objs) {
            std::string s2;
            kc::Vec3 op;
            if (!IsBuilding(o) || !kenshi::ObjectTemplate(o, s2) || s2 != sid || !kenshi::ObjectPosition(o, op)) continue;
            const float d = Dist(op, anchor);
            if (d < bestD) { bestD = d; best = o; bp = op; }
        }
        if (!best) return "err no " + sid + " within 4000";
        bool ok = false;
        if (shift && !kenshi::ShiftFlagFound()) return "err the game's Shift flag is not where 1.0.68 has it";
        const int before = kenshi::PermajobCount(c);
        kenshi::WithSelection(c, [&] {
            kenshi::ScopedShift held(shift);   // the key as the game's key handler sets it (read by our hook and the game)
            ok = kenshi::CallNewPlayerTaskOn(87, best, bp, kenshi::FurnitureParent(best));
        });
        return (ok ? "ok " : "err ") + w.TemplateName(sid) + " at " + std::to_string(int(Dist(bp, p))) + " jobs " + std::to_string(before) + "->" +
               std::to_string(kenshi::PermajobCount(c));
    }
    if (cmd == "researchreq") {   // the research window's add / remove (client: becomes a request)
        std::string what, sid;
        in >> what >> sid;
        void* gd = kenshi::GameDataBySid(sid);
        if (!r || !gd) return "err unknown tech " + sid;
        if (what == "queue") {
            bool ok = false;
            CallBoolGd(kenshi::FnResearchStart, r, gd, ok);
            return std::string("ok ") + (s.isHost() ? (ok ? "queued" : "refused") : "asked");
        }
        if (what == "cancel") return CallVoidGd(kenshi::FnResearchStop, r, gd) ? (s.isHost() ? "ok cancelled" : "ok asked") : "err call failed";
        return "err queue|cancel";
    }
    if (cmd == "researchprogress") {   // (host) the researchers' work, sped up: completes the first queued tech
        float amount = 0;
        in >> amount;
        if (!s.isHost()) return "err host only";
        if (!r || !(amount > 0)) return "err no research / amount";
        for (int i = 0; i < 50; ++i) CallProgress(r, amount / 50.0f);
        return "ok";
    }
    if (cmd == "blueprintreq") {   // a blueprint the member carries is read (client: becomes a request)
        size_t idx = 0;
        std::string part;
        in >> idx >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        kc::Vec3 p;
        kenshi::Character* c = squadAt(idx, p);
        void* inv = c ? kenshi::InventoryOfHolder(c) : nullptr;
        if (!inv) return "err no such squad member";
        std::vector<kc::ItemState> items;
        kenshi::ReadInventory(c, items);
        for (const auto& it : items) {
            const std::string name = Lower(w.TemplateName(it.templateSid));
            if (name.find(Lower(part)) == std::string::npos && it.templateSid != part) continue;
            void* item = kenshi::FindItemIn(c, it);
            if (!item) continue;
            bool learnt = false;
            CallLearn(item, learnt);
            if (s.isHost() && learnt) CallRemoveItem(kenshi::OwningInventory(c, item), item);
            return "ok " + it.templateSid + (s.isHost() ? (learnt ? " learnt" : " refused") : " asked");
        }
        return "err no such item";
    }
    if (cmd == "craftlist") {   // what the faction can craft (Research::craftableThings), by sid
        size_t n = 10;
        in >> n;
        if (!r) return "err no research";
        std::vector<void*> gds;
        CraftableThings(r, gds, 2000);
        std::vector<std::string> sids;
        for (void* gd : gds)
            if (std::string sid = SidOf(gd); !sid.empty()) sids.push_back(sid);
        std::sort(sids.begin(), sids.end());
        sids.erase(std::unique(sids.begin(), sids.end()), sids.end());
        std::string out = "ok " + std::to_string(sids.size());
        for (size_t i = 0; i < sids.size() && i < n; ++i) {
            std::string name = w.TemplateName(sids[i]);
            std::replace(name.begin(), name.end(), ' ', '_');
            out += " " + sids[i] + "=" + name;
        }
        return out;
    }
    if (cmd == "machineselect") {   // machineselect <part>: a click on that machine (Building::select): does its inventory panel open?
        std::string part;
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        kc::Vec3 me;
        if (!squadAt(0, me)) return "err no squad";
        void* b = w.NearestMachine(me, part, 3000.0f);
        kc::MachineState st;
        if (!b || !w.ReadMachineState(b, st, nullptr)) return "err no machine " + part;
        const int before = kenshi::OpenInventoryWindows();
        bool called = false;
        {
            HostCallScope scope;
            called = CallSelect(b);
        }
        const int after = kenshi::OpenInventoryWindows();
        return std::string(called ? "ok " : "err select faulted ") + kc::Session::MachineKey(st.sid, st.pos) + " ours=" + (w.IsPlayerBuilding(b) ? "1" : "0") +
               " windows=" + std::to_string(before) + "->" + std::to_string(after) + " panel=" + (kenshi::InventoryWindowShows(b) ? "1" : "0");
    }
    if (cmd == "machine" || cmd == "machineinv" || cmd == "machinereq") {
        std::string part;
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        kc::Vec3 me;
        if (!squadAt(0, me)) return "err no squad";
        void* b = w.NearestMachine(me, part, 3000.0f);
        kc::MachineState st;
        std::vector<kc::Handle> ops;
        if (!b || !w.ReadMachineState(b, st, &ops)) return "err no machine " + part;
        const std::string key = kc::Session::MachineKey(st.sid, st.pos);
        if (cmd == "machineinv") {
            kc::Handle h;
            std::vector<kc::ItemState> items;
            if (!kenshi::ObjectHandle(b, h) || !w.ReadInventory(h, items)) return "ok " + key + " stacks=0 total=0 ih=0";
            int total = 0;
            uint32_t hh = 2166136261u;
            for (const auto& it : items) {
                total += it.quantity;
                hh = Fnv(it.templateSid + "/" + it.section + "/" + std::to_string(it.quantity) + ";", hh);
            }
            std::ostringstream o;
            o << "ok " << key << " stacks=" << items.size() << " total=" << total << " ih=" << std::hex << hh;
            return o.str();
        }
        if (cmd == "machinereq") {
            std::string what, a1, a2;
            in >> what >> a1 >> a2;
            kc::MachineRequest req;
            req.sid = st.sid;
            req.pos = st.pos;
            if (what == "addcraft") { req.action = kc::MachineAction::AddCraft; req.baseSid = a1; req.materialSid = a2; }
            else if (what == "removecraft") { req.action = kc::MachineAction::RemoveCraft; req.index = std::atoi(a1.c_str()); }
            else if (what == "repeat") { req.action = kc::MachineAction::SetRepeat; req.value = a1 == "1"; }
            else if (what == "power") { req.action = kc::MachineAction::SetPower; req.value = a1 == "1"; }
            else if (what == "battery") { req.action = kc::MachineAction::SetBattery; req.value = a1 == "1"; }
            else return "err addcraft|removecraft|repeat|power|battery";
            if (s.isHost()) {
                std::string why;
                return w.ExecuteMachineRequest(req, kc::Handle{}, why) ? "ok done " + key : "err refused: " + why;
            }
            // client: the power switches through the building panel's own buttons (hooked); the
            // crafting window's add / remove / repeat as its hooks queue them
            if (req.action == kc::MachineAction::SetPower || req.action == kc::MachineAction::SetBattery) {
                uint8_t on = 0;
                Rd(b, req.action == kc::MachineAction::SetPower ? US_powerOn : US_battOn, on);
                if ((on != 0) != req.value) CallButton(req.action == kc::MachineAction::SetPower ? kenshi::FnTogglePowerButton : kenshi::FnToggleBattButton, b);
                return "ok asked " + key;
            }
            w.QueueMachineAsk(b, req.action, kenshi::GameDataBySid(req.baseSid), req.materialSid.empty() ? nullptr : kenshi::GameDataBySid(req.materialSid),
                              req.index, req.value);
            return "ok asked " + key;
        }
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "ok " << key << " ops=" << int(st.operatorCount) << "/" << int(st.maxOperators) << " names=";
        std::vector<std::string> names;
        for (const auto& h : ops) {
            const kc::Handle host = w.HostHandleOf(h);
            std::string n = w.CharacterNameOf(host.valid() ? host : h);
            std::replace(n.begin(), n.end(), ' ', '_');
            names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        for (size_t i = 0; i < names.size(); ++i) o << (i ? "," : "") << names[i];
        o << " flags=" << int(st.flags) << " power=" << st.power << " stored=" << st.stored << " prog=" << st.progress << " prod=" << st.production
          << " crafts=";
        for (size_t i = 0; i < st.crafts.size(); ++i) o << (i ? "," : "") << st.crafts[i].itemSid << ":" << st.crafts[i].progress;
        if (void* t = TownOf(b)) {
            o << " town=";
            for (int i = 0; i < 8; ++i) {
                float v = 0;
                Rd(t, TW_values + 4 * uintptr_t(i), v);
                o << (i ? "," : "") << v;
            }
        }
        return o.str();
    }
    handled = false;
    return {};
}

// 0.3.5 night-posture (A_FAIRE 16/10 point 7): a gone player's character is taken off every machine,
// training dummy or chair near it that still lists it as an operator (UseableStuff::stopOperating, the
// game's own removal from the operator set +0x3D0). Returns how many. Game thread, host.
int KenshiWorld::LeaveMachinesOf(kenshi::Character* c) {
    kc::Vec3 p;
    kc::Handle me;
    if (!c || !kenshi::GetPosition(c, p) || !kenshi::GetHandle(c, me)) return 0;
    std::vector<void*> objs;
    kenshi::ObjectsNear(p, 300.0f, objs);
    uint8_t hand[kenshi::off::HandSize];
    kenshi::MakeHand(me, hand);
    int n = 0;
    std::vector<kc::Handle> ops;
    for (void* o : objs) {
        if (!IsUseable(o)) continue;
        ReadOperators(o, ops);
        if (std::find(ops.begin(), ops.end(), me) == ops.end()) continue;
        if (CallStopOperating(o, hand)) ++n;
    }
    return n;
}

// ---------------------------------------------------------------- hooks
// On clients the research, the crafting orders and the power of the players' towns are the host's:
// the game may not change them by itself; our own calls (HostCallScope) apply the host's state.
namespace workshophooks {

using StartFn = bool (*)(void*, void*);
using StopFn = void (*)(void*, void*);
using PayFn = bool (*)(void*, void*);
using ProgressFn = void (*)(void*, float);
using LearnFn = bool (*)(void*);
using AddCraftFn = void* (*)(void*, void*, void*, float, int);
using RemoveCraftFn = void (*)(void*, int);
using WidgetFn = void (*)(void*, void*);
using RemovedFn = void (*)(void*, int, void*);
using GridFn = void (*)(void*);
using ButtonFn = void (*)(void*, void*);
StartFn o_start = nullptr;
StopFn o_stop = nullptr;
PayFn o_pay = nullptr;
ProgressFn o_progress = nullptr;
LearnFn o_learn = nullptr;
AddCraftFn o_addCraft = nullptr;
RemoveCraftFn o_removeCraft = nullptr;
WidgetFn o_queueAdd = nullptr;
WidgetFn o_queueRemove = nullptr;
RemovedFn o_queueRemoved = nullptr;
WidgetFn o_queueRepeat = nullptr;
GridFn o_grid = nullptr;
ButtonFn o_power = nullptr;
ButtonFn o_batt = nullptr;

bool Refused() { return KenshiWorld::ClientActive() && !InHostCall(); }

// the research window's add: the host queues it (and pays it once)
bool hk_start(void* r, void* gd) {
    if (!Refused()) return o_start(r, gd);
    if (KenshiWorld* w = TheWorld()) w->QueueResearchAsk(kc::ResearchAction::Queue, gd);
    return false;
}
// stopResearch has three callers: the two "remove" buttons of the research window, and
// refreshResearchList, which drops queued techs whose requirements fail (checkRequirements_andQueue).
// On a client only the buttons count (a request); the refresh's cleanup never runs there, even in our
// own replay: the queue is the host's.
constexpr uintptr_t kStopFromRefresh = 0x49A5B0;   // return address of the call in refreshResearchList
void hk_stop(void* r, void* gd) {
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - kenshi::Addr(0);
    if (KenshiWorld::ClientActive() && from == kStopFromRefresh) return;
    if (!Refused()) return o_stop(r, gd);
    if (KenshiWorld* w = TheWorld()) w->QueueResearchAsk(kc::ResearchAction::Cancel, gd);
}
// a client never consumes artifacts or books: our replay of the host's queue pays nothing
bool hk_pay(void* r, void* gd) {
    if (KenshiWorld::ClientActive()) return InHostCall();
    return o_pay(r, gd);
}
void hk_progress(void* r, float amount) {
    if (!Refused()) o_progress(r, amount);
}
// a blueprint read from the inventory: the host learns it (its game uses the blueprint up)
bool hk_learn(void* item) {
    if (!Refused()) return o_learn(item);
    if (KenshiWorld* w = TheWorld()) w->QueueBlueprintAsk(item);
    return false;
}
void* hk_addCraft(void* bench, void* base, void* mat, float progress, int crit) {
    if (Refused() && t_craftWindow > 0) {   // the crafting window's add: asked of the host
        if (KenshiWorld* w = TheWorld()) w->QueueMachineAsk(bench, kc::MachineAction::AddCraft, base, mat, 0, false);
        return nullptr;
    }
    void* ci = o_addCraft(bench, base, mat, progress, crit);
    if (ci && !KenshiWorld::ClientActive())
        if (KenshiWorld* w = TheWorld()) w->NoteCraftAdded(ci, base, mat);
    return ci;
}
void hk_removeCraft(void* bench, int index) {
    if (Refused() && t_craftWindow > 0) {
        if (KenshiWorld* w = TheWorld()) w->QueueMachineAsk(bench, kc::MachineAction::RemoveCraft, nullptr, nullptr, index, false);
        return;
    }
    o_removeCraft(bench, index);
}
void hk_queueAdd(void* q, void* sender) {
    CraftWindowScope s;
    o_queueAdd(q, sender);
}
void hk_queueRemove(void* q, void* sender) {
    CraftWindowScope s;
    o_queueRemove(q, sender);
}
void hk_queueRemoved(void* q, int index, void* data) {
    CraftWindowScope s;
    o_queueRemoved(q, index, data);
}
void hk_queueRepeat(void* q, void* sender) {
    if (!Refused()) return o_queueRepeat(q, sender);
    void* bench = CraftingBenchOfQueue(q);
    uint8_t rep = 0;
    if (bench && Rd(bench, CB_repeat, rep))
        if (KenshiWorld* w = TheWorld()) w->QueueMachineAsk(bench, kc::MachineAction::SetRepeat, nullptr, nullptr, 0, rep == 0);
}
void hk_grid(void* town) {
    if (KenshiWorld::ClientActive() && !InHostCall())
        if (KenshiWorld* w = TheWorld(); w && w->TownPowerFromHost(town)) return;
    o_grid(town);
}
void hk_power(void* us, void* line) {
    if (!Refused()) return o_power(us, line);
    uint8_t on = 0;
    if (Rd(us, US_powerOn, on))
        if (KenshiWorld* w = TheWorld()) w->QueueMachineAsk(us, kc::MachineAction::SetPower, nullptr, nullptr, 0, on == 0);
}
void hk_batt(void* us, void* line) {
    if (!Refused()) return o_batt(us, line);
    uint8_t on = 0;
    if (Rd(us, US_battOn, on))
        if (KenshiWorld* w = TheWorld()) w->QueueMachineAsk(us, kc::MachineAction::SetBattery, nullptr, nullptr, 0, on == 0);
}

} // namespace workshophooks

} // namespace kcp
