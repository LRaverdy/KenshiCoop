// Lot B: wire format of the faction relations and bounties (see protocol.h).
#include "kc/protocol.h"

#include <algorithm>
#include <cmath>

namespace kc {

namespace {

bool Done(Reader& r) { return r.ok() && r.atEnd(); }

uint32_t U32Var(Reader& r) {
    const uint64_t v = r.varint();
    if (v > 0xFFFFFFFFull) r.fail();
    return static_cast<uint32_t>(v);
}

void PutRelation(Writer& w, const RelationState& s) {
    w.u8(uint8_t((s.alliance ? 1 : 0) | (s.peace ? 2 : 0) | (s.war ? 4 : 0) | (s.coexists ? 8 : 0)));
    w.f32(s.relation);
    w.f32(s.trustPositives);
    w.f32(s.trustNegatives);
    w.f32(s.strength);
}

void GetRelation(Reader& r, RelationState& s) {
    const uint8_t f = r.u8();
    if (f > 15) r.fail();
    s.alliance = f & 1;
    s.peace = f & 2;
    s.war = f & 4;
    s.coexists = f & 8;
    s.relation = r.f32();
    s.trustPositives = r.f32();
    s.trustNegatives = r.f32();
    s.strength = r.f32();
}

} // namespace

void Encode(Writer& w, const FactionsMsg& m) {
    w.u8(uint8_t(Msg::Factions));
    w.i32(m.playerRank);
    w.f32(m.reputationTrust);
    w.f32(m.reputationBadassery);
    const size_t n = std::min<size_t>(m.factions.size(), kMaxFactions);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& f = m.factions[i];
        w.str(f.factionSid);
        w.u8(uint8_t((f.hasOurs ? 1 : 0) | (f.hasTheirs ? 2 : 0)));
        if (f.hasOurs) PutRelation(w, f.ours);
        if (f.hasTheirs) PutRelation(w, f.theirs);
    }
}

bool Decode(Reader& r, FactionsMsg& m) {
    m.playerRank = r.i32();
    m.reputationTrust = r.f32();
    m.reputationBadassery = r.f32();
    const uint32_t n = r.count(kMaxFactions, 2);
    m.factions.resize(n);
    for (auto& f : m.factions) {
        f.factionSid = r.str(kMaxSidLen);
        const uint8_t has = r.u8();
        if (has > 3 || f.factionSid.empty()) return false;
        f.hasOurs = has & 1;
        f.hasTheirs = has & 2;
        if (f.hasOurs) GetRelation(r, f.ours);
        if (f.hasTheirs) GetRelation(r, f.theirs);
        if (!r.ok()) return false;
    }
    return Done(r);
}

void Encode(Writer& w, const BountiesMsg& m) {
    w.u8(uint8_t(Msg::Bounties));
    const size_t n = std::min<size_t>(m.chars.size(), kMaxBountyChars);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& c = m.chars[i];
        w.varint(c.netId);
        const size_t b = std::min<size_t>(c.bounties.size(), kMaxBountiesPerChar);
        w.varint(b);
        for (size_t k = 0; k < b; ++k) {
            const auto& e = c.bounties[k];
            w.str(e.factionSid);
            w.i32(e.amount);
            w.u32(e.crimes);
            w.boolean(e.claimed);
            w.u64(e.since);
        }
        w.i32(c.crime);
        w.str(c.crimeFactionSid);
        w.f32(c.crimeExpiry);
        w.f32(c.prisonSentence);
        w.u64(c.prisonBegan);
        w.str(c.accessPassSid);
        w.u64(c.accessPassUntil);
    }
}

bool Decode(Reader& r, BountiesMsg& m) {
    const uint32_t n = r.count(kMaxBountyChars, 8);
    m.chars.resize(n);
    for (auto& c : m.chars) {
        c.netId = U32Var(r);
        const uint32_t b = r.count(kMaxBountiesPerChar, 18);
        c.bounties.resize(b);
        for (auto& e : c.bounties) {
            e.factionSid = r.str(kMaxSidLen);
            e.amount = r.i32();
            e.crimes = r.u32();
            e.claimed = r.boolean();
            e.since = r.u64();
            if (!r.ok() || e.factionSid.empty()) return false;
        }
        c.crime = r.i32();
        c.crimeFactionSid = r.str(kMaxSidLen);
        c.crimeExpiry = r.f32();
        c.prisonSentence = r.f32();
        c.prisonBegan = r.u64();
        c.accessPassSid = r.str(kMaxSidLen);
        c.accessPassUntil = r.u64();
        if (!r.ok() || c.netId == 0 || c.crime < 0 || c.crime > 64) return false;
    }
    return Done(r);
}

} // namespace kc
