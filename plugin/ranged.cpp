// Lot C: ranged combat. Bows, crossbows and harpoon guns, and turrets.
//
// Kenshi's ranged weapons are GunClass objects: GunClassPersonal for a character's crossbow (held by
// its RangedCombatClass, Character+0x2F0, at +0x28), GunClassTurret for a turret (TurretBuilding).
// GunClass::shoot fires one real projectile: it takes one from the projectile pool, puts it at the
// barrel, turns it along the aim direction plus a random deviation, and lets the pool fly it (each
// frame the projectile moves along its own orientation and traces what it crosses).
//
// Host: the shoot hook records each shot with the exact orientation its projectile left with.
// Client: the same hook refuses the local game's shots; KenshiCoop fires the host's, then turns the
// new projectile onto the host's path. What it hits does nothing (clients refuse damage).
#include "ranged.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <unordered_set>

#include "hooks.h"
#include "util.h"
#include "world.h"

namespace kcp::ranged {

ShootFn o_gunShoot = nullptr;
ProjectileGetFn o_projectileGet = nullptr;

namespace {
thread_local int t_capture = 0;
thread_local void* t_projectile = nullptr;
} // namespace

void* hk_projectileGet(void* pool, void* mesh, void* material, void* extra) {
    void* p = o_projectileGet(pool, mesh, material, extra);
    if (t_capture) t_projectile = p;
    return p;
}

void hk_gunShoot(void* gun, void* me, void* target, int stat, const float* aimPos) {
    // Clients fire only the host's shots (KenshiCoop's replay runs inside a HostCallScope).
    if (KenshiWorld::ClientActive() && !InHostCall()) return;
    ++t_capture;
    t_projectile = nullptr;
    o_gunShoot(gun, me, target, stat, aimPos);
    --t_capture;
    auto v = KenshiWorld::View();
    if (v->active && !v->client)
        if (KenshiWorld* w = TheWorld()) w->NoteShot(gun, static_cast<kenshi::Character*>(me), target, stat, aimPos, t_projectile);
}

void* LastProjectile() { return t_projectile; }

} // namespace kcp::ranged

// ---------------------------------------------------------------- game memory (kenshi layer)
namespace kenshi {

namespace {

constexpr uintptr_t CH_rangedCombat = 0x2F0;   // Character: RangedCombatClass*
// RangedCombatClass (checked against Character::isInRangedCombatMode and shoot's call site)
constexpr uintptr_t RCC_state = 0x0;           // RangedState (int)
constexpr uintptr_t RCC_gun = 0x28;            // GunClass*
constexpr uintptr_t RCC_combat = 0x36;         // bool combatMode
constexpr uintptr_t RCC_aimPos = 0x38;         // Vector3 currentAimPos
constexpr uintptr_t RCC_target = 0x48;         // hand currentTarget
constexpr uintptr_t GUN_turret = 0x128;        // GunClassTurret: TurretBuilding*
constexpr uintptr_t GUNV_aimAt = 0x28;         // GunClass vtable: void aimAt(const Vector3&)
constexpr uintptr_t TB_aimTarget = 0x4A8;      // TurretBuilding: the point it turns toward (what aimAt writes)
constexpr uintptr_t PROJ_node = 0x58;          // projectile: Ogre::SceneNode*

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool Rd(const void* p, uintptr_t off, T& out) {
    return p && SafeCopy(&out, reinterpret_cast<const uint8_t*>(p) + off, sizeof(T));
}
template <class T>
bool Wr(void* p, uintptr_t off, const T& v) {
    return p && SafeCopy(reinterpret_cast<uint8_t*>(p) + off, &v, sizeof(T));
}
bool VtableIs(const void* obj, uintptr_t vtRva) {
    uintptr_t vt = 0;
    return obj && Rd(obj, 0, vt) && vt == Addr(vtRva);
}

using ShootSig = void (*)(void* gun, void* me, void* target, int stat, const float* aimPos);
bool ShootSeh(void* fn, void* gun, void* me, void* target, int stat, const float* aim) {
    __try {
        reinterpret_cast<ShootSig>(fn)(gun, me, target, stat, aim);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using AimAtSig = void (*)(void* gun, const float* target);
bool AimAtSeh(void* fn, void* gun, const float* target) {
    __try {
        reinterpret_cast<AimAtSig>(fn)(gun, target);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Ogre::Node, from OgreMain_x64.dll's exports (the projectile's flight path is its node's orientation).
using GetOrientationSig = float* (*)(const void* node, float* out);   // Quaternion getOrientation() const (hidden return)
using SetOrientationSig = void (*)(void* node, float w, float x, float y, float z);
void* OgreExport(const char* name) {
    static HMODULE ogre = GetModuleHandleW(L"OgreMain_x64.dll");
    return ogre ? reinterpret_cast<void*>(GetProcAddress(ogre, name)) : nullptr;
}
bool GetOrientationSeh(void* fn, const void* node, float* q) {
    __try {
        reinterpret_cast<GetOrientationSig>(fn)(node, q);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SetOrientationSeh(void* fn, void* node, const kc::Quat& q) {
    __try {
        reinterpret_cast<SetOrientationSig>(fn)(node, q.w, q.x, q.y, q.z);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* RangedCombat(Character* c) {
    void* rcc = nullptr;
    return IsCharacter(c) && Rd(c, CH_rangedCombat, rcc) ? rcc : nullptr;
}

} // namespace

void* CharacterGun(Character* c) {
    void* gun = nullptr;
    void* rcc = RangedCombat(c);
    return rcc && Rd(rcc, RCC_gun, gun) ? gun : nullptr;
}

bool ReadRanged(Character* c, RangedView& out) {
    void* rcc = RangedCombat(c);
    if (!rcc) return false;
    uint8_t combat = 0;
    int32_t state = 0;
    float aim[3] = {};
    if (!Rd(rcc, RCC_combat, combat) || !Rd(rcc, RCC_state, state) || !Rd(rcc, RCC_aimPos, aim)) return false;
    out.combat = combat != 0;
    out.state = uint8_t(state >= 0 && state <= 4 ? state : 4);
    out.aimPos = {aim[0], aim[1], aim[2]};
    if (!std::isfinite(aim[0]) || !std::isfinite(aim[1]) || !std::isfinite(aim[2])) out.aimPos = {};
    out.target = kc::Handle{};
    kc::Handle t;
    if (HandleFromHand(reinterpret_cast<uint8_t*>(rcc) + RCC_target, t) && t.type == 1) out.target = t;   // characters only
    return true;
}

bool WriteRanged(Character* c, const RangedView& v, bool writeTarget) {
    void* rcc = RangedCombat(c);
    if (!rcc) return false;
    // ranged combat mode only with a weapon set up here (the game's ranged code expects one)
    const uint8_t combat = v.combat && CharacterGun(c) ? 1 : 0;
    Wr(rcc, RCC_combat, combat);
    if (!v.combat) return true;
    const int32_t state = v.state;
    const float aim[3] = {v.aimPos.x, v.aimPos.y, v.aimPos.z};
    Wr(rcc, RCC_state, state);
    Wr(rcc, RCC_aimPos, aim);
    if (writeTarget) {
        alignas(8) uint8_t hand[off::HandSize];
        MakeHand(v.target, hand);
        SafeCopy(reinterpret_cast<uint8_t*>(rcc) + RCC_target, hand, sizeof(hand));
    }
    return true;
}

bool IsTurret(const void* building) { return VtableIs(building, rva::VtTurretBuilding); }

void* GunTurret(void* gun) {
    void* turret = nullptr;
    return VtableIs(gun, rva::VtGunClassTurret) && Rd(gun, GUN_turret, turret) && IsTurret(turret) ? turret : nullptr;
}

void* TurretGun(void* turret) {
    if (!IsTurret(turret)) return nullptr;
    // the turret's GunClassTurret: the pointer field whose object is a turret gun of this very turret
    // (found once; the layout is the same for every turret)
    static uintptr_t found = 0;
    auto check = [&](uintptr_t off) -> void* {
        void* gun = nullptr;
        void* back = nullptr;
        return Rd(turret, off, gun) && VtableIs(gun, rva::VtGunClassTurret) && Rd(gun, GUN_turret, back) && back == turret ? gun : nullptr;
    };
    if (found)
        if (void* g = check(found)) return g;
    for (uintptr_t off = 0x400; off < 0x520; off += 8)
        if (void* g = check(off)) {
            if (!found) kcp::Log("turret: its gun is at +0x%llx", static_cast<unsigned long long>(off));
            found = off;
            return g;
        }
    return nullptr;
}

bool TurretAimPoint(void* turret, kc::Vec3& out) {
    float v[3] = {};
    if (!IsTurret(turret) || !Rd(turret, TB_aimTarget, v) || !std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
    out = {v[0], v[1], v[2]};
    return true;
}

bool AimTurret(void* turret, const kc::Vec3& target) {
    void* gun = TurretGun(turret);
    void* vt = nullptr;
    void* fn = nullptr;
    if (!gun || !Rd(gun, 0, vt) || !vt || !Rd(vt, GUNV_aimAt, fn) || !fn) return false;
    const float t[3] = {target.x, target.y, target.z};
    return AimAtSeh(fn, gun, t);
}

bool ProjectileOrientation(void* projectile, kc::Quat& out) {
    static void* fn = OgreExport("?getOrientation@Node@Ogre@@QEBA?AVQuaternion@2@XZ");
    void* node = nullptr;
    float q[4] = {1, 0, 0, 0};
    if (!fn || !Rd(projectile, PROJ_node, node) || !node || !GetOrientationSeh(fn, node, q)) return false;
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(len > 0.5f && len < 1.5f)) return false;
    out = {q[0] / len, q[1] / len, q[2] / len, q[3] / len};
    return true;
}

bool SetProjectileOrientation(void* projectile, const kc::Quat& q) {
    static void* fn = OgreExport("?setOrientation@Node@Ogre@@QEAAXMMMM@Z");
    void* node = nullptr;
    return fn && Rd(projectile, PROJ_node, node) && node && SetOrientationSeh(fn, node, q);
}

bool FireGun(void* gun, Character* me, Character* target, int stat, const kc::Vec3& aimPos, void*& projectile) {
    projectile = nullptr;
    if (!gun || !IsCharacter(me)) return false;
    const float aim[3] = {aimPos.x, aimPos.y, aimPos.z};
    // through the hooked entry: the hook catches the projectile the shot makes (and, on the host,
    // records the shot for the clients)
    if (!ShootSeh(FnAddr(FnGunShoot), gun, me, target, stat, aim)) return false;
    projectile = kcp::ranged::LastProjectile();
    return true;
}

} // namespace kenshi

// ---------------------------------------------------------------- the world (session side)
namespace kcp {

namespace {

float Dist3(const kc::Vec3& a, const kc::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// The orientation turning Ogre's forward axis (-Z) toward `d` (shortest arc): a projectile's path
// when its own orientation could not be read.
kc::Quat FacingQuat(kc::Vec3 d) {
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(len > 1e-4f)) return {};
    d = {d.x / len, d.y / len, d.z / len};
    // from (0,0,-1) to d: axis = (0,0,-1) x d = (d.y, -d.x, 0), cos = -d.z
    const float c = -d.z;
    if (c < -0.9999f) return {0, 0, 1, 0};   // straight backward: half turn about Y
    const float s = std::sqrt((1 + c) * 2);
    kc::Quat q{s * 0.5f, d.y / s, -d.x / s, 0};
    const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}

std::string TurretKey(const std::string& sid, const kc::Vec3& p) {
    return sid + "@" + std::to_string(int(std::lround(p.x))) + "," + std::to_string(int(std::lround(p.z)));
}

} // namespace

void KenshiWorld::NoteShot(void* gun, kenshi::Character* me, void* target, int stat, const float* aimPos, void* projectile) {
    WorldShot s;
    if (!kenshi::IsCharacter(me) || !kenshi::GetHandle(me, s.shooter)) return;
    if (target && kenshi::IsCharacter(target)) kenshi::GetHandle(static_cast<kenshi::Character*>(target), s.target);
    s.stat = uint8_t(stat >= 0 && stat < 256 ? stat : 0);
    float aim[3] = {};
    if (aimPos && kenshi::SafeCopy(aim, aimPos, sizeof(aim)) && std::isfinite(aim[0]) && std::isfinite(aim[1]) && std::isfinite(aim[2]))
        s.aimPos = {aim[0], aim[1], aim[2]};
    if (!projectile || !kenshi::ProjectileOrientation(projectile, s.dir)) {
        kc::Vec3 from;
        if (kenshi::GetPosition(me, from)) s.dir = FacingQuat({s.aimPos.x - from.x, s.aimPos.y - from.y - 15.0f, s.aimPos.z - from.z});
    }
    if (void* turret = kenshi::GunTurret(gun)) {
        if (!kenshi::ObjectTemplate(turret, s.turretSid) || !kenshi::ObjectPosition(turret, s.turretPos)) return;
    }
    std::lock_guard<std::mutex> lk(shotsMutex_);
    if (shotsOut_.size() < 256) shotsOut_.push_back(std::move(s));
    ++rangedCounters.noted;
}

void KenshiWorld::TakeShots(std::vector<WorldShot>& out) {
    std::lock_guard<std::mutex> lk(shotsMutex_);
    out.swap(shotsOut_);
    shotsOut_.clear();
}

void* KenshiWorld::FindTurret(const std::string& sid, const kc::Vec3& pos) {
    const std::string key = TurretKey(sid, pos);
    if (auto it = turretCache_.find(key); it != turretCache_.end()) {
        void* t = kenshi::ResolveObject(it->second);
        if (kenshi::IsTurret(t)) return t;
        turretCache_.erase(it);
    }
    std::vector<void*> around;
    kenshi::ObjectsNear(pos, 30.0f, around);
    void* best = nullptr;
    float bestD = 8.0f;   // turrets do not move: the same place, to a few units
    for (void* o : around) {
        std::string s;
        kc::Vec3 p;
        if (!kenshi::IsTurret(o) || !kenshi::ObjectTemplate(o, s) || s != sid || !kenshi::ObjectPosition(o, p)) continue;
        const float d = Dist3(p, pos);
        if (d < bestD) { bestD = d; best = o; }
    }
    kc::Handle h;
    if (best && kenshi::ObjectHandle(best, h)) turretCache_[key] = h;
    return best;
}

bool KenshiWorld::ReplayShot(const WorldShot& s) {
    kenshi::Character* me = Find(s.shooter);
    if (!me) return false;
    void* gun = nullptr;
    if (!s.turretSid.empty()) {
        void* turret = FindTurret(s.turretSid, s.turretPos);
        if (!turret) { ++rangedCounters.noTurret; return false; }
        gun = kenshi::TurretGun(turret);
    } else {
        gun = kenshi::CharacterGun(me);
    }
    if (!gun) { ++rangedCounters.noGun; return false; }
    kenshi::Character* target = s.target.valid() ? Find(s.target) : nullptr;
    void* projectile = nullptr;
    {
        HostCallScope scope;   // lets the shot through our own shoot hook
        if (!kenshi::FireGun(gun, me, target, s.stat, s.aimPos, projectile)) return false;
    }
    // the host's projectile left with its own random deviation: this one takes the same path
    if (projectile && kenshi::SetProjectileOrientation(projectile, s.dir)) ++rangedCounters.oriented;
    ++rangedCounters.replayed;
    return true;
}

bool KenshiWorld::ReadRangedAim(const kc::Handle& h, WorldAim& out) {
    kenshi::Character* c = Find(h);
    kenshi::RangedView v;
    if (!c || !kenshi::ReadRanged(c, v) || !v.combat) return false;
    out.state = v.state;
    out.aimPos = v.aimPos;
    out.target = v.target;
    return true;
}

void KenshiWorld::ApplyRangedAim(const kc::Handle& h, bool ranged, const WorldAim& a) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kenshi::RangedView v;
    v.combat = ranged;
    v.state = a.state;
    v.aimPos = a.aimPos;
    // the target as this machine knows it (stand-ins have local handles)
    bool withTarget = false;
    if (a.target.valid())
        if (kenshi::Character* t = Find(a.target)) withTarget = kenshi::GetHandle(t, v.target);
    kenshi::WriteRanged(c, v, withTarget);
}

void KenshiWorld::ReadTurrets(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::TurretAim>& out) {
    out.clear();
    std::unordered_set<void*> seen;
    for (const auto& center : centers) {
        std::vector<void*> around;
        kenshi::ObjectsNear(center, radius, around);
        for (void* o : around) {
            if (!kenshi::IsTurret(o) || !seen.insert(o).second) continue;
            kc::TurretAim t;
            if (!kenshi::TurretAimPoint(o, t.target) || !kenshi::ObjectTemplate(o, t.sid) || !kenshi::ObjectPosition(o, t.pos)) continue;
            out.push_back(std::move(t));
            if (out.size() >= kc::kMaxTurretAims) return;
        }
    }
}

void KenshiWorld::ApplyTurret(const kc::TurretAim& t) {
    void* turret = FindTurret(t.sid, t.pos);
    if (turret && kenshi::AimTurret(turret, t.target)) return;
}

// ---------------------------------------------------------------- debug commands (tests)
namespace {

std::string KeyOfHandle(const kc::Handle& h) {
    char b[96];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}

std::vector<kc::Handle> Squad(KenshiWorld& w) {
    std::vector<kc::Handle> hs;
    w.PlayerCharacters(hs);
    std::sort(hs.begin(), hs.end(), [](const kc::Handle& a, const kc::Handle& b) { return std::tie(a.index, a.serial) < std::tie(b.index, b.serial); });
    return hs;
}

// "<squad index>" or a handle key "type:container:serial:index:serial" (the host's handle)
kenshi::Character* Pick(KenshiWorld& w, const std::string& k) {
    if (k.empty()) return nullptr;
    if (k.find(':') == std::string::npos) {
        auto squad = Squad(w);
        const size_t i = size_t(std::strtoul(k.c_str(), nullptr, 10));
        return i < squad.size() ? w.FindSquad(squad[i]) : nullptr;
    }
    kc::Handle h;
    if (sscanf(k.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial) != 5) return nullptr;
    return w.Find(h);
}

std::vector<void*> TurretsAround(KenshiWorld& w, float radius) {
    std::vector<void*> out;
    auto squad = Squad(w);
    kc::Vec3 p;
    if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), p)) return out;
    std::vector<void*> objs;
    kenshi::ObjectsNear(p, radius, objs);
    for (void* o : objs)
        if (kenshi::IsTurret(o)) out.push_back(o);
    std::sort(out.begin(), out.end(), [](void* a, void* b) {   // the same order on every machine: by place
        kc::Vec3 pa, pb;
        kenshi::ObjectPosition(a, pa);
        kenshi::ObjectPosition(b, pb);
        return std::tie(pa.x, pa.z) < std::tie(pb.x, pb.z);
    });
    return out;
}

} // namespace

bool RangedDebugCommand(kc::Session& s, KenshiWorld& w, std::istringstream& in, const std::string& cmd, std::string& out) {
    char b[400];
    if (cmd == "rangedlist") {   // rangedlist [radius]: characters near squad member 0 with a ranged weapon set up: "name key combat" each
        float radius = 3000.0f;
        in >> radius;
        auto squad = Squad(w);
        kc::Vec3 me, p;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), me)) { out = "err no squad"; return true; }
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        out = "ok";
        for (kenshi::Character* c : all) {
            kenshi::RangedView v;
            kc::Handle h;
            std::string name;
            if (!kenshi::CharacterGun(c) || !kenshi::GetPosition(c, p) || !kenshi::GetHandle(c, h) || kenshi::IsDead(c)) continue;
            const float d = std::sqrt((p.x - me.x) * (p.x - me.x) + (p.z - me.z) * (p.z - me.z));
            if (d > radius) continue;
            kenshi::CharacterName(c, name);
            kenshi::ReadRanged(c, v);
            for (char& ch : name) if (ch == ' ') ch = '_';
            out += " " + name + "|" + KeyOfHandle(w.HostHandleOf(h)) + "|" + std::to_string(v.combat ? 1 : 0) + "|" + std::to_string(int(d));
        }
        return true;
    }
    if (cmd == "shoot") {   // shoot <shooter> <target>: (host) the shooter fires its ranged weapon once at the target (squad index or handle key)
        std::string a, t;
        in >> a >> t;
        kenshi::Character* shooter = Pick(w, a);
        kenshi::Character* target = Pick(w, t);
        void* gun = kenshi::CharacterGun(shooter);
        kc::Vec3 tp;
        if (!shooter || !target || !kenshi::GetPosition(target, tp)) { out = "err no such shooter or target"; return true; }
        if (!gun) { out = "err that character has no ranged weapon set up"; return true; }
        void* projectile = nullptr;
        const bool ok = kenshi::FireGun(gun, shooter, target, 0, {tp.x, tp.y + 15.0f, tp.z}, projectile);
        kc::Quat q;
        const bool oriented = projectile && kenshi::ProjectileOrientation(projectile, q);
        snprintf(b, sizeof(b), "%s projectile=%d dir=%.3f,%.3f,%.3f,%.3f", ok ? "ok" : "err shoot failed", projectile ? 1 : 0, q.w, q.x, q.y, q.z);
        out = b;
        (void)oriented;
        return true;
    }
    if (cmd == "shots") {   // shots: shot counters (host: noted / sent; client: replayed / failed)
        const auto& st = s.rangedStats();
        snprintf(b, sizeof(b), "ok sent=%llu replayed=%llu failed=%llu aims=%llu turrets=%llu noted=%llu fired=%llu nogun=%llu noturret=%llu oriented=%llu",
                 (unsigned long long)st.shotsSent, (unsigned long long)st.shotsReplayed, (unsigned long long)st.shotsFailed,
                 (unsigned long long)st.aimsApplied, (unsigned long long)st.turretsApplied, (unsigned long long)w.rangedCounters.noted,
                 (unsigned long long)w.rangedCounters.replayed, (unsigned long long)w.rangedCounters.noGun, (unsigned long long)w.rangedCounters.noTurret,
                 (unsigned long long)w.rangedCounters.oriented);
        out = b;
        return true;
    }
    if (cmd == "turrets") {   // turrets [radius]: turrets near squad member 0 (sorted by place): "sid@x,z>aimx,aimy,aimz" each
        float radius = 3000.0f;
        in >> radius;
        out = "ok";
        for (void* t : TurretsAround(w, radius)) {
            std::string sid;
            kc::Vec3 p, aim;
            kenshi::ObjectTemplate(t, sid);
            kenshi::ObjectPosition(t, p);
            kenshi::TurretAimPoint(t, aim);
            snprintf(b, sizeof(b), " %s@%d,%d>%.1f,%.1f,%.1f%s", sid.c_str(), int(std::lround(p.x)), int(std::lround(p.z)), aim.x, aim.y, aim.z,
                     kenshi::TurretGun(t) ? "" : "(nogun)");
            out += b;
        }
        return true;
    }
    if (cmd == "turretaim") {   // turretaim <index> <x> <y> <z>: (host) turret #index (as `turrets` lists them) turns toward that point
        size_t i = 0;
        float x = 0, y = 0, z = 0;
        in >> i >> x >> y >> z;
        auto ts = TurretsAround(w, 3000.0f);
        if (i >= ts.size()) { out = "err no such turret"; return true; }
        out = kenshi::AimTurret(ts[i], {x, y, z}) ? "ok" : "err aim failed";
        return true;
    }
    if (cmd == "rangedaim") {   // rangedaim <character>: its ranged combat here: "combat state aim target"
        std::string a;
        in >> a;
        kenshi::Character* c = Pick(w, a);
        kenshi::RangedView v;
        if (!c || !kenshi::ReadRanged(c, v)) { out = "err no such character"; return true; }
        snprintf(b, sizeof(b), "ok combat=%d state=%d aim=%.2f,%.2f,%.2f target=%s gun=%d", v.combat ? 1 : 0, int(v.state), v.aimPos.x, v.aimPos.y,
                 v.aimPos.z, v.target.valid() ? KeyOfHandle(v.target).c_str() : "-", kenshi::CharacterGun(c) ? 1 : 0);
        out = b;
        return true;
    }
    return false;
}

} // namespace kcp
