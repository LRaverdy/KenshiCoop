// Items on the ground.
//
// The host's ground is the one that counts. Every way an item reaches the ground in Kenshi ends in
// Item::activate (shared by all 14 item classes), but so does a zone streaming in its saved items,
// which every machine already has from the save: activate alone cannot tell a drop. So the host
// learns of drops twice over:
//   1. the drop hooks (plugin/hooks.cpp): Inventory::dropItem (what an inventory window does with an
//      item released over the world, for a character, a pack animal, a chest, a backpack),
//      CharacterHuman::dropItem and CharacterAnimal::dropItem (the game's own drops: full inventory,
//      AI). They announce the item the moment it is in the world;
//   2. a scan of the items lying around every player's character, twice a second (GroundScan): an
//      item that turns up where a character already stood watching was dropped by a path no hook saw
//      (it is announced); one that leaves the ground there was taken (a pickup is announced); a stack
//      whose count changed is announced again. Items carried into view by a walk, a teleport or a zone
//      loading are only noted: the clients have them from the save.
// Each item is announced once (the hooks nest: Inventory::dropItem calls the character's dropItem).
//
// A client replays them (ApplyGround): its copy of a host item is aliased to the host's handle. A
// repeat is ignored; an identical item already lying on the spot and standing for no other host item
// (its own copy from the save) is adopted instead of made twice; GroundReconcile removes a copy made
// before the zone holding the save's own copy loaded.
#include "world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "hooks.h"

namespace kcp {

namespace {
constexpr float kScanRadius = 300.0f;               // 30 m around each player's character
constexpr float kScanInside = kScanRadius - 40.0f;  // an item at the very edge may come and go
constexpr double kScanEvery = 0.5;
constexpr double kSettle = 2.0;                     // a character that jumped (teleport, zone change) watches again after this
constexpr float kJump = 60.0f;                      // more than this between two scans: it jumped
constexpr float kMoved = 30.0f;                     // an announced item that moved this far is placed again
constexpr double kForget = 30.0;                    // an item out of everyone's sight is forgotten after this

float Dist(const kc::Vec3& a, const kc::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
bool NearAnyXZ(const kc::Vec3& p, const std::vector<kc::Vec3>& centres, float r) {
    for (const auto& c : centres) {
        const float dx = p.x - c.x, dz = p.z - c.z;
        if (dx * dx + dz * dz < r * r) return true;
    }
    return false;
}
bool SameStack(const kc::ItemState& a, const kc::ItemState& b) { return a.sameKind(b) && a.quantity == b.quantity; }
} // namespace

void KenshiWorld::ResetGround() {
    std::lock_guard<std::mutex> lk(groundMutex_);
    localDrops_.clear();
    groundAlias_.clear();
    groundMade_.clear();
    groundTrack_.clear();
    groundOut_.clear();
    groundWatch_.clear();
    groundCentre_.clear();
    groundSince_ = NowSeconds();
    groundScanAt_ = groundReconcileAt_ = 0;
}

KenshiWorld::GroundStats KenshiWorld::GroundCounters() {
    std::lock_guard<std::mutex> lk(groundMutex_);
    return groundStats_;
}

// ---------------------------------------------------------------- host

void KenshiWorld::GroundOutLocked(kc::GroundEvent e) {
    if (e.kind == kc::GroundKind::Dropped) {
        auto t = groundTrack_.find(e.item);
        if (t != groundTrack_.end() && t->second.announced && SameStack(t->second.st, e.state) && Dist(t->second.pos, e.pos) < kMoved) {
            ++groundStats_.repeatsSkipped;   // already announced (nested drop hooks)
            return;
        }
        GroundTrack& g = groundTrack_[e.item];
        g.st = e.state;
        g.pos = e.pos;
        g.announced = true;
        g.seen = NowSeconds();
    } else {
        groundTrack_.erase(e.item);
    }
    if (groundOut_.size() < 4096) groundOut_.push_back(std::move(e));
}

void KenshiWorld::NoteGround(kc::GroundEvent e) {
    if (client_ || !active_) return;
    std::lock_guard<std::mutex> lk(groundMutex_);
    GroundOutLocked(std::move(e));
}

void KenshiWorld::NoteItemDropped(void* item) {
    if (client_ || !active_ || !item || !kenshi::ItemInWorld(item)) return;
    kc::GroundEvent e;
    e.kind = kc::GroundKind::Dropped;
    if (!kenshi::DescribeGroundItem(item, e.item, e.state, e.pos)) return;
    std::lock_guard<std::mutex> lk(groundMutex_);
    const uint64_t before = groundStats_.repeatsSkipped;
    GroundOutLocked(std::move(e));
    if (groundStats_.repeatsSkipped == before) ++groundStats_.hookDrops;
}

void KenshiWorld::GroundScan(double now) {
    // where the players' characters are, and which of them stood still enough to be watching
    std::vector<kc::Vec3> centres, watched;
    std::unordered_map<const void*, std::pair<kc::Vec3, double>> next;
    for (auto& [h, c] : squad_) {
        kc::Vec3 p;
        if (!kenshi::GetPosition(c, p)) continue;
        double since = now;
        if (auto it = groundCentre_.find(c); it != groundCentre_.end() && Dist(it->second.first, p) < kJump) since = it->second.second;
        next[c] = {p, since};
        if (!NearAnyXZ(p, centres, 50.0f)) centres.push_back(p);
        if (now - since >= kSettle) watched.push_back(p);
    }
    groundCentre_.swap(next);
    struct Seen { kc::Handle h; kc::ItemState st; kc::Vec3 pos; };
    std::vector<Seen> seen;
    std::unordered_set<kc::Handle, kc::HandleHash> seenSet;
    std::vector<void*> items;
    for (const auto& c : centres) {
        kenshi::WorldItemsNear(c, kScanRadius, items);
        for (void* it : items) {
            Seen s;
            if (!kenshi::DescribeGroundItem(it, s.h, s.st, s.pos) || !s.h.valid() || !seenSet.insert(s.h).second) continue;
            seen.push_back(std::move(s));
        }
    }
    const bool settled = now - groundSince_ > 5.0;   // just (re)loaded: everything lying there is the save's
    std::lock_guard<std::mutex> lk(groundMutex_);
    for (auto& s : seen) {
        auto t = groundTrack_.find(s.h);
        if (t == groundTrack_.end()) {
            const bool appeared = settled && NearAnyXZ(s.pos, groundWatch_, kScanInside);
            if (appeared) {
                ++groundStats_.scanDrops;
                Log("ground: %s x%d turned up at (%.0f,%.0f,%.0f) with no drop hook: announced", s.st.templateSid.c_str(), s.st.quantity,
                    s.pos.x, s.pos.y, s.pos.z);
                kc::GroundEvent e;
                e.kind = kc::GroundKind::Dropped;
                e.item = s.h;
                e.state = s.st;
                e.pos = s.pos;
                GroundOutLocked(std::move(e));
            } else {
                GroundTrack& g = groundTrack_[s.h];
                g.st = s.st;
                g.pos = s.pos;
                g.announced = false;
                g.seen = now;
            }
            continue;
        }
        GroundTrack& g = t->second;
        const bool changed = !SameStack(g.st, s.st);
        if (changed || (g.announced && Dist(g.pos, s.pos) > kMoved)) {
            // a stack that grew or shrank where it lies, or an announced item that rolled / fell
            // away: the clients replace their copy
            if (changed) ++groundStats_.scanChanged;
            Log("ground: %s x%d -> %s x%d at (%.0f,%.0f,%.0f): announced again", g.st.templateSid.c_str(), g.st.quantity,
                s.st.templateSid.c_str(), s.st.quantity, s.pos.x, s.pos.y, s.pos.z);
            kc::GroundEvent gone;
            gone.kind = kc::GroundKind::PickedUp;
            gone.item = s.h;
            gone.state = g.st;
            gone.pos = g.pos;
            GroundOutLocked(std::move(gone));
            kc::GroundEvent back;
            back.kind = kc::GroundKind::Dropped;
            back.item = s.h;
            back.state = s.st;
            back.pos = s.pos;
            GroundOutLocked(std::move(back));
            continue;
        }
        g.pos = s.pos;
        g.seen = now;
    }
    for (auto it = groundTrack_.begin(); it != groundTrack_.end();) {
        if (seenSet.count(it->first)) { ++it; continue; }
        // not where a player is looking any more: forgotten after a while (seen again later, it is
        // only noted: whoever comes near has it already)
        if (!NearAnyXZ(it->second.pos, centres, kScanInside)) {
            it = now - it->second.seen > kForget ? groundTrack_.erase(it) : std::next(it);
            continue;
        }
        // in plain sight yet not found: gone from the ground (taken by a path no hook saw)
        void* obj = kenshi::ResolveItem(it->first);
        if (obj && kenshi::ItemInWorld(obj)) { ++it; continue; }
        ++groundStats_.scanGone;
        Log("ground: %s x%d left the ground at (%.0f,%.0f,%.0f) with no pickup hook: announced", it->second.st.templateSid.c_str(),
            it->second.st.quantity, it->second.pos.x, it->second.pos.y, it->second.pos.z);
        kc::GroundEvent e;
        e.kind = kc::GroundKind::PickedUp;
        e.item = it->first;
        e.state = it->second.st;
        e.pos = it->second.pos;
        it = groundTrack_.erase(it);
        if (groundOut_.size() < 4096) groundOut_.push_back(std::move(e));
    }
    groundWatch_.swap(watched);
}

void KenshiWorld::TakeGroundEvents(std::vector<kc::GroundEvent>& out) {
    const double now = NowSeconds();
    if (active_ && !client_ && live_ && now >= groundScanAt_) {
        groundScanAt_ = now + kScanEvery;
        GroundScan(now);
    }
    std::lock_guard<std::mutex> lk(groundMutex_);
    out.swap(groundOut_);
    groundOut_.clear();
}

// ---------------------------------------------------------------- client

void KenshiWorld::QueueLocalDrop(void* holder, void* item) {
    kc::ItemState s;
    if (!holder || !kenshi::DescribeInventoryItem(item, s)) return;
    std::lock_guard<std::mutex> lk(groundMutex_);
    if (localDrops_.size() < 256) localDrops_.emplace_back(holder, s);
}

void KenshiWorld::TakeLocalDrops(std::vector<std::pair<kc::Handle, kc::ItemState>>& out) {
    out.clear();
    std::vector<std::pair<void*, kc::ItemState>> raw;
    {
        std::lock_guard<std::mutex> lk(groundMutex_);
        raw.swap(localDrops_);
    }
    for (auto& [holder, s] : raw) {
        kc::Handle h;
        const bool ok = kenshi::IsCharacter(holder) ? kenshi::GetHandle(static_cast<kenshi::Character*>(holder), h) : kenshi::ObjectHandle(holder, h);
        if (ok && h.valid()) out.emplace_back(h, std::move(s));
        else Log("ground: a drop from an inventory we cannot name was not sent (%s)", s.templateSid.c_str());
    }
}

namespace {
// An item lying right there, the same stack, standing for no host item yet.
void* UnclaimedTwin(const kc::ItemState& want, const kc::Vec3& at, float within, const void* except,
                    const std::unordered_map<kc::Handle, kc::Handle, kc::HandleHash>& alias) {
    std::vector<void*> around;
    kenshi::WorldItemsNear(at, within + 10.0f, around);
    void* best = nullptr;
    float bestD = within;
    for (void* it : around) {
        kc::Handle h;
        kc::ItemState st;
        kc::Vec3 p;
        if (it == except || !kenshi::DescribeGroundItem(it, h, st, p) || !SameStack(st, want)) continue;
        bool claimed = false;
        for (const auto& [host, mine] : alias) claimed = claimed || mine == h;
        const float d = Dist(p, at);
        if (!claimed && d < bestD) { bestD = d; best = it; }
    }
    return best;
}
} // namespace

void KenshiWorld::ApplyGround(const kc::GroundEvent& e) {
    if (!client_) return;
    HostCallScope scope;
    std::lock_guard<std::mutex> lk(groundMutex_);
    if (e.kind == kc::GroundKind::PickedUp) {
        // the same item here (from the shared save), or the copy we made when the host dropped it
        auto a = groundAlias_.find(e.item);
        void* item = kenshi::ResolveItem(a != groundAlias_.end() ? a->second : e.item);
        if (!item || !kenshi::ItemLoose(item)) {   // an item from the save: same thing at the same spot
            std::vector<void*> around;
            kenshi::LooseItemsNear(e.pos, 30.0f, around);
            float best = 15.0f;
            for (void* it : around) {
                kc::Handle ih;
                kc::ItemState st;
                kc::Vec3 p;
                if (!kenshi::DescribeGroundItem(it, ih, st, p) || st.templateSid != e.state.templateSid) continue;
                bool claimed = false;   // another host item's copy is not this one
                for (const auto& [host, mine] : groundAlias_) claimed = claimed || (mine == ih && host != e.item);
                const float d = Dist(p, e.pos);
                if (!claimed && d < best) { best = d; item = it; }
            }
        }
        const bool onGround = item && kenshi::ItemLoose(item);
        const bool gone = onGround && kenshi::DestroyItem(item);
        groundStats_.removed += gone;
        Log("host picked up item %u:%u: %s", e.item.index, e.item.serial,
            gone ? "removed here" : !item ? "we do not have it" : !onGround ? "not on the ground here" : "could not remove it");
        if (a != groundAlias_.end()) {
            groundMade_.erase(a->second);
            groundAlias_.erase(a);
        }
        return;
    }
    // A repeat (the same announcement twice, a stack re-announced unchanged): our copy is there.
    if (auto a = groundAlias_.find(e.item); a != groundAlias_.end()) {
        void* mine = kenshi::ResolveItem(a->second);
        kc::Handle h2;
        kc::ItemState st;
        kc::Vec3 p;
        if (mine && kenshi::ItemLoose(mine) && kenshi::DescribeGroundItem(mine, h2, st, p) && SameStack(st, e.state) && Dist(p, e.pos) < kMoved) {
            ++groundStats_.repeats;
            return;
        }
        if (mine && kenshi::ItemLoose(mine)) kenshi::DestroyItem(mine);   // another stack now, or moved: replaced
        groundMade_.erase(a->second);
        groundAlias_.erase(a);
    }
    // Our own copy from the save already lying there (the host announced an item its scan only saw
    // turn up): adopted, not made twice.
    if (void* twin = UnclaimedTwin(e.state, e.pos, 3.0f, nullptr, groundAlias_)) {
        kc::Handle h;
        if (kenshi::ObjectHandle(twin, h)) {
            groundAlias_[e.item] = h;
            ++groundStats_.matched;
            Log("host dropped item %u:%u (%s x%d): the same one already lies here (%u:%u), kept", e.item.index, e.item.serial,
                e.state.templateSid.c_str(), e.state.quantity, h.index, h.serial);
            return;
        }
    }
    kc::Handle mine;
    std::string why;
    if (kenshi::CreateGroundItem(e.state, e.pos, mine, &why)) {
        groundAlias_[e.item] = mine;
        groundMade_.insert(mine);
        ++groundStats_.created;
        Log("host dropped item %u:%u (%s x%d): placed here as %u:%u", e.item.index, e.item.serial, e.state.templateSid.c_str(), e.state.quantity,
            mine.index, mine.serial);
    }
    else Log("cannot place the host's dropped item %s: %s", e.state.templateSid.c_str(), why.c_str());
}

void KenshiWorld::GroundReconcile() {
    const double now = NowSeconds();
    if (now < groundReconcileAt_) return;
    groundReconcileAt_ = now + 2.0;
    HostCallScope scope;
    std::lock_guard<std::mutex> lk(groundMutex_);
    if (groundMade_.empty()) return;
    for (auto& [host, mine] : groundAlias_) {
        if (!groundMade_.count(mine)) continue;
        void* ours = kenshi::ResolveItem(mine);
        kc::Handle h;
        kc::ItemState st;
        kc::Vec3 p;
        if (!ours || !kenshi::ItemInWorld(ours) || !kenshi::DescribeGroundItem(ours, h, st, p)) continue;
        void* twin = UnclaimedTwin(st, p, 2.0f, ours, groundAlias_);
        kc::Handle th;
        if (!twin || !kenshi::ObjectHandle(twin, th)) continue;
        // the save's own copy streamed in under ours: one of the two goes (ours, the game's stays)
        kenshi::DestroyItem(ours);
        groundMade_.erase(mine);
        Log("ground: %s x%d lay here twice (our copy of host item %u:%u and the save's %u:%u): ours removed", st.templateSid.c_str(), st.quantity,
            host.index, host.serial, th.index, th.serial);
        mine = th;
        ++groundStats_.merged;
    }
}

} // namespace kcp
