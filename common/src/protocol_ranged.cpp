// Lot C: ranged combat messages (shots, aim of characters and turrets).
#include "kc/protocol.h"

#include <algorithm>
#include <cmath>

namespace kc {

namespace {

void PutVec3(Writer& w, const Vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
Vec3 GetVec3(Reader& r) { Vec3 v; v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); return v; }
uint32_t GetVarU32(Reader& r) {
    const uint64_t v = r.varint();
    if (v > 0xFFFFFFFFull) r.fail();
    return static_cast<uint32_t>(v);
}
bool Finished(Reader& r) { return r.ok() && r.atEnd(); }

} // namespace

void Encode(Writer& w, const ShotsMsg& m) {
    w.u8(uint8_t(Msg::Shots));
    const size_t n = std::min<size_t>(m.shots.size(), kMaxShotsPerMsg);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        const ShotEvent& s = m.shots[i];
        w.varint(s.shooterNetId);
        w.varint(s.targetNetId);
        w.u8(s.stat);
        PutVec3(w, s.aimPos);
        w.f32(s.dir.w); w.f32(s.dir.x); w.f32(s.dir.y); w.f32(s.dir.z);   // full precision: a 100 m shot
        w.str(s.turretSid);
        if (!s.turretSid.empty()) PutVec3(w, s.turretPos);
    }
}

bool Decode(Reader& r, ShotsMsg& m) {
    const uint32_t n = r.count(kMaxShotsPerMsg, 20);
    m.shots.resize(n);
    for (auto& s : m.shots) {
        s.shooterNetId = GetVarU32(r);
        s.targetNetId = GetVarU32(r);
        s.stat = r.u8();
        s.aimPos = GetVec3(r);
        s.dir.w = r.f32(); s.dir.x = r.f32(); s.dir.y = r.f32(); s.dir.z = r.f32();
        s.turretSid = r.str(kMaxSidLen);
        if (!s.turretSid.empty()) s.turretPos = GetVec3(r);
        if (!r.ok() || s.shooterNetId == 0) return false;
        const float len = std::sqrt(s.dir.w * s.dir.w + s.dir.x * s.dir.x + s.dir.y * s.dir.y + s.dir.z * s.dir.z);
        if (!(len > 0.5f && len < 1.5f)) return false;   // not an orientation
    }
    return Finished(r);
}

void Encode(Writer& w, const RangedMsg& m) {
    w.u8(uint8_t(Msg::Ranged));
    const size_t na = std::min<size_t>(m.aims.size(), kMaxRangedAims);
    w.varint(na);
    for (size_t i = 0; i < na; ++i) {
        const RangedAim& a = m.aims[i];
        w.varint(a.netId);
        w.u8(a.state);
        PutVec3(w, a.aimPos);
        w.varint(a.targetNetId);
    }
    const size_t nt = std::min<size_t>(m.turrets.size(), kMaxTurretAims);
    w.varint(nt);
    for (size_t i = 0; i < nt; ++i) {
        w.str(m.turrets[i].sid);
        PutVec3(w, m.turrets[i].pos);
        PutVec3(w, m.turrets[i].target);
    }
    const size_t ns = std::min<size_t>(m.stopped.size(), kMaxRangedAims);
    w.varint(ns);
    for (size_t i = 0; i < ns; ++i) w.varint(m.stopped[i]);
}

bool Decode(Reader& r, RangedMsg& m) {
    const uint32_t na = r.count(kMaxRangedAims, 15);
    m.aims.resize(na);
    for (auto& a : m.aims) {
        a.netId = GetVarU32(r);
        a.state = r.u8();
        a.aimPos = GetVec3(r);
        a.targetNetId = GetVarU32(r);
        if (!r.ok() || a.netId == 0 || a.state > 4) return false;
    }
    const uint32_t nt = r.count(kMaxTurretAims, 25);
    m.turrets.resize(nt);
    for (auto& t : m.turrets) {
        t.sid = r.str(kMaxSidLen);
        t.pos = GetVec3(r);
        t.target = GetVec3(r);
        if (!r.ok() || t.sid.empty()) return false;
    }
    const uint32_t ns = r.count(kMaxRangedAims, 1);
    m.stopped.resize(ns);
    for (auto& id : m.stopped) {
        id = GetVarU32(r);
        if (!r.ok() || id == 0) return false;
    }
    return Finished(r);
}

} // namespace kc
