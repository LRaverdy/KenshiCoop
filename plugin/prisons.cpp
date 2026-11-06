// Lot D: prisons. Captive characters as the host's game has them (cages, shackles, slavery, prison
// sentences), read on the host and imposed on clients. See common/src/session_prisons.cpp.
#include <cmath>
#include <cstdio>

#include "hooks.h"
#include "util.h"
#include "world.h"

namespace kcp {

namespace {
std::string Key(const kc::Handle& h) {
    char b[80];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}

float Dist3(const kc::Vec3& a, const kc::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// Furniture of that kind at that place (handles differ between machines).
void* FindByKindAndPlace(const std::string& sid, const kc::Vec3& pos) {
    std::vector<void*> around;
    kenshi::ObjectsNear(pos, 60.0f, around);
    void* best = nullptr;
    float bestD = 30.0f;
    for (void* o : around) {
        std::string s;
        kc::Vec3 p;
        if (!kenshi::ObjectTemplate(o, s) || s != sid || !kenshi::ObjectPosition(o, p)) continue;
        const float d = Dist3(p, pos);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}
} // namespace

bool KenshiWorld::ReadCaptive(const kc::Handle& h, kc::CaptiveState& out) {
    kenshi::Character* c = Find(h);
    kenshi::Captivity k;
    if (!c || !kenshi::ReadCaptivity(c, k)) return false;
    out = kc::CaptiveState{};
    // In a bed too (inSomething 1), through the same "furniture it is in" (kind and place): a sleeper
    // the host's game keeps in its bed was a free standing body here, pushed out of the bed onto the
    // roof or through the floor (faraway test, 11/10: every offset of 33 to 79 units in height was an
    // NPC with the host's action "sleepinbed"). The client tells a bed from a cage by the furniture's
    // function (BF_BED). (A flag of its own in CaptiveState needs a protocol change: see A_FAIRE.)
    if ((k.inSomething == 2 || k.inSomething == 1) && k.cage && kenshi::ObjectTemplate(k.cage, out.cageSid) &&
        kenshi::ObjectPosition(k.cage, out.cagePos))
        out.caged = true;
    else
        out.cageSid.clear();
    out.chained = k.chained;
    out.slaveOwner = k.slaveOwner;
    out.slaveState = uint8_t(k.slaveState);
    out.slaveOf = kenshi::FactionSidOf(k.slaveOf);
    out.escaped = k.escaped;
    out.kidnapped = k.kidnapped;
    out.sentence = k.sentence;
    out.sentenceBegan = k.sentence > 0 ? k.sentenceBegan : 0;
    return true;
}

// The host's captivity on our copy: the same local cage (setPrisonMode puts it in, the game's own
// way: position, pose, occupancy), then the flags. Our own game never changes them (hooks).
void KenshiWorld::ApplyCaptive(const kc::Handle& h, const kc::CaptiveState& s) {
    kenshi::Character* c = Find(h);
    // the posture code leaves a body whose pose the captivity holds alone (KenshiWorld::PostureHeld):
    // known even before our copy is found. 0.3.5 (A_FAIRE 16/10 point 10): only a cage, a bed or
    // shackles hold a pose; a slave or a prisoner walking free (plopette, "slave-state=2 escaped
    // sentence=160h", never stood up on any client after a knock out) is stood up like anyone.
    hostCaptivityKnown_.insert(h);
    if (const uint8_t bits = kc::motion::HostPoseBits(s.caged, s.chained)) hostCaptive_[h] = bits;
    else hostCaptive_.erase(h);
    kenshi::Captivity local;
    if (!c || !kenshi::ReadCaptivity(c, local)) return;
    HostCallScope scope;
    auto leave = [&](const char* why) {   // out of the bed or cage it is in here
        bool ok = true;
        if (local.inSomething == 2) ok = kenshi::SetPrisonMode(c, false, nullptr);
        else if (local.inSomething == 1) ok = kenshi::SetInBed(c, false, nullptr);
        else return;
        Log("captivity: %s out of its %s %s (%s)", Key(h).c_str(), local.inSomething == 1 ? "bed" : "cage", why, ok ? "ok" : "call failed");
    };
    if (s.caged) {
        void* cage = FindByKindAndPlace(s.cageSid, s.cagePos);
        const bool bed = cage && kenshi::BuildingFunctionOf(cage) == kenshi::kBuildingFunctionBed;
        const int want = bed ? 1 : 2;
        if (!cage) {
            if (captiveMissing_.insert(h).second) Log("captivity: furniture %s not found here for %s", s.cageSid.c_str(), Key(h).c_str());
        } else if (local.inSomething != want || local.cage != cage) {
            leave("first");
            const bool ok = bed ? kenshi::SetInBed(c, true, cage) : kenshi::SetPrisonMode(c, true, cage);
            int now = 0;
            kenshi::ReadInSomething(c, now);
            Log("captivity: %s put in its %s as on the host (%s, inSomething now %d)", Key(h).c_str(), bed ? "bed" : "cage", ok ? "ok" : "call failed", now);
            captiveMissing_.erase(h);
        }
        captiveHold_.insert(h);
    } else {
        captiveHold_.erase(h);
        captiveMissing_.erase(h);
        leave("as on the host");
    }
    kenshi::Captivity want = local;
    want.chained = s.chained;
    want.slaveOwner = s.slaveOwner;
    if (auto a = alias_.find(s.slaveOwner); a != alias_.end()) want.slaveOwner = a->second;   // our stand-in for it
    want.slaveState = s.slaveState;
    want.slaveOf = kenshi::FactionBySid(s.slaveOf);
    want.escaped = s.escaped;
    want.kidnapped = s.kidnapped;
    want.sentenceBegan = s.sentenceBegan;
    want.sentence = s.sentence;
    const bool same = want.chained == local.chained && want.slaveOwner == local.slaveOwner && want.slaveState == local.slaveState &&
                      want.slaveOf == local.slaveOf && want.escaped == local.escaped && want.kidnapped == local.kidnapped &&
                      want.sentenceBegan == local.sentenceBegan && want.sentence == local.sentence;
    if (!same) kenshi::WriteCaptivity(c, want);
}

// Client (0.3.4, A_FAIRE 12/10 point 11): the host holds that character in nothing. Called for every
// such copy at every refresh (Session::ClientCaptives, 2 s): reads one int when it is in nothing here.
// In a bed or a cage here: taken out the game's own way (setInBed / setPrisonMode off: position,
// pose, the furniture's occupant set), as ApplyCaptive does when "free" arrives. Told when it stays in.
bool KenshiWorld::EnsureFree(const kc::Handle& h) {
    // the host's captivity marks go: the posture code and Apply follow the host again
    hostCaptive_.erase(h);
    hostCaptivityKnown_.insert(h);
    captiveHold_.erase(h);
    captiveMissing_.erase(h);
    kenshi::Character* c = Find(h);
    if (c) ClearCaptivityTraces(h, c);
    int in = 0;
    if (!c || !kenshi::ReadInSomething(c, in) || (in != 1 && in != 2)) return false;
    if (carriedHere_.count(h)) return false;   // on a shoulder: the carry code owns it
    bool ok = false;
    {
        HostCallScope scope;
        ok = in == 2 ? kenshi::SetPrisonMode(c, false, nullptr) : kenshi::SetInBed(c, false, nullptr);
    }
    int now = 0;
    kenshi::ReadInSomething(c, now);
    ++captiveFreed;
    const double t = NowSeconds();
    uint32_t held = 0;
    const bool stays = now == 1 || now == 2;
    double& told = freeToldAt_[h];
    if ((!stays || t - told > 60.0) && freeLog_.Allow(t, &held)) {
        if (stays) told = t;
        Log("captivity: %s was still in its %s here while the host has it free: %s (%s, inSomething now %d)%s", Key(h).c_str(),
            in == 1 ? "bed" : "cage", stays ? "IT STAYS IN" : "taken out", ok ? "ok" : "call failed", now,
            held ? (" [" + std::to_string(held) + " more such lines not written]").c_str() : "");
    }
    return true;
}

// Client (0.3.5, A_FAIRE 16/10 point 10): the host holds that character in nothing, yet our copy still
// carries captivity marks (shackles, a slave state, a sentence, kidnapped, escaped): leftovers of a
// "free" that never reached us (a copy found after it, a lost update, a save from an older state).
// The host's word is imposed: every mark cleared (the cage or bed is EnsureFree's). At most once per
// body and 30 s (a game that writes them back is not fought every 2 s).
void KenshiWorld::ClearCaptivityTraces(const kc::Handle& h, kenshi::Character* c) {
    kenshi::Captivity k;
    if (!kenshi::ReadCaptivity(c, k)) return;
    if (!k.chained && k.slaveState == 0 && !k.slaveOf && !k.escaped && !k.kidnapped && k.sentence <= 0.0f && !k.slaveOwner.valid()) return;
    const double t = NowSeconds();
    double& at = traceClearedAt_[h];
    if (t - at < 30.0) return;
    at = t;
    kenshi::Captivity want = k;
    want.chained = false;
    want.slaveOwner = kc::Handle{};
    want.slaveState = 0;
    want.slaveOf = nullptr;
    want.escaped = false;
    want.kidnapped = false;
    want.sentence = 0.0f;
    want.sentenceBegan = 0;
    HostCallScope scope;
    const bool ok = kenshi::WriteCaptivity(c, want);
    Log("captivity: %s is free on the host but kept marks here (chained=%d slave-state=%d sentence=%.0fh kidnapped=%d escaped=%d): cleared (%s)",
        Key(h).c_str(), int(k.chained), k.slaveState, double(k.sentence), int(k.kidnapped), int(k.escaped), ok ? "ok" : "write failed");
}

// A character the host keeps in a cage or a bed stays where the cage or bed holds it: no position
// corrections.
bool KenshiWorld::CaptiveHold(const kc::Handle& h, kenshi::Character* c) {
    if (!captiveHold_.count(h)) return false;
    int in = 0;
    return kenshi::ReadInSomething(c, in) && (in == 2 || in == 1);
}

} // namespace kcp
