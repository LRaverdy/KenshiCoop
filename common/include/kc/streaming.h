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
