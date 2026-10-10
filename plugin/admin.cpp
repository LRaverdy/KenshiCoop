#include "admin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "hooks.h"
#include "kenshi.h"
#include "util.h"

namespace kcp {

namespace {

namespace adm = kc::admin;

adm::GodRegistry g_gods;
bool g_wasHosting = false;
double g_nextGodPass = 0;
uint32_t g_generation = 0;

struct Member {
    kc::Handle h;
    kenshi::Character* c = nullptr;
    uint8_t owner = 0;
};

std::vector<Member> Squad(kc::Session& s, KenshiWorld& w) {
    std::vector<kc::Handle> hs;
    w.PlayerCharacters(hs);
    std::vector<Member> out;
    for (const auto& h : hs)
        if (kenshi::Character* c = w.FindSquad(h)) out.push_back({h, c, s.ownerOf(h)});
    return out;
}

std::string KeyOf(kc::Session& s, uint8_t id) {
    if (id == s.localId()) return adm::kHostKey;
    auto it = s.players().find(id);
    return it == s.players().end() ? std::string() : adm::PlayerKey(it->second.steamId, it->second.name);
}

std::vector<std::string> CurrentKeys(kc::Session& s) {
    std::vector<std::string> keys{adm::kHostKey};
    for (const auto& [id, p] : s.players()) keys.push_back(adm::PlayerKey(p.steamId, p.name));
    return keys;
}

// Who a target names: player ids (the host's own included), or none when it names nobody.
std::vector<uint8_t> Resolve(kc::Session& s, const adm::Target& t) {
    std::vector<uint8_t> ids;
    if (t.all || t.host) ids.push_back(s.localId());
    if (t.all)
        for (const auto& [id, p] : s.players()) ids.push_back(id);
    if (t.id >= 0) {
        if (t.id == s.localId() || s.players().count(uint8_t(t.id))) ids.push_back(uint8_t(t.id));
    }
    return ids;
}

std::string NameOf(kc::Session& s, uint8_t id) {
    if (id == s.localId()) return "l'hôte";
    auto it = s.players().find(id);
    return it == s.players().end() ? "joueur " + std::to_string(id) : it->second.name;
}

std::string EnglishWho(kc::Session& s, const adm::Target& t) {
    if (t.all) return "everyone";
    if (t.host || t.id == s.localId()) return "the host";
    auto it = s.players().find(uint8_t(std::max(0, t.id)));
    return "player " + std::to_string(t.id) + (it != s.players().end() ? " (" + it->second.name + ")" : "");
}

std::string FrenchWho(kc::Session& s, const adm::Target& t) {
    if (t.all) return "tout le monde";
    return NameOf(s, uint8_t(t.host ? s.localId() : t.id));
}

// The affected player's notice: theirs in their chat, the host's as a toast.
void Notify(kc::Session& s, KenshiWorld& w, uint8_t id, const std::string& fr) {
    if (id == s.localId()) w.Toast(fr);
    else if (!s.SendNotice(id, fr)) Log("admin: no notice for player %u (gone?)", unsigned(id));
}

bool Wanted(const std::vector<uint8_t>& ids, uint8_t owner) { return std::find(ids.begin(), ids.end(), owner) != ids.end(); }

void ApplyGods(kc::Session& s, KenshiWorld& w) {
    if (!g_gods.any()) {
        if (kenshi::GodModeCount()) kenshi::SetGodModes({});
        return;
    }
    std::vector<kenshi::Character*> on;
    for (const auto& m : Squad(s, w))
        if (g_gods.On(KeyOf(s, m.owner))) on.push_back(m.c);
    kenshi::SetGodModes(on);
}

bool Down(kenshi::Character* c) { return kenshi::IsDown(c) || kenshi::IsUnconscious(c) || kenshi::IsDead(c) || kenshi::IsRagdoll(c); }

std::string Fmt(const char* fmt, double a, double b) {
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
}

std::string RunGod(const adm::Command& c, kc::Session& s, KenshiWorld& w, const std::vector<uint8_t>& ids) {
    if (c.who.all) g_gods.SetAll(c.on);
    else
        for (uint8_t id : ids) g_gods.Set(KeyOf(s, id), c.on, CurrentKeys(s));
    int n = 0;
    {
        CallScopeGuard scopeRepair("admin");
        HostCallScope scope;
        for (const auto& m : Squad(s, w)) {
            if (!c.who.all && !Wanted(ids, m.owner)) continue;
            if (c.on) kenshi::HealCompletely(m.c);   // god mode starts from a healthy body
            ++n;
        }
    }
    ApplyGods(s, w);
    for (uint8_t id : ids)
        Notify(s, w, id, c.on ? "L'hôte t'a mis en mode dieu : plus aucun dégât ni K.-O." : "L'hôte a retiré ton mode dieu.");
    Log("admin: god mode %s for %s: %d character(s), %zu in god mode now", c.on ? "on" : "off", EnglishWho(s, c.who).c_str(), n,
        kenshi::GodModeCount());
    return std::string("mode dieu ") + (c.on ? "activé" : "désactivé") + " pour " + FrenchWho(s, c.who) + " (" + std::to_string(n) + " perso(s))";
}

std::string RunHeal(const adm::Command& c, kc::Session& s, KenshiWorld& w, const std::vector<uint8_t>& ids) {
    int n = 0, woke = 0;
    CallScopeGuard scopeRepair("admin");
    HostCallScope scope;
    for (const auto& m : Squad(s, w)) {
        if (!Wanted(ids, m.owner) || kenshi::IsDead(m.c)) continue;
        if (!kenshi::HealCompletely(m.c)) continue;
        ++n;
        if (kenshi::IsUnconscious(m.c) && kenshi::StandUp(m.c)) ++woke;   // still out cold after the healing: wake it
    }
    for (uint8_t id : ids) Notify(s, w, id, "L'hôte a soigné tes personnages.");
    Log("admin: healed %d character(s) of %s (%d woken up)", n, EnglishWho(s, c.who).c_str(), woke);
    return std::to_string(n) + " perso(s) de " + FrenchWho(s, c.who) + " soigné(s)";
}

std::string RunMoney(const adm::Command& c, kc::Session& s, KenshiWorld& w, bool& ok) {
    int32_t cur = 0;
    if (!kenshi::ReadPlayerMoney(cur)) { ok = false; return "argent illisible"; }
    const long long next = std::max(0LL, std::min(2000000000LL, (long long)cur + c.money));
    kenshi::WritePlayerMoney(int32_t(next));
    Log("admin: money %lld -> %lld cats", (long long)cur, next);
    const std::string fr = "L'hôte a " + std::string(c.money > 0 ? "ajouté " : "retiré ") + std::to_string(std::llabs(c.money)) +
                           " cats à l'argent commun.";
    for (const auto& [id, p] : s.players()) Notify(s, w, id, fr);
    return "argent : " + std::to_string(cur) + " -> " + std::to_string(next) + " cats";
}

std::string RunXp(const adm::Command& c, kc::Session& s, KenshiWorld& w, const std::vector<uint8_t>& ids, bool& ok) {
    std::vector<size_t> skills;
    if (c.skill == adm::kAllSkills) for (size_t i = 0; i < kc::kStatCount; ++i) skills.push_back(i);
    else skills.push_back(size_t(c.skill));
    int chars = 0, written = 0;
    double gained = 0;
    std::string detail;
    CallScopeGuard scopeRepair("admin");
    HostCallScope scope;
    for (const auto& m : Squad(s, w)) {
        if (!Wanted(ids, m.owner)) continue;
        bool any = false;
        for (size_t k : skills) {
            float before = 0;
            if (!kenshi::ReadStat(m.c, k, before)) continue;
            any = true;
            auto read = [&] { float v = -1; return kenshi::ReadStat(m.c, k, v) ? v : -1.0f; };
            auto inc = [&](float a) { kenshi::GainExperience(m.c, k, a); };
            if (c.levels) {
                const float target = std::min(adm::kStatCap, before + float(c.amount));
                // near 100 the game's gains shrink to nothing: the last bit is written as the game would
                if (!adm::RaiseTo(target, read, inc, 600) && kenshi::WriteStat(m.c, k, target)) ++written;
            } else {
                adm::GiveXp(c.amount, inc);
            }
            const float after = read();
            gained += after - before;
            if (skills.size() == 1) {
                std::string nm;
                kenshi::CharacterName(m.c, nm);
                Log("admin: xp: %s %s %.3f -> %.3f", nm.c_str(), adm::kSkills[k].key, before, after);
                detail += (detail.empty() ? "" : ", ") + (nm.empty() ? std::string("?") : nm) + Fmt(" %.1f -> %.1f", before, after);
            }
        }
        chars += any ? 1 : 0;
    }
    if (!chars) { ok = false; return "aucun personnage trouvé pour " + FrenchWho(s, c.who); }
    const std::string skillFr = c.skill == adm::kAllSkills ? "toutes les compétences" : adm::kSkills[c.skill].fr;
    const std::string skillEn = c.skill == adm::kAllSkills ? "every skill" : adm::kSkills[c.skill].key;
    char amount[32];
    snprintf(amount, sizeof(amount), "%g", c.amount);
    const std::string what = c.levels ? std::string(amount) + " niveau(x)" : std::string(amount) + " points d'expérience";
    for (uint8_t id : ids) Notify(s, w, id, "L'hôte t'a donné " + what + (c.skill == adm::kAllSkills ? " dans " : " en ") + skillFr + ".");
    Log("admin: xp %s %s in %s for %s: %d character(s), +%.2f levels in all%s", amount, c.levels ? "levels" : "points", skillEn.c_str(),
        EnglishWho(s, c.who).c_str(), chars, gained, written ? " (some written directly near 100)" : "");
    char total[48];
    snprintf(total, sizeof(total), ", +%.1f niveaux en tout", gained);
    return what + (c.skill == adm::kAllSkills ? " dans " : " en ") + skillFr + " pour " + FrenchWho(s, c.who) + " (" + std::to_string(chars) +
           " perso(s))" + (detail.empty() ? std::string(total) : " : " + detail);
}

bool FirstPosition(const std::vector<Member>& squad, uint8_t owner, kc::Vec3& out) {
    for (const auto& m : squad)
        if (m.owner == owner && !kenshi::IsDead(m.c) && kenshi::GetPosition(m.c, out)) return true;
    for (const auto& m : squad)
        if (m.owner == owner && kenshi::GetPosition(m.c, out)) return true;
    return false;
}

std::string RunTp(const adm::Command& c, kc::Session& s, KenshiWorld& w, const std::vector<uint8_t>& ids, bool& ok) {
    const auto squad = Squad(s, w);
    const uint8_t host = s.localId();
    kc::Vec3 dest;
    int destOwner = -1;   // the player they are brought to (their own characters stay put)
    std::string whereFr, whereEn;
    switch (c.tp) {
    case adm::TpTo::Host: {
        destOwner = host;
        bool have = false;
        std::vector<kc::Handle> sel;   // the host's selected character, else its first one
        kenshi::SelectedHandles(sel);
        for (const auto& h : sel)
            for (const auto& m : squad)
                if (!have && m.h == h && m.owner == host && kenshi::GetPosition(m.c, dest)) have = true;
        if (!have) have = FirstPosition(squad, host, dest);
        if (!have) { ok = false; return "l'hôte n'a pas de personnage"; }
        whereFr = "près de lui";
        whereEn = "the host";
        break;
    }
    case adm::TpTo::Player: {
        const std::vector<uint8_t> to = Resolve(s, c.to);
        if (to.empty()) { ok = false; return "destination inconnue : tape players pour les numéros"; }
        destOwner = to.front();
        if (!FirstPosition(squad, to.front(), dest)) { ok = false; return NameOf(s, to.front()) + " n'a pas de personnage"; }
        whereFr = "près de " + NameOf(s, to.front());
        whereEn = "player " + std::to_string(to.front());
        break;
    }
    case adm::TpTo::Point:
        if (!LastMoveOrderPoint(dest)) { ok = false; return "aucun point : fais d'abord un clic droit au sol (ordre de déplacement), puis recommence"; }
        whereFr = "sur un point de la carte";
        whereEn = "the host's last move point";
        break;
    case adm::TpTo::Pos:
        dest = c.pos;
        whereFr = "sur un point de la carte";
        whereEn = "a position";
        break;
    }
    std::vector<kc::Handle> movers;
    std::vector<uint8_t> moved;
    size_t down = 0;
    for (const auto& m : squad) {
        if (!Wanted(ids, m.owner) || int(m.owner) == destOwner) continue;
        movers.push_back(m.h);
        down += Down(m.c) ? 1 : 0;
        if (!Wanted(moved, m.owner)) moved.push_back(m.owner);
    }
    if (movers.empty()) { ok = false; return "personne à téléporter (" + FrenchWho(s, c.who) + " n'a pas de personnage, ou est déjà la destination)"; }
    // their games load the destination zone at once and freeze meanwhile: the link must wait (fix G6)
    for (uint8_t id : moved)
        if (id != host) s.ExpectStall(id, 120.0);
    const int n = w.TeleportCharacters(movers, dest);
    for (uint8_t id : moved) {
        if (id == host) Notify(s, w, id, "Tu es téléporté " + whereFr + ".");
        else Notify(s, w, id, "L'hôte t'a téléporté " + (c.tp == adm::TpTo::Host ? std::string("près de lui") : whereFr) + ".");
    }
    if (c.tp == adm::TpTo::Player && destOwner != host && Wanted(moved, host)) Notify(s, w, uint8_t(destOwner), "L'hôte vient te rejoindre.");
    Log("admin: teleported %d of %zu character(s) of %s to %s at (%.1f, %.1f, %.1f)%s", n, movers.size(), EnglishWho(s, c.who).c_str(),
        whereEn.c_str(), dest.x, dest.y, dest.z, down ? " (some were lying down: stood up first)" : "");
    if (!n) ok = false;
    return std::to_string(n) + " perso(s) de " + FrenchWho(s, c.who) + " téléporté(s) " + whereFr;
}

std::string RunList(kc::Session& s, KenshiWorld& w) {
    std::string out;
    for (const auto& p : AdminPlayers(s, w, "hôte")) {
        out += (out.empty() ? "" : "\n") + std::to_string(p.id) + ". " + p.name + (p.host ? "" : "  ping " + std::to_string(p.pingMs) + " ms") + "  " +
               std::to_string(p.characters) + " perso(s)" + (p.down ? ", " + std::to_string(p.down) + " à terre" : "") + (p.god ? "  [dieu]" : "");
    }
    return out;
}

// tests: compact and stable
std::string RunState(kc::Session& s, KenshiWorld& w) {
    std::string out = std::string("godall=") + (g_gods.all() ? "1" : "0") + " engine=" + std::to_string(kenshi::GodModeCount());
    for (const auto& p : AdminPlayers(s, w, "host"))
        out += " p" + std::to_string(p.id) + "=" + (p.god ? "1" : "0") + "/" + std::to_string(p.characters);
    kc::Vec3 pt;
    if (LastMoveOrderPoint(pt)) out += Fmt(" point=%.1f,%.1f", pt.x, pt.z);
    return out;
}

} // namespace

std::string AdminRun(const std::string& line, kc::Session& s, KenshiWorld& w, bool* okOut) {
    bool ok = true;
    auto done = [&](std::string r) {
        if (okOut) *okOut = ok;
        return r;
    };
    if (!s.isHost()) {
        ok = false;
        Log("admin: refused (only the host of a session can use it): %s", line.c_str());
        return done("réservé à l'hôte d'une partie");
    }
    if (!w.Ready()) { ok = false; return done("aucune partie chargée"); }
    adm::Command c;
    std::string err;
    if (!adm::Parse(line, c, err)) { ok = false; return done(err); }
    std::vector<uint8_t> ids;
    if (c.verb == adm::Verb::God || c.verb == adm::Verb::Heal || c.verb == adm::Verb::Xp || c.verb == adm::Verb::Tp) {
        ids = Resolve(s, c.who);
        if (ids.empty()) { ok = false; return done("joueur inconnu : tape players pour les numéros"); }
    }
    switch (c.verb) {
    case adm::Verb::God: return done(RunGod(c, s, w, ids));
    case adm::Verb::Heal: return done(RunHeal(c, s, w, ids));
    case adm::Verb::Money: return done(RunMoney(c, s, w, ok));
    case adm::Verb::Xp: return done(RunXp(c, s, w, ids, ok));
    case adm::Verb::Tp: return done(RunTp(c, s, w, ids, ok));
    case adm::Verb::List: return done(RunList(s, w));
    case adm::Verb::State: return done(RunState(s, w));
    case adm::Verb::None: break;
    }
    ok = false;
    return done(adm::Usage());
}

void AdminUpkeep(kc::Session& s, KenshiWorld& w) {
    if (!s.isHost()) {
        if (g_wasHosting) {
            g_wasHosting = false;
            g_gods.Clear();
            kenshi::SetGodModes({});
            Log("admin: no longer hosting: god modes switched off");
        }
        return;
    }
    g_wasHosting = true;
    if (!w.Ready()) return;
    if (w.WorldGeneration() != g_generation) {   // another world: its move point means nothing here
        g_generation = w.WorldGeneration();
        ForgetMoveOrderPoint();
    }
    const double now = NowSeconds();
    if (now < g_nextGodPass) return;
    g_nextGodPass = now + 0.2;
    ApplyGods(s, w);
}

std::vector<AdminPlayer> AdminPlayers(kc::Session& s, KenshiWorld& w, const std::string& hostName) {
    std::vector<AdminPlayer> out;
    if (!s.isHost()) return out;
    AdminPlayer me;
    me.id = s.localId();
    me.name = hostName;
    me.host = true;
    me.god = g_gods.On(adm::kHostKey);
    out.push_back(me);
    for (const auto& [id, p] : s.players()) {
        AdminPlayer a;
        a.id = id;
        a.name = p.name;
        a.pingMs = p.rttMs;
        a.god = g_gods.On(adm::PlayerKey(p.steamId, p.name));
        out.push_back(a);
    }
    if (!w.Ready()) return out;
    for (const auto& m : Squad(s, w))
        for (auto& a : out)
            if (a.id == m.owner) {
                ++a.characters;
                a.down += Down(m.c) ? 1 : 0;
            }
    return out;
}

bool AdminGodAll() { return g_gods.all(); }

} // namespace kcp
