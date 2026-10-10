#pragma once
// Client side: how our copies of the host's characters follow the host's motion and posture.
// Pure rules (no game calls), shared by the session (interpolation buffer) and the plugin (what
// KenshiWorld::Apply does to a character each frame), and unit tested in tests/test_main.cpp.
#include <algorithm>
#include <cmath>
#include <deque>

#include "kc/protocol.h"

namespace kc::motion {

// ---------------------------------------------------------------- vectors (ground plane: x, z)
inline float Dist3(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline float Dist2(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
inline float Dot2(const Vec3& a, const Vec3& b) { return a.x * b.x + a.z * b.z; }
inline bool IsZero2(const Vec3& v) { return v.x == 0.0f && v.z == 0.0f; }
// Unit vector on the ground from a to b; zero when they are closer than `minLen`.
inline Vec3 Dir2(const Vec3& a, const Vec3& b, float minLen) {
    const float d = Dist2(a, b);
    if (!(d > minLen)) return {};
    return {(b.x - a.x) / d, 0.0f, (b.z - a.z) / d};
}
inline Vec3 Flat(const Vec3& v) {
    const float l = std::sqrt(v.x * v.x + v.z * v.z);
    return l > 1e-6f ? Vec3{v.x / l, 0.0f, v.z / l} : Vec3{};
}

// ---------------------------------------------------------------- interpolation buffer
// One state of the host's character at a host time.
struct Sample {
    double t;
    EntityState s;
};

// How often a character's position really changes in what the host sends. A character the host's
// game moves only a few times per second (far from the host's own squad: a client 900 m away in a
// crowded village) arrives as the same position repeated for a while, then a jump: rendering it one
// snapshot late (the old fixed 0.05 s) held it still, then jumped it, while its walk kept playing.
struct Cadence {
    double gap = 0.05;     // average time between two different positions while it walks (s)
    double delay = -1;     // how late it is rendered now (s); < 0: not set yet
    double steppedAt = 0;  // when the delay was last moved toward its target
};

constexpr float kSamePos = 0.02f;         // positions closer than this are "the same"
constexpr double kMaxMerge = 0.5;         // a walker standing still that long is pushed as a new sample
constexpr double kMaxDelay = 0.45;        // never render a character later than this
constexpr double kMaxExtrapolate = 0.25;  // past the newest sample: carry on that far at most (s)
constexpr float kMaxSpeed = 120.0f;       // faster than this between two samples: a teleport, never extrapolated

inline bool Moving(const EntityState& s) { return (s.flags & kFlagMoving) != 0; }

// A snapshot entry for one character, into its buffer (oldest first). False: stale or duplicate.
// - A walker sent again at the same position is merged into the previous sample (its time kept,
//   the rest updated): the next real position is interpolated over the whole stretch instead of
//   held still and then jumped to.
// - A character the host stopped sending (unchanged, not walking) stood still until just before
//   the new sample; a walker whose samples were lost walked in between (interpolated).
inline bool PushSample(std::deque<Sample>& buf, Cadence& cad, double t, const EntityState& st, double step, double keepSeconds) {
    if (!buf.empty() && buf.back().t >= t) return false;
    if (!buf.empty()) {
        Sample& back = buf.back();
        const bool same = Dist3(back.s.pos, st.pos) < kSamePos;
        if (same && Moving(back.s) && Moving(st) && t - back.t < kMaxMerge) {
            const Vec3 pos = back.s.pos;
            back.s = st;
            back.s.pos = pos;
            return true;
        }
        if (!same && Moving(st)) {
            const double gap = std::min(1.0, t - back.t);
            cad.gap += (gap - cad.gap) * 0.2;
        }
        if (!Moving(back.s) && t - back.t > 1.5 * step) buf.push_back({t - step, back.s});
    }
    buf.push_back({t, st});
    while (buf.size() > 2 && buf.front().t < t - keepSeconds) buf.pop_front();
    return true;
}

// How late a character is rendered: the base delay plus its own cadence, so that its next real
// position has nearly always arrived when it is needed.
inline double RenderDelay(const Cadence& cad, double base) {
    return std::clamp(base + cad.gap, base, std::max(base, kMaxDelay));
}
// The delay actually used, moved toward RenderDelay a little each frame: its render clock runs
// between 0.75 and 1.25 times as fast as real time, never backwards (a delay that jumped up made a
// walker step back).
constexpr double kDelaySlew = 0.25;
inline double StepDelay(Cadence& cad, double base, double now) {
    const double want = RenderDelay(cad, base);
    if (cad.delay < 0) {
        cad.delay = want;
    } else {
        const double dt = std::clamp(now - cad.steppedAt, 0.0, 0.1);
        cad.delay += std::clamp(want - cad.delay, -kDelaySlew * dt, kDelaySlew * dt);
    }
    cad.steppedAt = now;
    return cad.delay;
}
// The delay in use (without moving it): for lookups between frames.
inline double CurrentDelay(const Cadence& cad, double base) { return cad.delay < 0 ? RenderDelay(cad, base) : cad.delay; }

inline Vec3 LerpV(const Vec3& a, const Vec3& b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
inline Quat NlerpQ(Quat a, const Quat& b, float t) {
    if (a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z < 0) { a.w = -a.w; a.x = -a.x; a.y = -a.y; a.z = -a.z; }   // the short way round
    Quat q{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (len > 1e-6f) { q.w /= len; q.x /= len; q.y /= len; q.z /= len; } else q = b;
    return q;
}

// The host's state at render time `rt`. Past the newest sample, a walker carries on along its last
// velocity for a moment (never past its destination): a late packet no longer stops it dead.
inline EntityState StateAt(const std::deque<Sample>& b, double rt, float snapDistance) {
    if (b.empty()) return {};
    if (rt <= b.front().t) return b.front().s;
    if (rt >= b.back().t) {
        EntityState out = b.back().s;
        if (!Moving(out) || b.size() < 2) return out;
        const Sample* a = nullptr;   // the latest earlier sample at another position
        for (size_t i = b.size() - 1; i-- > 0;)
            if (Dist3(b[i].s.pos, out.pos) >= kSamePos) { a = &b[i]; break; }
        if (!a || b.back().t - a->t > 1.0 || b.back().t <= a->t) return out;
        const float dt = float(b.back().t - a->t);
        const Vec3 v{(out.pos.x - a->s.pos.x) / dt, (out.pos.y - a->s.pos.y) / dt, (out.pos.z - a->s.pos.z) / dt};
        const float speed = std::sqrt(v.x * v.x + v.z * v.z);
        if (speed > kMaxSpeed || Dist3(a->s.pos, out.pos) > snapDistance) return out;
        const float ahead = float(std::min(rt - b.back().t, kMaxExtrapolate));
        float dist = speed * ahead;
        dist = std::min(dist, Dist2(out.pos, out.dest));   // never past where it is going
        if (speed > 1e-4f) {
            const float k = dist / speed;
            out.pos = {out.pos.x + v.x * k, out.pos.y + v.y * k, out.pos.z + v.z * k};
        }
        return out;
    }
    for (size_t i = 1; i < b.size(); ++i) {
        if (b[i].t < rt) continue;
        const Sample& a = b[i - 1];
        const Sample& c = b[i];
        if (Dist3(a.s.pos, c.s.pos) > snapDistance) return c.s;   // teleport: never swept across
        const float t = float((rt - a.t) / std::max(1e-6, c.t - a.t));
        EntityState out = c.s;
        out.pos = LerpV(a.s.pos, c.s.pos, t);
        out.rot = NlerpQ(a.s.rot, c.s.rot, t);
        out.flags = t < 0.5f ? a.s.flags : c.s.flags;
        return out;
    }
    return b.back().s;
}

// ---------------------------------------------------------------- facing and position pull
// Where the host's character is really going: from the state we render toward the newest one we
// know (rendered a little late, it lies behind on the way), else toward its destination. Zero when
// it is not going anywhere.
inline Vec3 TravelDir(const EntityState& rendered, const EntityState& latest) {
    Vec3 d = Dir2(rendered.pos, latest.pos, 0.05f);
    if (IsZero2(d)) d = Dir2(rendered.pos, latest.dest, 0.5f);
    return d;
}

// Where our copy must face. Standing, or fighting (side steps and back steps are real there): the
// host's facing. Walking: the host's facing while it agrees with the way it goes (within 45
// degrees), otherwise the way it goes: the host's game hardly turns characters far from its own
// squad, and a walk played facing elsewhere slid them sideways or backwards (the "moonwalk").
inline Vec3 WantFacing(const Vec3& hostForward, const Vec3& travel, bool moving, bool combat) {
    const Vec3 host = Flat(hostForward);
    if (!moving || combat || IsZero2(travel)) return IsZero2(host) ? hostForward : host;
    if (IsZero2(host) || Dot2(host, travel) < 0.7071f) return travel;
    return host;
}

// One frame of pulling our copy onto the host's position (rendered state), frame-rate independent:
// the same pull at 30 and 150 images per second. While it walks, never backwards against the way
// it goes: a copy a little ahead of a late host position is only held back gently (a fraction of
// walking speed, so it still moves forwards), instead of being dragged back while its legs walk
// forwards.
struct PullParams {
    float dt = 1.0f / 60.0f;   // real seconds since the last frame
    float gameSpeed = 1.0f;    // the host's game speed (1, 2, 3...)
    bool moving = false;       // the host's character walks
    bool combat = false;       // it fights (steps back on purpose)
};
constexpr float kAheadTolerance = 5.0f;   // units ahead (at game speed 1) only held back gently
constexpr float kBackRate = 2.0f;         // units per second (at game speed 1) it may be held back by
inline Vec3 Pull(const Vec3& local, const Vec3& target, const Vec3& travel, const PullParams& p) {
    const float err = Dist3(local, target);
    if (err <= 0.01f) return local;
    if (err < 0.05f) return target;
    const float dt = std::clamp(p.dt, 0.0f, 0.1f);
    const float tau = err > 2.0f ? 0.024f : 0.058f;   // = 0.5 and 0.25 of the way per frame at 60 fps
    const float k = 1.0f - std::exp(-dt / tau);
    Vec3 d{target.x - local.x, target.y - local.y, target.z - local.z};
    if (p.moving && !p.combat && !IsZero2(travel)) {
        const float along = Dot2(d, travel);   // < 0: we are ahead on its way
        const float speed = std::max(1.0f, p.gameSpeed);
        if (along < 0 && -along <= kAheadTolerance * speed) {
            const Vec3 side{d.x - travel.x * along, d.y, d.z - travel.z * along};
            const float back = std::min(-along, kBackRate * speed * dt);
            return {local.x + side.x * k - travel.x * back, local.y + side.y * k, local.z + side.z * k - travel.z * back};
        }
    }
    return {local.x + d.x * k, local.y + d.y * k, local.z + d.z * k};
}

// ---------------------------------------------------------------- posture (lying / standing)
// Our copy lies where and when the host's does. Each body keeps a book of what was done to it, so
// that a correction that does not take (the game puts the body back as it was: a shackled captive
// sits up, a carried body stays on its shoulder) is not retried forever: the session of 10/10 had
// captives on a client fall, sit up and fall again every 2 s for minutes (1811 lines).
struct PostureBook {
    double windowStart = -1e9;
    int falls = 0;          // falls started in this window
    int relays = 0;         // bodies stood up to fall again closer to the host's
    int failed = 0;         // falls in a row after which it was not lying
    double lastRelay = -1e9;
    bool pending = false;   // a fall was started; not checked yet
    bool gaveUp = false;    // until the window ends: its posture is left as it is
};
constexpr double kPostureWindow = 60.0;   // the limits below count over this long
constexpr int kMaxFalls = 4;              // per body and window
constexpr int kMaxRelays = 2;             // per body and window
constexpr int kMaxFailedFalls = 2;        // falls in a row that did not take: give up
constexpr double kRelayCooldown = 10.0;
// A collapsing body ends 7 to 12 units from where its feet were (humans; animals more): an offset
// below this is how bodies fall, not a desync worth standing it up for.
constexpr float kRelayOffset = 20.0f;

// Held: carried, shackled, caged, in a bed, a prisoner or a slave, sitting where its captor put it.
// The game's captivity code owns its pose; the posture code never touches it.
struct PostureFacts {
    bool held = false;
    bool hostDown = false, hostDead = false;
    bool localDown = false, localDead = false;
};

inline void PostureRoll(PostureBook& b, double now) {
    if (now - b.windowStart < kPostureWindow) return;
    const bool pending = b.pending;
    b = PostureBook{};
    b.windowStart = now;
    b.pending = pending;
}
// May it be laid down as on the host now?
inline bool MayFall(PostureBook& b, const PostureFacts& f, double now) {
    PostureRoll(b, now);
    return !f.held && !f.hostDead && f.hostDown && !f.localDown && !b.gaveUp && b.falls < kMaxFalls;
}
inline void NoteFall(PostureBook& b, double now) {
    PostureRoll(b, now);
    ++b.falls;
    b.pending = true;
}
// The wait after a fall is over: was it lying then? Two falls in a row that did not take: no more
// until the window ends.
inline void NoteFallSettled(PostureBook& b, bool lying, bool hostStillDown) {
    if (!b.pending) return;
    b.pending = false;
    if (lying || !hostStillDown) { b.failed = 0; return; }
    if (++b.failed >= kMaxFailedFalls) b.gaveUp = true;
}
// May it be stood up because it lies `offset` units from the host's body? Only with a fall left to
// lay it down again, never twice within the cooldown.
inline bool MayRelay(PostureBook& b, const PostureFacts& f, float offset, bool seen, double now) {
    PostureRoll(b, now);
    return !f.held && f.hostDown && f.localDown && !f.hostDead && !f.localDead && seen && offset > kRelayOffset && !b.gaveUp &&
           b.relays < kMaxRelays && b.falls < kMaxFalls && now - b.lastRelay > kRelayCooldown;
}
inline void NoteRelay(PostureBook& b, double now) {
    PostureRoll(b, now);
    ++b.relays;
    b.lastRelay = now;
}
// May it be stood up because the host's is standing? Never a held one (a shackled captive sits).
inline bool MayStand(const PostureFacts& f) { return !f.held && !f.hostDown && !f.hostDead && f.localDown && !f.localDead; }

// ---------------------------------------------------------------- dead bodies
// A body dead on the host stays dead and lying here, forever: never stood up, never revived, and
// a stand-in recreated for it (its zone unloaded and loaded again here) comes back dead and lying.
// The character factory makes a living character: death and ragdoll are applied at once.
enum class DeadStep { None, Kill, Lay };
// What to do with our copy of a body the host has dead. Kill: it is alive here (a stand-in just
// recreated, a death not replayed yet). Lay: dead here but standing while the host's lies (the
// host's body is in ragdoll and ours is not). Never a held one (a corpse on someone's shoulder).
inline DeadStep DeadBodyStep(bool hostDead, bool hostRagdoll, bool localDead, bool localRagdoll, bool held) {
    if (!hostDead || held) return DeadStep::None;
    if (!localDead) return DeadStep::Kill;
    if (hostRagdoll && !localRagdoll) return DeadStep::Lay;
    return DeadStep::None;
}
// May the dead body be laid down (again) now? The same per-body budget as falls: a ragdoll the game
// refuses is not retried every 2 s.
inline bool MayLayDead(PostureBook& b, double now) {
    PostureRoll(b, now);
    return !b.gaveUp && b.falls < kMaxFalls;
}
// The host's vitals for a body its state says is dead: dead, whatever they say (the vitals sync
// clears a local "dead" the host's values lack, and must never revive a corpse).
inline uint8_t VitalsFlagsFor(uint8_t vitalsFlags, uint8_t stateFlags) {
    return (stateFlags & kFlagDead) ? uint8_t(vitalsFlags | kVitDead) : vitalsFlags;
}
// A stand-in recreated from this host state is made dead (and lying) at once.
inline bool SpawnsDead(const EntityState& s) { return (s.flags & kFlagDead) != 0; }

} // namespace kc::motion
