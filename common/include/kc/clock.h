// Client: keeps the local game clock on the host's.
// A client runs at the host's speed, paused when it is. That alone drifts: every speed change and
// pause reaches the client a little late (half a round trip, a frame), a long frame here may be cut
// short by the game, and a client that stalls (a zone loading, four games on one PC) loses whatever
// the host ran meanwhile: the 4-player stress test ended 0.16 h (10 game minutes) apart, and the old
// trim (+-5 %, nothing beyond 0.25 h) took minutes to win that back, while the host kept pausing.
// The host's hours come with every TimeState (twice a second):
//  - a gap over kSnap while running, or over kPausedSnap while the host is paused (its hours are
//    then exact, nothing to extrapolate): the clock is set to the host's hours (Snap());
//  - a smaller gap while both run: the client runs a few percent faster or slower until it is gone.
#pragma once
#include <algorithm>
#include <cmath>

#include "kc/protocol.h"

namespace kc {

class ClockSync {
public:
    static constexpr double kEngage = 0.002;      // hours apart before the speed is trimmed (~0.2 s of game at x1)
    static constexpr double kRelease = 0.0005;    // ...and until they are this close again
    static constexpr double kSnap = 0.008;        // running, further apart: set to the host's hours (~30 game seconds)
    static constexpr double kPausedSnap = 0.002;  // paused: the host's hours are exact, set them past this
    static constexpr double kSnapEvery = 1.5;     // seconds between two snaps (the next message shows the result)
    static constexpr float kSmallTrim = 0.03f, kBigTrim = 0.10f;   // relative speed changes
    static constexpr double kBigError = 0.004;    // hours apart for the bigger one

    void Reset() { *this = ClockSync{}; }

    // A TimeState from the host, received at `now` (local seconds); `delay`: one-way latency (s).
    void OnHost(const TimeState& t, double now, double delay) {
        const bool running = !t.paused && t.speed > 0.0f;
        // game hours per real second at speed 1, measured over a stretch at one speed (a longer
        // stretch: the arrival jitter of single messages matters less)
        if (running && segRunning_ && t.speed == segSpeed_) {
            const double dt = now - segAt_, dh = t.gameHours - segHours_;
            if (dt >= 2.0 && dh > 0.0) {
                const double sample = dh / (dt * t.speed);
                if (rate_ <= 0.0) rate_ = sample;
                else if (sample > 0.5 * rate_ && sample < 2.0 * rate_) rate_ += 0.3 * (sample - rate_);
            }
        } else {
            segRunning_ = running;
            segSpeed_ = t.speed;
            segAt_ = now;
            segHours_ = t.gameHours;
        }
        last_ = t;
        at_ = now;
        delay_ = std::clamp(delay, 0.0, 1.0);
        have_ = true;
        fresh_ = true;
    }

    // What the local game should run at now, given its own clock (`localHours`): the host's pause,
    // the host's speed (trimmed while the clocks are a little apart), and the host's hours as of now.
    // A snap asked for (Snap() >= 0) is for this call only: set the local clock to that many hours.
    TimeState Target(double now, double localHours) {
        snap_ = -1.0;
        if (!have_) return last_;
        TimeState out = last_;
        const bool running = !last_.paused && last_.speed > 0.0f;
        if (running && rate_ > 0.0)   // the host's clock went on since its message left
            out.gameHours += rate_ * last_.speed * std::clamp(now - at_ + delay_, 0.0, 2.0);
        error_ = out.gameHours - localHours;
        // Only on a fresh message (what our clock reads after a snap shows from the next frame on)
        // and not too often; running, only once the rate is known (the target is extrapolated).
        const bool canSnap = fresh_ && now - lastSnap_ >= kSnapEvery && (!running || rate_ > 0.0);
        if (canSnap && std::fabs(error_) > (running ? kSnap : kPausedSnap)) {
            snap_ = out.gameHours;
            lastSnap_ = now;
            ++snaps_;
            trimming_ = false;
            trim_ = 0.0f;
            fresh_ = false;
            return out;
        }
        fresh_ = false;
        if (!running || rate_ <= 0.0) {
            trimming_ = false;
            trim_ = 0.0f;
            return out;
        }
        if (!trimming_ && std::fabs(error_) > kEngage) trimming_ = true;
        if (trimming_ && std::fabs(error_) < kRelease) trimming_ = false;
        // two steps only: the game's speed is set when it changes, not every frame
        trim_ = !trimming_ ? 0.0f : (std::fabs(error_) > kBigError ? kBigTrim : kSmallTrim) * (error_ > 0 ? 1.0f : -1.0f);
        out.speed = last_.speed * (1.0f + trim_);
        return out;
    }

    double error() const { return error_; }   // host hours - ours, at the last Target()
    double rate() const { return rate_; }
    float trim() const { return trim_; }
    bool have() const { return have_; }
    double Snap() const { return snap_; }      // < 0: none; else the hours to set, from the last Target()
    int snaps() const { return snaps_; }

private:
    TimeState last_;
    double at_ = 0, delay_ = 0;
    bool have_ = false;
    bool fresh_ = false;
    bool segRunning_ = false;
    float segSpeed_ = 0;
    double segAt_ = 0, segHours_ = 0;
    double rate_ = 0;
    double error_ = 0;
    bool trimming_ = false;
    float trim_ = 0;
    double snap_ = -1.0;
    double lastSnap_ = -1e9;
    int snaps_ = 0;
};

}  // namespace kc
