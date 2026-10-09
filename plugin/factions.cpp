// Lot B: faction relations, bounties and crimes, read on the host and imposed on clients.
//
// Layouts (Kenshi 1.0.68, checked by disassembly, see docs/MOTEUR.md):
//  Faction: +0x78 FactionRelations*, +0x240 GameData* (string id), PlayerInterface +0x2A0 the player's.
//  FactionRelations: +0x10 playerRank (int), +0x14 globalReputationTrust, +0x18 ...ForBadassery,
//    +0x20 boost::unordered_map<Faction*, RelationData> (node: next +0, key +0x10, value +0x18).
//  RelationData: alliance, peaceTreaty, war, coexists (bytes 0..3), relation +4, trust+ +8, trust- +0xC,
//    perceived strength +0x10.
//  Character +0xF0 BountyManager: +0 unordered_map<Faction*, Bounty> (node: key +0x10, amount +0x18,
//    crimes +0x1C, claimed +0x20, since +0x28), +0x48 access pass faction, +0x50 its expiry,
//    +0x58 crime being committed, +0x60 against which faction, +0x90 crime expiry,
//    +0x98 prison sentence began, +0xA0 hours to serve.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "hooks.h"
#include "kenshi.h"
#include "util.h"
#include "world.h"

namespace kcp {

namespace {

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try { std::memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T>
bool Rd(const void* p, uintptr_t off, T& out) {
    return p && SafeCopy(&out, reinterpret_cast<const uint8_t*>(p) + off, sizeof(T));
}
template <class T>
bool Wr(void* p, uintptr_t off, const T& v) {
    return p && SafeCopy(reinterpret_cast<uint8_t*>(p) + off, &v, sizeof(T));
}

bool ReadStr(const void* p, std::string& out) {
    uint64_t size = 0, cap = 0;
    if (!Rd(p, 0x10, size) || !Rd(p, 0x18, cap) || size > cap || cap > 4096) return false;
    const void* chars = p;
    if (cap >= 16 && !Rd(p, 0, chars)) return false;
    out.resize(size_t(size));
    return size == 0 || SafeCopy(out.data(), chars, size_t(size));
}

constexpr uintptr_t PI_faction = 0x2A0, FAC_relations = 0x78, FAC_data = 0x240, GD_sid = 0x58;
constexpr uintptr_t FR_rank = 0x10, FR_trust = 0x14, FR_badass = 0x18, FR_map = 0x20;
constexpr uintptr_t MAP_bucketCount = 0x18, MAP_size = 0x20, MAP_buckets = 0x38;
constexpr uintptr_t NODE_next = 0x0, NODE_key = 0x10, NODE_value = 0x18;
constexpr uintptr_t CH_bounties = 0xF0;
constexpr uintptr_t BM_pass = 0x48, BM_passUntil = 0x50, BM_crime = 0x58, BM_crimeFaction = 0x60, BM_crimeExpiry = 0x90,
                    BM_prisonBegan = 0x98, BM_prisonLeft = 0xA0;
constexpr uintptr_t BN_amount = 0x18, BN_crimes = 0x1C, BN_claimed = 0x20, BN_since = 0x28;   // bounty map node
constexpr uintptr_t BP_amount = 0x8, BP_crimes = 0xC, BP_claimed = 0x10, BP_since = 0x18;     // pair returned by operator[]

std::string FactionSid(void* f) {
    void* gd = nullptr;
    std::string s;
    if (Rd(f, FAC_data, gd) && gd) ReadStr(reinterpret_cast<uint8_t*>(gd) + GD_sid, s);
    return s;
}

void* PlayerFaction() {
    void* f = nullptr;
    kenshi::PlayerInterface* pi = kenshi::Player();
    return pi && Rd(pi, PI_faction, f) ? f : nullptr;
}

std::vector<void*> AllFactions() {
    std::vector<void*> out;
    void* mgr = nullptr;
    uint32_t count = 0;
    void** data = nullptr;
    kenshi::GameWorld* w = kenshi::World();
    if (!w || !Rd(w, kenshi::off::GW_factionMgr, mgr) || !mgr || !Rd(mgr, kenshi::off::LK_count, count) || count > 10000 ||
        !Rd(mgr, kenshi::off::LK_data, data) || !data)
        return out;
    for (uint32_t i = 0; i < count; ++i) {
        void* f = nullptr;
        if (Rd(data, i * sizeof(void*), f) && f) out.push_back(f);
    }
    return out;
}

// every (key, node) of one of the game's boost::unordered_map<Faction*, T>
template <class F>
void ForEachNode(const void* map, F&& fn) {
    uint64_t size = 0, buckets = 0;
    void** table = nullptr;
    if (!Rd(map, MAP_size, size) || size == 0 || size > 100000 || !Rd(map, MAP_bucketCount, buckets) || !Rd(map, MAP_buckets, table) || !table) return;
    void* node = nullptr;
    if (!Rd(table, buckets * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* key = nullptr;
        if (Rd(node, NODE_key, key)) fn(key, node);
        if (!Rd(node, NODE_next, node)) break;
    }
}

bool ReadRelation(const void* data, kc::RelationState& s) {
    uint8_t flags[4] = {};
    float v[4] = {};
    if (!Rd(data, 0, flags) || !Rd(data, 4, v)) return false;
    s.alliance = flags[0] != 0;
    s.peace = flags[1] != 0;
    s.war = flags[2] != 0;
    s.coexists = flags[3] != 0;
    float* f[4] = {&s.relation, &s.trustPositives, &s.trustNegatives, &s.strength};
    for (int i = 0; i < 4; ++i) *f[i] = std::isfinite(v[i]) ? v[i] : 0.0f;
    return true;
}

bool WriteRelation(void* data, const kc::RelationState& s) {
    const uint8_t flags[4] = {uint8_t(s.alliance), uint8_t(s.peace), uint8_t(s.war), uint8_t(s.coexists)};
    const float v[4] = {s.relation, s.trustPositives, s.trustNegatives, s.strength};
    return Wr(data, 0, flags) && Wr(data, 4, v);
}

using FnRelDataSig = void* (*)(void* relations, void* faction);
void* RelationDataSeh(void* relations, void* faction) {
    __try { return reinterpret_cast<FnRelDataSig>(kenshi::FnAddr(kenshi::FnGetRelationData))(relations, faction); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
using FnBountyIndexSig = void* (*)(void* map, void* const* key);
void* BountyEntrySeh(void* map, void* faction) {
    __try { return reinterpret_cast<FnBountyIndexSig>(kenshi::FnAddr(kenshi::FnBountyMapIndex))(map, &faction); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// the relation data `relations` holds about `other`, without creating it
const void* FindRelation(void* relations, void* other) {
    const void* found = nullptr;
    if (relations)
        ForEachNode(reinterpret_cast<uint8_t*>(relations) + FR_map, [&](void* key, void* node) {
            if (key == other && !found) found = reinterpret_cast<uint8_t*>(node) + NODE_value;
        });
    return found;
}

std::map<std::string, void*> FactionsBySid() {
    std::map<std::string, void*> out;
    for (void* f : AllFactions()) {
        std::string s = FactionSid(f);
        if (!s.empty()) out.emplace(std::move(s), f);
    }
    return out;
}

} // namespace

bool KenshiWorld::ReadFactions(kc::FactionsMsg& out) {
    out = kc::FactionsMsg{};
    void* pf = PlayerFaction();
    void* prel = nullptr;
    if (!pf || !Rd(pf, FAC_relations, prel) || !prel) return false;
    Rd(prel, FR_rank, out.playerRank);
    Rd(prel, FR_trust, out.reputationTrust);
    Rd(prel, FR_badass, out.reputationBadassery);
    if (!std::isfinite(out.reputationTrust)) out.reputationTrust = 0;
    if (!std::isfinite(out.reputationBadassery)) out.reputationBadassery = 0;
    std::map<void*, const void*> ours;
    ForEachNode(reinterpret_cast<uint8_t*>(prel) + FR_map, [&](void* key, void* node) { ours[key] = reinterpret_cast<uint8_t*>(node) + NODE_value; });
    for (void* f : AllFactions()) {
        if (f == pf) continue;
        kc::FactionRelationEntry e;
        e.factionSid = FactionSid(f);
        if (e.factionSid.empty() || e.factionSid.size() > kc::kMaxSidLen) continue;
        if (auto it = ours.find(f); it != ours.end()) e.hasOurs = ReadRelation(it->second, e.ours);
        void* rel = nullptr;
        if (Rd(f, FAC_relations, rel) && rel)
            if (const void* d = FindRelation(rel, pf)) e.hasTheirs = ReadRelation(d, e.theirs);
        if (e.hasOurs || e.hasTheirs) out.factions.push_back(std::move(e));
    }
    std::sort(out.factions.begin(), out.factions.end(), [](const auto& a, const auto& b) { return a.factionSid < b.factionSid; });
    out.factions.erase(std::unique(out.factions.begin(), out.factions.end(), [](const auto& a, const auto& b) { return a.factionSid == b.factionSid; }),
                       out.factions.end());
    if (out.factions.size() > kc::kMaxFactions) out.factions.resize(kc::kMaxFactions);
    return true;
}

size_t KenshiWorld::ApplyFactions(const kc::FactionsMsg& m) {
    void* pf = PlayerFaction();
    void* prel = nullptr;
    if (!pf || !Rd(pf, FAC_relations, prel) || !prel) return 0;
    size_t n = 0;
    int32_t rank = 0;
    float trust = 0, badass = 0;
    if (Rd(prel, FR_rank, rank) && rank != m.playerRank) n += Wr(prel, FR_rank, m.playerRank);
    if (Rd(prel, FR_trust, trust) && trust != m.reputationTrust) n += Wr(prel, FR_trust, m.reputationTrust);
    if (Rd(prel, FR_badass, badass) && badass != m.reputationBadassery) n += Wr(prel, FR_badass, m.reputationBadassery);
    const auto bySid = FactionsBySid();
    HostCallScope scope;
    for (const auto& e : m.factions) {
        auto it = bySid.find(e.factionSid);
        if (it == bySid.end() || it->second == pf) continue;
        void* f = it->second;
        auto impose = [&](void* relations, void* about, const kc::RelationState& want) {
            if (!relations) return;
            void* d = RelationDataSeh(relations, about);   // made if this world has none yet
            kc::RelationState cur;
            if (d && ReadRelation(d, cur) && !(cur == want) && WriteRelation(d, want)) ++n;
        };
        if (e.hasOurs) impose(prel, f, e.ours);
        void* rel = nullptr;
        if (e.hasTheirs && Rd(f, FAC_relations, rel)) impose(rel, pf, e.theirs);
    }
    return n;
}

bool KenshiWorld::ReadBounties(const kc::Handle& h, kc::CharBounties& out) {
    out = kc::CharBounties{};
    kenshi::Character* c = Find(h);
    if (!kenshi::IsCharacter(c)) return false;
    auto* bm = reinterpret_cast<uint8_t*>(c) + CH_bounties;
    ForEachNode(bm, [&](void* key, void* node) {
        kc::BountyEntry b;
        b.factionSid = FactionSid(key);
        uint8_t claimed = 0;
        if (b.factionSid.empty() || !Rd(node, BN_amount, b.amount) || !Rd(node, BN_crimes, b.crimes)) return;
        Rd(node, BN_claimed, claimed);
        Rd(node, BN_since, b.since);
        b.claimed = claimed != 0;
        if ((b.amount != 0 || b.crimes != 0) && out.bounties.size() < kc::kMaxBountiesPerChar) out.bounties.push_back(std::move(b));
    });
    std::sort(out.bounties.begin(), out.bounties.end(), [](const auto& a, const auto& b) { return a.factionSid < b.factionSid; });
    void* crimeFaction = nullptr;
    void* pass = nullptr;
    Rd(bm, BM_crime, out.crime);
    if (out.crime < 0 || out.crime > 64) out.crime = 0;
    if (Rd(bm, BM_crimeFaction, crimeFaction) && crimeFaction) out.crimeFactionSid = FactionSid(crimeFaction);
    Rd(bm, BM_crimeExpiry, out.crimeExpiry);
    Rd(bm, BM_prisonLeft, out.prisonSentence);
    Rd(bm, BM_prisonBegan, out.prisonBegan);
    if (Rd(bm, BM_pass, pass) && pass) out.accessPassSid = FactionSid(pass);
    Rd(bm, BM_passUntil, out.accessPassUntil);
    if (!std::isfinite(out.crimeExpiry)) out.crimeExpiry = 0;
    if (!std::isfinite(out.prisonSentence)) out.prisonSentence = 0;
    return true;
}

size_t KenshiWorld::ApplyBounties(const kc::Handle& h, const kc::CharBounties& want) {
    kenshi::Character* c = Find(h);
    if (!kenshi::IsCharacter(c)) return 0;
    auto* bm = reinterpret_cast<uint8_t*>(c) + CH_bounties;
    const auto bySid = FactionsBySid();
    size_t n = 0;
    // bounties the host no longer has: emptied (the entry itself stays, as the game's own clearing
    // does more than that)
    std::vector<void*> stale;
    ForEachNode(bm, [&](void* key, void* node) {
        const std::string sid = FactionSid(key);
        const bool kept = std::any_of(want.bounties.begin(), want.bounties.end(), [&](const kc::BountyEntry& b) { return b.factionSid == sid; });
        int32_t amount = 0;
        uint32_t crimes = 0;
        if (!kept && Rd(node, BN_amount, amount) && Rd(node, BN_crimes, crimes) && (amount || crimes)) stale.push_back(node);
    });
    for (void* node : stale) {
        Wr(node, BN_amount, int32_t(0));
        Wr(node, BN_crimes, uint32_t(0));
        ++n;
    }
    HostCallScope scope;
    for (const auto& b : want.bounties) {
        auto it = bySid.find(b.factionSid);
        if (it == bySid.end()) continue;
        void* pair = BountyEntrySeh(bm, it->second);
        int32_t amount = 0;
        uint32_t crimes = 0;
        uint8_t claimed = 0;
        uint64_t since = 0;
        if (!pair || !Rd(pair, BP_amount, amount) || !Rd(pair, BP_crimes, crimes)) continue;
        Rd(pair, BP_claimed, claimed);
        Rd(pair, BP_since, since);
        if (amount == b.amount && crimes == b.crimes && (claimed != 0) == b.claimed && since == b.since) continue;
        Wr(pair, BP_amount, b.amount);
        Wr(pair, BP_crimes, b.crimes);
        Wr(pair, BP_claimed, uint8_t(b.claimed ? 1 : 0));
        Wr(pair, BP_since, b.since);
        ++n;
    }
    // the crime being committed, the prison sentence and the access pass
    auto factionOf = [&](const std::string& sid) -> void* {
        if (sid.empty()) return nullptr;
        auto it = bySid.find(sid);
        return it == bySid.end() ? nullptr : it->second;
    };
    auto set = [&](uintptr_t off, auto value) {
        decltype(value) cur{};
        if (Rd(bm, off, cur) && cur != value && Wr(bm, off, value)) ++n;
    };
    set(BM_crime, want.crime);
    set(BM_crimeFaction, factionOf(want.crimeFactionSid));
    set(BM_crimeExpiry, want.crimeExpiry);
    set(BM_prisonLeft, want.prisonSentence);
    set(BM_prisonBegan, want.prisonBegan);
    set(BM_pass, factionOf(want.accessPassSid));
    set(BM_passUntil, want.accessPassUntil);
    return n;
}

} // namespace kcp
