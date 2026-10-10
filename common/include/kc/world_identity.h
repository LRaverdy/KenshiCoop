// Tells a (re)loaded world from the same world seen again after a gap.
//
// The game's main loop does not run for a few frames now and then: a window resized, a zone
// loading after a far teleport, a hitch. The mod's tick then passes through "not live" and must
// decide, once the world is back, whether it is the same world (keep the stand-ins and caches) or
// another one (a load, a resync: every pointer of the old world is freed, forget them all).
//
// It used to compare every character of the player's squad, pointer by pointer, with a snapshot
// taken when the world last became ready. Two faults made clients see a new world that was not:
//  - the snapshot was not kept up to date: a stand-in created since (another player joining) made
//    the next gap a "new world";
//  - a client's squad holds the stand-ins of the other players' characters, which the mod itself
//    creates and removes.
// A false new world cleared the stand-in record, the stand-ins already in the game were no longer
// known, and the session made new ones: in the 4-player stress test the first client had 12 extra
// copies of the two players who joined after it, the second one 4 (one series per false new world).
//
// Now: the snapshot is taken every live frame, and only of the player's own squad without the
// stand-ins; and when a world does count as new, the stand-ins we made in the previous one that
// are still there (same handle, same object) are named, for the caller to destroy them: they must
// not pile up in the player's squad.
#pragma once
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "kc/protocol.h"

namespace kc {

class WorldIdentity {
public:
    struct Object {
        Handle handle;
        const void* ptr = nullptr;
    };
    struct Verdict {
        bool newWorld = false;
        std::vector<Object> leftovers;   // stand-ins of the previous world still in the game: destroy them
    };

    // A frame in which the game's main loop did not run (menus, loading screens, zone loads, hitches).
    void Gap() { gap_ = true; }

    // A live frame with a ready world. `player`: the game's player object; `squad`: the player's
    // squad without the stand-ins; `standIns`: every stand-in the mod made (its local handle and
    // object). `stillThere(o)`: the object o.handle resolves to now is o.ptr (a leftover).
    template <class StillThere>
    Verdict Observe(const void* player, std::vector<const void*> squad, const std::vector<Object>& standIns, StillThere stillThere) {
        Verdict v;
        std::sort(squad.begin(), squad.end());
        squad.erase(std::unique(squad.begin(), squad.end()), squad.end());
        if (!seen_ || player != player_) v.newWorld = true;
        else if (gap_ && squad != squad_) v.newWorld = true;   // only a gap can hide a load
        if (v.newWorld && seen_)
            for (const Object& o : standIns_)
                if (stillThere(o)) v.leftovers.push_back(o);
        seen_ = true;
        gap_ = false;
        player_ = player;
        squad_ = std::move(squad);
        standIns_ = v.newWorld ? std::vector<Object>{} : standIns;
        if (v.newWorld) ++generation_;
        return v;
    }

    uint32_t generation() const { return generation_; }

private:
    bool seen_ = false;
    bool gap_ = false;
    const void* player_ = nullptr;
    std::vector<const void*> squad_;
    std::vector<Object> standIns_;
    uint32_t generation_ = 0;
};

}  // namespace kc
