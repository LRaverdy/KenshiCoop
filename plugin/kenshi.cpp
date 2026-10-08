#include "kenshi.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>

namespace kenshi {

const FunctionSig kFunctions[FnCount] = {
    {"GameWorld::mainLoop_GPUSensitiveStuff", 0x788A00, {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x70, 0x48}},
    {"PlayerInterface::playerMove", 0x7FA850, {0x40, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x83}},
    {"PlayerInterface::addOrderSelectedCharacters", 0x7F9E20, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x41, 0x56, 0x48, 0x8D}},
    {"PlayerInterface::newPlayerTaskSelectedCharacters", 0x7FA650, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57}},
    {"PlayerInterface::setOrderSelectedCharacters", 0x7F3880, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48}},
    {"PlayerInterface::stopCharactersMovement", 0x7F6470, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x28, 0x02, 0x00}},
    {"Character::playerMoveOrderDefault", 0x5D22B0, {0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83}},
    {"AI::update4Frame", 0x5965B0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0x79, 0x10, 0x00, 0x0F}},
    {"AI::periodicUpdate", 0x5112B0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"GameWorld::setFrameSpeedMultiplier", 0x787CB0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48}},
    {"GameWorld::userPause", 0x787FB0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89}},
    {"HandleTable::resolve", 0x2676E0, {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    {"GameWorld::togglePause", 0x787D40, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48}},
    {"MedicalSystem::applyDamage", 0x64F300, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"MedicalSystem::knockout", 0x644980, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0xF3, 0x0F, 0x10, 0x05, 0xA6, 0xF4}},
    {"Character::declareDead", 0x7A6200, {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    {"SaveManager::getSingleton", 0x37DD00, {0x48, 0x83, 0xEC, 0x38, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF}},
    {"SaveManager::save", 0x47B920, {0x40, 0x57, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x48, 0xC7, 0x44}},
    {"SaveManager::load", 0x47B480, {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x70, 0x01, 0x00, 0x00, 0x48}},
};

namespace {

uintptr_t g_base = 0;

// ---- SEH-guarded primitives. These functions hold no C++ objects with destructors (required
// for __try) and are the only places that touch raw game memory.

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Rd(const void* p, uintptr_t offset, T& out) {
    if (!p) return false;
    return SafeCopy(&out, reinterpret_cast<const uint8_t*>(p) + offset, sizeof(T));
}

uintptr_t Vtable(const void* obj) {
    uintptr_t vt = 0;
    return Rd(obj, 0, vt) ? vt : 0;
}

using VirtGetVec3 = void* (*)(void* self, float* ret);
bool CallGetVec3(void* fn, void* self, float* ret) {
    __try {
        reinterpret_cast<VirtGetVec3>(fn)(self, ret);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtBool = bool (*)(void* self);
bool CallBool(void* fn, void* self, bool& out) {
    __try {
        out = reinterpret_cast<VirtBool>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtTeleport = void (*)(void* self, const float* pos, const float* quat);
bool CallTeleport(void* fn, void* self, const float* pos, const float* quat) {
    __try {
        reinterpret_cast<VirtTeleport>(fn)(self, pos, quat);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtSetDest = void (*)(void* self, const float* pos, int priority, bool notVertical);
bool CallSetDest(void* fn, void* self, const float* pos, int prio) {
    __try {
        reinterpret_cast<VirtSetDest>(fn)(self, pos, prio, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtVoid = void (*)(void* self);
bool CallVoid(void* fn, void* self) {
    __try {
        reinterpret_cast<VirtVoid>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnFloat = void (*)(void* self, float v);
bool CallFloat(void* fn, void* self, float v) {
    __try {
        reinterpret_cast<FnFloat>(fn)(self, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnBoolArg = void (*)(void* self, bool v);
bool CallBoolArg(void* fn, void* self, bool v) {
    __try {
        reinterpret_cast<FnBoolArg>(fn)(self, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnResolve = void* (*)(void* table, const void* hand, bool flag);
void* CallResolve(void* fn, void* table, const void* hand) {
    __try {
        return reinterpret_cast<FnResolve>(fn)(table, hand, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

using FnMedFloat = void (*)(void* med, float v);
bool CallMedFloat(void* fn, void* med, float v) {
    __try {
        reinterpret_cast<FnMedFloat>(fn)(med, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Wr(void* p, uintptr_t offset, const T& v) {
    if (!p) return false;
    return SafeCopy(reinterpret_cast<uint8_t*>(p) + offset, &v, sizeof(T));
}

// Reads a boost::unordered_set of pointers (Kenshi's layout: see off::US_*).
void ReadPointerSet(const void* set, std::vector<void*>& out, size_t cap) {
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(set, off::US_size, size) || size == 0 || size > cap) return;
    if (!Rd(set, off::US_bucketCount, bucketCount) || !Rd(set, off::US_buckets, buckets) || !buckets || bucketCount > (1u << 24)) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* v = nullptr;
        if (!Rd(node, off::USNode_value, v)) return;
        out.push_back(v);
        if (!Rd(node, off::USNode_next, node)) return;
    }
}

// std::string as compiled by Visual Studio 2010 (Kenshi's compiler): 40 bytes,
// { char buf[16] | char* ptr; size_t size; size_t capacity; allocator (1 byte, padded) }.
// Strings shorter than 16 chars live inline, so building one never allocates, and the game
// never has to free memory owned by a different C runtime.
struct GameString {
    alignas(8) uint8_t raw[0x28] = {};
};
static_assert(sizeof(GameString) == 0x28, "VS2010 std::string is 40 bytes");
bool MakeGameString(const std::string& s, GameString& out) {
    if (s.size() > 15) return false;
    std::memset(out.raw, 0, sizeof(out.raw));
    std::memcpy(out.raw, s.data(), s.size());
    const uint64_t size = s.size(), cap = 15;
    std::memcpy(out.raw + 0x10, &size, 8);
    std::memcpy(out.raw + 0x18, &cap, 8);
    return true;
}
bool ReadGameString(const void* p, std::string& out) {
    uint64_t size = 0, cap = 0;
    if (!Rd(p, 0x10, size) || !Rd(p, 0x18, cap) || size > cap || cap > 4096) return false;
    const void* chars = p;
    if (cap >= 16 && !Rd(p, 0, chars)) return false;
    out.resize(size_t(size));
    return size == 0 || SafeCopy(out.data(), chars, size_t(size));
}

using FnNoArgPtr = void* (*)();
void* CallNoArgPtr(void* fn) {
    __try {
        return reinterpret_cast<FnNoArgPtr>(fn)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
using FnStrBool = void (*)(void* self, const void* str, bool b);
bool CallStrBool(void* fn, void* self, const void* str, bool b) {
    __try {
        reinterpret_cast<FnStrBool>(fn)(self, str, b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using FnStr = void (*)(void* self, const void* str);
bool CallStr(void* fn, void* self, const void* str) {
    __try {
        reinterpret_cast<FnStr>(fn)(self, str);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* VSlot(const void* obj, uintptr_t slotOffset) {
    const uintptr_t vt = Vtable(obj);
    if (!vt) return nullptr;
    void* fn = nullptr;
    return Rd(reinterpret_cast<void*>(vt), slotOffset, fn) ? fn : nullptr;
}

bool Finite(const kc::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void* Medical(const Character* c) {
    return IsCharacter(c) ? reinterpret_cast<uint8_t*>(const_cast<Character*>(c)) + off::CH_medical : nullptr;
}

void* Movement(const Character* c) {
    void* m = nullptr;
    if (!IsCharacter(c) || !Rd(c, off::CH_movement, m) || !m) return nullptr;
    return Vtable(m) == Addr(rva::VtCharMovement) ? m : nullptr;
}

bool ReadHandle(const void* p, kc::Handle& h) {
    uint8_t raw[off::HandSize];
    if (!SafeCopy(raw, p, sizeof(raw))) return false;
    std::memcpy(&h.type, raw + off::H_type, 4);
    std::memcpy(&h.container, raw + off::H_container, 4);
    std::memcpy(&h.containerSerial, raw + off::H_containerSerial, 4);
    std::memcpy(&h.index, raw + off::H_index, 4);
    std::memcpy(&h.serial, raw + off::H_serial, 4);
    return true;
}

} // namespace

bool Init(std::string* err) {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_base) { if (err) *err = "cannot find kenshi_x64.exe module"; return false; }
    for (const auto& f : kFunctions) {
        uint8_t live[sizeof(f.prologue)];
        if (!SafeCopy(live, reinterpret_cast<void*>(g_base + f.rva), sizeof(live)) ||
            std::memcmp(live, f.prologue, sizeof(live)) != 0) {
            if (err) *err = std::string("unexpected code at ") + f.name + " (game updated or another mod patched it)";
            return false;
        }
    }
    return true;
}

uintptr_t Base() { return g_base; }
uintptr_t Addr(uintptr_t r) { return g_base + r; }
void* FnAddr(Fn f) { return reinterpret_cast<void*>(g_base + kFunctions[f].rva); }

GameWorld* World() {
    auto* w = reinterpret_cast<GameWorld*>(Addr(rva::GameWorldInstance));
    return Vtable(w) == Addr(rva::VtGameWorld) ? w : nullptr;
}

PlayerInterface* Player() {
    GameWorld* w = World();
    void* p = nullptr;
    if (!w || !Rd(w, off::GW_player, p) || !p) return nullptr;
    return Vtable(p) == Addr(rva::VtPlayerInterface) ? static_cast<PlayerInterface*>(p) : nullptr;
}

bool IsCharacter(const void* obj) {
    if (!obj) return false;
    const uintptr_t vt = Vtable(obj);
    return vt && (vt == Addr(rva::VtCharacterHuman) || vt == Addr(rva::VtCharacter) || vt == Addr(rva::VtCharacterAnimal));
}

void PlayerCharacters(std::vector<Character*>& out) {
    out.clear();
    PlayerInterface* pi = Player();
    if (!pi) return;
    const auto* lk = reinterpret_cast<const uint8_t*>(pi) + off::PI_playerCharacters;
    uint32_t count = 0;
    Character** data = nullptr;
    if (!Rd(lk, off::LK_count, count) || !Rd(lk, off::LK_data, data) || !data || count > 4096) return;
    for (uint32_t i = 0; i < count; ++i) {
        Character* c = nullptr;
        if (Rd(data, i * sizeof(void*), c) && IsCharacter(c)) out.push_back(c);
    }
}

void SelectedHandles(std::vector<kc::Handle>& out) {
    out.clear();
    PlayerInterface* pi = Player();
    if (!pi) return;
    const auto* set = reinterpret_cast<const uint8_t*>(pi) + off::PI_selectedCharacters;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(set, off::US_size, size) || size == 0 || size > 4096) return;
    if (!Rd(set, off::US_bucketCount, bucketCount) || !Rd(set, off::US_buckets, buckets) || !buckets || bucketCount > (1u << 24)) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        kc::Handle h;
        if (!ReadHandle(reinterpret_cast<uint8_t*>(node) + off::USNode_value, h)) return;
        out.push_back(h);
        if (!Rd(node, off::USNode_next, node)) return;
    }
}

void ActiveCharacters(std::vector<Character*>& out) {
    out.clear();
    GameWorld* w = World();
    if (!w) return;
    std::vector<void*> raw;
    ReadPointerSet(reinterpret_cast<const uint8_t*>(w) + off::GW_charUpdateList, raw, 65536);
    for (void* p : raw) if (IsCharacter(p)) out.push_back(static_cast<Character*>(p));
}

Character* Resolve(const kc::Handle& h) {
    // Build a real `hand` (its vtable pointer is copied from a live one) and ask the game.
    static uintptr_t handVtable = 0;
    if (!handVtable) {
        std::vector<Character*> squad;
        PlayerCharacters(squad);
        if (squad.empty() || !Rd(squad.front(), off::RO_handle, handVtable) || !handVtable) return nullptr;
    }
    alignas(8) uint8_t raw[off::HandSize] = {};
    std::memcpy(raw, &handVtable, 8);
    std::memcpy(raw + off::H_type, &h.type, 4);
    std::memcpy(raw + off::H_container, &h.container, 4);
    std::memcpy(raw + off::H_containerSerial, &h.containerSerial, 4);
    std::memcpy(raw + off::H_index, &h.index, 4);
    std::memcpy(raw + off::H_serial, &h.serial, 4);
    void* obj = CallResolve(FnAddr(FnHandleResolve), reinterpret_cast<void*>(Addr(rva::HandleTable)), raw);
    if (!IsCharacter(obj)) return nullptr;
    kc::Handle check;   // the resolver must hand back exactly that object
    return GetHandle(static_cast<Character*>(obj), check) && check == h ? static_cast<Character*>(obj) : nullptr;
}

bool GetHandle(const Character* c, kc::Handle& out) {
    return IsCharacter(c) && ReadHandle(reinterpret_cast<const uint8_t*>(c) + off::RO_handle, out);
}

bool GetPosition(Character* c, kc::Vec3& out) {
    if (!IsCharacter(c)) return false;
    void* fn = VSlot(c, slot::RO_getPosition);
    float v[3] = {};
    if (!fn || !CallGetVec3(fn, c, v)) return false;
    out = {v[0], v[1], v[2]};
    return Finite(out);
}

bool GetRotation(const Character* c, kc::Quat& out) {
    float q[4];
    if (!IsCharacter(c) || !Rd(c, off::RO_rot, q)) return false;
    out = {q[0], q[1], q[2], q[3]};
    return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]);
}

bool GetMovement(const Character* c, kc::Vec3& dest, bool& moving, float& speed) {
    void* m = Movement(c);
    float d[3];
    if (!m || !Rd(m, off::CM_destination, d) || !Rd(m, off::CM_currentlyMoving, moving) || !Rd(m, off::CM_currentSpeed, speed)) return false;
    dest = {d[0], d[1], d[2]};
    return Finite(dest) && std::isfinite(speed);
}

bool IsDown(Character* c) {
    void* fn = VSlot(c, slot::RO_isUnconcious);
    bool v = false;
    return IsCharacter(c) && fn && CallBool(fn, c, v) && v;
}

Character* AICharacter(const AI* ai) {
    Character* c = nullptr;
    if (!ai || Vtable(ai) != Addr(rva::VtAI) || !Rd(ai, off::AI_me, c)) return nullptr;
    return IsCharacter(c) ? c : nullptr;
}

Character* MedicalCharacter(const void* medical) {
    Character* c = nullptr;
    if (!medical || !Rd(medical, off::MS_me, c) || !IsCharacter(c)) return nullptr;
    return Medical(c) == medical ? c : nullptr;
}

bool ReadVitals(Character* c, kc::EntityVitals& out) {
    void* m = Medical(c);
    if (!m) return false;
    bool unc = false, dead = false;
    if (!Rd(m, off::MS_blood, out.blood) || !Rd(m, off::MS_koTimer, out.koTimer) ||
        !Rd(m, off::MS_unconscious, unc) || !Rd(m, off::MS_dead, dead))
        return false;
    if (!std::isfinite(out.blood) || !std::isfinite(out.koTimer)) return false;
    out.flags = uint8_t((unc ? kc::kVitUnconscious : 0) | (dead ? kc::kVitDead : 0));
    out.parts.clear();
    const auto* lk = reinterpret_cast<const uint8_t*>(m) + off::MS_anatomy;
    uint32_t count = 0;
    void** data = nullptr;
    if (!Rd(lk, off::LK_count, count) || !Rd(lk, off::LK_data, data) || !data || count > kc::kMaxBodyParts) return true;
    for (uint32_t i = 0; i < count; ++i) {
        void* part = nullptr;
        kc::PartVitals pv;
        if (!Rd(data, i * sizeof(void*), part) || !part || !Rd(part, off::HP_flesh, pv.flesh) ||
            !Rd(part, off::HP_stun, pv.stun) || !Rd(part, off::HP_bandage, pv.bandage) ||
            !std::isfinite(pv.flesh) || !std::isfinite(pv.stun) || !std::isfinite(pv.bandage)) {
            out.parts.clear();
            return true;
        }
        out.parts.push_back(pv);
    }
    return true;
}

bool WriteVitals(Character* c, const kc::EntityVitals& v) {
    void* m = Medical(c);
    if (!m) return false;
    Wr(m, off::MS_blood, v.blood);
    Wr(m, off::MS_koTimer, v.koTimer);
    const auto* lk = reinterpret_cast<const uint8_t*>(m) + off::MS_anatomy;
    uint32_t count = 0;
    void** data = nullptr;
    // same save + same race => same anatomy; anything else is left alone
    if (!Rd(lk, off::LK_count, count) || !Rd(lk, off::LK_data, data) || !data || count != v.parts.size()) return true;
    for (uint32_t i = 0; i < count; ++i) {
        void* part = nullptr;
        if (!Rd(data, i * sizeof(void*), part) || !part) continue;
        Wr(part, off::HP_flesh, v.parts[i].flesh);
        Wr(part, off::HP_stun, v.parts[i].stun);
        Wr(part, off::HP_bandage, v.parts[i].bandage);
    }
    return true;
}

bool IsDead(Character* c) {
    bool v = false;
    void* m = Medical(c);
    return m && Rd(m, off::MS_dead, v) && v;
}

bool IsUnconscious(Character* c) {
    bool v = false;
    void* m = Medical(c);
    return m && Rd(m, off::MS_unconscious, v) && v;
}

bool CallDeclareDead(Character* c) {
    return IsCharacter(c) && CallVoid(FnAddr(FnDeclareDead), c);
}

bool CallKnockout(Character* c) {
    void* m = Medical(c);
    return m && CallMedFloat(FnAddr(FnMedKnockout), m, 0.0f);
}

bool GetGameHours(double& out) {
    void* clock = nullptr;
    if (!Rd(reinterpret_cast<void*>(Addr(rva::GameClockOwner)), 0, clock) || !clock) return false;
    return Rd(clock, off::Clock_hours, out) && std::isfinite(out) && out >= 0;
}

bool SetGameHours(double hours) {
    void* clock = nullptr;
    if (!std::isfinite(hours) || hours < 0) return false;
    if (!Rd(reinterpret_cast<void*>(Addr(rva::GameClockOwner)), 0, clock) || !clock) return false;
    return Wr(clock, off::Clock_hours, hours);
}

namespace {
void* SaveManagerInstance() { return CallNoArgPtr(FnAddr(FnSaveManagerGet)); }
}

bool SaveManagerBusy() {
    void* sm = SaveManagerInstance();
    int signal = 0;
    return !sm || !Rd(sm, off::SM_signal, signal) || signal != 0;
}

bool SaveFolder(std::string& out) {
    void* sm = SaveManagerInstance();
    bool user = false;
    if (!sm || !Rd(reinterpret_cast<void*>(Addr(rva::SaveUsesUserPath)), 0, user)) return false;
    return ReadGameString(reinterpret_cast<uint8_t*>(sm) + (user ? off::SM_userSavePath : off::SM_localSavePath), out) && !out.empty();
}

bool RequestSave(const std::string& name, std::string* folderOut) {
    void* sm = SaveManagerInstance();
    GameString gs;
    if (!sm || !MakeGameString(name, gs) || SaveManagerBusy()) return false;
    if (!CallStrBool(FnAddr(FnSaveManagerSave), sm, gs.raw, false)) return false;
    // save() silently refuses in some states (editor, another operation): it then leaves no request
    if (!SaveManagerBusy()) return false;
    return !folderOut || ReadGameString(reinterpret_cast<uint8_t*>(sm) + off::SM_location, *folderOut);
}

bool RequestLoad(const std::string& name) {
    void* sm = SaveManagerInstance();
    GameString gs;
    if (!sm || !MakeGameString(name, gs) || SaveManagerBusy()) return false;
    return CallStr(FnAddr(FnSaveManagerLoad), sm, gs.raw);
}

float GetFrameSpeed() {
    float v = 1.0f;
    GameWorld* w = World();
    return (w && Rd(w, off::GW_frameSpeedMult, v) && std::isfinite(v)) ? v : 1.0f;
}

bool GetPaused() {
    bool v = false;
    GameWorld* w = World();
    return w && Rd(w, off::GW_paused, v) && v;
}

bool Teleport(Character* c, const kc::Vec3& pos, const kc::Quat& rot) {
    void* m = Movement(c);
    if (!m || !Finite(pos)) return false;
    void* fn = VSlot(m, slot::CM_setPositionDirectionAndTeleport);
    const float p[3] = {pos.x, pos.y, pos.z};
    const float q[4] = {rot.w, rot.x, rot.y, rot.z};
    return fn && CallTeleport(fn, m, p, q);
}

bool SetDestination(Character* c, const kc::Vec3& dest) {
    void* m = Movement(c);
    if (!m || !Finite(dest)) return false;
    void* fn = VSlot(m, slot::CM_setDestination);
    const float p[3] = {dest.x, dest.y, dest.z};
    return fn && CallSetDest(fn, m, p, HIGH_PRIORITY);
}

bool Halt(Character* c) {
    void* m = Movement(c);
    void* fn = m ? VSlot(m, slot::CM_halt) : nullptr;
    return fn && CallVoid(fn, m);
}

bool CallSetFrameSpeed(float speed) {
    GameWorld* w = World();
    return w && std::isfinite(speed) && CallFloat(FnAddr(FnSetFrameSpeedMultiplier), w, speed);
}

bool CallTogglePause(bool paused) {
    GameWorld* w = World();
    return w && CallBoolArg(FnAddr(FnTogglePause), w, paused);
}

bool CallUserPause(bool paused) {
    GameWorld* w = World();
    return w && CallBoolArg(FnAddr(FnUserPause), w, paused);
}

} // namespace kenshi
