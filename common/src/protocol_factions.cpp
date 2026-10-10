// Lot B: wire format of the faction relations and bounties, and of the diplomacy (see protocol.h).
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

// ---- diplomacy
void Encode(Writer& w, const DiplomacyMsg& m) {
    w.u8(uint8_t(Msg::Diplomacy));
    w.u8(uint8_t(m.part));
    switch (m.part) {
    case DiploPart::Pairs: {
        const size_t n = std::min<size_t>(m.pairs.size(), kMaxDiploPairs);
        w.varint(n);
        for (size_t i = 0; i < n; ++i) {
            w.str(m.pairs[i].from);
            w.str(m.pairs[i].to);
            PutRelation(w, m.pairs[i].rel);
        }
        break;
    }
    case DiploPart::Uniques: {
        const size_t n = std::min<size_t>(m.uniques.size(), kMaxUniques);
        w.varint(n);
        for (size_t i = 0; i < n; ++i) {
            w.str(m.uniques[i].sid);
            w.u8(uint8_t(m.uniques[i].state | (m.uniques[i].byPlayer ? 0x80 : 0)));
        }
        break;
    }
    case DiploPart::Towns: {
        const size_t n = std::min<size_t>(m.towns.size(), kMaxTowns);
        w.varint(n);
        for (size_t i = 0; i < n; ++i) {
            w.str(m.towns[i].sid);
            w.str(m.towns[i].ownerSid);
            w.str(m.towns[i].overrideSid);
        }
        break;
    }
    }
}

bool Decode(Reader& r, DiplomacyMsg& m) {
    m = DiplomacyMsg{};
    const uint8_t part = r.u8();
    if (part < uint8_t(DiploPart::Pairs) || part > uint8_t(DiploPart::Towns)) return false;
    m.part = DiploPart(part);
    switch (m.part) {
    case DiploPart::Pairs:
        m.pairs.resize(r.count(kMaxDiploPairs, 2 + 2 + 17));
        for (auto& p : m.pairs) {
            p.from = r.str(kMaxSidLen);
            p.to = r.str(kMaxSidLen);
            GetRelation(r, p.rel);
            if (!r.ok() || p.from.empty() || p.to.empty() || p.from == p.to) return false;
        }
        break;
    case DiploPart::Uniques:
        m.uniques.resize(r.count(kMaxUniques, 3));
        for (auto& u : m.uniques) {
            u.sid = r.str(kMaxSidLen);
            const uint8_t v = r.u8();
            u.state = v & 0x7F;
            u.byPlayer = (v & 0x80) != 0;
            if (!r.ok() || u.sid.empty() || u.state > kUniqueImprisoned) return false;
        }
        break;
    case DiploPart::Towns:
        m.towns.resize(r.count(kMaxTowns, 4));
        for (auto& t : m.towns) {
            t.sid = r.str(kMaxSidLen);
            t.ownerSid = r.str(kMaxSidLen);
            t.overrideSid = r.str(kMaxSidLen);
            if (!r.ok() || t.sid.empty()) return false;
        }
        break;
    }
    return Done(r);
}

} // namespace kc
