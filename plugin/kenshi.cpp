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

void* VSlot(const void* obj, uintptr_t slotOffset) {
    const uintptr_t vt = Vtable(obj);
    if (!vt) return nullptr;
    void* fn = nullptr;
    return Rd(reinterpret_cast<void*>(vt), slotOffset, fn) ? fn : nullptr;
}

bool Finite(const kc::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

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

bool CallUserPause(bool paused) {
    GameWorld* w = World();
    return w && CallBoolArg(FnAddr(FnUserPause), w, paused);
}

} // namespace kenshi
