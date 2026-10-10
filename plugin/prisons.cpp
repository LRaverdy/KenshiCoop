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
    if (k.inSomething == 2 && k.cage && kenshi::ObjectTemplate(k.cage, out.cageSid) && kenshi::ObjectPosition(k.cage, out.cagePos))
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
    // the posture code leaves captives alone (KenshiWorld::PostureHeld): known even before our copy is found
    if (s.caged || s.chained || s.slaveState != 0 || s.kidnapped || s.sentence > 0) hostCaptive_.insert(h);
    else hostCaptive_.erase(h);
    kenshi::Captivity local;
    if (!c || !kenshi::ReadCaptivity(c, local)) return;
    HostCallScope scope;
    if (s.caged) {
        void* cage = FindByKindAndPlace(s.cageSid, s.cagePos);
        if (!cage) {
            if (captiveMissing_.insert(h).second) Log("captivity: cage %s not found here for %s", s.cageSid.c_str(), Key(h).c_str());
        } else if (local.inSomething != 2 || local.cage != cage) {
            if (local.inSomething == 2) kenshi::SetPrisonMode(c, false, nullptr);
            const bool ok = kenshi::SetPrisonMode(c, true, cage);
            int now = 0;
            kenshi::ReadInSomething(c, now);
            Log("captivity: %s put in its cage as on the host (%s, inSomething now %d)", Key(h).c_str(), ok ? "ok" : "call failed", now);
            captiveMissing_.erase(h);
        }
        captiveHold_.insert(h);
    } else {
        captiveHold_.erase(h);
        captiveMissing_.erase(h);
        if (local.inSomething == 2) {
            const bool ok = kenshi::SetPrisonMode(c, false, nullptr);
            Log("captivity: %s out of its cage as on the host (%s)", Key(h).c_str(), ok ? "ok" : "call failed");
        }
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

// A character the host keeps in a cage stays where the cage holds it: no position corrections.
bool KenshiWorld::CaptiveHold(const kc::Handle& h, kenshi::Character* c) {
    if (!captiveHold_.count(h)) return false;
    int in = 0;
    return kenshi::ReadInSomething(c, in) && in == 2;
}

} // namespace kcp
