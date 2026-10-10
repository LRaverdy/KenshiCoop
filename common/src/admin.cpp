#include "kc/admin.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace kc::admin {

const SkillInfo kSkills[kStatCount] = {
    {"strength", "Force"},
    {"melee_attack", "Attaque"},
    {"labouring", "Travail"},
    {"science", "Science"},
    {"engineering", "Ingénierie"},
    {"robotics", "Robotique"},
    {"weaponsmith", "Forge d'armes"},
    {"armoursmith", "Forge d'armures"},
    {"medic", "Médecine"},
    {"thieving", "Vol"},
    {"turrets", "Tourelles"},
    {"farming", "Agriculture"},
    {"cooking", "Cuisine"},
    {"stealth", "Discrétion"},
    {"athletics", "Athlétisme"},
    {"dexterity", "Dextérité"},
    {"melee_defence", "Défense"},
    {"toughness", "Robustesse"},
    {"assassination", "Assassinat"},
    {"swimming", "Natation"},
    {"perception", "Perception"},
    {"katanas", "Katanas"},
    {"sabres", "Sabres"},
    {"hackers", "Hachoirs"},
    {"heavy_weapons", "Armes lourdes"},
    {"blunt", "Contondantes"},
    {"martial_arts", "Arts martiaux"},
    {"dodge", "Esquive"},
    {"polearms", "Armes d'hast"},
    {"crossbows", "Arbalètes"},
    {"precision", "Tir de précision"},
    {"lockpicking", "Crochetage"},
    {"bowsmith", "Fabrication d'arbalètes"},
    {"mass_combat", "Combat de masse"},
};

namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Underscored(std::string s) {
    for (auto& c : s) if (c == ' ') c = '_';
    return Lower(s);
}

bool ParseDouble(const std::string& s, double& out) {
    // from_chars: never the game's locale (a French one would read "1.5" as 1)
    const char* b = s.data();
    const char* e = b + s.size();
    if (b != e && *b == '+') ++b;
    auto r = std::from_chars(b, e, out);
    return r.ec == std::errc() && r.ptr == e && std::isfinite(out);
}

bool ParseInt(const std::string& s, long long& out) {
    const char* b = s.data();
    const char* e = b + s.size();
    if (b != e && *b == '+') ++b;
    auto r = std::from_chars(b, e, out);
    return r.ec == std::errc() && r.ptr == e;
}

} // namespace

int FindSkill(const std::string& s) {
    const std::string k = Underscored(s);
    if (k.empty()) return -1;
    if (k == "all" || k == "tout" || k == "toutes" || k == "tous") return kAllSkills;
    long long n = -1;
    if (ParseInt(k, n)) return n >= 0 && n < static_cast<long long>(kStatCount) ? int(n) : -1;
    for (size_t i = 0; i < kStatCount; ++i)
        if (k == kSkills[i].key || k == Underscored(kSkills[i].fr)) return int(i);
    return -1;
}

float IncreaseStatModel(float stat, float amount, float upper) {
    const float k = 1.0f / upper;
    float f = (upper - stat) * k;
    f *= f;
    if (amount <= 0.0f || f <= 0.0f || amount > 20.0f || f > 20.0f) return stat;
    const float next = f * amount + stat;
    return std::isnan(next) ? stat : next;
}

int GiveXp(double amount, const IncreaseFn& inc) {
    int calls = 0;
    for (double left = std::min(amount, kMaxXp); left > 1e-6; left -= kMaxXpPerCall, ++calls)
        inc(float(std::min<double>(left, kMaxXpPerCall)));
    return calls;
}

bool RaiseTo(float target, const ReadFn& read, const IncreaseFn& inc, int maxCalls, int* calls) {
    target = std::min(target, kStatCap);
    int n = 0;
    bool reached = false;
    for (; n < maxCalls; ++n) {
        const float cur = read();
        if (!std::isfinite(cur) || cur < 0) break;
        if (cur >= target - 1e-3f) { reached = true; break; }
        float f = (kStatCap - cur) / kStatCap;
        f *= f;
        if (f <= 1e-9f) break;
        const float a = std::min((target - cur) / f, kMaxXpPerCall);
        inc(a);
        if (read() <= cur) break;   // the game refused it: no use insisting
    }
    if (!reached && n >= maxCalls) reached = read() >= target - 1e-3f;
    if (calls) *calls = n;
    return reached;
}

bool ParseTarget(const std::string& s, Target& out) {
    out = Target{};
    const std::string k = Lower(s);
    if (k == "all" || k == "tous" || k == "tout" || k == "everyone") { out.all = true; return true; }
    if (k == "host" || k == "hote" || k == "hôte" || k == "moi" || k == "me") { out.host = true; return true; }
    long long n = -1;
    if (ParseInt(k, n) && n >= 0 && n <= 255) { out.id = int(n); return true; }
    return false;
}

std::string Usage() {
    return "admin god <id|all|host> on|off  -  mode dieu (persiste aux changements de zone et reconnexions)\n"
           "admin xp <id|all|host> <compétence|all> <n> [levels]  -  n points d'expérience (ou n niveaux)\n"
           "admin tp <id|all> host  -  près de l'hôte ; admin tp host <id>  -  l'hôte près du joueur\n"
           "admin tp <id|all> <id>  -  près d'un autre joueur ; admin tp <id|all> point  -  au dernier point de déplacement ordonné par l'hôte\n"
           "admin tp <id|all> <x> <y> <z>  -  à ces coordonnées\n"
           "admin heal <id|all|host>  -  soigne et réveille\n"
           "admin money <n>  -  cats ajoutés à l'argent commun\n"
           "admin spawn <sid|nom> <n> [here|<id>] [fabricant] [modèle]  -  n objets au sol près de toi (here) ou d'un joueur ; _ pour les espaces\n"
           "admin list  -  joueurs, persos et mode dieu";
}

bool Parse(const std::string& line, Command& out, std::string& err) {
    out = Command{};
    std::istringstream in(line);
    std::vector<std::string> t;
    for (std::string w; in >> w;) t.push_back(w);
    if (t.empty()) { err = "usage :\n" + Usage(); return false; }
    const std::string verb = Lower(t[0]);
    auto who = [&](size_t i) {
        if (t.size() <= i || !ParseTarget(t[i], out.who)) { err = "joueur attendu : un numéro (tape players), all ou host"; return false; }
        return true;
    };
    if (verb == "list" || verb == "players" || verb == "liste") { out.verb = Verb::List; return true; }
    if (verb == "state") { out.verb = Verb::State; return true; }
    if (verb == "god" || verb == "dieu") {
        out.verb = Verb::God;
        if (!who(1)) return false;
        const std::string a = t.size() > 2 ? Lower(t[2]) : "on";
        if (a == "on" || a == "1" || a == "oui") out.on = true;
        else if (a == "off" || a == "0" || a == "non") out.on = false;
        else { err = "usage : admin god <id|all|host> on|off"; return false; }
        if (t.size() > 3) { err = "usage : admin god <id|all|host> on|off"; return false; }
        return true;
    }
    if (verb == "heal" || verb == "soigne" || verb == "soigner") {
        out.verb = Verb::Heal;
        return who(1);
    }
    if (verb == "money" || verb == "argent") {
        out.verb = Verb::Money;
        if (t.size() != 2 || !ParseInt(t[1], out.money) || out.money == 0 || std::llabs(out.money) > 2000000000LL) {
            err = "usage : admin money <n>  (ex. admin money 5000, négatif pour en retirer)";
            return false;
        }
        return true;
    }
    if (verb == "xp") {
        out.verb = Verb::Xp;
        if (!who(1)) return false;
        if (t.size() < 4 || t.size() > 5) { err = "usage : admin xp <id|all|host> <compétence|all> <n> [levels]"; return false; }
        out.skill = FindSkill(t[2]);
        if (out.skill == -1) { err = "compétence inconnue : " + t[2] + " (ex. strength, melee_attack, 1, all)"; return false; }
        if (!ParseDouble(t[3], out.amount) || out.amount <= 0) { err = "quantité attendue : un nombre positif"; return false; }
        if (t.size() == 5) {
            const std::string u = Lower(t[4]);
            if (u == "levels" || u == "level" || u == "niveaux" || u == "niveau" || u == "lvl") out.levels = true;
            else if (u != "xp") { err = "unité : xp ou levels"; return false; }
        }
        if (out.amount > (out.levels ? kMaxLevels : kMaxXp)) {
            err = out.levels ? "100 niveaux au plus" : "20000 points d'expérience au plus par commande";
            return false;
        }
        return true;
    }
    if (verb == "tp") {
        out.verb = Verb::Tp;
        if (!who(1)) return false;
        if (t.size() == 5) {
            double v[3];
            for (int i = 0; i < 3; ++i)
                if (!ParseDouble(t[2 + i], v[i])) { err = "coordonnées attendues : x y z"; return false; }
            out.tp = TpTo::Pos;
            out.pos = {float(v[0]), float(v[1]), float(v[2])};
            return true;
        }
        if (t.size() != 3) { err = "usage : admin tp <id|all> host | <id> | point | <x> <y> <z>"; return false; }
        const std::string d = Lower(t[2]);
        if (d == "point") { out.tp = TpTo::Point; return true; }
        Target to;
        if (!ParseTarget(d, to) || to.all) { err = "destination : host, un numéro de joueur, point, ou x y z"; return false; }
        if (to.host) { out.tp = TpTo::Host; return true; }
        out.tp = TpTo::Player;
        out.to = to;
        return true;
    }
    if (verb == "spawn" || verb == "give" || verb == "donne") {
        out.verb = Verb::Spawn;
        const char* usage = "usage : admin spawn <sid|nom> <n> [here|<id>] [fabricant] [modèle]  (ex. admin spawn 580-gamedata.base 20 here)";
        if (t.size() < 3 || t.size() > 6) { err = usage; return false; }
        out.item = t[1];
        long long n = 0;
        if (!ParseInt(t[2], n) || n <= 0) { err = "quantité attendue : un nombre entier positif"; return false; }
        if (n > kMaxSpawn) { err = std::to_string(kMaxSpawn) + " objets au plus par commande"; return false; }
        out.count = int(n);
        out.spawnAt = Target{};
        out.spawnAt.host = true;
        if (t.size() > 3) {
            const std::string d = Lower(t[3]);
            if (d != "here" && d != "ici") {
                Target to;
                if (!ParseTarget(d, to) || to.all) { err = "cible : here (près de toi) ou un numéro de joueur"; return false; }
                out.spawnAt = to;
            }
        }
        if (t.size() > 4) out.maker = t[4];
        if (t.size() > 5) out.model = t[5];
        return true;
    }
    err = "commande admin inconnue : " + t[0] + "\n" + Usage();
    return false;
}

std::string PlayerKey(uint64_t steamId, const std::string& name) {
    return steamId ? "steam:" + std::to_string(steamId) : "name:" + name;
}

void GodRegistry::Set(const std::string& key, bool on, const std::vector<std::string>& currentKeys) {
    if (on) { keys_.insert(key); return; }
    if (all_) {   // everyone else stays as they are: on
        all_ = false;
        keys_.insert(currentKeys.begin(), currentKeys.end());
    }
    keys_.erase(key);
}

void GodRegistry::SetAll(bool on) {
    all_ = on;
    if (!on) keys_.clear();
}

bool SpawnableType(int t) {
    return t == kTypeWeapon || t == kTypeArmour || t == kTypeItem || t == kTypeBackpack || t == kTypeMap || t == kTypeCrossbow || t == kTypeLimb;
}

ItemCat Classify(int type, int function, bool artifact) {
    switch (type) {
    case kTypeWeapon: return ItemCat::Weapon;
    case kTypeCrossbow: return ItemCat::Crossbow;
    case kTypeArmour: return ItemCat::Armour;
    case kTypeBackpack: return ItemCat::Backpack;
    case kTypeMap: return ItemCat::Map;
    case kTypeLimb: return ItemCat::Limb;
    case kTypeItem: break;
    default: return ItemCat::Other;
    }
    if (artifact) return ItemCat::Artifact;   // ancient books, AI cores, engineering research...
    // the game data's "item function" (gamedata.base, rebirth.mod)
    switch (function) {
    case 0: return ItemCat::Crafting;          // fabrics, leather, steel bars, hemp, raw iron...
    case 1: case 2: case 12: return ItemCat::Medical;   // first aid kits, splints, skeleton repair kits
    case 3: case 15: return ItemCat::Food;     // meals, raw meat
    case 7: return ItemCat::Building;          // building materials, iron plates, copper alloy plates
    case 8: return ItemCat::Drug;              // hashish, rum, sake
    case 9: return ItemCat::Tool;              // hacksaw, tools
    case 11: return ItemCat::Blueprint;
    case 13: return ItemCat::Book;
    case 14: return ItemCat::Money;
    case 16: return ItemCat::Ammo;             // bolts
    case 17: return ItemCat::Limb;             // severed limbs
    default: return ItemCat::Other;
    }
}

const char* CategoryFr(ItemCat c) {
    switch (c) {
    case ItemCat::Building: return "Matériaux de construction";
    case ItemCat::Crafting: return "Matériaux d'artisanat";
    case ItemCat::Food: return "Nourriture";
    case ItemCat::Medical: return "Soins";
    case ItemCat::Weapon: return "Arme";
    case ItemCat::Crossbow: return "Arbalète";
    case ItemCat::Armour: return "Armure";
    case ItemCat::Backpack: return "Sac à dos";
    case ItemCat::Book: return "Livre";
    case ItemCat::Blueprint: return "Plan";
    case ItemCat::Artifact: return "Artefact";
    case ItemCat::Tool: return "Outil";
    case ItemCat::Drug: return "Drogue, alcool";
    case ItemCat::Ammo: return "Munitions";
    case ItemCat::Limb: return "Membre";
    case ItemCat::Map: return "Carte";
    case ItemCat::Money: return "Argent";
    case ItemCat::Other: break;
    }
    return "Autre";
}

std::vector<int> SplitStacks(int total, int maxStack) {
    std::vector<int> out;
    if (total <= 0) return out;
    maxStack = std::max(1, maxStack);
    for (int left = total; left > 0; left -= maxStack) out.push_back(std::min(left, maxStack));
    return out;
}

std::vector<std::pair<float, float>> SpreadOffsets(size_t n, float spacing) {
    std::vector<std::pair<float, float>> out;
    out.reserve(n);
    // square rings around the centre: ring r holds 8r spots
    for (int r = 1; out.size() < n; ++r)
        for (int i = -r; i <= r && out.size() < n; ++i)
            for (int j = -r; j <= r && out.size() < n; ++j)
                if (std::max(std::abs(i), std::abs(j)) == r) out.emplace_back(float(i) * spacing, float(j) * spacing);
    return out;
}

bool SearchMatches(const std::string& name, const std::string& sid, const std::string& query) {
    const std::string q = Lower(query);
    if (q == Lower(sid)) return true;
    const std::string n = Lower(name);
    std::istringstream in(q);
    for (std::string w; in >> w;)
        if (n.find(w) == std::string::npos) return false;
    return true;
}

const Favourite kFavourites[] = {
    {"580-gamedata.base", "Matériaux de construction", 20},
    {"584-gamedata.base", "Tissu", 20},
    {"42159-gamedata.base", "Fer (plaques)", 20},
    {"42158-gamedata.base", "Cuivre", 20},
    {"1946-gamedata.base", "Nourriture (pain)", 20},
    {"209-gamedata.base", "Bandages (trousse basique)", 10},
};
const size_t kFavouriteCount = sizeof(kFavourites) / sizeof(kFavourites[0]);

} // namespace kc::admin
