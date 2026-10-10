// Diplomacy: what the world thinks beyond the player faction is the host's, everywhere.
// - Relations between two factions that are not the player's: the host notes them all when it starts
//   hosting (every client loads the host's save, which holds them) and, every few seconds, sends the
//   pairs that changed since (a war declared after a leader's death, an alliance from a dialogue).
// - Unique characters (faction leaders, named NPCs): dead, alive or imprisoned, and whether the
//   player did it. The game's world states are computed from these and from the player faction's
//   relations; town overrides, dialogue conditions and campaigns test the world states, so clients
//   holding the same states take the same decisions when their own game evaluates them.
// - Towns: owner faction and override (a town taken over, destroyed, replaced by the world states).
// Each part is sent when it changes, and in full to a player who just arrived. Clients impose the
// host's values on their game, again every few seconds (their own game must not drift).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "kc/session.h"

namespace kc {

namespace {

uint64_t HashBytes(const Writer& w) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = w.data();
    for (size_t i = 0; i < w.size(); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

// Encodes one part, dropping the tail until it fits in a packet (sids are short: it never should).
Writer EncodePart(DiplomacyMsg& m) {
    for (;;) {
        Writer w(8192);
        Encode(w, m);
        const size_t n = m.part == DiploPart::Pairs ? m.pairs.size() : m.part == DiploPart::Uniques ? m.uniques.size() : m.towns.size();
        if (w.size() <= kDiplomacyBudget || n == 0) return w;
        const size_t keep = n - std::max<size_t>(1, n / 10);
        if (m.part == DiploPart::Pairs) m.pairs.resize(keep);
        else if (m.part == DiploPart::Uniques) m.uniques.resize(keep);
        else m.towns.resize(keep);
    }
}

const char* StandingFr(Standing s) { return s == Standing::Ally ? "allié" : s == Standing::Enemy ? "ennemi" : "neutre"; }
const char* StandingEn(Standing s) { return s == Standing::Ally ? "ally" : s == Standing::Enemy ? "enemy" : "neutral"; }

} // namespace

void Session::ResetDiplomacy() {
    nextDiplo_ = nextDiploApply_ = 0;
    diploBaseline_ = false;
    diploBase_.clear();
    diploChanged_.clear();
    for (auto& h : diploHash_) h = 0;
    for (auto& h : haveDiplo_) h = false;
    diploServed_.clear();
    hostDiplo_ = DiplomacyState{};
    diploDirty_ = false;
    diploView_ = DiplomacyView{};
}

Session::DiplomacyView Session::diplomacyView() const {
    DiplomacyView v = diploView_;
    v.pairs = isHost() ? diploChanged_.size() : hostDiplo_.pairs.size();
    v.uniques = hostDiplo_.uniques.size();
    v.towns = hostDiplo_.towns.size();
    return v;
}

void Session::SendDiplomacy(double now) {
    if (now < nextDiplo_) return;
    nextDiplo_ = now + 3.0;
    std::vector<PeerId> fresh;
    std::set<PeerId> live;
    for (auto& [pid, p] : players_) {
        if (!p.inGame) continue;
        live.insert(p.peer);
        if (!diploServed_.count(p.peer)) fresh.push_back(p.peer);
    }
    for (auto it = diploServed_.begin(); it != diploServed_.end();) it = live.count(*it) ? std::next(it) : diploServed_.erase(it);

    DiplomacyState cur;
    if (!world_.ReadDiplomacy(cur)) return;
    // pairs: what changed since the host started (the save every client loads holds the rest)
    if (!diploBaseline_) {
        for (const auto& p : cur.pairs) diploBase_[{p.from, p.to}] = p.rel;
        diploBaseline_ = true;
    } else {
        for (const auto& p : cur.pairs) {
            const std::pair<std::string, std::string> key{p.from, p.to};
            auto sent = diploChanged_.find(key);
            if (sent != diploChanged_.end()) {
                if (!SamePairRelation(sent->second, p.rel)) sent->second = p.rel;   // moved again (back included)
                continue;
            }
            auto base = diploBase_.find(key);
            if (base == diploBase_.end() || !SamePairRelation(base->second, p.rel)) {
                if (diploChanged_.size() < kMaxDiploPairs) diploChanged_[key] = p.rel;
            }
        }
    }
    DiplomacyState next;
    next.pairs.reserve(diploChanged_.size());
    for (const auto& [key, rel] : diploChanged_) next.pairs.push_back({key.first, key.second, rel});
    next.uniques = std::move(cur.uniques);
    next.towns = std::move(cur.towns);
    std::sort(next.uniques.begin(), next.uniques.end(), [](const UniqueState& a, const UniqueState& b) { return a.sid < b.sid; });
    std::sort(next.towns.begin(), next.towns.end(), [](const TownState& a, const TownState& b) { return a.sid < b.sid; });

    if (haveDiplo_[0]) NoteDiplomacyChanges(hostDiplo_, next);
    for (int i = 0; i < 3; ++i) {
        DiplomacyMsg m;
        m.part = DiploPart(i + 1);
        if (i == 0) m.pairs = next.pairs;
        else if (i == 1) m.uniques = next.uniques;
        else m.towns = next.towns;
        const Writer w = EncodePart(m);
        const uint64_t h = HashBytes(w);
        if (h != diploHash_[i]) {
            BroadcastReliable(w, true);
            diploHash_[i] = h;
            ++diploView_.sent;
        } else {
            for (PeerId peer : fresh) SendReliable(peer, w);
        }
        haveDiplo_[i] = true;
    }
    hostDiplo_ = std::move(next);
    for (PeerId peer : fresh) diploServed_.insert(peer);
}

bool Session::ClientDiplomacyPacket(Msg type, Reader& r) {
    if (type != Msg::Diplomacy) return false;
    if (state_ != SessionState::Connected) return true;
    DiplomacyMsg m;
    if (!Decode(r, m)) return true;
    const int i = int(m.part) - 1;
    DiplomacyState before = hostDiplo_;
    if (m.part == DiploPart::Pairs) hostDiplo_.pairs = std::move(m.pairs);
    else if (m.part == DiploPart::Uniques) hostDiplo_.uniques = std::move(m.uniques);
    else hostDiplo_.towns = std::move(m.towns);
    if (haveDiplo_[i]) NoteDiplomacyChanges(before, hostDiplo_);   // not on the first one: that is the state, not news
    haveDiplo_[i] = true;
    ++diploView_.received;
    diploDirty_ = true;
    return true;
}

void Session::ClientDiplomacyTick(double now) {
    if (!diploDirty_ && now < nextDiploApply_) return;
    const bool fresh = diploDirty_;
    nextDiploApply_ = now + 3.0;
    diploDirty_ = false;
    size_t n = 0;
    if (haveDiplo_[0]) n += world_.ApplyFactionPairs(hostDiplo_.pairs);
    if (haveDiplo_[1]) n += world_.ApplyUniques(hostDiplo_.uniques);
    if (haveDiplo_[2]) n += world_.ApplyTowns(hostDiplo_.towns);
    if (!n) return;
    diploView_.corrected += n;
    static double lastDriftLog = -1e9;
    if (fresh || now - lastDriftLog > 30.0) {
        if (!fresh) lastDriftLog = now;
        log_("diplomacy: " + std::to_string(n) + " faction pair / unique character / town values set to the host's" +
             (fresh ? "" : " (the local game had changed them)"));
    }
}

void Session::NoteDiplomacyChanges(const DiplomacyState& before, const DiplomacyState& after) {
    // wars, alliances and standings between two factions (one line per pair of factions)
    std::map<std::pair<std::string, std::string>, const RelationState*> old, now;
    for (const auto& p : before.pairs) old[{p.from, p.to}] = &p.rel;
    for (const auto& p : after.pairs) now[{p.from, p.to}] = &p.rel;
    for (const auto& [key, rel] : now) {
        if (key.second < key.first && now.count({key.second, key.first})) continue;   // the other direction speaks for both
        const std::string a = world_.TemplateName(key.first), b = world_.TemplateName(key.second);
        // before: the last version, else (host) the pair as it was when hosting began
        const RelationState* prev = nullptr;
        if (auto o = old.find(key); o != old.end()) prev = o->second;
        else if (auto b0 = diploBase_.find(key); b0 != diploBase_.end()) prev = &b0->second;
        const bool wasWar = prev && prev->war, wasAlly = prev && prev->alliance;
        const Standing was = prev ? StandingOf(*prev) : Standing::Neutral;
        const Standing is = StandingOf(*rel);
        if (rel->war != wasWar)
            AddChat(rel->war ? "Diplomatie : guerre entre " + a + " et " + b : "Diplomatie : fin de la guerre entre " + a + " et " + b,
                    "diplomacy: " + a + (rel->war ? " now at war with " : " no longer at war with ") + b);
        else if (rel->alliance != wasAlly)
            AddChat(rel->alliance ? "Diplomatie : " + a + " et " + b + " sont alliés" : "Diplomatie : " + a + " et " + b + " ne sont plus alliés",
                    "diplomacy: " + a + (rel->alliance ? " now allied with " : " no longer allied with ") + b);
        else if (prev && is != was)
            AddChat("Diplomatie : " + a + " considère maintenant " + b + " comme " + StandingFr(is),
                    "diplomacy: " + a + " now sees " + b + " as " + StandingEn(is));
    }
    // unique characters: deaths, imprisonments, releases
    std::map<std::string, const UniqueState*> oldU;
    for (const auto& u : before.uniques) oldU[u.sid] = &u;
    for (const auto& u : after.uniques) {
        auto o = oldU.find(u.sid);
        if (o == oldU.end() || o->second->state == u.state) continue;
        const std::string who = world_.TemplateName(u.sid);
        if (u.state == kUniqueDead)
            AddChat("Monde : " + who + " est mort" + (u.byPlayer ? " (de la main des joueurs)" : ""), "world: " + who + " is dead" + (u.byPlayer ? " (by the players)" : ""));
        else if (u.state == kUniqueImprisoned)
            AddChat("Monde : " + who + " est emprisonné" + (u.byPlayer ? " par les joueurs" : ""), "world: " + who + " is imprisoned" + (u.byPlayer ? " by the players" : ""));
        else if (o->second->state == kUniqueImprisoned)
            AddChat("Monde : " + who + " est libre", "world: " + who + " is free");
    }
    // towns: owner and override
    std::map<std::string, const TownState*> oldT;
    for (const auto& t : before.towns) oldT[t.sid] = &t;
    for (const auto& t : after.towns) {
        auto o = oldT.find(t.sid);
        if (o == oldT.end()) continue;
        const std::string town = world_.TemplateName(t.sid);
        if (o->second->overrideSid != t.overrideSid && !t.overrideSid.empty())
            AddChat("Monde : " + town + " a changé (" + world_.TemplateName(t.overrideSid) + ")", "world: town " + town + " now " + world_.TemplateName(t.overrideSid));
        if (o->second->ownerSid != t.ownerSid)
            AddChat("Monde : " + town + " appartient maintenant à " + (t.ownerSid.empty() ? std::string("personne") : world_.TemplateName(t.ownerSid)),
                    "world: town " + town + " now owned by " + (t.ownerSid.empty() ? std::string("nobody") : world_.TemplateName(t.ownerSid)));
    }
}

void Session::NoteFactionChanges(const FactionsMsg& before, const FactionsMsg& after) {
    for (const auto& f : after.factions) {
        auto old = std::find_if(before.factions.begin(), before.factions.end(), [&](const FactionRelationEntry& e) { return e.factionSid == f.factionSid; });
        if (old == before.factions.end()) continue;
        // what the faction thinks of the player decides how its people treat us (the game's own test)
        const bool useTheirs = f.hasTheirs && old->hasTheirs;
        if (!useTheirs && !(f.hasOurs && old->hasOurs)) continue;
        const RelationState& is = useTheirs ? f.theirs : f.ours;
        const RelationState& was = useTheirs ? old->theirs : old->ours;
        const std::string name = world_.TemplateName(f.factionSid);
        const bool war = f.ours.war || f.theirs.war, wasWar = old->ours.war || old->theirs.war;
        char rel[32];
        snprintf(rel, sizeof(rel), "%.0f", double(is.relation));
        if (war != wasWar)
            AddChat(war ? "Diplomatie : " + name + " est en guerre contre vous" : "Diplomatie : " + name + " n'est plus en guerre contre vous",
                    "diplomacy: " + name + (war ? " at war with the player" : " no longer at war with the player"));
        else if (StandingOf(is) != StandingOf(was))
            AddChat("Diplomatie : " + name + " vous considère maintenant comme " + StandingFr(StandingOf(is)) + " (relation " + rel + ")",
                    "diplomacy: " + name + " now sees the player as " + StandingEn(StandingOf(is)) + " (relation " + rel + ")");
    }
}

void Session::NoteBountyChanges(const BountiesMsg& before, const BountiesMsg& after) {
    for (const auto& c : after.chars) {
        auto old = std::find_if(before.chars.begin(), before.chars.end(), [&](const CharBounties& o) { return o.netId == c.netId; });
        if (old == before.chars.end()) continue;
        auto ent = entities_.find(c.netId);
        std::string who = ent != entities_.end() ? world_.CharacterNameOf(ent->second.handle) : std::string();
        if (who.empty()) who = "#" + std::to_string(c.netId);
        auto amountIn = [](const CharBounties& cb, const std::string& sid) {
            for (const auto& b : cb.bounties) if (b.factionSid == sid) return b.amount;
            return 0;
        };
        for (const auto& b : c.bounties) {
            const int32_t was = amountIn(*old, b.factionSid);
            if (b.amount > was)
                AddChat("Prime : " + who + " est recherché par " + world_.TemplateName(b.factionSid) + " (" + std::to_string(b.amount) + " cats)",
                        "bounty: " + who + " wanted by " + world_.TemplateName(b.factionSid) + " for " + std::to_string(b.amount));
        }
        for (const auto& b : old->bounties) {
            if (b.amount > 0 && amountIn(c, b.factionSid) <= 0)
                AddChat("Prime : " + who + " n'est plus recherché par " + world_.TemplateName(b.factionSid),
                        "bounty: " + who + " no longer wanted by " + world_.TemplateName(b.factionSid));
        }
    }
}

} // namespace kc
