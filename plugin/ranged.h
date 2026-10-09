// Lot C: ranged combat hooks (bows, crossbows, harpoons, turrets). See plugin/ranged.cpp.
#pragma once

namespace kcp::ranged {

// GunClass::shoot(Character* me, RootObject* target, StatsEnumerated stat, const Vector3& aimpos):
// every projectile any character or turret fires. Host: recorded for the clients. Clients: only the
// host's shots, replayed by KenshiCoop, go through.
using ShootFn = void (*)(void* gun, void* me, void* target, int stat, const float* aimPos);
extern ShootFn o_gunShoot;
void hk_gunShoot(void* gun, void* me, void* target, int stat, const float* aimPos);

// The projectile pool's get(mesh, material), called by shoot only: the projectile that shot made.
using ProjectileGetFn = void* (*)(void* pool, void* mesh, void* material, void* extra);
extern ProjectileGetFn o_projectileGet;
void* hk_projectileGet(void* pool, void* mesh, void* material, void* extra);

void* LastProjectile();   // the projectile the last shoot call on this thread made (or null)

} // namespace kcp::ranged

#include <sstream>
#include <string>

namespace kc { class Session; }

namespace kcp {
class KenshiWorld;
// Debug channel commands of lot C (rangedlist, shoot, shots, turrets, turretaim, rangedaim):
// true when `cmd` is one of them (`out` is its answer).
bool RangedDebugCommand(kc::Session& s, KenshiWorld& w, std::istringstream& in, const std::string& cmd, std::string& out);
} // namespace kcp
