// Workshop: research, crafting benches, machines (operators, power, production) and town power.
//
// Host: its game alone researches, crafts, produces and computes power. Every second it reads the
// player faction's research (finished techs, blueprints read, queue, progress) and the player
// machines near the players (operators, power, battery, progress bar, production amount, crafting
// orders, repeat), plus the power panel of their towns, and sends what changed (Research,
// Machines); everything again every 15 s (research) / 5 s (machines), and in full to a player who
// just arrived. A machine's inventory becomes an entity synced to every player while it is tracked
// (input and output inventories identical everywhere, opened or not).
// Requests (ResearchRequest, MachineRequest) name the asking player's character: the central check
// (Authorize, kMessageRules: OwnCharacter) admits them, they run one after the other on the host's
// game (two players at once: the second sees the first's result; same bench: last writer wins) and
// the player gets a Result (done, or refused with the reason in French).
// Client: imposes the host's research and machines; its own game never researches, pays, crafts,
// learns a blueprint or computes a town's power (plugin/workshop.cpp hooks). The research and
// crafting windows' buttons and the building panel's power switches become requests.
#include "kc/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace kc {

namespace {
constexpr double kResearchInterval = 1.0, kResearchFull = 15.0, kResearchReapply = 3.0;
constexpr double kMachinesInterval = 1.0, kMachinesFull = 5.0, kMachinesReapply = 2.0;
constexpr float kMachineRadius = 1500.0f;   // an outpost around the players' characters
constexpr float kMachineReach = 2500.0f;    // a request's character must be at that machine's base
constexpr size_t kMachineApplyBudget = 48;
constexpr size_t kMaxPending = 32;

float Dist(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

std::vector<std::string> QueueSids(const ResearchState& s) {
    std::vector<std::string> out;
    for (const auto& q : s.queue) out.push_back(q.sid);
    return out;
}

const char* ResearchWord(ResearchAction a) {
    switch (a) {
    case ResearchAction::Queue: return "queue";
    case ResearchAction::Cancel: return "cancel";
    case ResearchAction::LearnBlueprint: return "learn blueprint";
    }
    return "?";
}

const char* MachineWord(MachineAction a) {
    switch (a) {
    case MachineAction::AddCraft: return "add a crafting order";
    case MachineAction::RemoveCraft: return "remove a crafting order";
    case MachineAction::SetRepeat: return "repeat";
    case MachineAction::SetPower: return "power switch";
    case MachineAction::SetBattery: return "battery switch";
    }
    return "?";
}
} // namespace

std::string Session::MachineKey(const std::string& sid, const Vec3& pos) {
    char b[96];
    std::snprintf(b, sizeof(b), "@%d,%d,%d", int(std::lround(pos.x)), int(std::lround(pos.y)), int(std::lround(pos.z)));
    return sid + b;
}

const MachineState* Session::knownMachine(const std::string& key) const {
    if (isHost()) {
        auto it = machinesSent_.find(key);
        return it == machinesSent_.end() ? nullptr : &it->second;
    }
    auto it = clientMachines_.find(key);
    return it == clientMachines_.end() ? nullptr : &it->second.state;
}

void Session::ResetWorkshop() {
    researchSent_ = ResearchState{};
    haveResearchSent_ = false;
    researchServed_.clear();
    machinesServed_.clear();
    nextResearch_ = researchFullAt_ = 0;
    pendingResearchReqs_.clear();
    pendingMachineReqs_.clear();
    machinesSent_.clear();
    machineNetIds_.clear();
    townsSent_.clear();
    nextMachines_ = machinesFullAt_ = 0;
    hostResearch_ = ResearchState{};
    haveResearch_ = researchDirty_ = false;
    researchReapplyAt_ = 0;
    clientMachines_.clear();
    clientTowns_.clear();
    machinesReapplyAt_ = 0;
    workshop_ = WorkshopView{};
}

// ---------------------------------------------------------------- host

void Session::HostResearchPacket(uint8_t from, Reader& r) {
    ResearchRequest m;
    auto pl = players_.find(from);
    if (pl == players_.end() || !pl->second.inGame || !Decode(r, m)) return;
    if (pendingResearchReqs_.size() < kMaxPending) pendingResearchReqs_.emplace_back(from, std::move(m));
}

void Session::HostMachinePacket(uint8_t from, Reader& r) {
    MachineRequest m;
    auto pl = players_.find(from);
    if (pl == players_.end() || !pl->second.inGame || !Decode(r, m)) return;
    if (pendingMachineReqs_.size() < kMaxPending) pendingMachineReqs_.emplace_back(from, std::move(m));
}

// One after the other, in the order they came: two players acting at once each see the other's
// result (a tech already queued is refused, the same bench ends with the last order).
void Session::HostWorkshopRequests() {
    auto answer = [&](uint8_t player, Msg type, uint32_t seq, uint32_t actor, bool ok, const std::string& why) {
        Result res;
        res.request = type;
        res.seq = seq;
        res.netId = actor;
        res.state = ok ? ResultState::Done : ResultState::Rejected;
        res.reason = ok ? ResultReason::None : ResultReason::Failed;
        res.text = ok ? std::string() : why;
        SendResult(player, res);
        ++(ok ? workshop_.requestsDone : workshop_.requestsRefused);
    };
    for (const auto& [from, req] : pendingResearchReqs_) {
        if (!AdmitActor(from, req.actorNetId, "research request", Msg::ResearchRequest, req.seq)) continue;
        auto pl = players_.find(from);
        const std::string who = pl != players_.end() ? pl->second.name : "player " + std::to_string(from);
        std::string why;
        const bool ok = world_.ExecuteResearchRequest(req, entities_[req.actorNetId].handle, why);
        log_("[" + who + "] research: " + ResearchWord(req.action) + " " + world_.TemplateName(req.sid) + (ok ? " -> done" : " -> refused: " + why));
        answer(from, Msg::ResearchRequest, req.seq, req.actorNetId, ok, why.empty() ? "Recherche refusée par l'hôte." : why);
        nextResearch_ = 0;    // everyone sees the result now
        nextInventory_ = 0;   // artifacts or a blueprint consumed
    }
    pendingResearchReqs_.clear();
    for (const auto& [from, req] : pendingMachineReqs_) {
        if (!AdmitActor(from, req.actorNetId, "machine request", Msg::MachineRequest, req.seq)) continue;
        auto pl = players_.find(from);
        const std::string who = pl != players_.end() ? pl->second.name : "player " + std::to_string(from);
        const Handle actor = entities_[req.actorNetId].handle;
        std::string why;
        bool ok = false;
        if (world_.DistanceTo(actor, req.pos) > kMachineReach) why = "Trop loin de cette machine : un de tes personnages doit être sur place.";
        else ok = world_.ExecuteMachineRequest(req, actor, why);
        log_("[" + who + "] machine: " + MachineWord(req.action) + " on " + world_.TemplateName(req.sid) +
             (req.action == MachineAction::AddCraft ? " (" + world_.TemplateName(req.baseSid) + ")" : std::string()) + (ok ? " -> done" : " -> refused: " + why));
        answer(from, Msg::MachineRequest, req.seq, req.actorNetId, ok, why.empty() ? "Ordre refusé par l'hôte." : why);
        nextMachines_ = 0;
    }
    pendingMachineReqs_.clear();
}

void Session::HostResearch(double now) {
    if (now < nextResearch_) return;
    nextResearch_ = now + kResearchInterval;
    std::vector<PeerId> fresh;
    std::set<PeerId> live;
    for (auto& [id, p] : players_) {
        if (!p.inGame) continue;
        live.insert(p.peer);
        if (!researchServed_.count(p.peer)) fresh.push_back(p.peer);
    }
    for (auto it = researchServed_.begin(); it != researchServed_.end();) it = live.count(*it) ? std::next(it) : researchServed_.erase(it);
    if (live.empty()) return;
    ResearchState s;
    if (!world_.ReadResearch(s)) return;
    const bool changed = !haveResearchSent_ || !(s == researchSent_);
    const bool full = now >= researchFullAt_;
    if (full) researchFullAt_ = now + kResearchFull;
    if (haveResearchSent_) {   // the news, in the log
        std::unordered_set<std::string> before(researchSent_.finished.begin(), researchSent_.finished.end());
        int n = 0;
        for (const auto& f : s.finished)
            if (!before.count(f) && n++ < 8) log_("research: " + world_.TemplateName(f) + " is now known");
        if (QueueSids(s) != QueueSids(researchSent_)) {
            std::string q;
            for (const auto& e : s.queue) q += (q.empty() ? "" : ", ") + world_.TemplateName(e.sid);
            log_("research queue: " + (q.empty() ? std::string("empty") : q));
        }
    }
    Writer w(4096);
    Encode(w, s);
    if (changed || full) {
        BroadcastReliable(w, true);
        ++workshop_.researchSent;
    } else {
        for (PeerId p : fresh) SendReliable(p, w);
    }
    for (PeerId p : fresh) researchServed_.insert(p);
    researchSent_ = std::move(s);
    haveResearchSent_ = true;
}

void Session::HostMachines(double now) {
    if (now < nextMachines_) return;
    nextMachines_ = now + kMachinesInterval;
    std::vector<PeerId> fresh;
    std::set<PeerId> live;
    for (auto& [id, p] : players_) {
        if (!p.inGame) continue;
        live.insert(p.peer);
        if (!machinesServed_.count(p.peer)) fresh.push_back(p.peer);
    }
    for (auto it = machinesServed_.begin(); it != machinesServed_.end();) it = live.count(*it) ? std::next(it) : machinesServed_.erase(it);
    if (live.empty()) return;
    std::vector<Vec3> centers;
    for (auto& [id, e] : entities_) {
        EntityState st;
        if (e.squad && !e.container && world_.Read(e.handle, st)) centers.push_back(st.pos);
    }
    if (centers.empty()) return;
    world_.ReadMachines(centers, kMachineRadius, scratchMachines_, scratchTowns_);
    const bool full = now >= machinesFullAt_ || !fresh.empty();
    if (now >= machinesFullAt_) machinesFullAt_ = now + kMachinesFull;
    MachinesMsg m;
    m.full = full;
    std::unordered_set<std::string> seen;
    std::vector<uint32_t> newInventories;
    int logged = 0;
    for (auto& wm : scratchMachines_) {
        MachineState s = std::move(wm.state);
        const std::string key = MachineKey(s.sid, s.pos);
        if (!seen.insert(key).second) continue;
        // its inventory: an entity every player keeps in step with the host's
        uint32_t id = 0;
        if (wm.handle.valid()) {
            if (auto b = byHandle_.find(wm.handle); b != byHandle_.end() && entities_.count(b->second) && entities_[b->second].container) {
                id = b->second;
            } else if (b == byHandle_.end()) {
                Entity e;
                e.netId = nextNetId_++;
                e.handle = wm.handle;
                e.container = true;
                e.containerPos = s.pos;
                e.keep = true;
                id = e.netId;
                byHandle_[wm.handle] = id;
                entities_[id] = std::move(e);
                newInventories.push_back(id);
            }
            if (id) {
                entities_[id].machine = true;
                machineNetIds_[key] = id;
            }
        }
        s.netId = id;
        s.operators.clear();
        for (const Handle& h : wm.operators) {
            auto b = byHandle_.find(h);
            if (b == byHandle_.end()) continue;
            auto e = entities_.find(b->second);
            if (e != entities_.end() && !e->second.container && s.operators.size() < kMaxMachineOperators) s.operators.push_back(b->second);
        }
        auto prev = machinesSent_.find(key);
        const bool changed = prev == machinesSent_.end() || !prev->second.sameState(s);
        if (changed && prev != machinesSent_.end() && logged < 8) {
            if (prev->second.operatorCount != s.operatorCount) {
                ++logged;
                log_("machine: " + world_.TemplateName(s.sid) + " workers " + std::to_string(prev->second.operatorCount) + " -> " +
                     std::to_string(s.operatorCount) + "/" + std::to_string(s.maxOperators));
            }
            if (prev->second.crafts.size() != s.crafts.size()) {
                ++logged;
                log_("machine: " + world_.TemplateName(s.sid) + " crafting orders " + std::to_string(prev->second.crafts.size()) + " -> " +
                     std::to_string(s.crafts.size()));
            }
            if ((prev->second.flags ^ s.flags) & kMachPowerOn) {
                ++logged;
                log_("machine: " + world_.TemplateName(s.sid) + ((s.flags & kMachPowerOn) ? " switched on" : " switched off"));
            }
        }
        if (changed || full) m.machines.push_back(s);
        if (machinesSent_.size() < 4096) machinesSent_[key] = std::move(s);
    }
    // machines no longer near anyone: forgotten; their inventory entity goes unless a player has it open
    for (auto it = machineNetIds_.begin(); it != machineNetIds_.end();) {
        if (seen.count(it->first)) { ++it; continue; }
        if (auto e = entities_.find(it->second); e != entities_.end() && e->second.machine) {
            e->second.machine = false;
            if (e->second.openBy.empty()) {
                byHandle_.erase(e->second.handle);
                for (auto& [pid, sy] : sync_) sy.sent.erase(e->first);
                entities_.erase(e);
            }
        }
        machinesSent_.erase(it->first);
        it = machineNetIds_.erase(it);
    }
    const bool townsChanged = !(scratchTowns_ == townsSent_);
    if (townsChanged || full) m.towns = scratchTowns_;
    townsSent_ = scratchTowns_;
    if (!m.machines.empty() || !m.towns.empty()) {
        for (size_t i = 0; i < std::max<size_t>(m.machines.size(), 1); i += kMaxMachinesPerMsg) {
            MachinesMsg part;
            part.full = m.full;
            if (!m.machines.empty())
                part.machines.assign(m.machines.begin() + ptrdiff_t(i), m.machines.begin() + ptrdiff_t(std::min(m.machines.size(), i + kMaxMachinesPerMsg)));
            if (i == 0) part.towns = m.towns;
            Writer w(8192);
            Encode(w, part);
            BroadcastReliable(w, true);
        }
        ++workshop_.machinesSent;
    }
    // the inventories of new machines (and every machine's, for a player who just arrived) follow
    for (uint32_t id : newInventories) entities_[id].invHash = 0;
    if (!fresh.empty())
        for (auto& [key, id] : machineNetIds_)
            if (auto e = entities_.find(id); e != entities_.end()) e->second.invHash = 0;
    for (PeerId p : fresh) machinesServed_.insert(p);
    if (!newInventories.empty() || !fresh.empty()) nextInventory_ = 0;
}

// ---------------------------------------------------------------- client

void Session::ClientResearchPacket(Reader& r) {
    ResearchState s;
    if (state_ != SessionState::Connected || !Decode(r, s)) return;
    ++workshop_.researchReceived;
    if (haveResearch_) {   // news for the player
        std::unordered_set<std::string> before(hostResearch_.finished.begin(), hostResearch_.finished.end());
        int n = 0;
        for (const auto& f : s.finished)
            if (!before.count(f) && n++ < 4) AddChat("* Recherche terminée : " + world_.TemplateName(f), "research finished on the host: " + f);
    }
    if (!haveResearch_ || !(s == hostResearch_)) researchDirty_ = true;
    hostResearch_ = std::move(s);
    haveResearch_ = true;
}

void Session::ClientMachinesPacket(Reader& r) {
    MachinesMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    ++workshop_.machinesReceived;
    for (auto& s : m.machines) {
        const std::string key = MachineKey(s.sid, s.pos);
        // the machine's inventory: our copy of it, kept in step with the host's
        if (s.netId && !entities_.count(s.netId)) {
            Handle local;
            if (world_.FindContainer(s.sid, s.pos, local)) {
                Entity e;
                e.netId = s.netId;
                e.handle = local;
                e.container = true;
                e.machine = true;
                e.present = true;
                e.checked = true;
                e.containerPos = s.pos;
                entities_[s.netId] = std::move(e);
            }
        } else if (s.netId) {
            entities_[s.netId].machine = true;
        }
        if (clientMachines_.size() > 4096) clientMachines_.clear();
        ClientMachine& c = clientMachines_[key];
        if (!c.found || !c.state.sameState(s)) c.dirty = true;
        c.state = std::move(s);
    }
    for (auto& t : m.towns) {
        const std::string key = MachineKey(t.sid, t.pos);
        if (clientTowns_.size() > 256) clientTowns_.clear();
        clientTowns_[key] = t;
        world_.ApplyTownPower(t);
    }
}

uint32_t Session::OwnActorNear(const Vec3* pos) {
    uint32_t best = 0;
    float bestD = 1e30f;
    for (auto& [id, e] : entities_) {
        if (!e.squad || e.container || e.owner != localId_) continue;
        float d = 0;
        if (pos) {
            EntityState st;
            d = world_.Read(e.handle, st) ? Dist(st.pos, *pos) : 1e29f;
        }
        if (!best || d < bestD) { best = id; bestD = d; }
    }
    return best;
}

void Session::ClientWorkshop(double now) {
    // what the player asked in the research, crafting and building windows: the host decides
    world_.TakeWorkshopAsks(scratchResearchAsks_, scratchMachineAsks_);
    for (auto& a : scratchResearchAsks_) {
        Entity* e = a.actor.valid() ? entityByHandle(a.actor) : nullptr;
        a.req.actorNetId = e ? e->netId : OwnActorNear(nullptr);
        if (!a.req.actorNetId || !ClientMaySend(Msg::ResearchRequest, a.req.actorNetId, "research request")) continue;
        a.req.seq = ++workshopSeq_;
        Writer w;
        Encode(w, a.req);
        SendReliable(net_.serverPeer(), w);
        ++workshop_.asked;
        log_(std::string("research: ") + ResearchWord(a.req.action) + " " + world_.TemplateName(a.req.sid) + " asked of the host");
    }
    scratchResearchAsks_.clear();
    for (auto& a : scratchMachineAsks_) {
        Entity* e = a.actor.valid() ? entityByHandle(a.actor) : nullptr;
        a.req.actorNetId = e && e->owner == localId_ ? e->netId : OwnActorNear(&a.req.pos);
        if (!a.req.actorNetId || !ClientMaySend(Msg::MachineRequest, a.req.actorNetId, "machine request")) continue;
        a.req.seq = ++workshopSeq_;
        Writer w;
        Encode(w, a.req);
        SendReliable(net_.serverPeer(), w);
        ++workshop_.asked;
        log_(std::string("machine: ") + MachineWord(a.req.action) + " on " + world_.TemplateName(a.req.sid) + " asked of the host");
    }
    scratchMachineAsks_.clear();
    // the host's research, imposed (again now and then: a window refreshed by the local game)
    if (haveResearch_ && (researchDirty_ || now >= researchReapplyAt_)) {
        researchReapplyAt_ = now + kResearchReapply;
        const bool wasDirty = researchDirty_;
        researchDirty_ = false;
        const size_t n = world_.ApplyResearch(hostResearch_);
        workshop_.researchApplied += n;
        if (n && !wasDirty) log_("research: " + std::to_string(n) + " local change(s) undone (the host's research is imposed)");
    }
    // machines and town power: dirty ones now, every one again every 2 s
    if (now >= machinesReapplyAt_) {
        machinesReapplyAt_ = now + kMachinesReapply;
        for (auto& [key, c] : clientMachines_) c.dirty = true;
        for (auto& [key, t] : clientTowns_) world_.ApplyTownPower(t);
    }
    size_t budget = kMachineApplyBudget;
    std::vector<Handle> ops;
    for (auto& [key, c] : clientMachines_) {
        if (!c.dirty) continue;
        if (budget == 0) break;
        --budget;
        ops.clear();
        for (uint32_t id : c.state.operators)
            if (auto it = entities_.find(id); it != entities_.end() && !it->second.container && it->second.present) ops.push_back(it->second.handle);
        c.found = world_.ApplyMachine(c.state, ops);
        if (c.found) ++workshop_.machinesApplied;
        c.dirty = false;
    }
}

} // namespace kc
