// Player statistics (kc/player_stats.h): counted by the host for every player of its world, the
// host included, saved in the world's file and shown to everyone (the "Joueurs" tab of the game's
// window, plugin/player_stats_ui.cpp).
//
// Host, twice a second (HostStats):
//  - play time and last session of every player in the world (the host while it hosts);
//  - for each squad character (owner = the player who commands it now): the step since the last
//    sample (kc::DistanceStep: teleports and resync jumps are not counted, nor the steps of a
//    character lying down or dead), K.-O. and death (the health flags going on);
//  - characters per player; every 5 s the best skill of their characters.
// Events counted where the host runs them: trades and loot (HostTradeOp, HostInvOp), placements
// (HostBuildings), research queued (HostWorkshopRequests), recruits (RecruitJoined), sessions
// (StatsStart, FinishJoin). Sent every 10 s when they changed and to a player who arrives; the
// file is written every minute, when a player leaves and when the session ends.
#include "kc/session.h"

#include <algorithm>
#include <ctime>

namespace kc {

namespace {
int64_t WallNow() { return int64_t(std::time(nullptr)); }

uint64_t HashBytes(const Writer& w) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < w.size(); ++i) h = (h ^ w.data()[i]) * 0x100000001b3ull;
    return h;
}
} // namespace

std::string Session::HostStatsKey() const {
    return cfg_.steamId ? "steam:" + std::to_string(cfg_.steamId) : "name:" + cfg_.name;
}

PlayerStats* Session::StatsOf(uint8_t playerId) {
    if (!statsActive_) return nullptr;
    if (playerId == hostId_) {
        PlayerStats& p = statsBook_.Get(HostStatsKey(), cfg_.name);
        p.host = true;
        return &p;
    }
    auto pl = players_.find(playerId);
    if (pl == players_.end()) return nullptr;
    return &statsBook_.Get(PlayerKey(pl->second), pl->second.name);
}

std::vector<PlayerStats> Session::playerStats() const {
    if (!isHost() || !statsActive_) return clientStats_;   // client: the host's; host after a session: the last view
    std::vector<PlayerStats> out = statsBook_.Sorted();
    std::map<std::string, uint8_t> online;
    online[HostStatsKey()] = hostId_;
    for (const auto& [id, p] : players_)
        if (p.inGame) online[PlayerKey(p)] = id;
    for (auto& s : out) {
        auto it = online.find(s.key);
        s.online = it != online.end();
        s.playerId = s.online ? it->second : 0;
    }
    std::stable_sort(out.begin(), out.end(), [](const PlayerStats& a, const PlayerStats& b) {
        if (a.online != b.online) return a.online;
        return a.online ? a.playerId < b.playerId : a.lastSeen > b.lastSeen;
    });
    return out;
}

void Session::StatsStart() {
    statsBook_.Clear();
    statsChars_.clear();
    clientStats_.clear();
    std::string text;
    if (world_.LoadPlayerStats(text)) {
        std::string err;
        if (statsBook_.Parse(text, &err))
            log_("player stats: " + std::to_string(statsBook_.size()) + " player(s) read from this world's file");
        else
            log_("player stats: this world's file could not be read (" + err + "): counting from zero (the old file is kept as .bak)");
    } else {
        log_("player stats: none saved for this world yet");
    }
    statsActive_ = true;
    PlayerStats* me = StatsOf(hostId_);
    const int64_t wall = WallNow();
    ++me->sessions;
    if (!me->firstSeen) me->firstSeen = wall;
    me->lastSeen = wall;
    const double now = clock_();
    statsAt_ = now;
    statsSendAt_ = 0;
    statsSaveAt_ = now + kStatsSaveInterval;
    statsSkillAt_ = 0;
    statsSentHash_ = 0;
    ++statsVersion_;
}

void Session::StatsJoined(const RemotePlayer& p) {
    PlayerStats* s = StatsOf(p.id);
    if (!s) return;
    const int64_t wall = WallNow();
    const bool back = s->sessions > 0;
    ++s->sessions;
    if (!s->firstSeen) s->firstSeen = wall;
    s->lastSeen = wall;
    log_("player stats: " + p.name + (back ? " is back (session " + std::to_string(s->sessions) + ", " + FormatPlayTime(s->playSeconds) + " played so far)"
                                           : " plays this world for the first time"));
    ++statsVersion_;
    SendPlayerStats(p.peer);
}

void Session::SendPlayerStats(PeerId onlyTo) {
    PlayerStatsMsg m;
    m.players = playerStats();
    if (m.players.size() > kMaxStatsPlayers) m.players.resize(kMaxStatsPlayers);
    Writer w(1024);
    Encode(w, m);
    if (onlyTo != kNoPeer) {
        SendReliable(onlyTo, w);
        return;
    }
    statsSentHash_ = HashBytes(w);
    BroadcastReliable(w, true);
}

bool Session::SavePlayerStatsNow(const char* why) {
    if (!isHost() || !statsActive_) return false;
    const bool ok = world_.SavePlayerStats(statsBook_.Serialize());
    log_(std::string("player stats: ") + (ok ? "saved" : "NOT saved") + " (" + why + ", " + std::to_string(statsBook_.size()) + " player(s))");
    return ok;
}

void Session::HostStats(double now) {
    if (!statsActive_ || now - statsAt_ < kStatsTick) return;
    const double dt = std::min(now - statsAt_, 5.0);   // a game frozen for minutes is not that much play
    statsAt_ = now;
    const int64_t wall = WallNow();
    const TimeState t = world_.GetTime();
    const float speed = t.paused ? 0.0f : t.speed;

    std::map<uint8_t, uint32_t> characters;
    for (auto& [id, e] : entities_) {
        if (!e.squad || e.container || e.bag) continue;
        PlayerStats* owner = StatsOf(e.owner);
        if (!owner) continue;
        ++characters[e.owner];
        StatsChar& c = statsChars_[id];
        EntityState st;
        EntityVitals v;
        const bool haveSt = world_.Read(e.handle, st);
        const bool haveVit = world_.ReadVitals(e.handle, v);
        const bool down = (haveSt && (st.flags & (kFlagDown | kFlagDead))) || (haveVit && (v.flags & (kVitUnconscious | kVitDead)));
        if (haveSt) {
            if (c.havePos && !down) owner->distance += DistanceStep(c.pos, st.pos, dt, speed);
            c.pos = st.pos;
            c.havePos = true;
        } else {
            c.havePos = false;   // not in the world now (zone unloaded): no step across the gap
        }
        if (haveVit) {
            const uint8_t was = c.haveVit ? c.vit : v.flags;   // the first look counts nothing
            if (!(was & kVitDead) && (v.flags & kVitDead)) {
                ++owner->deaths;
                log_("player stats: " + owner->name + "'s character " + world_.CharacterNameOf(e.handle) + " died");
            } else if (!(was & kVitUnconscious) && (v.flags & kVitUnconscious) && !(v.flags & kVitDead)) {
                ++owner->knockouts;
            }
            c.vit = v.flags;
            c.haveVit = true;
        }
    }
    for (auto it = statsChars_.begin(); it != statsChars_.end();) {
        auto e = entities_.find(it->first);
        if (e == entities_.end() || !e->second.squad) it = statsChars_.erase(it);
        else ++it;
    }

    // play time and characters of everyone in the world, the host first
    auto tickPlayer = [&](uint8_t id) {
        PlayerStats* s = StatsOf(id);
        if (!s) return;
        s->playSeconds += dt;
        s->lastSeen = wall;
        auto c = characters.find(id);
        s->characters = c == characters.end() ? 0 : c->second;
    };
    tickPlayer(hostId_);
    for (const auto& [id, p] : players_)
        if (p.inGame) tickPlayer(id);

    // best skill (the highest level any of their characters reached), every 5 s
    if (now >= statsSkillAt_) {
        statsSkillAt_ = now + 5.0;
        std::vector<float> skills;
        uint16_t modes = 0;
        uint8_t style = 0;
        for (auto& [id, e] : entities_) {
            if (!e.squad || e.container || e.bag) continue;
            PlayerStats* owner = StatsOf(e.owner);
            if (!owner || !world_.ReadProgress(e.handle, skills, modes, style) || skills.size() != kStatCount) continue;
            const size_t best = size_t(std::max_element(skills.begin(), skills.end()) - skills.begin());
            const float level = float(int(skills[best]));   // the level shown (the fraction is the progress)
            if (level > owner->bestSkill) {
                owner->bestSkill = level;
                owner->bestSkillName = StatNameFr(best);
            }
        }
    }
    ++statsVersion_;

    if (now >= statsSendAt_) {
        statsSendAt_ = now + kStatsSendInterval;
        PlayerStatsMsg m;
        m.players = playerStats();
        if (m.players.size() > kMaxStatsPlayers) m.players.resize(kMaxStatsPlayers);
        Writer w(1024);
        Encode(w, m);
        if (HashBytes(w) != statsSentHash_) {
            statsSentHash_ = HashBytes(w);
            BroadcastReliable(w, true);
        }
    }
    if (now >= statsSaveAt_) {
        statsSaveAt_ = now + kStatsSaveInterval;
        if (!world_.SavePlayerStats(statsBook_.Serialize())) log_("player stats: NOT saved (every minute)");   // silent when it works
    }
}

void Session::ClientStatsPacket(Reader& r) {
    PlayerStatsMsg m;
    if (!Decode(r, m)) return;   // (it may come while our world still loads)
    clientStats_ = std::move(m.players);
    ++statsVersion_;
}

} // namespace kc
