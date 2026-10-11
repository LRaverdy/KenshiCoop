#pragma once
// Looting a body or an animal from a client: how close the looter must stand, for every kind of
// creature. Pure rules (no game calls), shared by the plugin (KenshiWorld::RequestLoot, the host's
// drop check) and unit tested in tests/test_main.cpp.
//
// Kenshi has two classes of characters and both are looted the same way (task 26 LOOT_TARGET, the
// loot window): CharacterHuman for every humanoid race (greenlanders, scorchlanders, sheks, hivers,
// skeletons, fogmen, cannibals...) and CharacterAnimal for every animal (goats to leviathans). There
// is no separate butchering task: an animal's meat, skins, teeth or pearls are its template's
// "death items", put in its inventory when it dies, and taken from the loot window like any item.
// What differs is the size: a leviathan's body is ~25 times as wide as a human's, so a looter
// standing against it is far from its position (its centre).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "kc/protocol.h"

namespace kc::loot {

// ---------------------------------------------------------------- characters
// Handle types (hand+0x08) of characters: 1 a CharacterHuman, 0x5B a CharacterAnimal. 0: a building.
constexpr uint32_t kHandleBuilding = 0, kHandleHuman = 1, kHandleAnimal = 0x5B;
inline bool IsCharacterHandle(uint32_t type) { return type == kHandleHuman || type == kHandleAnimal; }
// Game data item types of the templates: a character, a race, an animal character.
constexpr int kTemplateCharacter = 1, kTemplateRace = 7, kTemplateAnimalCharacter = 76;
// The handle type a character made from a template of that item type gets (0: not a character).
inline uint32_t HandleTypeOfTemplate(int itemType) {
    return itemType == kTemplateCharacter ? kHandleHuman : itemType == kTemplateAnimalCharacter ? kHandleAnimal : 0;
}

// ---------------------------------------------------------------- body size
// A body's size: its race's game data (item type 7) and its scale. The scale is Character vt 0x390:
// 1 for a CharacterHuman, lerp(scale min, scale max, age) for a CharacterAnimal (a young animal is
// smaller; a king gorillo, a megacrab or a megaraptor is up to 3.5 times its race's size).
struct BodySize {
    float footprint = 0;                  // "pathfind footprint radius"
    float hullX = 0, hullY = 0, hullZ = 0;   // "hull size X / Y / Z" (Y: the height)
    float scale = 1;
};

constexpr float kLootReach = 12.0f;     // looter to body, beyond the body's own radius (1 unit ~ 10 cm)
constexpr float kMaxBody = 400.0f;      // bound of a radius or a height read from the game
constexpr float kStoppedSlack = 1.5f;   // a looter that stopped walking may be this much farther
constexpr float kDropReach = 160.0f;    // a drop from a chest or a body: the dropping character stands this close

inline float SaneSize(float v) { return std::isfinite(v) && v > 0 ? std::min(v, kMaxBody) : 0.0f; }
inline float SaneScale(float s) { return std::isfinite(s) && s >= 0.05f ? std::min(s, 5.0f) : 1.0f; }
// How far the body reaches out from its position, on the ground.
inline float BodyRadius(const BodySize& b) {
    return std::min(kMaxBody, std::max({SaneSize(b.footprint), SaneSize(b.hullX), SaneSize(b.hullZ)}) * SaneScale(b.scale));
}
inline float BodyHeight(const BodySize& b) { return std::min(kMaxBody, SaneSize(b.hullY) * SaneScale(b.scale)); }
// How far from the body's position (on the ground) the looter may stand for the loot window to open.
inline float Reach(const BodySize& b) { return kLootReach + 2.0f * BodyRadius(b); }
// ... and up or down: a big body's position (its ragdoll's root) is high above the ground.
inline float VerticalReach(const BodySize& b) { return Reach(b) + BodyHeight(b); }

inline float GroundDist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
// The looter is close enough to open the window (slack > 1: the looter stopped walking there, it
// cannot come closer: a big ragdoll blocks it).
inline bool InReach(const Vec3& looter, const Vec3& body, const BodySize& b, float slack = 1.0f) {
    return GroundDist(looter, body) <= Reach(b) * slack && std::fabs(looter.y - body.y) <= VerticalReach(b) * slack;
}
// Where the looter is sent: the body's edge on the looter's side (the centre of a big body is inside
// it; a human's edge is a few units from its centre, as before).
inline Vec3 ApproachPoint(const Vec3& looter, const Vec3& body, const BodySize& b) {
    const float r = BodyRadius(b);
    const float d = GroundDist(looter, body);
    if (!(d > r) || r <= 0) return body;
    return {body.x + (looter.x - body.x) / d * r, body.y, body.z + (looter.z - body.z) / d * r};
}
// How long a loot order waits for the looter to get there (it walks ~14 units a second).
inline double OrderTimeout(float dist) { return 30.0 + double(std::min(std::max(dist, 0.0f), 3000.0f)) / 10.0; }
// The clock of a client's loot order. Paused time does not count (the looter cannot walk: the host
// paused, or a joining player's character editor holds the game; in the in-game test of 11/10 every
// order that needed a walk expired that way, the editor never closed). "Stopped by the body" needs the
// looter to have walked first: one that never left its place is waiting, not blocked.
struct OrderClock {
    double since = 0;        // given at
    double until = 0;        // expires at (pushed back by paused time)
    double last = 0;         // last tick
    double stillSince = 0;   // the looter stopped walking at (0: walking, or not yet seen walking)
    double pausedFor = 0;    // seconds of pause seen
    bool walked = false;     // seen walking at least once
};
inline OrderClock StartOrder(double now, float dist) {
    OrderClock o;
    o.since = o.last = now;
    o.until = now + OrderTimeout(dist);
    return o;
}
// One tick. moving: the looter walks (unknown: pass true, it never counts as stopped).
inline void Tick(OrderClock& o, double now, bool paused, bool moving) {
    const double dt = now > o.last ? now - o.last : 0.0;
    o.last = now;
    if (paused) {
        o.until += dt;
        o.since += dt;
        o.pausedFor += dt;
        o.stillSince = 0;
        return;
    }
    if (moving) {
        o.walked = true;
        o.stillSince = 0;
    } else if (o.stillSince == 0) {
        o.stillSince = now;
    }
}
// The looter walked, then stood still for a second: it cannot come closer.
inline bool Stopped(const OrderClock& o, double now) { return o.walked && o.stillSince != 0 && now - o.stillSince >= 1.0 && now - o.since >= 1.5; }
inline bool Expired(const OrderClock& o, double now) { return now >= o.until; }
// Host: a drop from a body, by a character standing at most this far from the body's position.
inline float DropReach(const BodySize& b) { return kDropReach + 2.0f * BodyRadius(b) + BodyHeight(b); }

// ---------------------------------------------------------------- the creatures of the game
// Every race of the 1.0.68 game data (gamedata.base, Newwworld.mod, rebirth.mod) with its size and,
// for an animal, the scale range of its templates (all of them: the biggest is a king's or a
// mega-something's). Used when a body's race cannot be read, and by the unit tests.
struct CreatureRace {
    const char* raceSid;
    const char* name;
    bool animal;   // its characters are CharacterAnimal (handle type 0x5B)
    float footprint, hullX, hullY, hullZ;
    float scaleMin, scaleMax;
    const char* templates;   // the creatures of that race, for the logs and the tests
};
inline const CreatureRace* CreatureRaces(size_t& count) {
    static const CreatureRace kRaces[] = {
        // humanoids: CharacterHuman, scale 1
        {"17-gamedata.quack", "Greenlander", false, 4, 5, 17, 5, 1, 1, "Greenlander"},
        {"18019-gamedata.base", "Scorchlander", false, 4, 5, 17, 5, 1, 1, "Scorchlander"},
        {"5276-chareditor.mod", "Shek", false, 4, 5, 17, 5, 1, 1, "Shek"},
        {"18960-gamedata.base", "Hive Worker Drone", false, 4, 5, 17, 5, 1, 1, "Hive Worker Drone"},
        {"18961-gamedata.base", "Hive Soldier Drone", false, 4, 5, 17, 5, 1, 1, "Hive Soldier Drone"},
        {"5346-stick_people.mod", "Hive Prince", false, 4, 5, 17, 5, 1, 1, "Hive Prince"},
        {"57335-Newwworld.mod", "Hive Queen", false, 4, 5, 17, 5, 1, 1, "Hive Queen"},
        {"18915-small_changes_otto.mod", "Deadhive Prince", false, 4, 5, 16, 5, 1, 1, "Deadhive"},
        {"17946-stick_people.mod", "Skeleton", false, 4, 5, 17, 5, 1, 1, "Skeleton"},
        {"57782-Newwworld.mod", "Soldierbot", false, 4, 5, 17, 5, 1, 1, "Soldierbot"},
        {"2603-gamedata.base", "Cannibal", false, 4, 5, 17, 5, 1, 1, "Cannibal"},
        {"50523-rebirth.mod", "Cannibal Skav", false, 4, 5, 17, 5, 1, 1, "Cannibal Skav"},
        {"96800-Newwworld.mod", "Flayed Man", false, 4, 5, 17, 5, 1, 1, "Flayed"},
        {"55396-gamedata.base", "Fishman", false, 3, 5, 20, 5, 1, 1, "Fogman"},
        {"1532524-rebirth.mod", "King Fishman", false, 3, 5, 20, 5, 1, 1, "Fogman Prince"},
        // animals: CharacterAnimal
        {"3992-gamedata.base", "Goat", true, 4, 8, 12, 8, 0.35f, 1, "Goat"},
        {"44910-rebirth.mod", "River Raptor", true, 6, 10, 16, 10, 0.4f, 1, "River Raptor"},
        {"42071-small_changes_otto.mod", "Swamp Raptor", true, 6, 10, 16, 10, 0.5f, 3.5f, "Swamp Raptor, Megaraptor"},
        {"3985-gamedata.base", "Bull", true, 7, 15, 20, 15, 0.45f, 1, "Wild Bull, Pack Bull"},
        {"43947-rebirth.mod", "Bull (domesticated)", true, 7, 15, 20, 15, 0.45f, 1, "Domesticated Bull"},
        {"3998-gamedata.base", "Garru", true, 7, 15, 20, 15, 0.35f, 1, "Garru, Pack Beast"},
        {"65260-Newwworld.mod", "Crimper", true, 9, 21, 21, 21, 1.5f, 1.5f, "Crimper"},
        {"3976-gamedata.base", "Bonedog", true, 10, 10, 15, 10, 0.3f, 1.1f, "Bonedog, Mountain Dog, Pet Dog"},
        {"66294-Newwworld.mod", "Boneyard Wolf", true, 10, 10, 15, 10, 0.35f, 1.5f, "Boneyard Wolf"},
        {"97570-Newwworld.mod", "Land Bat", true, 10, 12, 17, 12, 0.3f, 1.25f, "Landbat"},
        {"56089-Newwworld.mod", "Crab", true, 10, 25, 22, 25, 0.1f, 2.25f, "Crab, Megacrab"},
        {"42070-small_changes_otto.mod", "Robot Spider Worker", true, 10, 25, 22, 25, 1, 1, "Iron Spider"},
        {"48689-rebirth.mod", "Robot Guard Spider", true, 10, 25, 22, 25, 0.3f, 0.6f, "Security Spider"},
        {"43898-rebirth.mod", "Gorillo", true, 12, 21, 26, 21, 0.25f, 0.85f, "Gorillo, Great White Gorillo"},
        {"55668-rebirth.mod", "Black Gorillo", true, 12, 21, 26, 21, 0.3f, 2.5f, "Black Gorillo, King Gorillo"},
        {"43870-rebirth.mod", "Spider", true, 14, 25, 22, 25, 0.2f, 1, "Skin Spider"},
        {"50641-rebirth.mod", "Small Spider", true, 14, 25, 22, 25, 0.15f, 0.3f, "Blood Spider"},
        {"2860-gamedata.base", "Beak Thing", true, 16, 18, 50, 18, 0.3f, 1, "Beak Thing"},
        {"56088-rebirth.mod", "Skimmer", true, 22, 36, 38, 36, 0.2f, 0.8f, "Skimmer"},
        {"42068-small_changes_otto.mod", "Cage Beast", true, 24, 35, 40, 35, 0.35f, 1.1f, "Cage Beast"},
        {"3987-gamedata.base", "Swamp Turtle", true, 25, 50, 40, 50, 0.35f, 1.5f, "Swamp Turtle"},
        {"56112-Newwworld.mod", "Big-Bones", true, 40, 75, 90, 75, 1, 1, "Cleanser Unit"},
        {"3966-gamedata.base", "Leviathan", true, 40, 75, 90, 75, 0.4f, 1.5f, "Leviathan"},
    };
    count = sizeof(kRaces) / sizeof(kRaces[0]);
    return kRaces;
}
inline const CreatureRace* FindCreatureRace(const std::string& raceSid) {
    size_t n = 0;
    const CreatureRace* r = CreatureRaces(n);
    for (size_t i = 0; i < n; ++i)
        if (raceSid == r[i].raceSid) return &r[i];
    return nullptr;
}
inline BodySize SizeOf(const CreatureRace& r, float scale) {
    BodySize b;
    b.footprint = r.footprint;
    b.hullX = r.hullX;
    b.hullY = r.hullY;
    b.hullZ = r.hullZ;
    b.scale = scale;
    return b;
}
// A body whose race could not be read at all: the size of a human.
inline BodySize DefaultBody(float scale) {
    BodySize b;
    b.footprint = 4;
    b.hullX = b.hullZ = 5;
    b.hullY = 17;
    b.scale = scale;
    return b;
}

// ---------------------------------------------------------------- the scale and the age of an animal
// Character vt 0x390 is the scale and vt 0x398 the age (CharacterAnimal: +0x700; the factory's
// `age` argument is stored there, +0x6FC too). The scale is lerp(+0x6F0, +0x6F4, age).
inline float AnimalScale(float scaleMin, float scaleMax, float age) { return scaleMin + (scaleMax - scaleMin) * age; }

}  // namespace kc::loot
