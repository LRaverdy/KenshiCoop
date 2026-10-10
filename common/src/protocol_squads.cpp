// Squad window and AI settings: squad state, squad requests, job lists with their targets.
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
void PutHand(Writer& w, const Handle& h) {
    w.varint(h.type); w.varint(h.container); w.varint(h.containerSerial); w.varint(h.index); w.varint(h.serial);
}
Handle GetHand(Reader& r) {
    Handle h;
    h.type = GetVarU32(r); h.container = GetVarU32(r); h.containerSerial = GetVarU32(r); h.index = GetVarU32(r); h.serial = GetVarU32(r);
    return h;
}
bool Finished(Reader& r) { return r.ok() && r.atEnd(); }
std::string Clip(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

} // namespace

bool IsDeadSquadName(const std::string& name) { return name == kDeadSquadName; }

std::string CleanSquadName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) continue;   // control characters (UTF-8 bytes >= 0x80 are kept)
        out.push_back(char(c));
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    out = out.substr(b, out.find_last_not_of(' ') - b + 1);
    if (out.size() > kMaxSquadName) {
        out.resize(kMaxSquadName);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();   // a cut UTF-8 sequence
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80)) out.pop_back();
    }
    return out;
}

const char* ToString(SquadOp op) {
    switch (op) {
    case SquadOp::Move: return "move to squad";
    case SquadOp::Create: return "create squad";
    case SquadOp::Rename: return "rename squad";
    case SquadOp::Order: return "reorder squads";
    case SquadOp::Remove: return "remove squad";
    case SquadOp::RenameCharacter: return "rename character";
    }
    return "?";
}

void Encode(Writer& w, const SquadStateMsg& m) {
    w.u8(uint8_t(Msg::SquadState));
    w.varint(m.rev);
    const size_t ns = std::min<size_t>(m.squads.size(), kMaxSquads);
    w.varint(ns);
    for (size_t i = 0; i < ns; ++i) {
        const SquadEntry& s = m.squads[i];
        PutHand(w, s.id);
        w.str(Clip(s.name, kMaxSquadName * 2));
        const size_t k = std::min<size_t>(s.members.size(), kMaxSquadMembers);
        w.varint(k);
        for (size_t j = 0; j < k; ++j) w.varint(s.members[j]);
    }
    const size_t nm = std::min<size_t>(m.members.size(), kMaxSquadMembers);
    w.varint(nm);
    for (size_t i = 0; i < nm; ++i) {
        w.varint(m.members[i].netId);
        w.str(Clip(m.members[i].name, kMaxSquadName * 2));
        w.u8(m.members[i].flags);
    }
}
bool Decode(Reader& r, SquadStateMsg& m) {
    m.rev = GetVarU32(r);
    const uint32_t ns = r.count(kMaxSquads, 6);
    m.squads.resize(ns);
    for (auto& s : m.squads) {
        s.id = GetHand(r);
        s.name = r.str(kMaxSquadName * 2);
        const uint32_t k = r.count(kMaxSquadMembers, 1);
        s.members.resize(k);
        for (auto& id : s.members) {
            id = GetVarU32(r);
            if (!id) r.fail();
        }
        if (!r.ok()) return false;
    }
    const uint32_t nm = r.count(kMaxSquadMembers, 3);
    m.members.resize(nm);
    for (auto& c : m.members) {
        c.netId = GetVarU32(r);
        c.name = r.str(kMaxSquadName * 2);
        c.flags = r.u8();
        if (!r.ok() || !c.netId) return false;
    }
    return Finished(r);
}
bool SameSquadState(const SquadStateMsg& a, const SquadStateMsg& b) {
    if (a.squads.size() != b.squads.size() || a.members.size() != b.members.size()) return false;
    for (size_t i = 0; i < a.squads.size(); ++i)
        if (a.squads[i].id != b.squads[i].id || a.squads[i].name != b.squads[i].name || a.squads[i].members != b.squads[i].members) return false;
    for (size_t i = 0; i < a.members.size(); ++i)
        if (a.members[i].netId != b.members[i].netId || a.members[i].name != b.members[i].name || a.members[i].flags != b.members[i].flags) return false;
    return true;
}

void Encode(Writer& w, const SquadRequest& m) {
    w.u8(uint8_t(Msg::SquadRequest));
    w.varint(m.seq);
    w.varint(m.actor);
    w.u8(uint8_t(m.op));
    PutHand(w, m.squad);
    w.i32(m.index);
    w.str(Clip(m.name, kMaxSquadName * 2));
}
bool Decode(Reader& r, SquadRequest& m) {
    m.seq = GetVarU32(r);
    m.actor = GetVarU32(r);
    const uint8_t op = r.u8();
    m.squad = GetHand(r);
    m.index = r.i32();
    m.name = r.str(kMaxSquadName * 2);
    if (!Finished(r) || !m.actor || op < uint8_t(SquadOp::Move) || op > uint8_t(SquadOp::RenameCharacter)) return false;
    m.op = SquadOp(op);
    if (m.index < 0 || m.index > int32_t(kMaxSquadMembers)) return false;
    if ((m.op == SquadOp::Rename || m.op == SquadOp::Order || m.op == SquadOp::Remove) && !m.squad.valid()) return false;
    if ((m.op == SquadOp::Rename || m.op == SquadOp::RenameCharacter) && CleanSquadName(m.name).empty()) return false;
    return true;
}

void Encode(Writer& w, const JobStateMsg& m) {
    w.u8(uint8_t(Msg::JobState));
    const size_t n = std::min<size_t>(m.chars.size(), kMaxJobLists);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        const CharJobs& c = m.chars[i];
        w.varint(c.netId);
        const size_t k = std::min<size_t>(c.jobs.size(), kMaxJobsPerCharacter);
        w.varint(k);
        for (size_t j = 0; j < k; ++j) {
            const JobEntry& e = c.jobs[j];
            w.i32(e.task);
            PutHand(w, e.subject);
            w.str(Clip(e.subjectSid, kMaxSidLen));
            PutVec3(w, e.subjectPos);
            PutVec3(w, e.location);
        }
    }
}
bool Decode(Reader& r, JobStateMsg& m) {
    const uint32_t n = r.count(kMaxJobLists, 2);
    m.chars.resize(n);
    for (auto& c : m.chars) {
        c.netId = GetVarU32(r);
        const uint32_t k = r.count(kMaxJobsPerCharacter, 34);
        c.jobs.resize(k);
        for (auto& e : c.jobs) {
            e.task = r.i32();
            e.subject = GetHand(r);
            e.subjectSid = r.str(kMaxSidLen);
            e.subjectPos = GetVec3(r);
            e.location = GetVec3(r);
            if (!r.ok() || e.task < 0 || e.task >= 512 || !FiniteVec(e.subjectPos) || !FiniteVec(e.location)) return false;
        }
        if (!r.ok() || !c.netId) return false;
    }
    return Finished(r);
}

} // namespace kc
