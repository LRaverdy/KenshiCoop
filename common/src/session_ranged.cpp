// Lot C: ranged combat. Bows, crossbows and harpoon guns, and turrets.
//
// The host's game fires every projectile; its damage reaches clients through vitals like any other.
// What clients need is to *see* the shot: each projectile the host fires near the players is sent
// right away (who fired, with what, at what, and the exact path it left on), and the client's game
// fires the same projectile from the same weapon (its own shots for those characters are refused,
// its projectiles' damage too). Aim follows a few times a second: the point each character in
// ranged combat aims at (its upper body turns there) and where each turret near the players points.
#include "kc/session.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace kc {

namespace {

constexpr double kAimInterval = 0.2;       // host: aims are sent at most this often
constexpr double kAimRefresh = 2.0;        // and in full this often (unchanged ones too)
constexpr float kAimEpsilon = 0.5f;        // an aim point moving less than this is not news
constexpr float kTurretRadius = 400.0f;    // turrets this close to a player's character are followed

float Dist3(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

std::string TurretKey(const TurretAim& t) {
    // the same turret on every machine: its kind, and where it stands (to the unit)
    return t.sid + "@" + std::to_string(int(std::lround(t.pos.x))) + "," + std::to_string(int(std::lround(t.pos.z)));
}

} // namespace

void Session::ResetRanged() {
    scratchShots_.clear();
    rangedSent_.clear();
    turretSent_.clear();
    nextRangedAim_ = rangedRefreshAt_ = 0;
    pendingShots_.clear();
    clientAims_.clear();
    pendingStopped_.clear();
    pendingTurrets_.clear();
    rangedStats_ = RangedStats{};
}

void Session::HostRanged(double now) {
    // ---- shots: out as soon as they are fired
    world_.TakeShots(scratchShots_);
    if (!scratchShots_.empty()) {
        ShotsMsg m;
        for (const auto& s : scratchShots_) {
            Entity* shooter = entityByHandle(s.shooter);
            if (!shooter || shooter->container) continue;   // nobody near: no client shows that shooter
            ShotEvent e;
            e.shooterNetId = shooter->netId;
            if (s.target.valid())
                if (Entity* t = entityByHandle(s.target); t && !t->container) e.targetNetId = t->netId;
            e.stat = s.stat;
            e.aimPos = s.aimPos;
            e.dir = s.dir;
            e.turretSid = s.turretSid;
            e.turretPos = s.turretPos;
            m.shots.push_back(e);
            if (m.shots.size() == kMaxShotsPerMsg) {
                Writer w;
                Encode(w, m);
                BroadcastReliable(w, true);
                rangedStats_.shotsSent += m.shots.size();
                m.shots.clear();
            }
        }
        if (!m.shots.empty()) {
            Writer w;
            Encode(w, m);
            BroadcastReliable(w, true);
            rangedStats_.shotsSent += m.shots.size();
        }
        scratchShots_.clear();
    }
    // ---- aims: a few times a second, what changed (everything now and then)
    if (now < nextRangedAim_) return;
    nextRangedAim_ = now + kAimInterval;
    const bool refresh = now >= rangedRefreshAt_;
    if (refresh) rangedRefreshAt_ = now + kAimRefresh;
    RangedMsg m;
    std::unordered_set<uint32_t> ranged;
    std::vector<Vec3> players;
    for (auto& [id, e] : entities_) {
        if (e.container) continue;
        if (e.squad) {
            EntityState st;
            if (world_.Read(e.handle, st)) players.push_back(st.pos);
        }
        IWorld::WorldAim a;
        if (!world_.ReadRangedAim(e.handle, a)) continue;
        ranged.insert(id);
        RangedAim r;
        r.netId = id;
        r.state = a.state <= 4 ? a.state : 4;
        r.aimPos = a.aimPos;
        if (a.target.valid())
            if (Entity* t = entityByHandle(a.target); t && !t->container) r.targetNetId = t->netId;
        auto last = rangedSent_.find(id);
        if (!refresh && last != rangedSent_.end() && last->second.state == r.state && last->second.targetNetId == r.targetNetId &&
            Dist3(last->second.aimPos, r.aimPos) < kAimEpsilon)
            continue;
        rangedSent_[id] = r;
        if (m.aims.size() < kMaxRangedAims) m.aims.push_back(r);
    }
    for (auto it = rangedSent_.begin(); it != rangedSent_.end();) {
        if (ranged.count(it->first)) { ++it; continue; }
        if (m.stopped.size() < kMaxRangedAims) m.stopped.push_back(it->first);
        it = rangedSent_.erase(it);
    }
    std::vector<TurretAim> turrets;
    if (!players.empty()) world_.ReadTurrets(players, kTurretRadius, turrets);
    for (const auto& t : turrets) {
        const std::string key = TurretKey(t);
        auto last = turretSent_.find(key);
        if (!refresh && last != turretSent_.end() && Dist3(last->second.target, t.target) < kAimEpsilon) continue;
        turretSent_[key] = t;
        if (m.turrets.size() < kMaxTurretAims) m.turrets.push_back(t);
    }
    if (m.aims.empty() && m.turrets.empty() && m.stopped.empty()) return;
    Writer w;
    Encode(w, m);
    BroadcastReliable(w, true);
}

void Session::ClientRangedPacket(Msg type, Reader& r) {
    if (state_ != SessionState::Connected) return;
    if (type == Msg::Shots) {
        ShotsMsg m;
        if (!Decode(r, m)) return;
        for (auto& s : m.shots)
            if (pendingShots_.size() < 512) pendingShots_.push_back(std::move(s));
        return;
    }
    RangedMsg m;
    if (!Decode(r, m)) return;
    for (const auto& a : m.aims) clientAims_[a.netId] = a;
    for (uint32_t id : m.stopped) {
        clientAims_.erase(id);
        if (pendingStopped_.size() < 1024) pendingStopped_.push_back(id);
    }
    for (const auto& t : m.turrets) pendingTurrets_[TurretKey(t)] = t;
}

void Session::ClientRanged(double now) {
    (void)now;
    for (const auto& s : pendingShots_) {
        auto shooter = entities_.find(s.shooterNetId);
        if (shooter == entities_.end() || !shooter->second.present) { ++rangedStats_.shotsFailed; continue; }
        IWorld::WorldShot w;
        w.shooter = shooter->second.handle;
        if (s.targetNetId)
            if (auto t = entities_.find(s.targetNetId); t != entities_.end() && t->second.present) w.target = t->second.handle;
        w.stat = s.stat;
        w.aimPos = s.aimPos;
        w.dir = s.dir;
        w.turretSid = s.turretSid;
        w.turretPos = s.turretPos;
        if (world_.ReplayShot(w)) {
            ++rangedStats_.shotsReplayed;
        } else {
            if (rangedStats_.shotsFailed < 5 || rangedStats_.shotsFailed % 50 == 0)
                log_("a shot of the host's could not be fired here (" + std::string(s.turretSid.empty() ? "no ranged weapon on that character" : "turret not found") + ")");
            ++rangedStats_.shotsFailed;
        }
    }
    pendingShots_.clear();
    for (uint32_t id : pendingStopped_)
        if (auto e = entities_.find(id); e != entities_.end() && e->second.present) world_.ApplyRangedAim(e->second.handle, false, IWorld::WorldAim{});
    pendingStopped_.clear();
    // the host's aim, every tick: the local game would otherwise turn them toward its own idea
    for (auto it = clientAims_.begin(); it != clientAims_.end();) {
        auto e = entities_.find(it->first);
        if (e == entities_.end()) { it = clientAims_.erase(it); continue; }
        if (e->second.present) {
            IWorld::WorldAim a;
            a.state = it->second.state;
            a.aimPos = it->second.aimPos;
            if (it->second.targetNetId)
                if (auto t = entities_.find(it->second.targetNetId); t != entities_.end() && t->second.present) a.target = t->second.handle;
            world_.ApplyRangedAim(e->second.handle, true, a);
            ++rangedStats_.aimsApplied;
        }
        ++it;
    }
    for (const auto& [key, t] : pendingTurrets_) {
        world_.ApplyTurret(t);
        ++rangedStats_.turretsApplied;
    }
    pendingTurrets_.clear();
}

} // namespace kc
