// Player statistics: the book, its file, the distance filter, the stats tab's text and the message.
#include "kc/player_stats.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#include "kc/colors.h"

namespace kc {

namespace {

constexpr const char* kFileHeader = "kc-player-stats 1";
constexpr size_t kMaxKeyLen = 96;
constexpr size_t kMaxSkillNameLen = 48;

// One line, no control characters (a name typed in the game, a key from a connection).
std::string OneLine(const std::string& s, size_t max) {
    std::string out;
    out.reserve(std::min(s.size(), max));
    for (char c : s) {
        if (out.size() >= max) break;
        out.push_back(static_cast<unsigned char>(c) < 0x20 || c == 0x7F ? ' ' : c);
    }
    return out;
}

std::string Num(double v) {
    char b[64];
    const auto r = std::to_chars(b, b + sizeof(b), v, std::chars_format::fixed, 3);
    return r.ec == std::errc() ? std::string(b, r.ptr) : "0";
}
template <class T>
T ParseInt(const std::string& s) {
    T v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() ? v : T(0);
}
double ParseReal(const std::string& s) {
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && std::isfinite(v) && v >= 0 ? v : 0.0;
}

// "1 234" (French thousands)
std::string Grouped(int64_t v) {
    const bool neg = v < 0;
    std::string digits = std::to_string(neg ? -v : v);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out.push_back(' ');
        out.push_back(digits[i]);
    }
    return neg ? "-" + out : out;
}

// MyGUI colour code; "#" in a name must be doubled
std::string ColourCode(uint8_t r, uint8_t g, uint8_t b) {
    char c[8];
    std::snprintf(c, sizeof(c), "#%02X%02X%02X", r, g, b);
    return c;
}
std::string EscapeTags(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '#') out += "##";
        else out.push_back(c);
    }
    return out;
}

void PutStats(Writer& w, const PlayerStats& p) {
    w.str(p.key.substr(0, kMaxKeyLen));
    w.str(p.name.substr(0, kMaxNameLen));
    uint8_t flags = 0;
    if (p.host) flags |= 1;
    if (p.online) flags |= 2;
    w.u8(flags);
    w.u8(p.playerId);
    w.f64(std::max(0.0, p.playSeconds));
    w.varint(p.sessions);
    w.varint(uint64_t(std::max<int64_t>(0, p.firstSeen)));
    w.varint(uint64_t(std::max<int64_t>(0, p.lastSeen)));
    w.f64(std::max(0.0, p.distance));
    w.varint(p.knockouts);
    w.varint(p.deaths);
    w.varint(p.characters);
    w.varint(p.recruits);
    w.varint(uint64_t(std::max<int64_t>(0, p.cashEarned)));
    w.varint(uint64_t(std::max<int64_t>(0, p.cashSpent)));
    w.varint(p.itemsLooted);
    w.varint(p.buildings);
    w.varint(p.research);
    w.f32(std::clamp(p.bestSkill, 0.0f, 1000.0f));
    w.str(p.bestSkillName.substr(0, kMaxSkillNameLen));
}
uint32_t U32(Reader& r) {
    const uint64_t v = r.varint();
    if (v > 0xFFFFFFFFull) r.fail();
    return uint32_t(v);
}
int64_t I64(Reader& r) {
    const uint64_t v = r.varint();
    if (v > uint64_t(INT64_MAX)) r.fail();
    return int64_t(v);
}
bool GetStats(Reader& r, PlayerStats& p) {
    p.key = r.str(kMaxKeyLen);
    p.name = r.str(kMaxNameLen);
    const uint8_t flags = r.u8();
    p.host = (flags & 1) != 0;
    p.online = (flags & 2) != 0;
    p.playerId = r.u8();
    p.playSeconds = r.f64();
    p.sessions = U32(r);
    p.firstSeen = I64(r);
    p.lastSeen = I64(r);
    p.distance = r.f64();
    p.knockouts = U32(r);
    p.deaths = U32(r);
    p.characters = U32(r);
    p.recruits = U32(r);
    p.cashEarned = I64(r);
    p.cashSpent = I64(r);
    p.itemsLooted = U32(r);
    p.buildings = U32(r);
    p.research = U32(r);
    p.bestSkill = r.f32();
    p.bestSkillName = r.str(kMaxSkillNameLen);
    return r.ok() && p.playSeconds >= 0 && p.distance >= 0 && p.bestSkill >= 0 && p.bestSkill <= 1000.0f;
}

} // namespace

bool PlayerStats::operator==(const PlayerStats& o) const {
    return key == o.key && name == o.name && host == o.host && playSeconds == o.playSeconds && sessions == o.sessions &&
           firstSeen == o.firstSeen && lastSeen == o.lastSeen && distance == o.distance && knockouts == o.knockouts && deaths == o.deaths &&
           characters == o.characters && recruits == o.recruits && cashEarned == o.cashEarned && cashSpent == o.cashSpent &&
           itemsLooted == o.itemsLooted && buildings == o.buildings && research == o.research && bestSkill == o.bestSkill &&
           bestSkillName == o.bestSkillName && online == o.online && playerId == o.playerId;
}

PlayerStats& PlayerStatsBook::Get(const std::string& key, const std::string& name) {
    PlayerStats& p = players_[key];
    p.key = key;
    if (!name.empty()) p.name = OneLine(name, kMaxNameLen);
    return p;
}

const PlayerStats* PlayerStatsBook::Find(const std::string& key) const {
    auto it = players_.find(key);
    return it == players_.end() ? nullptr : &it->second;
}

PlayerStats* PlayerStatsBook::FindMutable(const std::string& key) {
    auto it = players_.find(key);
    return it == players_.end() ? nullptr : &it->second;
}

void PlayerStatsBook::Merge(const PlayerStatsBook& other) {
    for (const auto& [key, o] : other.players_) {
        auto it = players_.find(key);
        if (it == players_.end()) {
            PlayerStats p = o;
            p.online = false;
            p.playerId = 0;
            players_[key] = p;
            continue;
        }
        PlayerStats& p = it->second;
        if (p.name.empty()) p.name = o.name;   // the live name wins
        p.host = p.host || o.host;
        p.playSeconds += o.playSeconds;
        p.sessions += o.sessions;
        if (o.firstSeen && (!p.firstSeen || o.firstSeen < p.firstSeen)) p.firstSeen = o.firstSeen;
        p.lastSeen = std::max(p.lastSeen, o.lastSeen);
        p.distance += o.distance;
        p.knockouts += o.knockouts;
        p.deaths += o.deaths;
        if (!p.characters) p.characters = o.characters;
        p.recruits += o.recruits;
        p.cashEarned += o.cashEarned;
        p.cashSpent += o.cashSpent;
        p.itemsLooted += o.itemsLooted;
        p.buildings += o.buildings;
        p.research += o.research;
        if (o.bestSkill > p.bestSkill) {
            p.bestSkill = o.bestSkill;
            p.bestSkillName = o.bestSkillName;
        }
    }
}

std::string PlayerStatsBook::Serialize() const {
    std::ostringstream o;
    o << kFileHeader << '\n';
    o << "# KenshiCoop: statistics of the players of this world (written by the host)\n";
    for (const auto& [key, p] : players_) {
        o << "player " << OneLine(key, kMaxKeyLen) << '\n';
        o << "name " << OneLine(p.name, kMaxNameLen) << '\n';
        o << "host " << (p.host ? 1 : 0) << '\n';
        o << "play_seconds " << Num(p.playSeconds) << '\n';
        o << "sessions " << p.sessions << '\n';
        o << "first_seen " << p.firstSeen << '\n';
        o << "last_seen " << p.lastSeen << '\n';
        o << "distance " << Num(p.distance) << '\n';
        o << "knockouts " << p.knockouts << '\n';
        o << "deaths " << p.deaths << '\n';
        o << "characters " << p.characters << '\n';
        o << "recruits " << p.recruits << '\n';
        o << "cash_earned " << p.cashEarned << '\n';
        o << "cash_spent " << p.cashSpent << '\n';
        o << "items_looted " << p.itemsLooted << '\n';
        o << "buildings " << p.buildings << '\n';
        o << "research " << p.research << '\n';
        o << "best_skill " << Num(p.bestSkill) << '\n';
        o << "best_skill_name " << OneLine(p.bestSkillName, kMaxSkillNameLen) << '\n';
        o << "end\n";
    }
    return o.str();
}

bool PlayerStatsBook::Parse(const std::string& text, std::string* err) {
    players_.clear();
    std::istringstream in(text);
    std::string line;
    bool header = false;
    PlayerStats cur;
    bool open = false;
    auto close = [&] {
        if (open && !cur.key.empty()) players_[cur.key] = cur;
        open = false;
        cur = PlayerStats{};
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!header) {
            if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);   // a BOM left by an editor
            if (line != kFileHeader) {
                if (err) *err = "not a player statistics file";
                return false;
            }
            header = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        const size_t sp = line.find(' ');
        const std::string field = line.substr(0, sp);
        const std::string value = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        if (field == "player") {
            close();
            open = true;
            cur.key = OneLine(value, kMaxKeyLen);
        } else if (field == "end") {
            close();
        } else if (!open) {
            continue;
        } else if (field == "name") cur.name = OneLine(value, kMaxNameLen);
        else if (field == "host") cur.host = value == "1";
        else if (field == "play_seconds") cur.playSeconds = ParseReal(value);
        else if (field == "sessions") cur.sessions = ParseInt<uint32_t>(value);
        else if (field == "first_seen") cur.firstSeen = std::max<int64_t>(0, ParseInt<int64_t>(value));
        else if (field == "last_seen") cur.lastSeen = std::max<int64_t>(0, ParseInt<int64_t>(value));
        else if (field == "distance") cur.distance = ParseReal(value);
        else if (field == "knockouts") cur.knockouts = ParseInt<uint32_t>(value);
        else if (field == "deaths") cur.deaths = ParseInt<uint32_t>(value);
        else if (field == "characters") cur.characters = ParseInt<uint32_t>(value);
        else if (field == "recruits") cur.recruits = ParseInt<uint32_t>(value);
        else if (field == "cash_earned") cur.cashEarned = std::max<int64_t>(0, ParseInt<int64_t>(value));
        else if (field == "cash_spent") cur.cashSpent = std::max<int64_t>(0, ParseInt<int64_t>(value));
        else if (field == "items_looted") cur.itemsLooted = ParseInt<uint32_t>(value);
        else if (field == "buildings") cur.buildings = ParseInt<uint32_t>(value);
        else if (field == "research") cur.research = ParseInt<uint32_t>(value);
        else if (field == "best_skill") cur.bestSkill = float(std::min(1000.0, ParseReal(value)));
        else if (field == "best_skill_name") cur.bestSkillName = OneLine(value, kMaxSkillNameLen);
        // anything else: a field of a later version, skipped
    }
    close();   // a file cut short keeps its last player
    if (!header) {
        if (err) *err = "empty file";
        return false;
    }
    return true;
}

std::vector<PlayerStats> PlayerStatsBook::Sorted() const {
    std::vector<PlayerStats> out;
    out.reserve(players_.size());
    for (const auto& [k, p] : players_) out.push_back(p);
    std::stable_sort(out.begin(), out.end(), [](const PlayerStats& a, const PlayerStats& b) {
        if (a.online != b.online) return a.online;
        if (a.online) return a.playerId < b.playerId;
        return a.lastSeen > b.lastSeen;
    });
    return out;
}

float DistanceStep(const Vec3& from, const Vec3& to, double realSeconds, float speed) {
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(d) || d < kMinStep) return 0.0f;
    const double gameSeconds = std::max(0.0, realSeconds) * std::max(0.0f, speed);
    const double most = kMaxUnitsPerGameSecond * gameSeconds + kStepSlack;
    return d <= most ? d : 0.0f;
}

std::string FormatPlayTime(double seconds) {
    const int64_t s = int64_t(std::max(0.0, seconds));
    char b[64];
    if (s < 60) std::snprintf(b, sizeof(b), "%lld s", static_cast<long long>(s));
    else if (s < 3600) std::snprintf(b, sizeof(b), "%lld min", static_cast<long long>(s / 60));
    else std::snprintf(b, sizeof(b), "%lld h %02lld min", static_cast<long long>(s / 3600), static_cast<long long>((s / 60) % 60));
    return b;
}

std::string FormatDistance(double units) {
    const double metres = std::max(0.0, units) / 10.0;
    char b[64];
    if (metres < 1000.0) std::snprintf(b, sizeof(b), "%.0f m", metres);
    else {
        std::snprintf(b, sizeof(b), "%.1f km", metres / 1000.0);
        for (char* p = b; *p; ++p) if (*p == '.') *p = ',';
    }
    return b;
}

std::string FormatDate(int64_t unixTime) {
    if (unixTime <= 0) return "—";   // an em dash
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return "?";
    char b[32];
    std::snprintf(b, sizeof(b), "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    return b;
}

std::string PlayerStatsPageText(const std::vector<PlayerStats>& players, bool inSession) {
    // the page is parchment with dark text (Kenshi_WordWrap): the player colours are darkened to read
    const std::string ink = "#140806";
    const std::string dim = "#5A4A3A";
    std::string t;
    t += ink + "STATISTIQUES DES JOUEURS DE CE MONDE\n";
    t += dim + (inSession ? "Comptées par l'hôte, gardées d'une session à l'autre." : "Hors session : dernières statistiques reçues.") + "\n\n";
    if (players.empty()) {
        t += ink + "Aucune statistique pour l'instant.\n";
        return t;
    }
    for (const PlayerStats& p : players) {
        std::string colour = dim;
        if (p.online && p.playerId) {
            const PlayerColour c = PlayerColor(p.playerId);
            colour = ColourCode(uint8_t(c.r * 0.55), uint8_t(c.g * 0.55), uint8_t(c.b * 0.55));
        }
        t += colour + EscapeTags(p.name.empty() ? p.key : p.name) + ink + "  " + (p.online ? "(en ligne)" : "(hors ligne)") +
             (p.host ? " - hôte" : "") + "\n";
        const bool seen = !p.host;   // the host's own trades, loot, research and recruits are not seen
        auto add = [&](const std::string& s) { t += ink + "    " + s + "\n"; };
        add("Temps de jeu : " + FormatPlayTime(p.playSeconds) + "  |  Sessions : " + std::to_string(p.sessions) + "  |  Première : " +
            FormatDate(p.firstSeen) + "  |  Dernière : " + FormatDate(p.lastSeen));
        add("Distance parcourue : " + FormatDistance(p.distance) + "  |  K.-O. subis : " + std::to_string(p.knockouts) + "  |  Morts : " +
            std::to_string(p.deaths));
        std::string best = p.bestSkillName.empty() ? "—" : EscapeTags(p.bestSkillName) + " " + std::to_string(int(p.bestSkill));
        add("Personnages : " + std::to_string(p.characters) + (seen ? "  |  Recrues : " + std::to_string(p.recruits) : std::string()) +
            "  |  Meilleure compétence : " + best);
        if (seen)
            add("Commerce : " + Grouped(p.cashEarned) + " cats gagnés, " + Grouped(p.cashSpent) + " dépensés  |  Objets fouillés : " +
                std::to_string(p.itemsLooted));
        add("Constructions : " + std::to_string(p.buildings) + (seen ? "  |  Recherches lancées : " + std::to_string(p.research) : std::string()));
        t += "\n";
    }
    return FoldAccents(t);
}

// Kenshi's fonts (data/gui/fonts/kenshi_fonts.xml) only have the codes 32-126 and 8127-8217: an
// accented letter would not show at all. Latin-1 letters lose their accent (é -> e, Œ -> OE);
// everything else is kept (the dash and quotes of 8127-8217 are in the fonts).
std::string FoldAccents(const std::string& s) {
    static const char* const kFold[64] = {
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",   // C3 80-8F
        "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",  // C3 90-9F
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",   // C3 A0-AF
        "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};  // C3 B0-BF
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const unsigned char n = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        if (c == 0xC3 && n >= 0x80 && n <= 0xBF) { out += kFold[n - 0x80]; ++i; }
        else if (c == 0xC5 && (n == 0x92 || n == 0x93)) { out += n == 0x92 ? "OE" : "oe"; ++i; }
        else if (c == 0xC2 && n == 0xA0) { out += ' '; ++i; }   // no-break space
        else out.push_back(char(c));
    }
    return out;
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot open the temporary file";
            return false;
        }
        out.write(text.data(), std::streamsize(text.size()));
        out.flush();
        if (!out) {
            if (err) *err = "cannot write the temporary file";
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);   // replaces the old file (MoveFileEx with MOVEFILE_REPLACE_EXISTING)
    if (ec) {
        if (err) *err = "cannot replace the file: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool ReadTextFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    out = s.str();
    return true;
}

std::string SafeFileStem(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (out.size() >= 80) break;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                        c == ' ' || c >= 0x80;   // UTF-8 letters kept as they are
        out.push_back(ok ? char(c) : '_');
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();   // Windows drops them
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    return out.empty() ? std::string("monde") : out;
}

void Encode(Writer& w, const PlayerStatsMsg& m) {
    w.u8(uint8_t(Msg::PlayerStats));
    const size_t n = std::min<size_t>(m.players.size(), kMaxStatsPlayers);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) PutStats(w, m.players[i]);
}

bool Decode(Reader& r, PlayerStatsMsg& m) {
    m = PlayerStatsMsg{};
    const uint32_t n = r.count(kMaxStatsPlayers, 40);
    m.players.resize(n);
    for (auto& p : m.players)
        if (!GetStats(r, p)) return false;
    return r.ok() && r.atEnd();
}

}  // namespace kc
