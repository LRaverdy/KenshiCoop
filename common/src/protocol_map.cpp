// Map messages: every player character's position, the hostile squads near the players, pings.
#include "kc/protocol.h"

#include <algorithm>
#include <cmath>

namespace kc {

namespace {

void PutVec3(Writer& w, const Vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
Vec3 GetVec3(Reader& r) { Vec3 v; v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); return v; }
bool FiniteVec(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
uint32_t GetVarU32(Reader& r) {
    const uint64_t v = r.varint();
    if (v > 0xFFFFFFFFull) r.fail();
    return static_cast<uint32_t>(v);
}
bool Finished(Reader& r) { return r.ok() && r.atEnd(); }
std::string Clip(const std::string& s) { return s.size() > kMaxMapLabelLen ? s.substr(0, kMaxMapLabelLen) : s; }

} // namespace

void Encode(Writer& w, const MapMarkersMsg& m) {
    w.u8(uint8_t(Msg::MapMarkers));
    const size_t np = std::min<size_t>(m.players.size(), kMaxMapPlayers);
    w.varint(np);
    for (size_t i = 0; i < np; ++i) {
        w.u8(m.players[i].id);
        w.str(m.players[i].name.substr(0, kMaxNameLen));
    }
    const size_t nc = std::min<size_t>(m.chars.size(), kMaxMapChars);
    w.varint(nc);
    for (size_t i = 0; i < nc; ++i) {
        const MapChar& c = m.chars[i];
        w.varint(c.netId);
        w.u8(c.owner);
        w.u8(c.flags);
        w.str(Clip(c.name));
        PutVec3(w, c.pos);
    }
    const size_t nt = std::min<size_t>(m.threats.size(), kMaxMapThreats);
    w.varint(nt);
    for (size_t i = 0; i < nt; ++i) {
        const MapThreat& t = m.threats[i];
        PutVec3(w, t.pos);
        w.u8(t.count);
        w.u8(uint8_t(t.kind));
        w.str(Clip(t.label));
    }
}

bool Decode(Reader& r, MapMarkersMsg& m) {
    m = MapMarkersMsg{};
    const uint32_t np = r.count(kMaxMapPlayers, 2);
    m.players.resize(np);
    for (auto& p : m.players) {
        p.id = r.u8();
        p.name = r.str(kMaxNameLen);
        if (p.id == 0) return false;
    }
    const uint32_t nc = r.count(kMaxMapChars, 15);
    m.chars.resize(nc);
    for (auto& c : m.chars) {
        c.netId = GetVarU32(r);
        c.owner = r.u8();
        c.flags = r.u8();
        c.name = r.str(kMaxMapLabelLen);
        c.pos = GetVec3(r);
        if (!r.ok() || c.netId == 0 || (c.flags & ~0x07) || !FiniteVec(c.pos)) return false;
    }
    const uint32_t nt = r.count(kMaxMapThreats, 15);
    m.threats.resize(nt);
    for (auto& t : m.threats) {
        t.pos = GetVec3(r);
        t.count = r.u8();
        const uint8_t k = r.u8();
        t.label = r.str(kMaxMapLabelLen);
        if (!r.ok() || k < 1 || k > 3 || !FiniteVec(t.pos)) return false;
        t.kind = ThreatKind(k);
    }
    return Finished(r);
}

void Encode(Writer& w, const MapPingMsg& m) {
    w.u8(uint8_t(Msg::MapPing));
    w.varint(m.id);
    w.u8(m.owner);
    w.u8(uint8_t(m.kind));
    PutVec3(w, m.pos);
}

bool Decode(Reader& r, MapPingMsg& m) {
    m.id = GetVarU32(r);
    m.owner = r.u8();
    const uint8_t k = r.u8();
    m.pos = GetVec3(r);
    if (!r.ok() || k >= kPingKinds || !FiniteVec(m.pos)) return false;
    m.kind = PingKind(k);
    return Finished(r);
}

} // namespace kc
