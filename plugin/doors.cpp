// Lot A: doors and locks.
//
// The host's doors (DoorStuff: open / closed, broken) and locks (DoorLock: on doors, chests, cages,
// shackles: locked, lock level, broken) are the ones that count. The host reads those near the
// players and the session sends them (Doors); a client imposes them on its copy of the same object,
// found by kind and place (objects of the world have another handle on every machine).
//
// On clients the game never changes a door by itself: opening, closing, locking and unlocking are
// refused by the hooks below unless KenshiCoop itself applies the host's state. A door panel button
// the local player clicks is not run locally either: it goes to the host (DoorRequest), whose game
// runs it, and the result comes back like any other door change. Orders on doors (open, close, pick
// the lock, lock, unlock, bash) are player tasks and already go to the host (RouteOrder).
#include "world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "hooks.h"

namespace kcp {

namespace {

// DoorStuff (Building)
constexpr uintptr_t DS_lock = 0x370;        // DoorLock*
constexpr uintptr_t DS_openAmount = 0x37C;  // float 0 closed .. 1 open
constexpr uintptr_t DS_state = 0x380;       // DoorState: 0 closed, 1 open, 2 opening, 3 closing
constexpr uintptr_t DS_wantsLock = 0x384;   // bool: locks itself once closed
// DoorLock
constexpr uintptr_t DL_level = 0x0;         // int: lock level (0: no real lock)
constexpr uintptr_t DL_locked = 0x20;       // bool
constexpr uintptr_t DL_broken = 0x21;       // bool: the lock itself is broken
// Building virtual slots
constexpr uintptr_t BV_isBroken = 0x308;    // bool isBroken() const
constexpr uintptr_t BV_setBroken = 0x310;   // void setBroken(bool)
constexpr uintptr_t BV_doorStuff = 0x3C8;   // DoorStuff* doorStuff()   (this for a door, else null)
constexpr uintptr_t BV_getDoorLock = 0x400; // DoorLock* getDoorLock()  (doors +0x370, UseableStuff +0x438)

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
using FnPtr0 = void* (*)(void*);
using FnBool0 = bool (*)(void*);
using FnSetBool = void (*)(void*, bool);
using FnVoid0 = void (*)(void*);
using FnButton = void (*)(void*, void*);
void* CallPtr(void* obj, uintptr_t slot) {
    void* fn = Slot(obj, slot);
    if (!fn) return nullptr;
    __try { return reinterpret_cast<FnPtr0>(fn)(obj); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CallBoolSlot(void* obj, uintptr_t slot, bool& out) {
    void* fn = Slot(obj, slot);
    if (!fn) return false;
    __try { out = reinterpret_cast<FnBool0>(fn)(obj); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSetBoolSlot(void* obj, uintptr_t slot, bool v) {
    void* fn = Slot(obj, slot);
    if (!fn) return false;
    __try { reinterpret_cast<FnSetBool>(fn)(obj, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallDoorBool(kenshi::Fn f, void* door) {
    __try { reinterpret_cast<FnBool0>(kenshi::FnAddr(f))(door); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallDoorVoid(kenshi::Fn f, void* door) {
    __try { reinterpret_cast<FnVoid0>(kenshi::FnAddr(f))(door); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallDoorButton(kenshi::Fn f, void* door) {
    __try { reinterpret_cast<FnButton>(kenshi::FnAddr(f))(door, nullptr); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A building of the world (handle type 0): only those have the Building virtual slots used here.
bool IsBuilding(void* obj) {
    kc::Handle h;
    return obj && !kenshi::IsCharacter(obj) && kenshi::ObjectHandle(obj, h) && h.type == 0;
}

std::string DoorKey(const std::string& sid, const kc::Vec3& p) {
    char b[96];
    snprintf(b, sizeof(b), "@%d,%d,%d", int(std::lround(p.x)), int(std::lround(p.y)), int(std::lround(p.z)));
    return sid + b;
}

float Dist(const kc::Vec3& a, const kc::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

} // namespace
} // namespace kcp

// ---------------------------------------------------------------- game side
namespace kenshi {

bool ReadDoor(void* obj, kc::DoorState& out) {
    using namespace kcp;
    if (!IsBuilding(obj)) return false;
    void* door = CallPtr(obj, BV_doorStuff);
    void* lock = CallPtr(obj, BV_getDoorLock);
    if (!door && !lock) return false;
    out.kind = door ? kc::DoorKind::Door : kc::DoorKind::Lock;
    out.state = 0;
    out.flags = 0;
    out.lockLevel = 0;
    out.openAmount = 0;
    bool broken = false;
    if (CallBoolSlot(obj, BV_isBroken, broken) && broken) out.flags |= kc::kDoorBroken;
    if (door) {
        int32_t st = 0;
        float amount = 0;
        uint8_t wants = 0;
        Rd(door, DS_state, st);
        Rd(door, DS_openAmount, amount);
        Rd(door, DS_wantsLock, wants);
        out.state = uint8_t(st >= 0 && st <= 3 ? st : 0);
        out.openAmount = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
        if (wants) out.flags |= kc::kDoorWantsLock;
    }
    if (lock) {
        int32_t level = 0;
        uint8_t locked = 0, lockBroken = 0;
        Rd(lock, DL_level, level);
        Rd(lock, DL_locked, locked);
        Rd(lock, DL_broken, lockBroken);
        out.flags |= kc::kDoorHasLock;
        if (locked) out.flags |= kc::kDoorLocked;
        if (lockBroken) out.flags |= kc::kDoorLockBroken;
        out.lockLevel = std::clamp(level, 0, 1000000);
    }
    return true;
}

bool ApplyDoorState(void* obj, const kc::DoorState& d) {
    using namespace kcp;
    if (!IsBuilding(obj)) return false;
    void* door = CallPtr(obj, BV_doorStuff);
    void* lock = CallPtr(obj, BV_getDoorLock);
    if (d.kind == kc::DoorKind::Door && !door) return false;
    // broken first: a broken door cannot be closed
    const bool wantBroken = (d.flags & kc::kDoorBroken) != 0;
    bool broken = false;
    if (CallBoolSlot(obj, BV_isBroken, broken) && broken != wantBroken) CallSetBoolSlot(obj, BV_setBroken, wantBroken);
    if (door) {
        int32_t st = 0;
        Rd(door, DS_state, st);
        const bool hostOpen = d.state == 1 || d.state == 2;
        const bool localOpen = st == 1 || st == 2;
        if (hostOpen != localOpen) {
            // the game's own opening and closing (animation and sound); halfway the other way: turn back
            if (st == 0 && hostOpen) CallDoorBool(FnDoorOpen, door);
            else if (st == 1 && !hostOpen) CallDoorBool(FnDoorClose, door);
            else Wr(door, DS_state, int32_t(hostOpen ? 2 : 3));
        }
        const uint8_t wants = (d.flags & kc::kDoorWantsLock) ? 1 : 0;
        uint8_t localWants = 0;
        if (Rd(door, DS_wantsLock, localWants) && localWants != wants) Wr(door, DS_wantsLock, wants);
    }
    if (lock && (d.flags & kc::kDoorHasLock)) {
        int32_t level = 0;
        uint8_t locked = 0, lockBroken = 0;
        const uint8_t wantLocked = (d.flags & kc::kDoorLocked) ? 1 : 0, wantLockBroken = (d.flags & kc::kDoorLockBroken) ? 1 : 0;
        if (Rd(lock, DL_level, level) && level != d.lockLevel) Wr(lock, DL_level, d.lockLevel);
        if (Rd(lock, DL_locked, locked) && locked != wantLocked) Wr(lock, DL_locked, wantLocked);
        if (Rd(lock, DL_broken, lockBroken) && lockBroken != wantLockBroken) Wr(lock, DL_broken, wantLockBroken);
    }
    return true;
}

bool DoorLocked(void* obj) {
    kc::DoorState d;
    return ReadDoor(obj, d) && (d.flags & kc::kDoorLocked) && d.lockLevel > 0 && !(d.flags & (kc::kDoorLockBroken | kc::kDoorBroken));
}

bool PressDoorButton(void* door, kc::DoorAction action) {
    using namespace kcp;
    if (!IsBuilding(door) || !CallPtr(door, BV_doorStuff)) return false;
    return CallDoorButton(action == kc::DoorAction::LockButton ? FnDoorLockButton : FnDoorOpenButton, door);
}

} // namespace kenshi

// ---------------------------------------------------------------- world side
namespace kcp {

void KenshiWorld::ReadDoors(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::DoorState>& out) {
    out.clear();
    std::unordered_set<void*> seen;
    std::vector<kc::Vec3> done;
    std::vector<void*> objs;
    for (const auto& c : centers) {
        // characters standing together share one search
        bool covered = false;
        for (const auto& p : done) covered |= Dist(p, c) < radius * 0.25f;
        if (covered) continue;
        done.push_back(c);
        kenshi::ObjectsNear(c, radius, objs);
        for (void* o : objs) {
            if (!seen.insert(o).second) continue;
            kc::DoorState d;
            if (!kenshi::ReadDoor(o, d) || !kenshi::ObjectTemplate(o, d.sid) || !kenshi::ObjectPosition(o, d.pos)) continue;
            out.push_back(std::move(d));
        }
    }
}

void* KenshiWorld::FindDoor(const std::string& sid, const kc::Vec3& pos) {
    const std::string key = DoorKey(sid, pos);
    std::string s;
    kc::Vec3 p;
    if (auto it = doorCache_.find(key); it != doorCache_.end()) {
        void* o = kenshi::ResolveObject(it->second);
        if (o && kenshi::ObjectTemplate(o, s) && s == sid && kenshi::ObjectPosition(o, p) && Dist(p, pos) < 10.0f) return o;
        doorCache_.erase(it);
    }
    std::vector<void*> around;
    kenshi::ObjectsNear(pos, 30.0f, around);
    void* best = nullptr;
    float bestD = 10.0f;
    for (void* o : around) {
        kc::DoorState d;
        if (!kenshi::ObjectTemplate(o, s) || s != sid || !kenshi::ObjectPosition(o, p) || !kenshi::ReadDoor(o, d)) continue;
        const float dd = Dist(p, pos);
        if (dd < bestD) { bestD = dd; best = o; }
    }
    kc::Handle h;
    if (best && kenshi::ObjectHandle(best, h)) {
        if (doorCache_.size() > 4096) doorCache_.clear();
        doorCache_[key] = h;
    }
    return best;
}

bool KenshiWorld::ApplyDoor(const kc::DoorState& d) {
    void* o = FindDoor(d.sid, d.pos);
    if (!o) return false;
    kc::DoorState local;
    if (kenshi::ReadDoor(o, local) && local.sameState(d)) return true;
    HostCallScope scope;   // lets our own call through the door hooks
    const bool ok = kenshi::ApplyDoorState(o, d);
    if (ok && (local.flags ^ d.flags) & (kc::kDoorLocked | kc::kDoorBroken | kc::kDoorLockBroken))
        Log("door: %s now %s%s%s as on the host", TemplateName(d.sid).c_str(), (d.flags & kc::kDoorLocked) ? "locked" : "unlocked",
            (d.flags & kc::kDoorBroken) ? ", broken" : "", (d.flags & kc::kDoorLockBroken) ? ", lock broken" : "");
    return ok;
}

bool KenshiWorld::ContainerLocked(const kc::Handle& container) {
    void* o = kenshi::ResolveObject(container);
    return o && kenshi::DoorLocked(o);
}

bool KenshiWorld::ExecuteDoorRequest(const kc::DoorRequest& r) {
    void* o = FindDoor(r.sid, r.pos);
    if (!o) return false;
    HostCallScope scope;
    return kenshi::PressDoorButton(o, r.action);
}

void KenshiWorld::QueueDoorRequest(void* door, kc::DoorAction action) {
    kc::DoorRequest r;
    r.action = action;
    if (!kenshi::ObjectTemplate(door, r.sid) || !kenshi::ObjectPosition(door, r.pos)) return;
    std::lock_guard<std::mutex> lk(doorMutex_);
    if (doorReqs_.size() < 16) doorReqs_.push_back(std::move(r));
}

void KenshiWorld::TakeDoorRequests(std::vector<kc::DoorRequest>& out) {
    std::lock_guard<std::mutex> lk(doorMutex_);
    out.swap(doorReqs_);
    doorReqs_.clear();
}

// ---------------------------------------------------------------- hooks
// On clients the doors are the host's: the game may not open, close, lock or unlock one by itself
// (the walking characters' paths, AI); our own calls (HostCallScope) apply the host's state.
namespace doorhooks {

using BoolFn = bool (*)(void*);
using VoidFn = void (*)(void*);
using ButtonFn = void (*)(void*, void*);
BoolFn o_open = nullptr;
BoolFn o_close = nullptr;
VoidFn o_lock = nullptr;
VoidFn o_unlock = nullptr;
ButtonFn o_openButton = nullptr;
ButtonFn o_lockButton = nullptr;

bool Refused() { return KenshiWorld::ClientActive() && !InHostCall(); }

bool hk_open(void* door) { return Refused() ? false : o_open(door); }
bool hk_close(void* door) { return Refused() ? false : o_close(door); }
void hk_lock(void* door) { if (!Refused()) o_lock(door); }
void hk_unlock(void* door) { if (!Refused()) o_unlock(door); }
// The door panel's buttons, clicked by the local player: the host's game runs them.
void hk_openButton(void* door, void* line) {
    if (!Refused()) return o_openButton(door, line);
    if (KenshiWorld* w = TheWorld()) w->QueueDoorRequest(door, kc::DoorAction::OpenButton);
}
void hk_lockButton(void* door, void* line) {
    if (!Refused()) return o_lockButton(door, line);
    if (KenshiWorld* w = TheWorld()) w->QueueDoorRequest(door, kc::DoorAction::LockButton);
}

} // namespace doorhooks

} // namespace kcp
