// The squad window (Escouade) and the Tâches panel, on the game's side.
//
// Host: the squads of the player faction as they are (ReadSquadViews: the faction's order, empty
// squads too, each squad named by its Platoon's handle) and the squad requests the session
// authorized (move, swap, create, rename, reorder, remove, rename a character), run with the game's
// own functions (addCharacterAt, swapCharacters, createSquad, setName, changePlatoonIndex,
// destroyPlatoon, Character::setName).
// Client: our squads are matched to the host's (squadIds_: a squad of ours keeps the host squad it
// was matched to), then made the same: created, filled in the host's order, renamed, put in the host's
// order, removed when the host has no such squad and ours is empty. The squad window's own actions
// here never change anything locally: a portrait dropped becomes a request (QueueSquadRequest); a
// rename, a new squad, a squad dragged or removed is seen by the session comparing our squads with
// the host's (Session::ClientSquads).
// Jobs: the host's job list of each player character with each job's target (ReadJobList); a client
// builds the same list (removes, adds, reorders: ApplyJobList).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "hooks.h"
#include "util.h"
#include "world.h"

namespace kcp {

namespace {
std::string KeyOf(const kc::Handle& h) {   // as world.cpp names characters (localJobs_)
    char b[80];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}
uint64_t PackKey(const kc::Handle& h) { return (uint64_t(h.index) << 32) | h.serial; }
uint64_t LocalKey(void* squad) {
    kc::Handle h;
    return kenshi::SquadHandle(squad, h) ? PackKey(h) : 0;
}
void* SquadById(const kc::Handle& id) {
    std::vector<void*> all;
    kenshi::PlayerSquads(all);
    for (void* s : all) {
        kc::Handle h;
        if (kenshi::SquadHandle(s, h) && h == id) return s;
    }
    return nullptr;
}
float Distance(const kc::Vec3& a, const kc::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

// ---- host
void KenshiWorld::ReadSquadViews(std::vector<SquadView>& out) {
    out.clear();
    std::vector<void*> all;
    kenshi::PlayerSquads(all);
    for (void* sq : all) {
        SquadView v;
        if (!kenshi::SquadHandle(sq, v.id)) continue;
        v.key = PackKey(v.id);
        kenshi::SquadName(sq, v.name);
        std::vector<kenshi::Character*> members;
        kenshi::SquadMembers(sq, members);
        for (kenshi::Character* c : members) {
            kc::Handle h;
            if (kenshi::GetHandle(c, h)) v.members.push_back(HostHandleOf(h));
        }
        out.push_back(std::move(v));
    }
}

bool KenshiWorld::SquadMove(const kc::Handle& who, const kc::Handle& squad, int index, bool swap, const std::string& name, kc::Handle& created) {
    kenshi::Character* c = FindSquad(who);
    void* target = squad.valid() ? SquadById(squad) : nullptr;
    if (!c || (squad.valid() && !target)) return false;
    bool ok = false;
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    // creating or filling a squad selects it in the host's squad bar: the host's selection stays
    kenshi::KeepSelection([&] {
        if (!target) {
            target = kenshi::NewSquad();
            if (!target) return;
            if (!name.empty()) kenshi::SetSquadName(target, name);
            kenshi::SquadHandle(target, created);
        }
        std::vector<kenshi::Character*> members;
        kenshi::SquadMembers(target, members);
        if (swap && index >= 0 && index < int(members.size()) && kenshi::SquadOf(c) == target) {
            ok = kenshi::SwapInSquad(target, kenshi::SquadMemberIndex(c), kenshi::SquadMemberIndex(members[size_t(index)]));
            return;
        }
        const int at = index < int(members.size()) ? kenshi::SquadMemberIndex(members[size_t(index)]) : kenshi::SquadSize(target);
        ok = kenshi::MoveToSquad(target, c, at);
    });
    return ok;
}

bool KenshiWorld::SquadCreate(const std::string& name, kc::Handle& created) {
    void* sq = nullptr;
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    kenshi::KeepSelection([&] { sq = kenshi::NewSquad(); });
    if (!sq) return false;
    kenshi::SetSquadName(sq, name);
    return kenshi::SquadHandle(sq, created);
}

bool KenshiWorld::SquadRename(const kc::Handle& squad, const std::string& name) {
    void* sq = SquadById(squad);
    if (!sq) return false;
    kenshi::SetSquadName(sq, name);
    std::string now;
    return kenshi::SquadName(sq, now) && now == name;
}

// index: the place in ReadSquadViews' list, the others keeping their order.
bool KenshiWorld::SquadOrder(const kc::Handle& squad, int index) {
    std::vector<void*> all;
    kenshi::PlayerSquads(all);
    void* sq = SquadById(squad);
    const int cur = kenshi::SquadFactionIndex(sq);
    if (!sq || cur < 0) return false;
    std::vector<void*> rest;
    for (void* s : all) if (s != sq) rest.push_back(s);
    if (rest.empty()) return true;
    // faction indices once ours is taken out (changePlatoonIndex removes it, then inserts it there)
    auto shrunk = [&](void* s) { const int i = kenshi::SquadFactionIndex(s); return i > cur ? i - 1 : i; };
    const int at = index < int(rest.size()) ? shrunk(rest[size_t(index)]) : shrunk(rest.back()) + 1;
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    return kenshi::SetSquadOrder(sq, at);
}

bool KenshiWorld::SquadRemove(const kc::Handle& squad) {
    void* sq = SquadById(squad);
    if (!sq || kenshi::SquadSize(sq) != 0) return false;
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    bool ok = false;
    kenshi::KeepSelection([&] { ok = kenshi::DestroySquad(sq); });
    return ok;
}

bool KenshiWorld::RenameCharacter(const kc::Handle& h, const std::string& name) {
    kenshi::Character* c = FindSquad(h);
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    return c && kenshi::SetCharacterName(c, name);
}

bool KenshiWorld::OrderShared(const kc::Handle& h, const kc::Command& c) {
    sharedOrder_ = true;
    const bool ok = Order(h, c);
    sharedOrder_ = false;
    return ok;
}

// ---- client
kc::Handle KenshiWorld::HostSquadId(void* localSquad) const {
    auto it = squadIds_.find(LocalKey(localSquad));
    return it != squadIds_.end() ? it->second : kc::Handle{};
}

void KenshiWorld::ReadLocalSquads(std::vector<SquadView>& out) {
    out.clear();
    std::vector<void*> all;
    kenshi::PlayerSquads(all);
    std::unordered_set<uint64_t> present;
    for (void* sq : all) {
        SquadView v;
        v.key = LocalKey(sq);
        if (!v.key) continue;
        present.insert(v.key);
        if (auto it = squadIds_.find(v.key); it != squadIds_.end()) v.id = it->second;
        v.awaitingHost = squadAwaiting_.count(v.key) != 0;
        kenshi::SquadName(sq, v.name);
        std::vector<kenshi::Character*> members;
        kenshi::SquadMembers(sq, members);
        for (kenshi::Character* c : members) {
            kc::Handle h;
            if (kenshi::GetHandle(c, h)) v.members.push_back(HostHandleOf(h));
        }
        out.push_back(std::move(v));
    }
    for (auto it = squadIds_.begin(); it != squadIds_.end();) it = present.count(it->first) ? std::next(it) : squadIds_.erase(it);
    for (auto it = squadAwaiting_.begin(); it != squadAwaiting_.end();) it = present.count(*it) ? std::next(it) : squadAwaiting_.erase(it);
}

void KenshiWorld::ApplySquadViews(const std::vector<SquadView>& host) {
    std::vector<void*> locals;
    kenshi::PlayerSquads(locals);
    std::unordered_set<kc::Handle, HandleHash> hostIds;
    for (const auto& s : host) hostIds.insert(s.id);
    std::vector<void*> target(host.size(), nullptr);
    std::unordered_set<void*> taken;
    auto mappedElsewhere = [&](void* l) {
        auto it = squadIds_.find(LocalKey(l));
        return it != squadIds_.end() && hostIds.count(it->second);
    };
    // 1. the squad of ours matched before; 2. the same squad (same handle: loaded from the same save)
    for (size_t i = 0; i < host.size(); ++i)
        for (void* l : locals) {
            auto it = squadIds_.find(LocalKey(l));
            if (it != squadIds_.end() && it->second == host[i].id && !taken.count(l)) { target[i] = l; taken.insert(l); break; }
        }
    for (size_t i = 0; i < host.size(); ++i) {
        if (target[i]) continue;
        for (void* l : locals) {
            kc::Handle h;
            if (!taken.count(l) && !mappedElsewhere(l) && kenshi::SquadHandle(l, h) && h == host[i].id) { target[i] = l; taken.insert(l); break; }
        }
    }
    // 3. the squad of ours holding most of its members; 4. a new empty one of ours (made here and
    //    asked of the host, or one a portrait was dropped on); 5. a new one
    for (size_t i = 0; i < host.size(); ++i) {
        if (target[i]) continue;
        std::unordered_map<void*, int> count;
        for (const auto& h : host[i].members)
            if (kenshi::Character* c = FindSquad(h))
                if (void* sq = kenshi::SquadOf(c); sq && !taken.count(sq) && !mappedElsewhere(sq)) ++count[sq];
        int best = 0;
        for (auto& [sq, n] : count) if (n > best) { best = n; target[i] = sq; }
        if (target[i]) taken.insert(target[i]);
    }
    int created = 0;
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    for (size_t i = 0; i < host.size(); ++i) {
        if (target[i]) continue;
        for (void* l : locals)
            if (!taken.count(l) && !mappedElsewhere(l) && kenshi::SquadSize(l) == 0) { target[i] = l; taken.insert(l); break; }
        if (target[i] || created >= 4) continue;   // never a flood of empty squads, whatever goes wrong
        kenshi::KeepSelection([&] { target[i] = kenshi::NewSquad(); });
        if (!target[i]) continue;
        ++created;
        taken.insert(target[i]);
        Log("squads: a new squad for '%s'", host[i].name.c_str());
    }
    for (size_t i = 0; i < host.size(); ++i) {
        void* sq = target[i];
        if (!sq) continue;
        const uint64_t key = LocalKey(sq);
        squadIds_[key] = host[i].id;
        if (!host[i].members.empty()) squadAwaiting_.erase(key);
        int j = 0;
        for (const auto& h : host[i].members) {
            kenshi::Character* c = FindSquad(h);
            if (!c) continue;
            if (!(kenshi::SquadOf(c) == sq && kenshi::SquadMemberIndex(c) == j)) {
                kc::Handle before, after;
                kenshi::GetHandle(c, before);
                kenshi::MoveToSquad(sq, c, j);
                if (kenshi::GetHandle(c, after)) LocalRehandled(before, after);
            }
            ++j;
        }
        kenshi::SetSquadName(sq, host[i].name);
    }
    // the host's order: each squad after the one before it
    int prev = -1;
    for (void* sq : target) {
        if (!sq) continue;
        const int cur = kenshi::SquadFactionIndex(sq);
        if (cur >= 0 && cur < prev) { kenshi::SetSquadOrder(sq, prev); prev = kenshi::SquadFactionIndex(sq); }
        else if (cur >= 0) prev = cur;
    }
    // squads of ours the host no longer has: removed once empty
    for (void* l : locals) {
        auto it = squadIds_.find(LocalKey(l));
        if (it == squadIds_.end() || hostIds.count(it->second) || taken.count(l) || kenshi::SquadSize(l) != 0) continue;
        squadIds_.erase(it);
        bool ok = false;
        kenshi::KeepSelection([&] { ok = kenshi::DestroySquad(l); });
        Log("squads: a squad the host no longer has removed here (%s)", ok ? "ok" : "failed");
    }
    // the squad bar shows one of our squads, not an empty one
    std::vector<kenshi::Character*> shown;
    kenshi::SquadMembers(kenshi::ShownSquad(), shown);
    if (shown.empty())
        for (const auto& h : controllable_)
            if (kenshi::Character* c = FindSquad(h)) { kenshi::ShowSquad(kenshi::SquadOf(c)); break; }
}

void KenshiWorld::RemoveLocalSquad(uint64_t key) {
    std::vector<void*> all;
    kenshi::PlayerSquads(all);
    for (void* sq : all) {
        if (LocalKey(sq) != key || kenshi::SquadSize(sq) != 0) continue;
        CallScopeGuard scopeRepair("squads");
        HostCallScope scope;
        bool ok = false;
        kenshi::KeepSelection([&] { ok = kenshi::DestroySquad(sq); });
        Log("squads: the host refused our new squad: removed here (%s)", ok ? "ok" : "failed");
    }
    squadAwaiting_.erase(key);
}

void KenshiWorld::ApplyCharacterName(const kc::Handle& h, const std::string& name) {
    kenshi::Character* c = FindSquad(h);
    if (!c) return;
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    if (kenshi::SetCharacterName(c, name)) Log("names: a squad character takes the host's name '%s'", name.c_str());
}

bool KenshiWorld::ReadCharacterName(const kc::Handle& h, std::string& out) {
    kenshi::Character* c = Find(h);
    return c && kenshi::CharacterName(c, out);
}

void KenshiWorld::QueueSquadRequest(kenshi::Character* actor, void* targetSquad, int index) {
    LocalSquadRequest r;
    kc::Handle h;
    if (!kenshi::GetHandle(actor, h)) return;
    r.actor = HostHandleOf(h);
    r.op = kc::SquadOp::Move;
    r.index = std::max(0, index);
    r.squad = HostSquadId(targetSquad);
    if (!r.squad.valid()) {   // a squad of ours the host does not have yet: the host makes it with this character
        kenshi::SquadName(targetSquad, r.name);
        if (const uint64_t key = LocalKey(targetSquad)) squadAwaiting_.insert(key);
    }
    squadReqs_.push_back(std::move(r));
}

void KenshiWorld::TakeLocalSquadRequests(std::vector<LocalSquadRequest>& out) {
    out.swap(squadReqs_);
    squadReqs_.clear();
}

void KenshiWorld::SetShared(const std::vector<kc::Handle>& handles) {
    shared_.clear();
    shared_.insert(handles.begin(), handles.end());
}

// ---- jobs with their targets
namespace {
struct LocalJob {
    int task = -1;
    kc::Handle subject;   // host handle (characters)
    std::string sid;      // furniture: kind and place
    kc::Vec3 pos;
};
} // namespace

bool KenshiWorld::ReadJobList(const kc::Handle& h, std::vector<kc::JobEntry>& jobs) {
    jobs.clear();
    kenshi::Character* c = FindSquad(h);
    if (!c) return false;
    for (int i = 0, n = kenshi::PermajobCount(c); i < n; ++i) {
        kc::JobEntry e;
        e.task = kenshi::PermajobType(c, i);
        if (e.task < 0) continue;
        kc::Handle subj;
        kenshi::PermajobTarget(c, i, subj, e.location);
        if (subj.valid()) {
            void* o = kenshi::ResolveObject(subj);
            e.subject = HostHandleOf(subj);
            if (o && !kenshi::IsCharacter(o)) { kenshi::ObjectTemplate(o, e.subjectSid); kenshi::ObjectPosition(o, e.subjectPos); }
        }
        if (!std::isfinite(e.location.x) || !std::isfinite(e.location.y) || !std::isfinite(e.location.z)) e.location = {};
        jobs.push_back(std::move(e));
    }
    return true;
}

void KenshiWorld::ApplyJobList(const kc::Handle& h, const std::vector<kc::JobEntry>& jobs) {
    kenshi::Character* c = FindSquad(h);
    if (!c) return;
    auto readLocal = [&] {
        std::vector<LocalJob> v;
        for (int i = 0, n = kenshi::PermajobCount(c); i < n; ++i) {
            LocalJob l;
            l.task = kenshi::PermajobType(c, i);
            kc::Handle subj;
            kc::Vec3 loc;
            kenshi::PermajobTarget(c, i, subj, loc);
            if (subj.valid()) {
                void* o = kenshi::ResolveObject(subj);
                if (o && !kenshi::IsCharacter(o)) { kenshi::ObjectTemplate(o, l.sid); kenshi::ObjectPosition(o, l.pos); }
                else l.subject = HostHandleOf(subj);
            }
            v.push_back(std::move(l));
        }
        return v;
    };
    auto same = [](const kc::JobEntry& e, const LocalJob& l) {
        if (e.task != l.task) return false;
        if (!e.subjectSid.empty()) return l.sid == e.subjectSid && Distance(l.pos, e.subjectPos) < 40.0f;
        return l.sid.empty() && l.subject == e.subject;
    };
    // a job given here a moment ago: the host's list may not have it yet (kept a few seconds)
    std::vector<int> grace;
    if (auto lj = localJobs_.find(KeyOf(h)); lj != localJobs_.end()) {
        const unsigned long long now = GetTickCount64();
        for (const auto& p : lj->second)
            if (p.second >= now && std::none_of(jobs.begin(), jobs.end(), [&](const kc::JobEntry& e) { return e.task == p.first; })) grace.push_back(p.first);
    }
    CallScopeGuard scopeRepair("squads");
    HostCallScope scope;
    // 1. ours the host does not have: removed
    std::vector<LocalJob> local = readLocal();
    std::vector<bool> used(jobs.size(), false);
    std::vector<int> drop;
    for (int i = 0; i < int(local.size()); ++i) {
        bool found = false;
        for (size_t k = 0; k < jobs.size() && !found; ++k)
            if (!used[k] && same(jobs[k], local[size_t(i)])) used[k] = found = true;
        if (!found) {
            auto g = std::find(grace.begin(), grace.end(), local[size_t(i)].task);
            if (g != grace.end()) { grace.erase(g); continue; }
            drop.push_back(i);
        }
    }
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) kenshi::RemovePermajob(c, *it);
    // 2. the host's we lack: added (the same subject here: a character by its handle, furniture by kind and place)
    int added = 0, missing = 0, wrong = 0;
    for (size_t k = 0; k < jobs.size(); ++k) {
        if (used[k]) continue;
        const kc::JobEntry& e = jobs[k];
        void* subject = nullptr;
        if (!e.subjectSid.empty()) {
            std::vector<void*> around;
            kenshi::ObjectsNear(e.subjectPos, 60.0f, around);
            float best = 40.0f;
            for (void* o : around) {
                std::string s;
                kc::Vec3 p;
                if (!kenshi::ObjectTemplate(o, s) || s != e.subjectSid || !kenshi::ObjectPosition(o, p)) continue;
                if (const float d = Distance(p, e.subjectPos); d < best) { best = d; subject = o; }
            }
        } else if (e.subject.valid()) {
            subject = Find(e.subject);
        }
        if ((e.subject.valid() || !e.subjectSid.empty()) && !subject) { ++missing; continue; }   // not here (yet): next time
        // what this copy found must be of the kind the task expects: a character found for a
        // building's job (a stand-in, a handle that names something else here) is never handed to
        // the game (a BUILD job on an NPC crashes it: kenshi_x64+0x883B78)
        std::string why;
        const uint32_t flags = TargetFlagsOf(c, subject, subject != nullptr);
        if (kc::TaskTargetWrongKind(kc::TaskVia::AddJob, e.task, flags, &why)) {
            ++wrong;
            Log("jobs: %s: the host's job %d not copied here: %s (subject flags %#x)", KeyOf(h).c_str(), e.task, why.c_str(), unsigned(flags));
            continue;
        }
        if (kenshi::AddPermajob(c, e.task, subject, e.location)) ++added;
    }
    // 3. the host's order
    int moved = 0;
    local = readLocal();
    for (size_t k = 0; k < jobs.size() && k < local.size(); ++k) {
        if (same(jobs[k], local[k])) continue;
        for (size_t j = k + 1; j < local.size(); ++j) {
            if (!same(jobs[k], local[j])) continue;
            if (kenshi::MovePermajob(c, int(j), int(k))) { ++moved; local = readLocal(); }
            break;
        }
    }
    if (!drop.empty() || added || moved || missing || wrong)
        Log("jobs: %s made like the host's: %zu removed, %d added, %d moved%s%s", KeyOf(h).c_str(), drop.size(), added, moved,
            missing ? " (some targets are not here yet)" : "", wrong ? " (some targets are of the wrong kind here)" : "");
}

} // namespace kcp
