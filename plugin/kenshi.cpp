#include "kenshi.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>

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
    {"RootObjectFactory::createRandomCharacter", 0x5836E0, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    {"GameWorld::destroy(RootObject*)", 0x799AF0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"Character::endCombatMode", 0x5C91C0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8D, 0x05, 0x03, 0xC1, 0x0B}},
    {"Character::ragdollMode", 0x5CBD60, {0x45, 0x85, 0xC0, 0x0F, 0x84, 0x13, 0x02, 0x00, 0x00, 0x44, 0x89, 0x44}},
    {"WeatherRegion::updateBT", 0x9DDE50, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x79, 0x38, 0x00, 0x48}},
    {"Season::getNewWeather", 0x9DD980, {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41}},
    {"WeatherInstance::setupWeather", 0x9DCF60, {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48}},
    {"RootObjectFactory::createItem", 0x580750, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    {"ForgottenGUI::showTradeWindow", 0x791830, {0x44, 0x89, 0x49, 0x58, 0x8B, 0x42, 0x08, 0x89, 0x41, 0x68, 0x8B, 0x42}},
    {"Character::isRagdoll", 0x7D1440, {0x48, 0x83, 0xEC, 0x28, 0x80, 0xB9, 0xD4, 0x03, 0x00, 0x00, 0x00, 0x74}},
    {"PlayerInterface::recruit", 0x692820, {0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"PlayerInterface::addTaskNearestSelectedCharacter", 0x7FAE70, {0x40, 0x53, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC8}},
    {"PlayerInterface::addJobSelectedCharacters", 0x7F5A90, {0x40, 0x53, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x8B}},
    {"EffectHandler::EffectHandler", 0x1034F0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x70}},
    {"EffectHandler::affectObjects", 0x1020E0, {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x90}},
    {"EffectHandler::stop", 0x100B00, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xC7, 0x41, 0x58}},
    {"WeatherRegion::updateWeatherEffects", 0x9DCAF0, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41}},
    {"AnimationClass::startCombatAnimation", 0x5B7800, {0x40, 0x53, 0x55, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00}},
    {"AnimationClass::runCombatAnimation", 0x5B7600, {0x40, 0x53, 0x55, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00}},
    {"AnimationClass::endCombatAnimation", 0x5B3C60, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0xA8, 0x00, 0x00}},
    {"AnimationClass::playAction(AnimationData*)", 0x51EAB0, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xBA, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C}},
    {"AnimationClass::stopAction()", 0x51DFA0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE}},
    {"AnimationClass::stopAction(name)", 0x51E0E0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    {"AnimationClass::startStumble", 0x520490, {0x48, 0x85, 0xD2, 0x74, 0x62, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48}},
    {"AnimationClass::endStumble", 0x51E840, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x28, 0x02, 0x00}},
    {"AnimationClass::setCombatMode", 0x51CC30, {0x84, 0xD2, 0x75, 0x0C, 0x83, 0xB9, 0x44, 0x02, 0x00, 0x00, 0x00, 0x0F}},
    {"AnimationClass::setCarryMode", 0x51C8D0, {0xC7, 0x81, 0x40, 0x02, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x44, 0x88}},
    {"CharacterHuman::drawWeapon", 0x5DBF80, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    {"CharacterHuman::sheatheWeapon", 0x5CC820, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE}},
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

using FnPtrArg2 = void (*)(void* self, void* arg);
bool CallPtrArg2(void* fn, void* self, void* arg) {
    __try {
        reinterpret_cast<FnPtrArg2>(fn)(self, arg);
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

bool HandleFromHand(const void* hand, kc::Handle& out) {
    return hand && ReadHandle(hand, out) && out.valid();
}

namespace {
constexpr int kTradeLooting = 2;   // TradeWindowType::TW_LOOTING
using FnShowTrade = void (*)(void* gui, const void* a, const void* b, int type);
bool CallShowTrade(void* fn, void* gui, const void* a, const void* b, int type) {
    __try {
        reinterpret_cast<FnShowTrade>(fn)(gui, a, b, type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool OpenLootWindow(Character* looter, Character* target) {
    if (!IsCharacter(looter) || !IsCharacter(target)) return false;
    // The game stores the request; its GUI opens the window on its next update.
    const auto* a = reinterpret_cast<const uint8_t*>(looter) + off::RO_handle;
    const auto* b = reinterpret_cast<const uint8_t*>(target) + off::RO_handle;
    return CallShowTrade(FnAddr(FnShowTradeWindow), reinterpret_cast<void*>(Addr(rva::TradeGui)), a, b, kTradeLooting);
}

void DeadBodies(std::vector<Character*>& out) {
    out.clear();
    GameWorld* w = World();
    if (!w) return;
    const auto* map = reinterpret_cast<const uint8_t*>(w) + off::GW_deathParade;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 65536) return;
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets || bucketCount > (1u << 24)) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* c = nullptr;
        if (Rd(node, off::HandMapNode_mapped, c) && IsCharacter(c)) out.push_back(static_cast<Character*>(c));
        if (!Rd(node, off::USNode_next, node)) break;
    }
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
    // The local medical update may decide on its own that the character died or fainted (it sets
    // these before asking for the death, which clients refuse): the host's state wins. Falling and
    // dying themselves are replayed separately, so only the "it did not happen" side is written.
    const bool no = false;
    bool flag = false;
    if (!(v.flags & kc::kVitDead) && Rd(m, off::MS_dead, flag) && flag) Wr(m, off::MS_dead, no);
    if (!(v.flags & (kc::kVitDead | kc::kVitUnconscious)) && v.koTimer <= 0 && Rd(m, off::MS_unconscious, flag) && flag)
        Wr(m, off::MS_unconscious, no);
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

bool IsRagdoll(Character* c) {
    bool v = false;
    return IsCharacter(c) && CallBool(FnAddr(FnIsRagdoll), c, v) && v;
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

namespace {
std::unordered_map<std::string, void*> g_gameDataBySid;   // built once per world
bool g_gameDataBuilt = false;

using FnCreateChar = void* (*)(void* factory, void* faction, const float* pos, void* owner, void* data, void* home, float age);
void* CallCreateChar(void* fn, void* factory, void* faction, const float* pos, void* data, float age) {
    __try {
        return reinterpret_cast<FnCreateChar>(fn)(factory, faction, pos, nullptr, data, nullptr, age);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
using FnDestroy = bool (*)(void* world, void* obj, bool justUnloaded, const char* info);
bool CallDestroy(void* fn, void* world, void* obj) {
    __try {
        return reinterpret_cast<FnDestroy>(fn)(world, obj, false, "KenshiCoop");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using VirtFloatRet = float (*)(void* self);
bool CallFloatRet(void* fn, void* self, float& out) {
    __try {
        out = reinterpret_cast<VirtFloatRet>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GameDataSid(const void* gd, std::string& out) {
    return gd && ReadGameString(reinterpret_cast<const uint8_t*>(gd) + off::GD_stringID, out) && !out.empty();
}

void BuildGameDataIndex() {
    g_gameDataBuilt = true;
    g_gameDataBySid.clear();
    GameWorld* w = World();
    if (!w) return;
    const auto* map = reinterpret_cast<const uint8_t*>(w) + off::GW_gamedataBySid;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 2000000) return;
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    size_t bad = 0;
    std::string key, sid;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* gd = nullptr;
        if (ReadGameString(reinterpret_cast<uint8_t*>(node) + off::MapNode_key, key) && Rd(node, off::MapNode_mapped, gd) && gd) {
            // the GameData must agree with its key: a cheap layout self-check
            if (GameDataSid(gd, sid) && sid == key) g_gameDataBySid.emplace(key, gd);
            else ++bad;
        }
        if (!Rd(node, off::USNode_next, node)) break;
    }
    if (bad > g_gameDataBySid.size() / 10) g_gameDataBySid.clear();   // layout not as expected: refuse to use it
}

void* FindGameData(const std::string& sid) {
    if (!g_gameDataBuilt) BuildGameDataIndex();
    auto it = g_gameDataBySid.find(sid);
    return it == g_gameDataBySid.end() ? nullptr : it->second;
}

void* FindFaction(const std::string& sid) {
    GameWorld* w = World();
    void* mgr = nullptr;
    if (!w || !Rd(w, off::GW_factionMgr, mgr) || !mgr) return nullptr;
    uint32_t count = 0;
    void** data = nullptr;
    if (!Rd(mgr, off::LK_count, count) || !Rd(mgr, off::LK_data, data) || !data || count > 10000) return nullptr;
    std::string s;
    for (uint32_t i = 0; i < count; ++i) {
        void* f = nullptr;
        void* gd = nullptr;
        if (Rd(data, i * sizeof(void*), f) && f && Rd(f, off::FAC_data, gd) && GameDataSid(gd, s) && s == sid) return f;
    }
    return nullptr;
}
} // namespace

size_t GameDataIndexSize() { return g_gameDataBySid.size(); }

void ResetLookupCaches() {
    g_gameDataBuilt = false;
    g_gameDataBySid.clear();
}

bool ReadSpawnSource(Character* c, kc::SpawnInfo& out) {
    if (!IsCharacter(c)) return false;
    void* gd = nullptr;
    void* faction = nullptr;
    void* fgd = nullptr;
    if (!Rd(c, off::RO_data, gd) || !GameDataSid(gd, out.templateSid)) return false;
    if (!Rd(c, off::RO_owner, faction) || !faction || !Rd(faction, off::FAC_data, fgd) || !GameDataSid(fgd, out.factionSid)) return false;
    if (!ReadGameString(reinterpret_cast<uint8_t*>(c) + off::RO_name, out.name)) out.name.clear();
    out.age = 0;
    if (void* fn = VSlot(c, slot::CH_getAge)) CallFloatRet(fn, c, out.age);
    if (!std::isfinite(out.age)) out.age = 0;
    return out.templateSid.size() <= kc::kMaxSidLen && out.factionSid.size() <= kc::kMaxSidLen;
}

Character* CreateCharacter(const kc::SpawnInfo& info, const kc::Vec3& pos, std::string* err) {
    GameWorld* w = World();
    void* factory = nullptr;
    if (!w || !Rd(w, off::GW_factory, factory) || !factory) { if (err) *err = "no factory"; return nullptr; }
    void* gd = FindGameData(info.templateSid);
    if (!gd) { if (err) *err = "unknown template " + info.templateSid; return nullptr; }
    void* faction = FindFaction(info.factionSid);
    if (!faction) { if (err) *err = "unknown faction " + info.factionSid; return nullptr; }
    const float p[3] = {pos.x, pos.y, pos.z};
    void* obj = CallCreateChar(FnAddr(FnCreateRandomCharacter), factory, faction, p, gd, info.age);
    if (!IsCharacter(obj)) {
        if (err) {
            int type = -1;
            Rd(gd, off::GD_type, type);
            std::string name;
            ReadGameString(reinterpret_cast<const uint8_t*>(gd) + off::GD_name, name);
            char buf[160];
            snprintf(buf, sizeof buf, "factory did not return a character (template type %d '%.60s', got %p vt+%llx)", type, name.c_str(), obj,
                     obj ? static_cast<unsigned long long>(Vtable(obj) - Addr(0)) : 0ull);
            *err = buf;
        }
        return nullptr;
    }
    // Same name as on the host when it fits an inline string (the game never frees it).
    GameString gs;
    if (!info.name.empty() && MakeGameString(info.name, gs))
        if (void* fn = VSlot(obj, slot::RO_setName)) CallStr(fn, obj, gs.raw);
    return static_cast<Character*>(obj);
}

namespace {
using FnRecruitSig = bool (*)(void* pi, void* c, bool editor);
bool CallRecruit(void* fn, void* pi, void* c) {
    __try {
        reinterpret_cast<FnRecruitSig>(fn)(pi, c, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

std::string ClassRvas(const Character* c) {
    void* m = nullptr;
    Rd(c, off::CH_movement, m);
    char buf[64];
    snprintf(buf, sizeof buf, "%llx/%llx", static_cast<unsigned long long>(Vtable(c) - g_base),
             static_cast<unsigned long long>(m ? Vtable(m) - g_base : 0));
    return buf;
}

namespace {
using FnAddTaskNearestSig = void (*)(void* pi, void* building, int task, void* subject, bool shift, const float* loc, bool noAnimals);
bool CallAddTaskNearestRaw(void* fn, void* pi, int task, void* subject, const float* loc) {
    __try {
        reinterpret_cast<FnAddTaskNearestSig>(fn)(pi, nullptr, task, subject, false, loc, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool CallAddTaskNearest(int task, Character* subject) {
    kc::Vec3 p;
    PlayerInterface* pi = Player();
    if (!pi || !GetPosition(subject, p)) return false;
    const float loc[3] = {p.x, p.y, p.z};
    return CallAddTaskNearestRaw(FnAddr(FnAddTaskNearest), pi, task, subject, loc);
}

bool CharacterName(const Character* c, std::string& out) {
    return IsCharacter(c) && ReadGameString(reinterpret_cast<const uint8_t*>(c) + off::RO_name, out);
}

Character* CreateRecruit(Character* model, const std::string& name, const kc::Vec3& pos, std::string* err) {
    kc::SpawnInfo info;
    if (!ReadSpawnSource(model, info)) { if (err) *err = "cannot read the squad's character template"; return nullptr; }
    info.name = name.substr(0, 15);
    Character* c = CreateCharacter(info, pos, err);
    if (!c) return nullptr;
    PlayerInterface* pi = Player();
    if (!pi || !CallRecruit(FnAddr(FnRecruit), pi, c)) { if (err) *err = "recruit() failed"; return nullptr; }
    return c;
}

namespace {
void* CombatOf(const Character* c) {
    void* body = nullptr;
    void* combat = nullptr;
    if (!IsCharacter(c) || !Rd(c, off::CH_body, body) || !body || !Rd(body, off::BODY_combat, combat) || !combat) return nullptr;
    const uintptr_t vt = Vtable(combat);
    return (vt == Addr(rva::VtCombatClass) || vt == Addr(rva::VtCombatClassAI)) ? combat : nullptr;
}
using FnInitCombat = bool (*)(void* self, const void* hand, int end, bool focused);
bool CallInitCombat(void* fn, void* self, const void* hand, int end) {
    __try {
        reinterpret_cast<FnInitCombat>(fn)(self, hand, end, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool ReadCombat(Character* c, kc::Handle& target) {
    void* combat = CombatOf(c);
    bool active = false;
    if (!combat || !Rd(combat, off::CC_active, active) || !active) return false;
    return ReadHandle(reinterpret_cast<uint8_t*>(combat) + off::CC_target, target) && target.valid();
}

bool StartCombat(Character* c, const kc::Handle& target) {
    void* combat = CombatOf(c);
    if (!combat || !target.valid()) return false;
    alignas(8) uint8_t raw[off::HandSize] = {};
    const uintptr_t vt = Addr(rva::VtHand);
    std::memcpy(raw, &vt, 8);
    std::memcpy(raw + off::H_type, &target.type, 4);
    std::memcpy(raw + off::H_container, &target.container, 4);
    std::memcpy(raw + off::H_containerSerial, &target.containerSerial, 4);
    std::memcpy(raw + off::H_index, &target.index, 4);
    std::memcpy(raw + off::H_serial, &target.serial, 4);
    void* fn = VSlot(combat, slot::CC_initCombatMode);
    return fn && CallInitCombat(fn, combat, raw, 0);
}

bool EndCombat(Character* c) {
    return CombatOf(c) && CallVoid(FnAddr(FnEndCombatMode), c);
}

namespace {
using FnBoolInt = void (*)(void* self, bool b, int i);
bool CallBoolInt(void* fn, void* self, bool b, int i) {
    __try {
        reinterpret_cast<FnBoolInt>(fn)(self, b, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using FnInt = void (*)(void* self, int i);
bool CallInt(void* fn, void* self, int i) {
    __try {
        reinterpret_cast<FnInt>(fn)(self, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
constexpr int kRagdollWhole = 1;
constexpr int kProneNormal = 0;
} // namespace

namespace {
// WeatherRegion
constexpr uintptr_t WR_biomeGroup = 0x0, WR_seasonsBegin = 0x8, WR_seasonsEnd = 0x10, WR_instance = 0x30, WR_season = 0x38,
                    WR_seasonIndex = 0x40, WR_seasonEnd = 0x44, WR_effectsDirty = 0x69, WR_newWeather = 0xB1;
constexpr uintptr_t ABG_data = 0x10;                                   // AreaBiomeGroup: GameData*
constexpr uintptr_t SEASON_data = 0x40, SEASON_weatherCount = 0x10, SEASON_weathers = 0x18;
constexpr uintptr_t WEATHER_data = 0x8;
// WeatherInstance
constexpr uintptr_t WI_weather = 0x8, WI_effectStrength = 0x10, WI_strength = 0x14, WI_windSpeed = 0x18, WI_windDir = 0x1C,
                    WI_buEnded = 0x28, WI_buStart = 0x2C, WI_buEnd = 0x30, WI_buSpeedStart = 0x34, WI_buSpeedEnd = 0x38,
                    WI_buAngleStart = 0x3C, WI_buAngleEnd = 0x40, WI_start = 0x44, WI_end = 0x48, WI_updWind = 0x4C, WI_time = 0x50;

bool SidAt(const void* obj, uintptr_t gdOffset, std::string& out) {
    void* gd = nullptr;
    return obj && Rd(obj, gdOffset, gd) && GameDataSid(gd, out);
}

using FnPtrArg = void (*)(void* self, void* arg);
bool CallPtrArg(void* fn, void* self, void* arg) {
    __try {
        reinterpret_cast<FnPtrArg>(fn)(self, arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool ReadRegionWeather(void* region, kc::RegionWeather& w) {
    void* biome = nullptr;
    void* season = nullptr;
    void* inst = nullptr;
    void* weather = nullptr;
    if (!region || !Rd(region, WR_biomeGroup, biome) || !SidAt(biome, ABG_data, w.regionSid)) return false;
    if (!Rd(region, WR_season, season) || !SidAt(season, SEASON_data, w.seasonSid)) return false;
    if (!Rd(region, WR_instance, inst) || !inst || !Rd(inst, WI_weather, weather) || !SidAt(weather, WEATHER_data, w.weatherSid)) return false;
    float dir[3];
    uint8_t ended = 0;
    bool ok = Rd(region, WR_seasonEnd, w.seasonEnd) && Rd(inst, WI_effectStrength, w.effectStrength) && Rd(inst, WI_strength, w.strength) &&
              Rd(inst, WI_windSpeed, w.windSpeed) && Rd(inst, WI_windDir, dir) && Rd(inst, WI_buEnded, ended) &&
              Rd(inst, WI_buStart, w.windBuildUpStart) && Rd(inst, WI_buEnd, w.windBuildUpEnd) && Rd(inst, WI_buSpeedStart, w.windBuildUpSpeedStart) &&
              Rd(inst, WI_buSpeedEnd, w.windBuildUpSpeedEnd) && Rd(inst, WI_buAngleStart, w.windBuildUpAngleStart) &&
              Rd(inst, WI_buAngleEnd, w.windBuildUpAngleEnd) && Rd(inst, WI_start, w.startMinutes) && Rd(inst, WI_end, w.endMinutes) &&
              Rd(inst, WI_updWind, w.updateWindMinutes) && Rd(inst, WI_time, w.time);
    w.windDir = {dir[0], dir[1], dir[2]};
    w.windBuildUpEnded = ended != 0;
    const float fl[] = {w.effectStrength, w.strength, w.windSpeed, dir[0], dir[1], dir[2], w.windBuildUpSpeedStart, w.windBuildUpSpeedEnd,
                        w.windBuildUpAngleStart, w.windBuildUpAngleEnd, w.time};
    for (float f : fl) ok = ok && std::isfinite(f);
    return ok;
}

bool WriteRegionWeather(void* region, const kc::RegionWeather& w) {
    void* inst = nullptr;
    void* curSeason = nullptr;
    void** seasonsBegin = nullptr;
    void** seasonsEnd = nullptr;
    if (!region || !Rd(region, WR_instance, inst) || !inst || !Rd(region, WR_seasonsBegin, seasonsBegin) || !Rd(region, WR_seasonsEnd, seasonsEnd))
        return false;
    // season: find it by id among the region's seasons
    std::string sid;
    if (!Rd(region, WR_season, curSeason) || !SidAt(curSeason, SEASON_data, sid) || sid != w.seasonSid) {
        const size_t n = size_t(seasonsEnd - seasonsBegin);
        if (n > 64) return false;
        bool found = false;
        for (size_t i = 0; i < n && !found; ++i) {
            void* s = nullptr;
            if (Rd(seasonsBegin, i * sizeof(void*), s) && SidAt(s, SEASON_data, sid) && sid == w.seasonSid) {
                Wr(region, WR_season, s);
                Wr(region, WR_seasonIndex, int32_t(i));
                curSeason = s;
                found = true;
            }
        }
        if (!found) return false;
    }
    Wr(region, WR_seasonEnd, w.seasonEnd);
    // weather type: find it in the season's list; switching it goes through the game's own setup
    void* curWeather = nullptr;
    if (!Rd(inst, WI_weather, curWeather) || !SidAt(curWeather, WEATHER_data, sid) || sid != w.weatherSid) {
        uint32_t count = 0;
        void** list = nullptr;
        if (!Rd(curSeason, SEASON_weatherCount, count) || !Rd(curSeason, SEASON_weathers, list) || !list || count > 256) return false;
        void* target = nullptr;
        for (uint32_t i = 0; i < count && !target; ++i) {
            void* wt = nullptr;
            if (Rd(list, i * sizeof(void*), wt) && SidAt(wt, WEATHER_data, sid) && sid == w.weatherSid) target = wt;
        }
        if (!target || !CallPtrArg(FnAddr(FnInstanceSetupWeather), inst, target)) return false;
        const uint8_t one = 1;
        Wr(region, WR_newWeather, one);     // the main-thread update replays the weather change effects
        Wr(region, WR_effectsDirty, one);
    }
    const float dir[3] = {w.windDir.x, w.windDir.y, w.windDir.z};
    const uint8_t ended = w.windBuildUpEnded ? 1 : 0;
    Wr(inst, WI_effectStrength, w.effectStrength); Wr(inst, WI_strength, w.strength); Wr(inst, WI_windSpeed, w.windSpeed);
    Wr(inst, WI_windDir, dir); Wr(inst, WI_buEnded, ended); Wr(inst, WI_buStart, w.windBuildUpStart); Wr(inst, WI_buEnd, w.windBuildUpEnd);
    Wr(inst, WI_buSpeedStart, w.windBuildUpSpeedStart); Wr(inst, WI_buSpeedEnd, w.windBuildUpSpeedEnd);
    Wr(inst, WI_buAngleStart, w.windBuildUpAngleStart); Wr(inst, WI_buAngleEnd, w.windBuildUpAngleEnd);
    Wr(inst, WI_start, w.startMinutes); Wr(inst, WI_end, w.endMinutes); Wr(inst, WI_updWind, w.updateWindMinutes); Wr(inst, WI_time, w.time);
    return true;
}

namespace {
constexpr uintptr_t CH_inventory = 0x2E8;           // Inventory*
constexpr uintptr_t INV_allItems = 0x10;            // lektor<Item*>
constexpr uintptr_t IT_manufacturer = 0xC0, IT_material = 0xC8, IT_pos = 0xDC, IT_section = 0xE8, IT_charges = 0x118,
                    IT_quality = 0x11C, IT_equipped = 0x129, IT_quantity = 0x12C;
void* InventoryOf(const Character* c) {
    void* inv = nullptr;
    return IsCharacter(c) && Rd(c, CH_inventory, inv) ? inv : nullptr;
}
} // namespace

namespace {
constexpr uintptr_t INV_sections = 0x28;            // boost::unordered_map<std::string, InventorySection*>
constexpr uintptr_t SEC_width = 0x30, SEC_height = 0x34, SEC_enabled = 0xD0;
constexpr uintptr_t IT_inInventory = 0xD8, IT_width = 0x130, IT_height = 0x134;
constexpr uintptr_t ITEM_getLevel = 0x2B8;           // InventoryItemBase vtable: int getLevel() const
constexpr uintptr_t INVV_addItem = 0x10, INVV_removeDontDestroy = 0x28, INVV_removeAutoDestroy = 0x30, INVV_drop = 0x38;
constexpr uintptr_t SECV_addAt = 0x18;               // InventorySection: void _addItem(Item*, int x, int y)

using FnAddItem = bool (*)(void* inv, void* item, int qty, bool dropOnFail, bool destroyOnFail);
using FnRemoveReturns = void* (*)(void* inv, void* item, int qty, bool returnCopyIfSomeLeft);
using FnRemoveDestroy = bool (*)(void* inv, void* item, int qty);
using FnItemInt = int (*)(void* item);
using FnSecAddAt = void (*)(void* sec, void* item, int x, int y);
using FnCreateItemSig = void* (*)(void* factory, void* gd, const void* hand, void* company, void* material, int level, void* uniform);

bool CallAddItem(void* fn, void* inv, void* item, int qty) {
    __try { return reinterpret_cast<FnAddItem>(fn)(inv, item, qty, false, true); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* CallRemoveReturns(void* fn, void* inv, void* item, int qty) {
    __try { return reinterpret_cast<FnRemoveReturns>(fn)(inv, item, qty, true); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CallRemoveDestroy(void* fn, void* inv, void* item, int qty) {
    __try { return reinterpret_cast<FnRemoveDestroy>(fn)(inv, item, qty); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallItemInt(void* fn, void* item, int& out) {
    __try { out = reinterpret_cast<FnItemInt>(fn)(item); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSecAddAt(void* fn, void* sec, void* item, int x, int y) {
    __try { reinterpret_cast<FnSecAddAt>(fn)(sec, item, x, y); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* CallCreateItem(void* fn, void* factory, void* gd, const void* hand, void* company, void* material, int level) {
    __try { return reinterpret_cast<FnCreateItemSig>(fn)(factory, gd, hand, company, material, level, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

constexpr uintptr_t INV_sectionList = 0x68;         // lektor<InventorySection*> sectionsInSearchOrder
constexpr uintptr_t SEC_items = 0x40;               // std::vector<SectionItem>: first, last, end
constexpr size_t kSectionItemSize = 0x10;           // SectionItem { Item*; u16 x, y, w, h; }

void ReadPointerLektor(const uint8_t* lk, std::vector<void*>& out, uint32_t max) {
    uint32_t count = 0;
    void** data = nullptr;
    if (!Rd(lk, off::LK_count, count) || count > max || !count || !Rd(lk, off::LK_data, data) || !data) return;
    for (uint32_t i = 0; i < count; ++i) {
        void* p = nullptr;
        if (Rd(data, i * sizeof(void*), p) && p) out.push_back(p);
    }
}

// Every item a character carries: the free inventory list does not hold what is worn or wielded,
// that only lives in its equipment section.
std::vector<void*> InventoryItems(void* inv) {
    std::vector<void*> out;
    ReadPointerLektor(reinterpret_cast<const uint8_t*>(inv) + INV_allItems, out, kc::kMaxItemsPerInventory);
    std::vector<void*> sections;
    ReadPointerLektor(reinterpret_cast<const uint8_t*>(inv) + INV_sectionList, sections, 64);
    for (void* sec : sections) {
        uintptr_t first = 0, last = 0;
        if (!Rd(sec, SEC_items, first) || !Rd(sec, SEC_items + 8, last) || last < first || (last - first) % kSectionItemSize ||
            (last - first) / kSectionItemSize > kc::kMaxItemsPerInventory)
            continue;
        for (uintptr_t p = first; p < last; p += kSectionItemSize) {
            void* it = nullptr;
            if (Rd(reinterpret_cast<const void*>(p), 0, it) && it && std::find(out.begin(), out.end(), it) == out.end()) out.push_back(it);
        }
        if (out.size() > kc::kMaxItemsPerInventory) break;
    }
    return out;
}

bool ReadItemState(void* it, kc::ItemState& s) {
    void* gd = nullptr;
    if (!Rd(it, off::RO_data, gd) || !GameDataSid(gd, s.templateSid)) return false;
    void* mat = nullptr;
    void* man = nullptr;
    if (Rd(it, IT_material, mat) && mat) GameDataSid(mat, s.materialSid);
    if (Rd(it, IT_manufacturer, man) && man) GameDataSid(man, s.manufacturerSid);
    ReadGameString(reinterpret_cast<uint8_t*>(it) + IT_section, s.section);
    int32_t pos[2] = {0, 0};
    uint8_t eq = 0;
    Rd(it, IT_pos, pos);
    Rd(it, IT_equipped, eq);
    Rd(it, IT_quantity, s.quantity);
    Rd(it, IT_quality, s.quality);
    Rd(it, IT_charges, s.charges);
    s.x = int16_t(pos[0]);
    s.y = int16_t(pos[1]);
    s.equipped = eq != 0;
    if (void* fn = VSlot(it, ITEM_getLevel)) CallItemInt(fn, it, s.level);
    if (!std::isfinite(s.quality)) s.quality = 0;
    if (!std::isfinite(s.charges)) s.charges = 0;
    return s.templateSid.size() <= kc::kMaxSidLen;
}

void* FindSection(void* inv, const std::string& name) {
    const auto* map = reinterpret_cast<const uint8_t*>(inv) + INV_sections;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 256) return nullptr;
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets) return nullptr;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return nullptr;
    std::string key;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* sec = nullptr;
        if (ReadGameString(reinterpret_cast<uint8_t*>(node) + off::MapNode_key, key) && key == name && Rd(node, off::MapNode_mapped, sec)) return sec;
        if (!Rd(node, off::USNode_next, node)) break;
    }
    return nullptr;
}

// Put an item where a save load would: the named section at (x, y) when it fits, else anywhere.
bool PlaceItem(void* inv, void* item, const std::string& section, int x, int y, int qty) {
    if (void* sec = FindSection(inv, section)) {
        int w = 0, h = 0, iw = 1, ih = 1;
        uint8_t enabled = 1;
        Rd(sec, SEC_width, w); Rd(sec, SEC_height, h); Rd(item, IT_width, iw); Rd(item, IT_height, ih); Rd(sec, SEC_enabled, enabled);
        if (x >= 0 && y >= 0 && x + iw <= w && y + ih <= h) {
            const uint8_t one = 1;
            if (!enabled) Wr(sec, SEC_enabled, one);
            CallSecAddAt(VSlot(sec, SECV_addAt), sec, item, x, y);
            if (!enabled) Wr(sec, SEC_enabled, enabled);
            uint8_t inside = 0;
            if (Rd(item, IT_inInventory, inside) && inside) return true;
        }
    }
    return CallAddItem(VSlot(inv, INVV_addItem), inv, item, qty);
}

constexpr int kItemTypeWeapon = 2;   // itemType::WEAPON

void* CreateItemFromState(const kc::ItemState& s, std::string* why = nullptr) {
    GameWorld* w = World();
    void* factory = nullptr;
    void* gd = FindGameData(s.templateSid);
    if (!gd) { if (why) *why = "unknown item " + s.templateSid; return nullptr; }
    if (!w || !Rd(w, off::GW_factory, factory) || !factory) { if (why) *why = "no factory"; return nullptr; }
    void* company = s.manufacturerSid.empty() ? nullptr : FindGameData(s.manufacturerSid);
    void* material = s.materialSid.empty() ? nullptr : FindGameData(s.materialSid);
    alignas(8) uint8_t nullHand[off::HandSize] = {};
    const uintptr_t vt = Addr(rva::VtHand);
    const uint32_t nullType = 0xB;   // what the game itself passes for "no handle yet"
    std::memcpy(nullHand, &vt, 8);
    std::memcpy(nullHand + off::H_type, &nullType, 4);
    // Weapons are made from their manufacturer: createItem(company, hand, weapon type, material, ...),
    // exactly how Character::generateWeapon calls it (their level follows from the material).
    int type = -1;
    Rd(gd, off::GD_type, type);
    void* item = nullptr;
    if (type == kItemTypeWeapon) {
        if (!company) { if (why) *why = "weapon " + s.templateSid + " without its manufacturer"; return nullptr; }
        item = CallCreateItem(FnAddr(FnCreateItem), factory, company, nullHand, gd, material, s.level);
    } else {
        item = CallCreateItem(FnAddr(FnCreateItem), factory, gd, nullHand, company, material, s.level);
    }
    if (item) Wr(item, IT_quantity, s.quantity);
    else if (why) {
        *why = "factory refused " + s.templateSid + " (type " + std::to_string(type) + ", company '" + s.manufacturerSid + "' " +
               (company ? "found" : "missing") + ", material '" + s.materialSid + "' " + (material ? "found" : "missing") + ")";
    }
    return item;
}
} // namespace

bool RebuildInventory(Character* c, const std::vector<kc::ItemState>& items, std::string* err) {
    void* inv = InventoryOf(c);
    if (!inv) { if (err) *err = "no inventory"; return false; }
    // Keep every local item that already is exactly what the host has; only the rest changes.
    std::vector<kc::ItemState> missing = items;
    std::vector<void*> extra;
    bool extraEquipped = false;
    for (void* it : InventoryItems(inv)) {
        kc::ItemState s;
        if (!ReadItemState(it, s)) continue;
        auto m = std::find(missing.begin(), missing.end(), s);
        if (m != missing.end()) { missing.erase(m); continue; }
        extra.push_back(it);
        extraEquipped |= s.equipped;
    }
    if (extraEquipped) EndCombat(c);   // it may be holding the weapon that goes away
    for (void* it : extra) {
        int q = 1;
        Rd(it, IT_quantity, q);
        CallRemoveDestroy(VSlot(inv, INVV_removeAutoDestroy), inv, it, q);
    }
    bool ok = true;
    for (const auto& s : missing) {
        std::string why;
        void* item = CreateItemFromState(s, &why);
        if (!item) { ok = false; if (err) *err = why; continue; }
        if (!PlaceItem(inv, item, s.section, s.x, s.y, s.quantity)) { ok = false; if (err) *err = "cannot place " + s.templateSid; }
    }
    return ok;
}

bool MoveInventoryItem(Character* from, Character* to, const kc::InvOp& op, std::string* err) {
    void* src = InventoryOf(from);
    void* dst = InventoryOf(to);
    if (!src || !dst) { if (err) *err = "no inventory"; return false; }
    // the stack the client moved: same kind, preferably at the same place
    void* best = nullptr;
    int bestQty = 0;
    std::vector<kc::ItemState> states;
    for (void* it : InventoryItems(src)) {
        kc::ItemState s;
        void* gd = nullptr;
        if (!Rd(it, off::RO_data, gd) || !GameDataSid(gd, s.templateSid)) continue;
        void* mat = nullptr;
        void* man = nullptr;
        if (Rd(it, IT_material, mat) && mat) GameDataSid(mat, s.materialSid);
        if (Rd(it, IT_manufacturer, man) && man) GameDataSid(man, s.manufacturerSid);
        if (void* fn = VSlot(it, ITEM_getLevel)) CallItemInt(fn, it, s.level);
        if (!s.sameKind(op.item)) continue;
        int q = 0;
        int32_t pos[2] = {0, 0};
        std::string sec;
        Rd(it, IT_quantity, q);
        Rd(it, IT_pos, pos);
        ReadGameString(reinterpret_cast<uint8_t*>(it) + IT_section, sec);
        const bool samePlace = sec == op.item.section && pos[0] == op.item.x && pos[1] == op.item.y;
        if (q >= op.item.quantity && (!best || samePlace)) { best = it; bestQty = q; if (samePlace) break; }
    }
    if (!best) { if (err) *err = "item not found"; return false; }
    if (op.kind == kc::InvOpKind::Drop) {
        void* fn = VSlot(src, INVV_drop);
        return fn && CallPtrArg2(fn, src, best);
    }
    const int qty = std::min(op.item.quantity, bestQty);
    void* moving = CallRemoveReturns(VSlot(src, INVV_removeDontDestroy), src, best, qty);
    if (!moving) { if (err) *err = "cannot take the item"; return false; }
    if (PlaceItem(dst, moving, op.toSection, op.toX, op.toY, qty)) return true;
    PlaceItem(src, moving, op.item.section, op.item.x, op.item.y, qty);   // never lose it: put it back
    if (err) *err = "no room";
    return false;
}

bool ReadInventory(Character* c, std::vector<kc::ItemState>& out) {
    out.clear();
    void* inv = InventoryOf(c);
    if (!inv) return false;
    for (void* it : InventoryItems(inv)) {
        kc::ItemState s;
        if (ReadItemState(it, s)) out.push_back(std::move(s));
    }
    if (out.size() > kc::kMaxItemsPerInventory) out.resize(kc::kMaxItemsPerInventory);
    std::sort(out.begin(), out.end(), [](const kc::ItemState& a, const kc::ItemState& b) {
        return std::tie(a.section, a.y, a.x, a.templateSid, a.quantity) < std::tie(b.section, b.y, b.x, b.templateSid, b.quantity);
    });
    return true;
}

bool ExpireRegionWeather(void* region) {
    void* inst = nullptr;
    const int32_t zero = 0;
    return region && Rd(region, WR_instance, inst) && inst && Wr(inst, WI_end, zero);
}

namespace {
// WeatherRegion: vector<EffectGroup*>
constexpr uintptr_t WR_effectsBegin = 0x70, WR_effectsEnd = 0x78;
// EffectGroup: effect GameData, vector<EffectHandler*>, countdown to the next spawn
constexpr uintptr_t EG_data = 0x8, EG_handlersBegin = 0x20, EG_handlersEnd = 0x28, EG_spawnTimer = 0x40;
constexpr uintptr_t EG_vSpawn = 0x20;   // bool spawn()
// EffectHandler
constexpr uintptr_t EH_pos = 0x24, EH_strength = 0x48, EH_age = 0x54, EH_life = 0x58, EH_endless = 0x5C;
constexpr uintptr_t EH_vStop = 0x20;    // void stop()
constexpr uintptr_t EHP_struck = 0x69, EHP_strikeIn = 0x6C;                  // EffectHandlerPoint (lightning)
constexpr uintptr_t EHW_dir = 0x68, EHW_turnTo = 0x74, EHW_turnIn = 0x80;    // EffectHandlerWandering
constexpr float kNeverTurn = 1e9f;      // clients' wandering effects turn only when the host's do

thread_local const float* t_effectPos = nullptr;

bool ReadPointerVector(const void* obj, uintptr_t beginOff, uintptr_t endOff, size_t max, std::vector<void*>& out) {
    out.clear();
    void** b = nullptr;
    void** e = nullptr;
    if (!Rd(obj, beginOff, b) || !Rd(obj, endOff, e) || e < b || size_t(e - b) > max) return false;
    out.resize(size_t(e - b));
    return out.empty() || SafeCopy(out.data(), b, out.size() * sizeof(void*));
}

bool HandlerOfKind(const void* h, kc::EffectKind kind) {
    return Vtable(h) == Addr(kind == kc::EffectKind::Point ? rva::VtEffectHandlerPoint : rva::VtEffectHandlerWandering);
}

bool RdVec(const void* p, uintptr_t offset, kc::Vec3& v) {
    float f[3];
    if (!Rd(p, offset, f)) return false;
    v = {f[0], f[1], f[2]};
    return Finite(v);
}
bool WrVec(void* p, uintptr_t offset, const kc::Vec3& v) {
    const float f[3] = {v.x, v.y, v.z};
    return Finite(v) && Wr(p, offset, f);
}
} // namespace

bool ReadEffectGroups(void* region, std::vector<EffectGroupInfo>& out) {
    out.clear();
    std::vector<void*> groups;
    if (!region || !ReadPointerVector(region, WR_effectsBegin, WR_effectsEnd, 64, groups)) return false;
    std::vector<std::pair<kc::EffectKind, std::string>> counted;
    for (void* g : groups) {
        const uintptr_t vt = Vtable(g);
        EffectGroupInfo gi;
        if (vt == Addr(rva::VtEffectGroupPoint)) gi.kind = kc::EffectKind::Point;
        else if (vt == Addr(rva::VtEffectGroupWandering)) gi.kind = kc::EffectKind::Wandering;
        else continue;
        if (!SidAt(g, EG_data, gi.effectSid) || !ReadPointerVector(g, EG_handlersBegin, EG_handlersEnd, 1024, gi.handlers)) continue;
        gi.group = g;
        size_t same = 0;
        for (const auto& c : counted) same += c.first == gi.kind && c.second == gi.effectSid ? 1 : 0;
        if (same > 255) continue;
        gi.ordinal = uint8_t(same);
        counted.emplace_back(gi.kind, gi.effectSid);
        out.push_back(std::move(gi));
    }
    return true;
}

bool ReadEffect(void* h, kc::EffectKind kind, kc::WeatherEffect& e) {
    if (!HandlerOfKind(h, kind)) return false;
    uint8_t endless = 0;
    bool ok = RdVec(h, EH_pos, e.pos) && Rd(h, EH_strength, e.strength) && Rd(h, EH_age, e.age) && Rd(h, EH_life, e.life) &&
              Rd(h, EH_endless, endless);
    e.endless = endless != 0;
    if (kind == kc::EffectKind::Point) {
        uint8_t struck = 0;
        ok = ok && Rd(h, EHP_struck, struck) && Rd(h, EHP_strikeIn, e.strikeIn);
        e.struck = struck != 0;
    } else {
        ok = ok && RdVec(h, EHW_dir, e.dir) && RdVec(h, EHW_turnTo, e.turnTo);
    }
    return ok && std::isfinite(e.strength) && std::isfinite(e.age) && std::isfinite(e.life) && std::isfinite(e.strikeIn);
}

void BlockEffectSpawns(void* group) {
    const float later = 1e6f;
    Wr(group, EG_spawnTimer, later);
}

void* SpawnEffect(void* group, const kc::WeatherEffect& e) {
    void* fn = VSlot(group, EG_vSpawn);
    if (!fn || !Finite(e.pos)) return nullptr;
    std::vector<void*> before, after;
    if (!ReadPointerVector(group, EG_handlersBegin, EG_handlersEnd, 1024, before)) return nullptr;
    const float pos[3] = {e.pos.x, e.pos.y, e.pos.z};
    bool spawned = false;
    t_effectPos = pos;
    const bool called = CallBool(fn, group, spawned);
    t_effectPos = nullptr;
    if (!called || !spawned || !ReadPointerVector(group, EG_handlersBegin, EG_handlersEnd, 1024, after) || after.size() != before.size() + 1)
        return nullptr;
    void* h = after.back();
    if (!HandlerOfKind(h, e.kind)) return nullptr;
    // the host's random rolls instead of ours
    const uint8_t endless = e.endless ? 1 : 0;
    Wr(h, EH_strength, e.strength); Wr(h, EH_age, e.age); Wr(h, EH_life, e.life); Wr(h, EH_endless, endless);
    if (e.kind == kc::EffectKind::Point) {
        const uint8_t struck = e.struck ? 1 : 0;
        Wr(h, EHP_struck, struck);
        Wr(h, EHP_strikeIn, e.strikeIn);
    } else {
        WrVec(h, EHW_dir, e.dir);
        WrVec(h, EHW_turnTo, e.turnTo);
        Wr(h, EHW_turnIn, kNeverTurn);
    }
    return h;
}

bool WriteEffectState(void* h, const kc::EffectState& s) {
    if (!HandlerOfKind(h, kc::EffectKind::Wandering)) return false;
    return WrVec(h, EH_pos, s.pos) && WrVec(h, EHW_dir, s.dir) && WrVec(h, EHW_turnTo, s.turnTo) && Wr(h, EHW_turnIn, kNeverTurn);
}

bool StopEffect(void* h) {
    if (!HandlerOfKind(h, kc::EffectKind::Point) && !HandlerOfKind(h, kc::EffectKind::Wandering)) return false;
    void* fn = VSlot(h, EH_vStop);
    return fn && CallVoid(fn, h);
}

const float* EffectSpawnPosition() { return t_effectPos; }

bool RegionRebuildingEffects(void* region) {
    uint8_t dirty = 0;
    return Rd(region, WR_effectsDirty, dirty) && dirty != 0;
}

bool EffectShown(void* h) {
    uint8_t shown = 0;
    return Rd(h, 0x38, shown) && shown != 0;
}

void HurryEffectSpawn(void* group) {
    const float now = 0.0f;
    Wr(group, EG_spawnTimer, now);
}

std::string DescribeRegionWeathers(void* region) {
    constexpr uintptr_t WEATHER_effectsBegin = 0x10, WEATHER_effectsEnd = 0x18, kEffectEntry = 0x18;   // pair<GameData*, TripleInt>
    auto label = [](void* gd) {
        std::string sid, name;
        GameDataSid(gd, sid);
        ReadGameString(reinterpret_cast<const uint8_t*>(gd) + off::GD_name, name);
        return sid + " '" + name + "'";
    };
    std::vector<void*> seasons;
    if (!region || !ReadPointerVector(region, WR_seasonsBegin, WR_seasonsEnd, 64, seasons)) return "";
    std::string out;
    for (void* season : seasons) {
        uint32_t count = 0;
        void** list = nullptr;
        void* sgd = nullptr;
        if (!Rd(season, SEASON_data, sgd) || !Rd(season, SEASON_weatherCount, count) || !Rd(season, SEASON_weathers, list) || !list || count > 256)
            continue;
        std::string ssid;
        GameDataSid(sgd, ssid);
        for (uint32_t i = 0; i < count; ++i) {
            void* wt = nullptr;
            void* gd = nullptr;
            uint8_t* b = nullptr;
            uint8_t* e = nullptr;
            if (!Rd(list, i * sizeof(void*), wt) || !Rd(wt, WEATHER_data, gd)) continue;
            out += "  weather " + label(gd) + " season " + ssid + " effects:";
            if (Rd(wt, WEATHER_effectsBegin, b) && Rd(wt, WEATHER_effectsEnd, e) && e >= b && size_t(e - b) <= 64 * kEffectEntry)
                for (uint8_t* q = b; q < e; q += kEffectEntry) {
                    void* fx = nullptr;
                    if (Rd(q, 0, fx)) out += " " + label(fx);
                }
            out += "\n";
        }
    }
    return out;
}

void* CameraWeatherRegion() {
    void* system = nullptr;
    void* region = nullptr;
    return Rd(reinterpret_cast<void*>(Addr(rva::WeatherSystem)), 0, system) && system && Rd(system, 0, region) ? region : nullptr;
}

bool SetRagdoll(Character* c, bool on) {
    return IsCharacter(c) && CallBoolInt(FnAddr(FnRagdollMode), c, on, kRagdollWhole);
}

bool StandUp(Character* c) {
    void* m = Medical(c);
    if (!m) return false;
    const bool no = false;
    const float zero = 0.0f;
    Wr(m, off::MS_unconscious, no);
    Wr(m, off::MS_koTimer, zero);
    bool ok = SetRagdoll(c, false);
    if (void* fn = VSlot(c, slot::CH_setProneState)) ok = CallInt(fn, c, kProneNormal) && ok;
    return ok;
}

bool DestroyObject(void* obj) {
    GameWorld* w = World();
    return w && IsCharacter(obj) && CallDestroy(FnAddr(FnWorldDestroy), w, obj);
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

namespace {
constexpr uintptr_t CM_speedOrders = 0x20, CM_desiredSpeed = 0xBC;   // CharMovement: MoveSpeed, float
constexpr int32_t kMoveSpeedCount = 4;
} // namespace

bool ReadPace(const Character* c, uint8_t& gait, float& pace) {
    void* m = Movement(c);
    int32_t order = 0;
    if (!m || !Rd(m, CM_speedOrders, order) || !Rd(m, CM_desiredSpeed, pace) || !std::isfinite(pace)) return false;
    gait = uint8_t(order >= 0 && order < kMoveSpeedCount ? order : 0);
    return true;
}

bool WritePace(Character* c, uint8_t gait, float pace) {
    void* m = Movement(c);
    if (!m || gait >= kMoveSpeedCount || !std::isfinite(pace) || pace < 0) return false;
    float cur = 0;
    int32_t order = 0;
    if (Rd(m, CM_desiredSpeed, cur) && Rd(m, CM_speedOrders, order) && cur == pace && order == gait) return true;
    return Wr(m, CM_speedOrders, int32_t(gait)) && Wr(m, CM_desiredSpeed, pace);
}

namespace {
constexpr uintptr_t CH_animation = 0x448;      // Character: AnimationClass*
constexpr uintptr_t AC_owner = 0x2D8;          // AnimationClass: Character* me
constexpr uintptr_t AC_animList = 0x2C0;       // AnimationClass: AnimsListsManager::AnimList*
constexpr uintptr_t AL_allAnims = 0xB8;        // AnimList: unordered_map<string, AnimationData*>
constexpr uintptr_t AC_reqAction = 0x210, AC_reqCarryL = 0x21E, AC_reqCarryR = 0x21F, AC_reqCarried = 0x220,
                    AC_reqCombatMode = 0x244;  // AnimationRequirement (at +0xF0) fields
constexpr uintptr_t AD_dataName = 0x8;         // AnimationData
constexpr uintptr_t TQ_animation = 0x0;        // CombatTechniqueData
constexpr uintptr_t kTechniqueList = 0x2011F78;   // lektor<CombatTechniqueData*>: every technique

using FnCombatAnim = void (*)(void* ac, void* technique, float speed, void* extra);
using FnPlayAction = void (*)(void* ac, void* anim, float speedMult, float weight, bool stumble);
using FnAnimPtr = void (*)(void* ac, void* anim);
using FnAnimBool3 = void (*)(void* ac, bool a, bool b, bool c);
using FnAnimStr = bool (*)(void* ac, const void* name);
bool SehCombatAnim(void* fn, void* ac, void* t, float speed, void* extra) {
    __try { reinterpret_cast<FnCombatAnim>(fn)(ac, t, speed, extra); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehPlayAction(void* fn, void* ac, void* a, float s, float w, bool st) {
    __try { reinterpret_cast<FnPlayAction>(fn)(ac, a, s, w, st); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehAnimPtr(void* fn, void* ac, void* a) {
    __try { reinterpret_cast<FnAnimPtr>(fn)(ac, a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehAnimBool3(void* fn, void* ac, bool a, bool b, bool c) {
    __try { reinterpret_cast<FnAnimBool3>(fn)(ac, a, b, c); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehAnimStr(void* fn, void* ac, const void* name) {
    __try { reinterpret_cast<FnAnimStr>(fn)(ac, name); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool ReadStdString(const void* p, std::string& out) { return ReadGameString(p, out); }

Character* AnimOwner(const void* ac) {
    void* c = nullptr;
    return ac && Rd(ac, AC_owner, c) && IsCharacter(c) ? static_cast<Character*>(c) : nullptr;
}

void* AnimationOf(const Character* c) {
    void* ac = nullptr;
    return IsCharacter(c) && Rd(c, CH_animation, ac) && AnimOwner(ac) == c ? ac : nullptr;
}

std::string TechniqueName(const void* t) {
    std::string s;
    if (t) ReadGameString(reinterpret_cast<const uint8_t*>(t) + TQ_animation, s);
    return s;
}

std::string AnimDataName(const void* a) {
    std::string s;
    if (a) ReadGameString(reinterpret_cast<const uint8_t*>(a) + AD_dataName, s);
    return s;
}

void* FindTechnique(const std::string& name) {
    static std::unordered_map<std::string, void*> cache;
    if (name.empty()) return nullptr;
    if (auto it = cache.find(name); it != cache.end()) return it->second;
    // attacks, then blocks (and dodges), each a lektor<CombatTechniqueData*> right after the other
    for (uintptr_t listRva : {kTechniqueList, kTechniqueList + 0x18, kTechniqueList + 0x30, kTechniqueList - 0x18}) {
        const void* list = reinterpret_cast<const void*>(Addr(listRva));
        uint32_t n = 0;
        void** data = nullptr;
        if (!Rd(list, off::LK_count, n) || !Rd(list, off::LK_data, data) || !data || n > 100000) continue;
        for (uint32_t i = 0; i < n; ++i) {
            void* t = nullptr;
            if (Rd(data, i * sizeof(void*), t) && t && TechniqueName(t) == name) {
                cache[name] = t;
                return t;
            }
        }
    }
    return nullptr;
}

void* FindAnimData(const Character* c, const std::string& name) {
    void* ac = AnimationOf(c);
    void* list = nullptr;
    if (!ac || name.empty() || !Rd(ac, AC_animList, list) || !list) return nullptr;
    const uint8_t* map = reinterpret_cast<const uint8_t*>(list) + AL_allAnims;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    void* node = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 100000 || !Rd(map, off::US_bucketCount, bucketCount) ||
        !Rd(map, off::US_buckets, buckets) || !buckets || !Rd(buckets, bucketCount * sizeof(void*), node))
        return nullptr;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* a = nullptr;
        if (Rd(node, off::MapNode_mapped, a) && a && AnimDataName(a) == name) return a;
        if (!Rd(node, off::USNode_next, node)) break;
    }
    return nullptr;
}

bool ReadAnimModes(const Character* c, AnimModes& out) {
    void* ac = AnimationOf(c);
    void* action = nullptr;
    int32_t combat = 0;
    uint8_t l = 0, r = 0, carried = 0;
    if (!ac || !Rd(ac, AC_reqAction, action) || !Rd(ac, AC_reqCombatMode, combat) || !Rd(ac, AC_reqCarryL, l) || !Rd(ac, AC_reqCarryR, r) ||
        !Rd(ac, AC_reqCarried, carried))
        return false;
    out.action = AnimDataName(action);
    out.combat = combat == 1;
    out.carried = carried != 0;
    out.carryLeft = l != 0;
    out.carryRight = r != 0;
    return true;
}

std::string CurrentTechniqueName(const Character* c) {
    constexpr uintptr_t AC_reqTechnique = 0x208;
    void* ac = AnimationOf(c);
    void* t = nullptr;
    std::string n = ac && Rd(ac, AC_reqTechnique, t) && t ? TechniqueName(t) : "";
    for (char& ch : n) if (ch == ' ') ch = '_';
    return n.empty() ? "-" : n;
}

std::string ItemTemplate(const void* item);

std::string CurrentStumbleName(const Character* c) {
    constexpr uintptr_t AC_reqStumble = 0x228;
    void* ac = AnimationOf(c);
    void* a = nullptr;
    std::string n = ac && Rd(ac, AC_reqStumble, a) && a ? AnimDataName(a) : "";
    return n.empty() ? "-" : n;
}

bool WeaponInHands(const Character* c, std::string& itemSid, std::string& fromSection) {
    constexpr uintptr_t CHH_weaponInHands = 0x6D8, CHH_sheathLocation = 0x6E0;
    void* w = nullptr;
    if (Vtable(c) != Addr(rva::VtCharacterHuman) || !Rd(c, CHH_weaponInHands, w) || !w) return false;
    itemSid = ItemTemplate(w);
    ReadGameString(reinterpret_cast<const uint8_t*>(c) + CHH_sheathLocation, fromSection);
    return !itemSid.empty();
}

bool CallStartCombatAnim(void* fn, Character* c, void* technique, float speed) {
    void* ac = AnimationOf(c);
    GameString extra;
    if (!ac || !technique || !fn || !MakeGameString("", extra)) return false;
    return SehCombatAnim(fn, ac, technique, speed, &extra);   // the callee owns (and destroys) the by-value string
}

bool CallAnimVoid(void* fn, Character* c) {
    void* ac = AnimationOf(c);
    return ac && fn && CallVoid(fn, ac);
}

bool CallPlayAction(Character* c, void* animData, float speedMult, float weight, bool stumble) {
    void* ac = AnimationOf(c);
    return ac && animData && SehPlayAction(FnAddr(FnAnimPlayAction), ac, animData, speedMult, weight, stumble);
}

bool CallStopActionNamed(Character* c, const std::string& name) {
    void* ac = AnimationOf(c);
    if (!ac) return false;
    if (name.empty()) return CallVoid(FnAddr(FnAnimStopAction), ac);
    GameString s;
    if (!MakeGameString(name, s)) return CallVoid(FnAddr(FnAnimStopAction), ac);
    return SehAnimStr(FnAddr(FnAnimStopActionNamed), ac, &s);
}

bool CallStartStumble(Character* c, void* animData) {
    void* ac = AnimationOf(c);
    return ac && animData && SehAnimPtr(FnAddr(FnAnimStartStumble), ac, animData);
}

bool CallSetCombatMode(Character* c, bool on) {
    void* ac = AnimationOf(c);
    return ac && CallBoolArg(FnAddr(FnAnimSetCombatMode), ac, on);
}

bool CallSetCarryMode(Character* c, bool carried, bool left, bool right) {
    void* ac = AnimationOf(c);
    return ac && SehAnimBool3(FnAddr(FnAnimSetCarryMode), ac, carried, left, right);
}

namespace {
using FnDraw = bool (*)(void* c, void* item, void* section);
bool SehDraw(void* fn, void* c, void* item, void* section) {
    __try { return reinterpret_cast<FnDraw>(fn)(c, item, section); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

std::string ItemTemplate(const void* item) {
    void* gd = nullptr;
    std::string sid;
    if (item && Rd(item, off::RO_data, gd)) GameDataSid(gd, sid);
    return sid;
}

bool CallDrawWeapon(Character* c, const std::string& itemSid, const std::string& section) {
    void* inv = InventoryOf(c);
    if (!inv || itemSid.empty()) return false;
    for (void* it : InventoryItems(inv)) {
        if (ItemTemplate(it) != itemSid) continue;
        GameString s;
        if (!MakeGameString(section, s)) return false;
        return SehDraw(FnAddr(FnDrawWeapon), c, it, &s);   // the callee owns the by-value string
    }
    return false;
}

std::string DrawnFrom(const Character* c) {
    std::string s;
    if (Vtable(c) == Addr(rva::VtCharacterHuman)) ReadGameString(reinterpret_cast<const uint8_t*>(c) + 0x6E0, s);
    return s.empty() ? "-" : s;
}

bool CallSheathe(Character* c) { return IsCharacter(c) && CallVoid(FnAddr(FnSheatheWeapon), c); }

bool SetDestination(Character* c, const kc::Vec3& dest) {
    void* m = Movement(c);
    if (!m || !Finite(dest)) return false;
    void* fn = VSlot(m, slot::CM_setDestination);
    const float p[3] = {dest.x, dest.y, dest.z};
    return fn && CallSetDest(fn, m, p, HIGH_PRIORITY);
}

using VirtVec = void (*)(void* self, const float* v);
bool CallVec(void* fn, void* self, const float* v) {
    __try {
        reinterpret_cast<VirtVec>(fn)(self, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetPositionSimple(Character* c, const kc::Vec3& pos) {
    void* m = Movement(c);
    if (!m || !Finite(pos)) return false;
    void* fn = VSlot(m, slot::CM_setPositionSimple);
    const float p[3] = {pos.x, pos.y, pos.z};
    return fn && CallVec(fn, m, p);
}

bool IsMoving(const Character* c) {
    void* m = Movement(c);
    bool moving = false;
    return m && Rd(m, off::CM_currentlyMoving, moving) && moving;
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
