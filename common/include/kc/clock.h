// Client: keeps the local game clock on the host's.
// The game recomputes its hours from an internal counter every frame (writing them does not
// stick), so a client only runs at the host's speed, paused when it is. That alone drifts: every
// speed change and pause reaches the client a little late (half a round trip, a frame), and a long
// frame here may be cut short by the game. The 20-minute soak ended 0.01 h (36 game seconds) apart.
// The host's hours come with every TimeState (twice a second): while both games run, the client
// runs a few percent faster or slower until its clock is back on the host's.
#pragma once
#include <algorithm>
#include <cmath>

#include "kc/protocol.h"

namespace kc {

class ClockSync {
public:
    static constexpr double kEngage = 0.003;    // hours apart before the speed is trimmed (~1 s at x3)
    static constexpr double kRelease = 0.0005;  // ...and until they are this close again
    static constexpr double kGiveUp = 0.25;     // further apart than this: not drift (a reload): left alone
    static constexpr float kSmallTrim = 0.02f, kBigTrim = 0.05f;   // relative speed changes
    static constexpr double kBigError = 0.006;  // hours apart for the bigger one

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
    }

    // What the local game should run at now, given its own clock (`localHours`): the host's pause,
    // the host's speed (trimmed while the clocks are apart), and the host's hours as of now.
    TimeState Target(double now, double localHours) {
        if (!have_) return last_;
        TimeState out = last_;
        const bool running = !last_.paused && last_.speed > 0.0f;
        if (running && rate_ > 0.0)   // the host's clock went on since its message left
            out.gameHours += rate_ * last_.speed * std::clamp(now - at_ + delay_, 0.0, 2.0);
        error_ = out.gameHours - localHours;
        if (!running || rate_ <= 0.0 || std::fabs(error_) > kGiveUp) {
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

private:
    TimeState last_;
    double at_ = 0, delay_ = 0;
    bool have_ = false;
    bool segRunning_ = false;
    float segSpeed_ = 0;
    double segAt_ = 0, segHours_ = 0;
    double rate_ = 0;
    double error_ = 0;
    bool trimming_ = false;
    float trim_ = 0;
};

}  // namespace kc
