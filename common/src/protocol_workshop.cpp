// Workshop messages: the research state, research requests, machines (operators, power, crafting
// orders), town power totals, machine requests. Every count is bounded and every float checked.
#include "kc/protocol.h"

#include <algorithm>
#include <cmath>

namespace kc {

namespace {

void PutV(Writer& w, const Vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
Vec3 GetV(Reader& r) { Vec3 v; v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); return v; }
bool FiniteV(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
uint32_t VarU32(Reader& r) {
    const uint64_t v = r.varint();
    if (v > 0xFFFFFFFFull) r.fail();
    return static_cast<uint32_t>(v);
}
bool End(Reader& r) { return r.ok() && r.atEnd(); }
float Fin(float f) { return std::isfinite(f) ? f : 0.0f; }

void PutItem(Writer& w, const ItemState& s) {
    w.str(s.templateSid.substr(0, kMaxSidLen));
    w.str(s.section.substr(0, kMaxSidLen));
    w.varint(uint32_t(std::max(s.quantity, 0)));
    w.u16(uint16_t(s.x));
    w.u16(uint16_t(s.y));
}
void GetItem(Reader& r, ItemState& s) {
    s = ItemState{};
    s.templateSid = r.str(kMaxSidLen);
    s.section = r.str(kMaxSidLen);
    s.quantity = int32_t(std::min<uint32_t>(VarU32(r), 1000000));
    s.x = int16_t(r.u16());
    s.y = int16_t(r.u16());
}

} // namespace

// Same order as kenshi.cpp kStatOffsets.
const char* StatNameFr(size_t index) {
    static const char* const kNames[kStatCount] = {
        "Force", "Attaque", "Travail de force", "Science", "Ingénierie", "Robotique", "Forge d'armes", "Fabrication d'armures",
        "Médecine", "Vol", "Tourelles", "Agriculture", "Cuisine", "Discrétion", "Athlétisme", "Dextérité", "Défense",
        "Robustesse", "Assassinat", "Natation", "Perception", "Katanas", "Sabres", "Hackers", "Armes lourdes", "Armes contondantes",
        "Arts martiaux", "Esquive", "Armes d'hast", "Arbalètes", "Tir ami", "Crochetage", "Fabrication d'arbalètes", "Combat de masse"};
    return index < kStatCount ? kNames[index] : "?";
}

void Encode(Writer& w, const ResearchState& m) {
    w.u8(uint8_t(Msg::Research));
    w.varint(uint32_t(std::clamp(m.deskLevel, 0, 1000)));
    const size_t nf = std::min<size_t>(m.finished.size(), kMaxResearchFinished);
    w.varint(nf);
    for (size_t i = 0; i < nf; ++i) w.str(m.finished[i].substr(0, kMaxSidLen));
    const size_t nq = std::min<size_t>(m.queue.size(), kMaxResearchQueue);
    w.varint(nq);
    for (size_t i = 0; i < nq; ++i) {
        w.str(m.queue[i].sid.substr(0, kMaxSidLen));
        w.f32(Fin(m.queue[i].progress));
    }
}
bool Decode(Reader& r, ResearchState& m) {
    m = ResearchState{};
    m.deskLevel = int32_t(std::min<uint32_t>(VarU32(r), 1000));
    const uint32_t nf = r.count(kMaxResearchFinished, 1);
    m.finished.resize(nf);
    for (auto& s : m.finished) {
        s = r.str(kMaxSidLen);
        if (s.empty()) return false;
    }
    const uint32_t nq = r.count(kMaxResearchQueue, 5);
    m.queue.resize(nq);
    for (auto& q : m.queue) {
        q.sid = r.str(kMaxSidLen);
        q.progress = r.f32();
        if (q.sid.empty() || !std::isfinite(q.progress)) return false;
    }
    return End(r);
}

void Encode(Writer& w, const ResearchRequest& m) {
    w.u8(uint8_t(Msg::ResearchRequest));
    w.varint(m.seq);
    w.varint(m.actorNetId);
    w.u8(uint8_t(m.action));
    w.str(m.sid.substr(0, kMaxSidLen));
    if (m.action == ResearchAction::LearnBlueprint) PutItem(w, m.item);
}
bool Decode(Reader& r, ResearchRequest& m) {
    m = ResearchRequest{};
    m.seq = VarU32(r);
    m.actorNetId = VarU32(r);
    const uint8_t a = r.u8();
    if (a < 1 || a > 3) return false;
    m.action = ResearchAction(a);
    m.sid = r.str(kMaxSidLen);
    if (m.action == ResearchAction::LearnBlueprint) GetItem(r, m.item);
    return End(r) && m.actorNetId != 0 && !m.sid.empty();
}

void Encode(Writer& w, const MachinesMsg& m) {
    w.u8(uint8_t(Msg::Machines));
    w.boolean(m.full);
    const size_t n = std::min<size_t>(m.machines.size(), kMaxMachinesPerMsg);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        const MachineState& s = m.machines[i];
        w.str(s.sid.substr(0, kMaxSidLen));
        PutV(w, s.pos);
        w.varint(s.netId);
        w.u8(s.flags);
        w.u8(s.maxOperators);
        w.u8(s.operatorCount);
        const size_t no = std::min<size_t>(s.operators.size(), kMaxMachineOperators);
        w.varint(no);
        for (size_t k = 0; k < no; ++k) w.varint(s.operators[k]);
        w.f32(Fin(s.power));
        w.f32(Fin(s.stored));
        w.f32(Fin(s.progress));
        w.f32(Fin(s.production));
        const size_t nc = std::min<size_t>(s.crafts.size(), kMaxCraftOrders);
        w.varint(nc);
        for (size_t k = 0; k < nc; ++k) {
            const CraftOrder& c = s.crafts[k];
            w.str(c.baseSid.substr(0, kMaxSidLen));
            w.str(c.materialSid.substr(0, kMaxSidLen));
            w.str(c.itemSid.substr(0, kMaxSidLen));
            w.f32(Fin(c.progress));
        }
    }
    const size_t nt = std::min<size_t>(m.towns.size(), kMaxTownPower);
    w.varint(nt);
    for (size_t i = 0; i < nt; ++i) {
        const TownPower& t = m.towns[i];
        w.str(t.sid.substr(0, kMaxSidLen));
        PutV(w, t.pos);
        for (float v : t.values) w.f32(Fin(v));
        w.boolean(t.onBattery);
    }
}
bool Decode(Reader& r, MachinesMsg& m) {
    m = MachinesMsg{};
    m.full = r.boolean();
    const uint32_t n = r.count(kMaxMachinesPerMsg, 36);
    m.machines.resize(n);
    for (auto& s : m.machines) {
        s.sid = r.str(kMaxSidLen);
        s.pos = GetV(r);
        s.netId = VarU32(r);
        s.flags = r.u8();
        s.maxOperators = r.u8();
        s.operatorCount = r.u8();
        const uint32_t no = r.count(kMaxMachineOperators, 1);
        s.operators.resize(no);
        for (auto& o : s.operators) o = VarU32(r);
        s.power = r.f32();
        s.stored = r.f32();
        s.progress = r.f32();
        s.production = r.f32();
        const uint32_t nc = r.count(kMaxCraftOrders, 7);
        s.crafts.resize(nc);
        for (auto& c : s.crafts) {
            c.baseSid = r.str(kMaxSidLen);
            c.materialSid = r.str(kMaxSidLen);
            c.itemSid = r.str(kMaxSidLen);
            c.progress = r.f32();
            if (c.baseSid.empty() || !std::isfinite(c.progress)) return false;
        }
        if (s.sid.empty() || !FiniteV(s.pos) || !std::isfinite(s.power) || !std::isfinite(s.stored) || !std::isfinite(s.progress) ||
            !std::isfinite(s.production))
            return false;
    }
    const uint32_t nt = r.count(kMaxTownPower, 46);
    m.towns.resize(nt);
    for (auto& t : m.towns) {
        t.sid = r.str(kMaxSidLen);
        t.pos = GetV(r);
        for (float& v : t.values) {
            v = r.f32();
            if (!std::isfinite(v)) return false;
        }
        t.onBattery = r.boolean();
        if (t.sid.empty() || !FiniteV(t.pos)) return false;
    }
    return End(r);
}

void Encode(Writer& w, const MachineRequest& m) {
    w.u8(uint8_t(Msg::MachineRequest));
    w.varint(m.seq);
    w.varint(m.actorNetId);
    w.u8(uint8_t(m.action));
    w.str(m.sid.substr(0, kMaxSidLen));
    PutV(w, m.pos);
    w.str(m.baseSid.substr(0, kMaxSidLen));
    w.str(m.materialSid.substr(0, kMaxSidLen));
    w.varint(uint32_t(std::clamp(m.index, 0, 1000)));
    w.boolean(m.value);
}
bool Decode(Reader& r, MachineRequest& m) {
    m = MachineRequest{};
    m.seq = VarU32(r);
    m.actorNetId = VarU32(r);
    const uint8_t a = r.u8();
    if (a < 1 || a > 5) return false;
    m.action = MachineAction(a);
    m.sid = r.str(kMaxSidLen);
    m.pos = GetV(r);
    m.baseSid = r.str(kMaxSidLen);
    m.materialSid = r.str(kMaxSidLen);
    m.index = int32_t(std::min<uint32_t>(VarU32(r), 1000));
    m.value = r.boolean();
    if (m.action == MachineAction::AddCraft && m.baseSid.empty()) return false;
    return End(r) && m.actorNetId != 0 && !m.sid.empty() && FiniteV(m.pos);
}

} // namespace kc
