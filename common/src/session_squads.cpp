// The squad window (Escouade) and the AI settings, the host's everywhere.
//
// The host's game holds the only squads. It sends SquadState (every squad of the player faction, in
// the faction's order, empty ones too, with each member and each squad character's name) when it
// changes and every 10 s. A client never changes its own squads: what its player does in the squad
// window becomes a SquadRequest that names one of the player's own characters (actor safety), and the
// host runs the requests one at a time, in the order they arrive (the last one wins), then everyone
// gets the new SquadState. Renames typed on a client are kept on screen while the host has not
// answered; a refusal puts the host's name back.
//
// Who may do what (host checks, the client checks the same before sending):
//  - move a character between squads, make it lead (index 0): only one's own characters;
//  - create a squad; rename, reorder or remove one: a squad holding one of one's characters, or an
//    empty one (remove: empty only, never the last one);
//  - rename a character: one's own;
//  - AI settings (squad bar toggles, fight style, speed, Tâches panel): one's own characters, and the
//    characters nobody owns (recruits never given to a player, the host's avatar excepted): any
//    player, the last request the host runs wins.
#include "kc/session.h"

#include <algorithm>
#include <cctype>

namespace kc {

namespace {
constexpr double kSquadStateInterval = 0.5, kSquadStateFull = 10.0;
constexpr double kClientSquadsInterval = 0.5;
constexpr double kNameSettle = 1.0;        // a name typed here goes to the host once it stopped changing
constexpr double kCreateSettle = 1.5;      // a new empty squad here is asked of the host after this
constexpr double kPendingLife = 5.0;       // an unanswered request stops shaping what we show
constexpr double kDoneLinger = 1.0;        // after the host's "done": until its new state is in

std::string Lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Key(const Handle& h) {
    return std::to_string(h.type) + ":" + std::to_string(h.container) + ":" + std::to_string(h.containerSerial) + ":" + std::to_string(h.index) + ":" +
           std::to_string(h.serial);
}
const char* kSquadNotYours = "Action refusée : cette escouade n'a aucun de tes personnages (seul l'hôte peut la modifier).";
} // namespace

bool Session::IsSettingsCommand(const Command& c) {
    if (c.kind != CommandKind::Task) return false;
    switch (c.via) {
    case TaskVia::SetOrder: case TaskVia::RemovePermajob: case TaskVia::MovePermajob: case TaskVia::RemoveJob: return true;
    case TaskVia::AddJob: return c.shift;   // a permanent job (the Tâches panel)
    default: return false;
    }
}

bool Session::MaySetSettings(uint8_t player, uint32_t netId) const {
    if (CheckActor(player, netId) == ActorVerdict::Ok) return true;
    auto it = entities_.find(netId);
    if (it == entities_.end() || !it->second.squad || it->second.container || !shared_.count(netId)) return false;
    if (isHost()) {
        auto pl = players_.find(player);
        return pl != players_.end() && pl->second.inGame;
    }
    return player == localId_;
}

void Session::ResetSquads() {
    squadState_ = SquadStateMsg{};
    haveSquadState_ = squadStateDirty_ = false;
    nextSquadState_ = squadStateFullAt_ = nextClientSquads_ = 0;
    shared_.clear();
    pendingSquadReqs_.clear();
    squadPending_.clear();
    nameEdits_.clear();
    createSeen_.clear();
    createAsked_.clear();
    lastLocalIds_.clear();
    baseSquadNames_.clear();
    baseOrder_.clear();
    baseCharNames_.clear();
    jobListSent_.clear();
    hostJobLists_.clear();
    jobListDirty_.clear();
    haveJobState_ = false;
}

// Host: nobody's characters. Squad members still the host's by default (never given to a player, nor
// explicitly to the host), except the host's own avatar: the one named like the host, else the first
// of them. A character given to the host with the console is the host's.
void Session::ComputeShared() {
    shared_.clear();
    std::vector<Handle> order;
    world_.PlayerCharacters(order);
    bool hostHasOwn = false;
    std::vector<uint32_t> candidates;
    uint32_t avatar = 0;
    for (const Handle& h : order) {
        auto b = byHandle_.find(h);
        if (b == byHandle_.end()) continue;
        const Entity& e = entities_[b->second];
        if (!e.squad || e.container || e.owner != hostId_) continue;
        const bool given = std::any_of(owners_.begin(), owners_.end(), [&](const auto& o) { return o.first == h; });
        if (given) { hostHasOwn = true; continue; }
        candidates.push_back(e.netId);
        std::string n;
        if (!avatar && world_.ReadCharacterName(h, n) && !n.empty() && Lower(n) == Lower(cfg_.name)) avatar = e.netId;
    }
    if (!avatar && !hostHasOwn && !candidates.empty()) avatar = candidates.front();
    for (uint32_t id : candidates)
        if (id != avatar) shared_.insert(id);
}

bool Session::SquadEditable(uint8_t player, const SquadEntry& s) const {
    if (s.members.empty()) return true;
    for (uint32_t id : s.members)
        if (CheckActor(player, id) == ActorVerdict::Ok) return true;
    return false;
}

void Session::SendSquadState(double now) {
    if (now < nextSquadState_) return;
    nextSquadState_ = now + kSquadStateInterval;
    ComputeShared();
    std::vector<IWorld::SquadView> views;
    world_.ReadSquadViews(views);
    SquadStateMsg m;
    for (const auto& v : views) {
        if (!v.id.valid() || m.squads.size() >= kMaxSquads) continue;
        SquadEntry s;
        s.id = v.id;
        s.name = v.name.substr(0, kMaxSquadName * 2);
        for (const auto& h : v.members)
            if (auto it = byHandle_.find(h); it != byHandle_.end()) s.members.push_back(it->second);
        m.squads.push_back(std::move(s));
    }
    std::vector<uint32_t> ids;
    for (const auto& [id, e] : entities_)
        if (e.squad && !e.container) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (uint32_t id : ids) {
        if (m.members.size() >= kMaxSquadMembers) break;
        SquadMemberInfo info;
        info.netId = id;
        world_.ReadCharacterName(entities_[id].handle, info.name);
        info.name = info.name.substr(0, kMaxSquadName * 2);
        if (shared_.count(id)) info.flags |= kMemberShared;
        m.members.push_back(std::move(info));
    }
    if (m.squads.empty() && m.members.empty()) return;
    const bool changed = !haveSquadState_ || !SameSquadState(m, squadState_);
    if (!changed && now < squadStateFullAt_) return;
    m.rev = changed ? squadState_.rev + 1 : squadState_.rev;
    squadStateFullAt_ = now + kSquadStateFull;
    squadState_ = std::move(m);
    haveSquadState_ = true;
    Writer w(4096);
    Encode(w, squadState_);
    BroadcastReliable(w, true);
}

void Session::HostSquadRequest(uint8_t from, const SquadRequest& r) {
    const std::string who = players_.count(from) ? players_[from].name : "player " + std::to_string(from);
    if (!AdmitActor(from, r.actor, ToString(r.op), Msg::SquadRequest, r.seq)) return;
    const Handle actor = entities_[r.actor].handle;
    std::vector<IWorld::SquadView> views;
    world_.ReadSquadViews(views);
    auto find = [&](const Handle& id) -> const IWorld::SquadView* {
        for (const auto& v : views) if (v.id == id) return &v;
        return nullptr;
    };
    auto entryOf = [&](const IWorld::SquadView& v) {
        SquadEntry s;
        s.id = v.id;
        for (const auto& h : v.members)
            if (auto it = byHandle_.find(h); it != byHandle_.end()) s.members.push_back(it->second);
        return s;
    };
    auto refuse = [&](ResultReason why, const std::string& rule, const std::string& detail, const std::string& french) {
        Refuse(from, Msg::SquadRequest, r.seq, r.actor, why, rule, std::string(ToString(r.op)) + ": " + detail, french);
    };
    const IWorld::SquadView* target = r.squad.valid() ? find(r.squad) : nullptr;
    if (r.squad.valid() && !target) {
        refuse(ResultReason::WrongTarget, "a squad of the player faction", "unknown squad", "Action refusée : cette escouade n'existe plus.");
        return;
    }
    const std::string name = CleanSquadName(r.name);
    bool ok = false;
    std::string what;
    switch (r.op) {
    case SquadOp::Move: {
        int index = std::max(0, r.index);
        bool swap = false;
        if (target) {
            const auto& mem = target->members;
            index = std::min<int>(index, int(mem.size()));
            const auto cur = std::find(mem.begin(), mem.end(), actor);
            // within its squad, onto a member of the same player: the squad window's swap; anything else
            // inserts the character there (the others keep their order: nobody else's character is moved
            // in its place)
            if (cur != mem.end() && index < int(mem.size()) && mem[size_t(index)] != actor) {
                auto o = byHandle_.find(mem[size_t(index)]);
                swap = o != byHandle_.end() && CheckActor(from, o->second) == ActorVerdict::Ok;
            }
        }
        Handle created;
        ok = world_.SquadMove(actor, r.squad, index, swap, name, created);
        what = std::string(target ? "to squad '" + target->name + "'" : "to a new squad") + " at " + std::to_string(index) + (swap ? " (swap)" : "");
        break;
    }
    case SquadOp::Create: {
        if (views.size() >= kMaxSquads) {
            refuse(ResultReason::NotAllowed, "squad limit", "too many squads", "Action refusée : trop d'escouades.");
            return;
        }
        Handle created;
        ok = world_.SquadCreate(name.empty() ? std::string("Escouade") : name, created);
        what = "'" + name + "'";
        break;
    }
    case SquadOp::Rename: case SquadOp::Order: case SquadOp::Remove: {
        if (!SquadEditable(from, entryOf(*target))) {
            refuse(ResultReason::NotYourCharacter, "own squad", "squad '" + target->name + "' holds none of that player's characters", kSquadNotYours);
            return;
        }
        if (r.op == SquadOp::Rename) {
            ok = world_.SquadRename(r.squad, name);
            what = "'" + target->name + "' -> '" + name + "'";
        } else if (r.op == SquadOp::Order) {
            const int index = std::min<int>(std::max(0, r.index), int(views.size()) - 1);
            ok = world_.SquadOrder(r.squad, index);
            what = "'" + target->name + "' to place " + std::to_string(index);
        } else {
            if (!target->members.empty() || views.size() <= 1) {
                refuse(ResultReason::NotAllowed, "empty squad, not the last one", "squad '" + target->name + "' is not empty or the last one",
                       "Action refusée : seule une escouade vide (et pas la dernière) peut être retirée.");
                return;
            }
            ok = world_.SquadRemove(r.squad);
            what = "'" + target->name + "'";
        }
        break;
    }
    case SquadOp::RenameCharacter:
        ok = !name.empty() && world_.RenameCharacter(actor, name);
        what = "-> '" + name + "'";
        break;
    }
    log_("[" + who + "] squad request: " + ToString(r.op) + " " + what + (ok ? " -> ok" : " -> FAILED"));
    Result res;
    res.request = Msg::SquadRequest;
    res.seq = r.seq;
    res.netId = r.actor;
    res.state = ok ? ResultState::Done : ResultState::Rejected;
    res.reason = ok ? ResultReason::None : ResultReason::Failed;
    if (!ok) res.text = "Le changement d'escouade n'a pas pu être fait chez l'hôte.";
    SendResult(from, res);
    nextSquadState_ = 0;   // everyone sees the result at once
}

// ---- client
bool Session::RequestSquadChange(const SquadRequest& in) {
    if (state_ != SessionState::Connected) return false;
    if (!ClientMaySend(Msg::SquadRequest, in.actor, ToString(in.op))) return false;
    SquadRequest r = in;
    r.seq = ++squadSeq_;
    r.name = CleanSquadName(r.name);
    Writer w;
    Encode(w, r);
    SendReliable(net_.serverPeer(), w);
    SquadPending p;
    p.req = r;
    p.until = clock_() + kPendingLife;
    squadPending_[r.seq] = p;
    log_(std::string("squad request asked of the host: ") + ToString(r.op) + (r.name.empty() ? "" : " '" + r.name + "'"));
    return true;
}

void Session::OnSquadResult(const Result& m) {
    auto it = squadPending_.find(m.seq);
    if (it == squadPending_.end()) return;
    if (m.state == ResultState::Rejected) {
        if (it->second.req.op == SquadOp::Create && it->second.localKey) world_.RemoveLocalSquad(it->second.localKey);
        squadPending_.erase(it);
        nextClientSquads_ = 0;   // the host's state again, at once
        return;
    }
    it->second.until = std::min(it->second.until, clock_() + kDoneLinger);
}

void Session::ClientSquadStatePacket(Reader& r) {
    SquadStateMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    squadState_ = std::move(m);
    haveSquadState_ = squadStateDirty_ = true;
    std::unordered_set<uint32_t> shared;
    for (const auto& c : squadState_.members)
        if (c.flags & kMemberShared) shared.insert(c.netId);
    shared_ = std::move(shared);
    std::vector<Handle> hs;
    for (uint32_t id : shared_)
        if (auto e = entities_.find(id); e != entities_.end()) hs.push_back(e->second.handle);
    world_.SetShared(hs);
}

void Session::ClientSquads(double now) {
    // portraits dropped in our squad window: asked of the host (nothing moves here before it answers)
    world_.TakeLocalSquadRequests(scratchSquadReqs_);
    for (const auto& lr : scratchSquadReqs_) {
        Entity* e = entityByHandle(lr.actor);
        if (!e) { log_("squad change not sent: the host does not know that character"); continue; }
        SquadRequest r;
        r.actor = e->netId;
        r.op = lr.op;
        r.squad = lr.squad;
        r.index = lr.index;
        r.name = lr.name;
        RequestSquadChange(r);
    }
    for (auto it = squadPending_.begin(); it != squadPending_.end();) it = now > it->second.until ? squadPending_.erase(it) : std::next(it);
    if (!haveSquadState_ || state_ != SessionState::Connected) return;
    if (now < nextClientSquads_ && !squadStateDirty_) return;
    nextClientSquads_ = now + kClientSquadsInterval;
    squadStateDirty_ = false;

    uint32_t ownActor = 0;
    for (const auto& [id, e] : entities_)
        if (e.present && CheckActor(localId_, id) == ActorVerdict::Ok && (!ownActor || id < ownActor)) ownActor = id;
    auto ask = [&](SquadOp op, uint32_t actor, const Handle& squad, int index, const std::string& name, uint64_t localKey = 0) {
        SquadRequest r;
        r.actor = actor;
        r.op = op;
        r.squad = squad;
        r.index = index;
        r.name = name;
        if (!RequestSquadChange(r)) return;
        if (localKey) squadPending_[squadSeq_].localKey = localKey;
    };

    // the host's squads, with what we asked and it has not answered yet laid over them
    std::vector<SquadEntry> host = squadState_.squads;
    std::map<uint32_t, std::string> names;
    for (const auto& c : squadState_.members) names[c.netId] = c.name;
    auto findHost = [&](const Handle& id) -> SquadEntry* {
        for (auto& s : host) if (s.id == id) return &s;
        return nullptr;
    };
    for (const auto& [seq, p] : squadPending_) {
        const SquadRequest& q = p.req;
        if (q.op == SquadOp::Rename) { if (SquadEntry* s = findHost(q.squad)) s->name = q.name; }
        else if (q.op == SquadOp::RenameCharacter) names[q.actor] = q.name;
        else if (q.op == SquadOp::Remove) host.erase(std::remove_if(host.begin(), host.end(), [&](const SquadEntry& s) { return s.id == q.squad; }), host.end());
        else if (q.op == SquadOp::Order) {
            auto it = std::find_if(host.begin(), host.end(), [&](const SquadEntry& s) { return s.id == q.squad; });
            if (it != host.end()) {
                SquadEntry s = *it;
                host.erase(it);
                host.insert(host.begin() + std::min<ptrdiff_t>(q.index, ptrdiff_t(host.size())), std::move(s));
            }
        }
    }

    std::vector<IWorld::SquadView> local;
    world_.ReadLocalSquads(local);
    // 1. squad names typed here
    for (const auto& lv : local) {
        if (!lv.id.valid()) continue;
        SquadEntry* hs = findHost(lv.id);
        if (!hs) continue;
        const std::pair<int, std::string> key{0, Key(lv.id)};
        // changed here since we last made it the host's (else: the host's name goes on it, a rename the
        // host made included)
        const auto base = baseSquadNames_.find(Key(lv.id));
        if (!nameEdits_.count(key) && (base == baseSquadNames_.end() || base->second == lv.name)) continue;
        if (lv.name == hs->name) { nameEdits_.erase(key); continue; }
        if (!ownActor || !SquadEditable(localId_, *hs)) {
            nameEdits_.erase(key);
            log_("squad name changed here on a squad without our characters: the host's name is put back");
            continue;
        }
        NameEdit& ed = nameEdits_[key];
        if (ed.name != lv.name) ed = {lv.name, now};
        hs->name = lv.name;   // keep what is being typed
        if (now - ed.since >= kNameSettle) {
            const std::string clean = CleanSquadName(lv.name);
            if (!clean.empty()) { ask(SquadOp::Rename, ownActor, lv.id, 0, clean); hs->name = clean; }
            else
                for (const auto& o : squadState_.squads) if (o.id == lv.id) hs->name = o.name;   // nothing left: the host's name back
            nameEdits_.erase(key);
        }
    }
    // 2. squads removed here (the squad window's cross only removes an empty one)
    for (const auto& [key, id] : lastLocalIds_) {
        if (std::any_of(local.begin(), local.end(), [&](const IWorld::SquadView& v) { return v.key == key; })) continue;
        SquadEntry* hs = findHost(id);
        if (!hs || !hs->members.empty() || !ownActor) continue;
        ask(SquadOp::Remove, ownActor, id, 0, {});
        host.erase(std::remove_if(host.begin(), host.end(), [&](const SquadEntry& s) { return s.id == id; }), host.end());
    }
    // 3. squads created here: one of ours matched to none of the host's, still empty after a moment
    //    (a portrait dropped on it makes the host create it with that character instead)
    for (auto it = createSeen_.begin(); it != createSeen_.end();) {
        const bool still = std::any_of(local.begin(), local.end(), [&](const IWorld::SquadView& v) { return v.key == it->first && !v.id.valid(); });
        if (!still) { createAsked_.erase(it->first); it = createSeen_.erase(it); } else ++it;
    }
    for (const auto& lv : local) {
        if (lv.id.valid() || !lv.members.empty() || lv.awaitingHost || !lv.key) continue;
        const double seen = createSeen_.emplace(lv.key, now).first->second;
        if (now - seen < kCreateSettle || createAsked_.count(lv.key) || !ownActor) continue;
        createAsked_.insert(lv.key);
        ask(SquadOp::Create, ownActor, Handle{}, 0, CleanSquadName(lv.name), lv.key);
    }
    // 4. the order of the squads (a squad dragged in the squad window)
    {
        std::vector<Handle> mine, theirs;
        for (const auto& lv : local)
            if (lv.id.valid() && findHost(lv.id)) mine.push_back(lv.id);
        // changed here since we last put them in the host's order (else the host's order goes on them)
        for (const auto& id : baseOrder_)
            if (std::find(mine.begin(), mine.end(), id) != mine.end()) theirs.push_back(id);
        if (mine != theirs && mine.size() == theirs.size()) {
            for (size_t i = 0; i < theirs.size(); ++i) {
                auto without = [&](std::vector<Handle> v) { v.erase(std::find(v.begin(), v.end(), theirs[i])); return v; };
                if (without(mine) != without(theirs)) continue;
                const Handle moved = theirs[i];
                const size_t at = size_t(std::find(mine.begin(), mine.end(), moved) - mine.begin());
                // its place in the host's full list: before the squad that follows it here
                std::vector<SquadEntry> rest;
                SquadEntry m;
                for (auto& s : host) { if (s.id == moved) m = s; else rest.push_back(s); }
                size_t index = rest.size();
                if (at + 1 < mine.size())
                    for (size_t k = 0; k < rest.size(); ++k) if (rest[k].id == mine[at + 1]) { index = k; break; }
                if (ownActor && SquadEditable(localId_, m)) {
                    ask(SquadOp::Order, ownActor, moved, int(index), {});
                    rest.insert(rest.begin() + ptrdiff_t(index), m);
                    host = std::move(rest);
                } else {
                    log_("squad order changed here for a squad without our characters: the host's order is put back");
                }
                break;
            }
        }
    }
    // 5. character names typed here (the character's window): only one's own goes to the host
    for (const auto& c : squadState_.members) {
        auto e = entities_.find(c.netId);
        if (e == entities_.end() || !e->second.present) continue;
        std::string here;
        if (!world_.ReadCharacterName(e->second.handle, here) || here.empty()) continue;
        const std::pair<int, std::string> key{1, std::to_string(c.netId)};
        std::string& want = names[c.netId];
        const auto base = baseCharNames_.find(c.netId);
        if (!nameEdits_.count(key) && (base == baseCharNames_.end() || base->second == here)) continue;
        if (here == want) { nameEdits_.erase(key); continue; }
        if (CheckActor(localId_, c.netId) != ActorVerdict::Ok) { nameEdits_.erase(key); continue; }   // put back below
        NameEdit& ed = nameEdits_[key];
        if (ed.name != here) ed = {here, now};
        want = here;
        if (now - ed.since >= kNameSettle) {
            const std::string clean = CleanSquadName(here);
            if (!clean.empty()) { ask(SquadOp::RenameCharacter, c.netId, Handle{}, 0, clean); want = clean; }
            nameEdits_.erase(key);
        }
    }
    // 6. impose the result
    std::vector<IWorld::SquadView> views;
    for (const auto& s : host) {
        IWorld::SquadView v;
        v.id = s.id;
        v.name = s.name;
        for (uint32_t id : s.members)
            if (auto it = entities_.find(id); it != entities_.end() && it->second.present) v.members.push_back(it->second.handle);
        views.push_back(std::move(v));
    }
    world_.ApplySquadViews(views);
    for (const auto& [id, name] : names) {
        auto e = entities_.find(id);
        if (e == entities_.end() || !e->second.present || name.empty()) continue;
        std::string here;
        if (world_.ReadCharacterName(e->second.handle, here) && here != name) world_.ApplyCharacterName(e->second.handle, name);
    }
    // what ours look like now: a later difference is the player's doing
    lastLocalIds_.clear();
    baseSquadNames_.clear();
    baseOrder_.clear();
    world_.ReadLocalSquads(local);
    for (const auto& lv : local) {
        if (!lv.id.valid() || !lv.key) continue;
        lastLocalIds_[lv.key] = lv.id;
        baseSquadNames_[Key(lv.id)] = lv.name;
        baseOrder_.push_back(lv.id);
    }
    baseCharNames_.clear();
    for (const auto& c : squadState_.members) {
        auto e = entities_.find(c.netId);
        std::string here;
        if (e != entities_.end() && e->second.present && world_.ReadCharacterName(e->second.handle, here)) baseCharNames_[c.netId] = here;
    }
}

// ---- jobs with their targets (with fix G5's JobList, which old clients still read)
void Session::ClientJobStatePacket(Reader& r) {
    JobStateMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    if (!haveJobState_) { hostJobs_.clear(); jobsDirty_.clear(); }   // the old kind-only lists stop here
    haveJobState_ = true;
    for (auto& c : m.chars) {
        jobListDirty_.insert(c.netId);
        hostJobLists_[c.netId] = std::move(c.jobs);
    }
}

} // namespace kc
