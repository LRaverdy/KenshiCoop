// Lot A: doors and locks.
//
// Host: every 0.5 s, the doors and locked furniture within kDoorRadius of any squad member (the
// host's and the players' characters) are read; those that changed go to every player in the world
// (Doors), and all of them every 5 s (a late joiner, a lost change). A door button a client clicked
// (DoorRequest) is run by the host's game.
// Client: the host's doors are kept by kind and place and imposed on our copies of them; again now
// and then (a door in a zone that was not loaded yet, a local change); the door buttons the player
// clicks go to the host.
#include "kc/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kc {

namespace {
constexpr float kDoorRadius = 400.0f;        // 40 m around each character: what a player can see and reach
constexpr double kDoorsInterval = 0.5;
constexpr double kDoorsFullInterval = 5.0;
constexpr double kDoorsReapply = 3.0;
constexpr size_t kDoorApplyBudget = 64;     // per tick: a town has a few hundred

std::string DoorKey(const DoorState& d) {
    char b[96];
    std::snprintf(b, sizeof(b), "@%d,%d,%d", int(std::lround(d.pos.x)), int(std::lround(d.pos.y)), int(std::lround(d.pos.z)));
    return d.sid + b;
}

const char* DoorWord(const DoorState& d) {
    if (d.flags & kDoorBroken) return "broken";
    if (d.kind == DoorKind::Lock) return (d.flags & kDoorLocked) ? "locked" : "unlocked";
    if (d.flags & kDoorLocked) return "locked";
    return (d.state == 1 || d.state == 2) ? "open" : "closed";
}
} // namespace

void Session::ResetDoors() {
    doorsSent_.clear();
    clientDoors_.clear();
    pendingDoorReqs_.clear();
    scratchDoorReqs_.clear();
    nextDoors_ = nextDoorsFull_ = nextDoorsApply_ = 0;
    doorsApplied_ = 0;
}

void Session::HostDoorPacket(uint8_t from, Reader& r) {
    DoorRequest m;
    auto pl = players_.find(from);
    if (pl == players_.end() || !pl->second.inGame || !Decode(r, m)) return;
    if (pendingDoorReqs_.size() < 32) pendingDoorReqs_.emplace_back(from, std::move(m));
}

void Session::HostDoors(double now) {
    for (const auto& [from, req] : pendingDoorReqs_) {
        auto pl = players_.find(from);
        const std::string who = pl != players_.end() ? pl->second.name : "player " + std::to_string(from);
        const bool ok = world_.ExecuteDoorRequest(req);
        log_("[" + who + "] door " + (req.action == DoorAction::LockButton ? "lock button" : "open button") + " on " +
             world_.TemplateName(req.sid) + (ok ? "" : ": door NOT FOUND here"));
        nextDoors_ = 0;   // the result goes out now
    }
    pendingDoorReqs_.clear();
    if (now < nextDoors_) return;
    nextDoors_ = now + kDoorsInterval;
    bool anyInGame = false;
    for (auto& [pid, p] : players_) anyInGame |= p.inGame;
    if (!anyInGame) return;
    std::vector<Vec3> centers;
    for (auto& [id, e] : entities_) {
        EntityState st;
        if (e.squad && world_.Read(e.handle, st)) centers.push_back(st.pos);
    }
    if (centers.empty()) return;
    world_.ReadDoors(centers, kDoorRadius, scratchDoors_);
    const bool full = now >= nextDoorsFull_;
    if (full) nextDoorsFull_ = now + kDoorsFullInterval;
    DoorsMsg m;
    m.full = full;
    int logged = 0;
    for (const auto& d : scratchDoors_) {
        const std::string key = DoorKey(d);
        auto it = doorsSent_.find(key);
        const bool changed = it == doorsSent_.end() || !it->second.sameState(d);
        if (!full && !changed) continue;
        if (changed && it != doorsSent_.end() && logged < 8) {   // a change, not a door first seen
            ++logged;
            log_("door: " + world_.TemplateName(d.sid) + " " + DoorWord(it->second) + " -> " + DoorWord(d) +
                 ((d.flags & kDoorHasLock) ? " (lock level " + std::to_string(d.lockLevel) + ")" : std::string{}));
        }
        if (doorsSent_.size() > 8192) doorsSent_.clear();
        doorsSent_[key] = d;
        m.doors.push_back(d);
    }
    // in pieces of at most kMaxDoorsPerMsg (every piece of a full refresh says so)
    for (size_t i = 0; i < m.doors.size(); i += kMaxDoorsPerMsg) {
        DoorsMsg part;
        part.full = m.full;
        part.doors.assign(m.doors.begin() + ptrdiff_t(i), m.doors.begin() + ptrdiff_t(std::min(m.doors.size(), i + kMaxDoorsPerMsg)));
        Writer w(4096);
        Encode(w, part);
        BroadcastReliable(w, true);
    }
}

void Session::ClientDoorsPacket(Reader& r) {
    DoorsMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    for (auto& d : m.doors) {
        ClientDoor& c = clientDoors_[DoorKey(d)];
        if (!c.found || !c.state.sameState(d)) c.dirty = true;
        c.state = std::move(d);
    }
    if (clientDoors_.size() > 8192) clientDoors_.clear();   // a long trip: the next refreshes bring them back
}

void Session::ClientDoors(double now) {
    // door buttons the player clicked: the host's game runs them
    world_.TakeDoorRequests(scratchDoorReqs_);
    for (const auto& req : scratchDoorReqs_) {
        Writer w;
        Encode(w, req);
        SendReliable(net_.serverPeer(), w);
        log_("door button asked of the host: " + world_.TemplateName(req.sid));
    }
    scratchDoorReqs_.clear();
    if (now >= nextDoorsApply_) {
        nextDoorsApply_ = now + kDoorsReapply;
        for (auto& [key, c] : clientDoors_) c.dirty = true;
    }
    size_t budget = kDoorApplyBudget;
    for (auto& [key, c] : clientDoors_) {
        if (!c.dirty) continue;
        if (budget == 0) break;
        --budget;
        c.found = world_.ApplyDoor(c.state);
        if (c.found) ++doorsApplied_;
        c.dirty = false;   // not here: tried again with the next round
    }
}

} // namespace kc
