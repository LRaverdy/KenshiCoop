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
#include <set>

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
    if (Rd(prel, FR_trust, trust) && !kc::CloseEnough(trust, m.reputationTrust, 0.5f)) n += Wr(prel, FR_trust, m.reputationTrust);
    if (Rd(prel, FR_badass, badass) && !kc::CloseEnough(badass, m.reputationBadassery, 0.5f)) n += Wr(prel, FR_badass, m.reputationBadassery);
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
            if (d && ReadRelation(d, cur) && !kc::SameRelation(cur, want) && WriteRelation(d, want)) ++n;
        };
        if (e.hasOurs) impose(prel, f, e.ours);
        void* rel = nullptr;
        if (e.hasTheirs && Rd(f, FAC_relations, rel)) impose(rel, pf, e.theirs);
    }
    return n;
}

bool KenshiWorld::ReadBounties(const kc::Handle& h, kc::CharBounties& out) {
    out = kc::CharBounties{};
    if (client_) {   // a client's game holds none: what it shows is the host's
        auto it = hostBounties_.find(h);
        if (it != hostBounties_.end()) out = it->second;
        return Find(h) != nullptr;
    }
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
    if (client_) {
        // Not written into a client's game. A bounty or a crime there wakes its own law enforcement
        // (guards, bounty hunters) against a character the host drives: every client crashed within
        // 7-17 s of a new bounty (10/10), and again on loading the host's save holding it, which no
        // write of ours was involved in. The client keeps the host's copy (ReadBounties, the bounty
        // command) and empties whatever its own game puts there: the law is the host's.
        hostBounties_[h] = want;
        size_t n = 0;
        ForEachNode(bm, [&](void*, void* node) {
            int32_t amount = 0;
            uint32_t crimes = 0;
            if (Rd(node, BN_amount, amount) && Rd(node, BN_crimes, crimes) && (amount || crimes)) {
                Wr(node, BN_amount, int32_t(0));
                Wr(node, BN_crimes, uint32_t(0));
                ++n;
            }
        });
        int32_t crime = 0;
        if (Rd(bm, BM_crime, crime) && crime != 0 && Wr(bm, BM_crime, int32_t(0))) ++n;
        return n;
    }
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

// ---------------------------------------------------------------- diplomacy
// Layouts (Kenshi 1.0.68, by disassembly, see docs/MOTEUR.md "Diplomatie"):
//  UniqueNPCManager (*0x212EB00) is a boost::unordered_map<GameData*, UniqueCharacterState>: node
//    key +0x10, value +0x18 = { GameData* data, hand squad (0x20), int state +0x28, bool player +0x2C }
//    (node +0x18 / +0x20 / +0x40 / +0x44). state: 0 dead, 1 alive (also what an absent entry
//    means, getState 0x5E87F0), 2 imprisoned; player: the player faction did it.
//  TownList (*0x2134100) +0x50 lektor<TownBase*>; TownBase: +0x10 owner Faction*, +0x18 name,
//    +0x40 GameData*, +0x338 override GameData* (TownBase::setOverride 0x9FE7A0).
namespace {

constexpr uintptr_t UQ_state = 0x40, UQ_player = 0x44;                   // unique map node
constexpr uintptr_t UP_data = 0x8, UP_state = 0x30, UP_player = 0x34;    // pair returned by operator[]
constexpr uintptr_t TL_count = 0x58, TL_data = 0x60;
constexpr uintptr_t TB_owner = 0x10, TB_data = 0x40, TB_override = 0x338;

void* UniqueManager() {
    void* m = nullptr;
    return Rd(reinterpret_cast<void*>(kenshi::Addr(kenshi::rva::UniqueNPCManager)), 0, m) ? m : nullptr;
}

std::string DataSid(const void* gd) {
    std::string s;
    if (gd) ReadStr(reinterpret_cast<const uint8_t*>(gd) + GD_sid, s);
    return s;
}

std::vector<void*> AllTowns() {
    std::vector<void*> out;
    void* list = nullptr;
    uint32_t count = 0;
    void** data = nullptr;
    if (!Rd(reinterpret_cast<void*>(kenshi::Addr(kenshi::rva::TownList)), 0, list) || !list || !Rd(list, TL_count, count) || count > 20000 ||
        !Rd(list, TL_data, data) || !data)
        return out;
    for (uint32_t i = 0; i < count; ++i) {
        void* t = nullptr;
        if (Rd(data, i * sizeof(void*), t) && t) out.push_back(t);
    }
    return out;
}

using FnUniqueIndexSig = void* (*)(void* map, void* const* key);
void* UniqueEntrySeh(void* map, void* gd) {
    __try { return reinterpret_cast<FnUniqueIndexSig>(kenshi::FnAddr(kenshi::FnUniqueMapIndex))(map, &gd); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
using FnTownOverrideSig = void (*)(void* town, void* gd);
bool TownOverrideSeh(void* town, void* gd) {
    __try { reinterpret_cast<FnTownOverrideSig>(kenshi::FnAddr(kenshi::FnTownSetOverride))(town, gd); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using FnTownFactionSig = void (*)(void* town, void* faction, bool b);
bool TownFactionSeh(void* town, void* faction) {
    __try { reinterpret_cast<FnTownFactionSig>(kenshi::FnAddr(kenshi::FnTownSetFaction))(town, faction, false); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// a town as the mod names it: its game data id, its owner, its override
bool ReadTown(void* t, kc::TownState& out) {
    void* gd = nullptr;
    void* owner = nullptr;
    void* ov = nullptr;
    out = kc::TownState{};
    if (!Rd(t, TB_data, gd) || !gd) return false;
    out.sid = DataSid(gd);
    if (out.sid.empty() || out.sid.size() > kc::kMaxSidLen) return false;
    if (Rd(t, TB_owner, owner) && owner) out.ownerSid = FactionSid(owner);
    if (Rd(t, TB_override, ov) && ov) out.overrideSid = DataSid(ov);
    return true;
}

} // namespace

bool KenshiWorld::ReadDiplomacy(kc::DiplomacyState& out) {
    out = kc::DiplomacyState{};
    void* pf = PlayerFaction();
    if (!pf) return false;
    // every faction toward every other, the player faction left out (the Factions message has it)
    std::map<void*, std::string> sids;
    const auto factions = AllFactions();
    for (void* f : factions) sids[f] = FactionSid(f);
    for (void* f : factions) {
        const std::string& from = sids[f];
        if (f == pf || from.empty() || from.size() > kc::kMaxSidLen) continue;
        void* rel = nullptr;
        if (!Rd(f, FAC_relations, rel) || !rel) continue;
        ForEachNode(reinterpret_cast<uint8_t*>(rel) + FR_map, [&](void* key, void* node) {
            if (!key || key == pf || key == f) return;
            auto it = sids.find(key);
            if (it == sids.end() || it->second.empty() || it->second.size() > kc::kMaxSidLen || it->second == from) return;
            kc::FactionPairRelation p;
            p.from = from;
            p.to = it->second;
            if (ReadRelation(reinterpret_cast<uint8_t*>(node) + NODE_value, p.rel)) out.pairs.push_back(std::move(p));
        });
    }
    // unique characters
    if (void* m = UniqueManager())
        ForEachNode(m, [&](void* key, void* node) {
            kc::UniqueState u;
            int32_t state = 1;
            uint8_t player = 0;
            u.sid = DataSid(key);
            if (u.sid.empty() || u.sid.size() > kc::kMaxSidLen || !Rd(node, UQ_state, state) || state < 0 || state > 2) return;
            Rd(node, UQ_player, player);
            u.state = uint8_t(state);
            u.byPlayer = player != 0;
            out.uniques.push_back(std::move(u));
        });
    // towns
    for (void* t : AllTowns()) {
        kc::TownState ts;
        if (ReadTown(t, ts)) out.towns.push_back(std::move(ts));
    }
    std::sort(out.uniques.begin(), out.uniques.end(), [](const auto& a, const auto& b) { return a.sid < b.sid; });
    std::sort(out.towns.begin(), out.towns.end(), [](const auto& a, const auto& b) { return a.sid < b.sid; });
    out.towns.erase(std::unique(out.towns.begin(), out.towns.end(), [](const auto& a, const auto& b) { return a.sid == b.sid; }), out.towns.end());
    return true;
}

size_t KenshiWorld::ApplyFactionPairs(const std::vector<kc::FactionPairRelation>& pairs) {
    if (pairs.empty()) return 0;
    void* pf = PlayerFaction();
    const auto bySid = FactionsBySid();
    size_t n = 0;
    HostCallScope scope;
    for (const auto& p : pairs) {
        auto a = bySid.find(p.from), b = bySid.find(p.to);
        if (a == bySid.end() || b == bySid.end() || a->second == pf || b->second == pf || a->second == b->second) continue;
        void* rel = nullptr;
        if (!Rd(a->second, FAC_relations, rel) || !rel) continue;
        void* d = RelationDataSeh(rel, b->second);   // made if this world has none yet
        kc::RelationState cur;
        if (d && ReadRelation(d, cur) && !kc::SamePairRelation(cur, p.rel) && WriteRelation(d, p.rel)) ++n;
    }
    return n;
}

size_t KenshiWorld::ApplyUniques(const std::vector<kc::UniqueState>& uniques) {
    void* m = UniqueManager();
    if (!m || uniques.empty()) return 0;
    std::map<std::string, void*> nodes;   // this game's entries, by character id
    ForEachNode(m, [&](void* key, void* node) {
        std::string sid = DataSid(key);
        if (!sid.empty()) nodes.emplace(std::move(sid), node);
    });
    size_t n = 0;
    std::set<std::string> wanted;
    HostCallScope scope;
    for (const auto& u : uniques) {
        wanted.insert(u.sid);
        int32_t state = -1;
        uint8_t player = 0;
        auto it = nodes.find(u.sid);
        if (it != nodes.end()) {
            if (Rd(it->second, UQ_state, state) && Rd(it->second, UQ_player, player) && state == u.state && (player != 0) == u.byPlayer) continue;
            Wr(it->second, UQ_state, int32_t(u.state));
            Wr(it->second, UQ_player, uint8_t(u.byPlayer ? 1 : 0));
            ++n;
            continue;
        }
        if (u.state == kc::kUniqueAlive && !u.byPlayer) continue;   // an absent entry already means that
        void* gd = kenshi::GameDataBySid(u.sid);
        void* pair = gd ? UniqueEntrySeh(m, gd) : nullptr;   // made as the game's loading makes it
        if (!pair) continue;
        Wr(pair, UP_data, gd);
        Wr(pair, UP_state, int32_t(u.state));
        Wr(pair, UP_player, uint8_t(u.byPlayer ? 1 : 0));
        ++n;
    }
    // entries the host has not: alive there (getState's answer for a missing one)
    for (const auto& [sid, node] : nodes) {
        int32_t state = 1;
        if (wanted.count(sid) || !Rd(node, UQ_state, state) || state == 1) continue;
        Wr(node, UQ_state, int32_t(1));
        Wr(node, UQ_player, uint8_t(0));
        ++n;
    }
    return n;
}

size_t KenshiWorld::ApplyTowns(const std::vector<kc::TownState>& towns) {
    if (towns.empty()) return 0;
    std::map<std::string, const kc::TownState*> want;
    for (const auto& t : towns) want[t.sid] = &t;
    const auto bySid = FactionsBySid();
    size_t n = 0;
    HostCallScope scope;
    for (void* t : AllTowns()) {
        kc::TownState cur;
        if (!ReadTown(t, cur)) continue;
        auto it = want.find(cur.sid);
        if (it == want.end()) continue;
        const kc::TownState& w = *it->second;
        // the override first: it brings its own faction, the owner below corrects that if needed
        if (!w.overrideSid.empty() && w.overrideSid != cur.overrideSid) {
            void* gd = kenshi::GameDataBySid(w.overrideSid);
            if (gd && TownOverrideSeh(t, gd)) {
                ++n;
                Log("diplomacy: town %s takes the host's override %s (was %s)", cur.sid.c_str(), w.overrideSid.c_str(),
                    cur.overrideSid.empty() ? "none" : cur.overrideSid.c_str());
                ReadTown(t, cur);
            }
        } else if (w.overrideSid.empty() && !cur.overrideSid.empty()) {
            static std::set<std::string> said;   // the game has no way back to "no override": noted once
            if (said.insert(cur.sid).second)
                Log("diplomacy: town %s has override %s here, none on the host (left as is)", cur.sid.c_str(), cur.overrideSid.c_str());
        }
        if (!w.ownerSid.empty() && w.ownerSid != cur.ownerSid) {
            auto f = bySid.find(w.ownerSid);
            if (f != bySid.end() && TownFactionSeh(t, f->second)) {
                ++n;
                Log("diplomacy: town %s now owned by %s as on the host (was %s)", cur.sid.c_str(), w.ownerSid.c_str(), cur.ownerSid.c_str());
            }
        }
    }
    return n;
}

bool KenshiWorld::SetFactionPair(const std::string& fromSid, const std::string& toSid, const kc::RelationState& rel) {
    return ApplyFactionPairs({{fromSid, toSid, rel}, {toSid, fromSid, rel}}) > 0;
}

bool KenshiWorld::SetUniqueState(const std::string& sid, uint8_t state, bool byPlayer) {
    kc::DiplomacyState d;
    if (!ReadDiplomacy(d)) return false;
    bool found = false;
    for (auto& u : d.uniques)
        if (u.sid == sid) {
            u.state = state;
            u.byPlayer = byPlayer;
            found = true;
        }
    if (!found) d.uniques.push_back({sid, state, byPlayer});
    return ApplyUniques(d.uniques) > 0;
}

bool KenshiWorld::SetTownOwner(const std::string& townSid, const std::string& factionSid) {
    kc::DiplomacyState d;
    if (!ReadDiplomacy(d)) return false;
    for (auto& t : d.towns)
        if (t.sid == townSid) {
            t.ownerSid = factionSid;
            return ApplyTowns({t}) > 0;
        }
    return false;
}

} // namespace kcp
