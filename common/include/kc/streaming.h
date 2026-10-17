// Per-player streaming (0.3.1). What the host sends each client is built around that client's own
// characters, never around the host's camera or loaded area; the client recreates characters
// without stalling (a budget per frame, a retry delay that grows); a client that keeps missing NPCs
// gets its zone sent again by itself. Small game-agnostic tools, unit tested (TestStreaming).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "kc/protocol.h"

namespace kc {

// Within r of any of these points (3D, as everywhere in the session).
inline bool WithinAny(const Vec3& p, const std::vector<Vec3>& centres, float r) {
    const float r2 = r * r;
    for (const Vec3& c : centres) {
        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        if (dx * dx + dy * dy + dz * dz <= r2) return true;
    }
    return false;
}

// A player's streaming centres: their own characters, plus every squad member within r of one of
// them (the group they are with). Own characters first; none: empty (the caller decides).
inline std::vector<Vec3> GroupCentres(const std::vector<Vec3>& own, const std::vector<Vec3>& squad, float r) {
    std::vector<Vec3> out = own;
    if (own.empty() || r <= 0) return out;
    for (const Vec3& m : squad) {
        bool mine = false;
        for (const Vec3& o : own) mine = mine || (o.x == m.x && o.y == m.y && o.z == m.z);
        if (!mine && WithinAny(m, own, r)) out.push_back(m);
    }
    return out;
}

// A retry delay per key that doubles with each failure, up to a ceiling: a template the character
// factory refuses, a stand-in the local game keeps unloading. Success forgets the key.
template <class Key>
class Backoff {
public:
    Backoff(double first, double ceiling) : first_(first), ceiling_(ceiling) {}
    bool Allowed(const Key& k, double now) const {
        auto it = state_.find(k);
        return it == state_.end() || now >= it->second.next;
    }
    // One more failure at `now`: the next try waits first, 2 x first, 4 x first... (at most the
    // ceiling). Returns that wait.
    double Fail(const Key& k, double now) {
        State& s = state_[k];
        const double wait = std::min(ceiling_, first_ * std::pow(2.0, double(std::min(s.failures, 30))));
        ++s.failures;
        s.next = now + wait;
        return wait;
    }
    void Succeed(const Key& k) { state_.erase(k); }
    int failures(const Key& k) const {
        auto it = state_.find(k);
        return it == state_.end() ? 0 : it->second.failures;
    }
    double next(const Key& k) const {
        auto it = state_.find(k);
        return it == state_.end() ? 0.0 : it->second.next;
    }
    size_t size() const { return state_.size(); }
    void Clear() { state_.clear(); }

private:
    struct State { int failures = 0; double next = 0; };
    double first_, ceiling_;
    std::map<Key, State> state_;
};

// At most `burst` lines of one kind per `window` seconds. What is held back is counted and told
// with the next line that goes out.
class LogLimiter {
public:
    LogLimiter(int burst, double window) : burst_(burst), window_(window) {}
    // true: write the line; *held: lines held back since the last one written (to mention)
    bool Allow(double now, uint32_t* held = nullptr) {
        if (now - windowStart_ >= window_) {
            windowStart_ = now;
            used_ = 0;
        }
        if (used_ >= burst_) {
            ++held_;
            return false;
        }
        ++used_;
        if (held) *held = held_;
        held_ = 0;
        return true;
    }
    uint32_t held() const { return held_; }

private:
    int burst_;
    double window_;
    double windowStart_ = -1e18;
    int used_ = 0;
    uint32_t held_ = 0;
};

// ---- 0.3.5 (branch night-stream, A_FAIRE 16/10 points 11 and 12)

inline float FlatDist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Far tier. A player is sent everything around their own characters at full rate; the characters
// around the OTHER players' characters (any squad, the host's included) now go to them too, at a
// low rate: position and state at most every kFarTierEvery seconds when they changed, at least every
// kFarTierRefresh, health every kFarTierVitalsEvery. Before, they were not sent at all: a player
// looking at another player's squad far away saw the people around it frozen where their own game
// last had them ("dormant", 54 of them on Geoffrey's side at 23:04 on 16 October). Marked kFlagFar:
// the client keeps them on its game's cheap far update and only puts them where the host has them.
constexpr double kFarTierEvery = 2.0;
constexpr double kFarTierRefresh = 10.0;
constexpr double kFarTierVitalsEvery = 5.0;
inline bool FarTierDue(const EntityState& st, const EntityState& last, double sinceLast) {
    if (sinceLast < kFarTierEvery) return false;
    if (sinceLast >= kFarTierRefresh) return true;
    return FlatDist(st.pos, last.pos) > 2.0f || ((st.flags ^ last.flags) & ~kFlagFar) != 0 || st.combatTarget != last.combatTarget ||
           st.carrying != last.carrying;
}

// Client: a copy the host no longer sends (dormant) or sends at a low rate (far tier) is held where
// the host last had it. The 16/10 session put such copies back ~54 000 times (1055 lines written):
// the same copy, the same distance each time (9 units, 48 units, for minutes), so our game kept it
// there (a floor of its own, a far character ignoring written positions) and nothing walked it away.
// Now: measured flat (floors are the floor code's business), put back at most `maxTries` times per
// host position; after that the place our game keeps it at is its anchor, and it is only put back
// again if it really moves away from that anchor (then its local tasks are dropped as well) or if the
// host's position changes.
struct DormantHold {
    Vec3 target, anchor;
    bool haveTarget = false, anchored = false;
    int tries = 0;
};
enum class HoldStep : uint8_t {
    None,       // where it should be, or anchored and not moving: nothing to do
    PutBack,    // put it where the host has it
    Anchored,   // it stays off after the put-backs: left where our game keeps it (say it once)
    Moved,      // it walked away from its anchor here: stop it and put it back
};
inline HoldStep DormantStep(DormantHold& b, const Vec3& local, const Vec3& target, float drift = 8.0f, int maxTries = 2) {
    if (!b.haveTarget || FlatDist(b.target, target) > 4.0f) {
        b = DormantHold{};
        b.target = target;
        b.haveTarget = true;
    }
    if (b.anchored) {
        if (FlatDist(local, b.anchor) <= drift) return HoldStep::None;
        b.anchored = false;
        b.tries = 1;
        return HoldStep::Moved;
    }
    if (FlatDist(local, target) <= drift) return HoldStep::None;
    if (b.tries < maxTries) {
        ++b.tries;
        return HoldStep::PutBack;
    }
    b.anchored = true;
    b.anchor = local;
    return HoldStep::Anchored;
}

// Host, map markers: where to draw a hostile squad. Its characters' average, as before, unless the
// game moves the squad as a whole (its Platoon) while the characters stay where their zone was left
// (a raid on its way far from every player, 16/10 00:21: frozen on everyone's map). The platoon's
// position is used when the game says the squad is unloaded (an UnloadedPlatoon), or when the
// characters have not moved for kThreatStill seconds while the platoon has.
constexpr double kThreatStill = 4.0;
struct ThreatTrack {
    Vec3 avg, plat;
    bool have = false, havePlat = false, fromPlatoon = false;
    double avgMovedAt = -1e18, platMovedAt = -1e18, seenAt = 0;
};
inline Vec3 PickThreatPosition(ThreatTrack& t, const Vec3& avg, bool platOk, const Vec3& plat, bool unloaded, double now) {
    constexpr float kMoved = 5.0f, kApart = 50.0f;
    t.seenAt = now;
    if (!t.have || FlatDist(avg, t.avg) > kMoved) {
        if (t.have) t.avgMovedAt = now;
        t.avg = avg;
        t.have = true;
    }
    if (platOk && (!t.havePlat || FlatDist(plat, t.plat) > kMoved)) {
        if (t.havePlat) t.platMovedAt = now;
        t.plat = plat;
        t.havePlat = true;
    }
    t.fromPlatoon = platOk && (unloaded || (now - t.avgMovedAt > kThreatStill && now - t.platMovedAt < kThreatStill && FlatDist(plat, avg) > kApart));
    return t.fromPlatoon ? plat : avg;
}

// How long a client waits before recreating a stand-in that vanished `losses` times in a row
// (killed and cleaned up once is normal; again and again means the local game does not keep that
// place loaded): 2 s, 10 s, 30 s, then a minute, then two.
inline double StandInLossDelay(int losses) {
    static const double kDelays[] = {0.0, 2.0, 10.0, 30.0, 60.0};
    if (losses <= 0) return 0.0;
    if (losses < int(sizeof(kDelays) / sizeof(kDelays[0]))) return kDelays[losses];
    return 120.0;
}

// Host: from each client's sync reports (every 5 s), when to send that client's zone again by
// itself. A report is bad with NPCs still missing or standing characters off; several bad reports in
// a row ask for a zone resync, never more often than minGap; after maxTries without a good report
// the host says once that a full resync may be needed, and keeps trying, more slowly.
struct ZoneHealthConfig {
    uint16_t missingAt = 5;       // NPCs not there yet (near that player's characters)
    uint16_t offAt = 3;           // standing characters more than 5 units off
    int badReports = 3;           // in a row (15 s)
    double minGap = 60.0;         // seconds between two zone resyncs of one player
    int maxTries = 3;             // zone resyncs before "still differs"
    double slowGap = 300.0;       // after that, between two more
};

class ZoneHealth {
public:
    enum class Action { None, Resync, StillDiffers };
    explicit ZoneHealth(ZoneHealthConfig c = {}) : cfg_(c) {}
    // A report from that player. why: what made it bad (for the log), when the answer is not None.
    Action Report(uint8_t player, uint16_t missingNpcs, uint16_t farOff, double now, std::string& why) {
        State& s = state_[player];
        const bool bad = missingNpcs >= cfg_.missingAt || farOff >= cfg_.offAt;
        if (!bad) {
            s.bad = 0;
            s.tries = 0;
            s.toldStill = false;
            return Action::None;
        }
        ++s.bad;
        if (s.bad < cfg_.badReports) return Action::None;
        why.clear();
        if (missingNpcs >= cfg_.missingAt) why = std::to_string(missingNpcs) + " NPCs missing";
        if (farOff >= cfg_.offAt) why += (why.empty() ? "" : ", ") + std::to_string(farOff) + " characters off";
        why += " for " + std::to_string(s.bad * 5) + " s";
        if (s.tries >= cfg_.maxTries) {
            if (!s.toldStill) {
                s.toldStill = true;
                return Action::StillDiffers;
            }
            if (now - s.lastResync < cfg_.slowGap) return Action::None;
        } else if (now - s.lastResync < cfg_.minGap) {
            return Action::None;
        }
        s.lastResync = now;
        ++s.tries;
        s.bad = 0;
        return Action::Resync;
    }
    void Forget(uint8_t player) { state_.erase(player); }
    int tries(uint8_t player) const {
        auto it = state_.find(player);
        return it == state_.end() ? 0 : it->second.tries;
    }

private:
    struct State { int bad = 0; int tries = 0; double lastResync = -1e18; bool toldStill = false; };
    ZoneHealthConfig cfg_;
    std::map<uint8_t, State> state_;
};

} // namespace kc
