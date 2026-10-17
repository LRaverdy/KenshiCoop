// 0.3.5, lot 4 (night-items): what the 16 October session showed about items.
//  - point 19: a building's inventory panel (a click on a mine, a farm, a well, a storage) asks the host
//    for its items without anyone walking there (ContainerPeek); a take or a put walks the player's
//    character there first, as the game does, and the moves go once it arrived (HeldMoves).
//  - point 20: an item a player takes out of a chest or a storage is watched a few seconds on the host:
//    the host's game putting it back (a job of that character stores what it carries) is said to the
//    player, in French, instead of looking like a sync bug.
//  - point 18: the farms' inputs (water: the "Eau" bar), imposed from the host's (FarmInputs).
// Rule A for shared inventories (point 17) is in Session::TeamShared / HostMayTouch (session.cpp).
#include <algorithm>
#include <cmath>

#include "kc/session.h"

namespace kc {

namespace {
constexpr double kHeldWalk = 30.0;      // a take or put in a building panel waits this long for our character
constexpr double kTakenWatch = 8.0;     // an item taken out of a storage is watched this long
constexpr size_t kMaxHeldMoves = 8, kMaxTakenWatch = 32;
float Quantize(float v) { return std::isfinite(v) ? std::round(v * 200.0f) / 200.0f : 0.0f; }
}

bool Session::Peeked(const Entity& e, double now) const { return e.peekedAt >= 0 && now - e.peekedAt < kPeekLife; }

// ---------------------------------------------------------------- host

void Session::HostContainerPeeks(double now) {
    for (auto& [from, m] : peekAsks_) {
        auto pl = players_.find(from);
        if (pl == players_.end() || !AdmitActor(from, m.looterNetId, "look at a building's inventory", Msg::ContainerPeek)) continue;
        Handle h;
        if (!world_.FindContainer(m.sid, m.pos, h)) {
            log_("[" + pl->second.name + "] building " + world_.TemplateName(m.sid) + " not found here (its panel shows what their game has)");
            continue;
        }
        if (world_.ContainerLocked(h)) {   // as for a chest: a locked one shows nothing
            log_("[" + pl->second.name + "] building " + world_.TemplateName(m.sid) + " is locked: its items are not sent");
            continue;
        }
        uint32_t id = 0;
        if (auto b = byHandle_.find(h); b != byHandle_.end()) {
            id = b->second;
            auto it = entities_.find(id);
            if (it == entities_.end() || !it->second.container || it->second.bag) continue;   // never a character or a backpack
        } else {
            Entity e;
            e.netId = nextNetId_++;
            e.handle = h;
            e.container = true;
            e.containerPos = m.pos;
            e.keep = true;
            id = e.netId;
            byHandle_[h] = id;
            entities_[id] = std::move(e);
        }
        Entity& e = entities_[id];
        const bool fresh = !e.peekBy.count(from);
        e.peekBy[from] = now + kPeekLife;
        e.invHash = 0;        // its items go out now (to them)
        nextInventory_ = 0;
        ContainerOpened r;
        r.netId = id;
        r.looterNetId = 0;    // a look: nobody walks, no window opens on top of their panel
        if (!world_.ContainerKind(h, r.sid)) r.sid = m.sid;
        r.pos = e.containerPos;
        Writer w;
        Encode(w, r);
        SendReliable(pl->second.peer, w);
        if (fresh) log_("[" + pl->second.name + "] looks at " + world_.TemplateName(r.sid) + " (its panel: nobody walks)");
    }
    peekAsks_.clear();
    // looks that ran out, or whose player left
    for (auto& [id, e] : entities_)
        for (auto it = e.peekBy.begin(); it != e.peekBy.end();)
            it = now > it->second || !players_.count(it->first) ? e.peekBy.erase(it) : std::next(it);
}

int Session::CountKind(uint32_t netId, const ItemState& kind, bool withPartner) {
    auto count = [&](uint32_t id) {
        auto it = entities_.find(id);
        std::vector<ItemState> items;
        if (it == entities_.end() || !world_.ReadInventory(it->second.handle, items)) return 0;
        int n = 0;
        for (const auto& i : items)
            if (i.sameKind(kind)) n += i.quantity;
        return n;
    };
    int n = count(netId);
    if (!withPartner) return n;
    auto it = entities_.find(netId);
    if (it == entities_.end()) return n;
    if (it->second.bag) return n + count(it->second.bagOwner);   // a backpack: its wearer too
    if (auto b = bagOf_.find(netId); b != bagOf_.end()) n += count(b->second);   // a character: its backpack too
    return n;
}

void Session::NoteTaken(uint8_t from, const InvOp& op, uint32_t taker) {
    TakenWatch t;
    t.player = from;
    t.container = op.fromNetId;
    t.taker = taker;
    t.kind = op.item;
    t.qty = std::max(1, op.item.quantity);
    t.containerHad = CountKind(op.fromNetId, op.item, false);
    t.takerHad = CountKind(taker, op.item, true);
    t.until = clock_() + kTakenWatch;
    takenWatch_.erase(std::remove_if(takenWatch_.begin(), takenWatch_.end(),
                                     [&](const TakenWatch& w) { return w.player == from && w.container == t.container && w.kind.sameKind(t.kind); }),
                      takenWatch_.end());
    if (takenWatch_.size() >= kMaxTakenWatch) takenWatch_.erase(takenWatch_.begin());
    takenWatch_.push_back(std::move(t));
}

// 16 October (00:55-00:57): rob1 took 65 copper ore out of Stock Cuivre again and again; the host did each
// move ("client item move done") and the ore was back in the storage a moment later. rob1's characters
// were the copper mine's workers: the game's job puts the ore a worker carries into the storage (as in
// solo). Said to the player now; the move itself was right.
void Session::HostTakenWatch(double now) {
    if (takenWatch_.empty() || now < nextTakenWatch_) return;
    nextTakenWatch_ = now + 0.5;
    for (auto it = takenWatch_.begin(); it != takenWatch_.end();) {
        auto c = entities_.find(it->container);
        auto t = entities_.find(it->taker);
        if (c == entities_.end() || t == entities_.end() || now > it->until) {
            it = takenWatch_.erase(it);
            continue;
        }
        const int inContainer = CountKind(it->container, it->kind, false);
        const int onTaker = CountKind(it->taker, it->kind, true);
        if (inContainer >= it->containerHad + it->qty && onTaker <= it->takerHad - it->qty) {
            std::string sid;
            const std::string where = world_.ContainerKind(c->second.handle, sid) ? world_.TemplateName(sid) : std::string("le contenant");
            const Entity* w = Wearer(t->second);
            const std::string who = w ? world_.CharacterNameOf(w->handle) : std::string("?");
            const std::string item = world_.TemplateName(it->kind.templateSid);
            const std::string name = players_.count(it->player) ? players_[it->player].name : "player " + std::to_string(it->player);
            log_("[" + name + "] " + item + " x" + std::to_string(it->qty) + " taken out of " + where + " went back into it on the host: " + who +
                 " has a job that stores what it carries there (the game's own rule, as in solo), not a sync fault");
            SendNotice(it->player, "Le jeu a remis " + item + " dans " + where + " : " + who +
                                       " a un travail (mine, transport, stockage) qui y range ce qu'il porte. Retire-lui ce travail, ou prends l'objet avec un autre personnage.");
            it = takenWatch_.erase(it);
            continue;
        }
        ++it;
    }
}

// ---------------------------------------------------------------- client

void Session::ClientContainerPeeks(double now) {
    world_.TakeContainerPeeks(scratchPeeks_);
    for (const auto& r : scratchPeeks_) {
        // a building we already follow: a machine (every player keeps its inventory in step with the host's)
        // needs nothing more; another one is asked again (its items may have changed)
        Entity* known = nullptr;
        for (auto& [id, e] : entities_)
            if (r.object.valid() && e.container && !e.bag && e.present && e.handle == r.object) known = &e;
        const std::string key = r.sid + "@" + std::to_string(int(r.pos.x)) + "," + std::to_string(int(r.pos.z));
        const bool again = key == lastPeekKey_ && now - lastPeekAt_ < 1.0;   // the panel's hook fires twice a click
        lastPeekKey_ = key;
        lastPeekAt_ = now;
        if (known) {
            known->peekedAt = now;
            if (known->machine) {
                if (!again) log_("building panel: " + world_.TemplateName(r.sid) + " is a machine the host keeps in step here: nothing to ask, nobody walks");
                continue;
            }
        }
        if (again) continue;
        Entity* l = r.looter.valid() ? entityByHandle(r.looter) : nullptr;
        const uint32_t looter = l && l->squad && !l->container && l->owner == localId_ ? l->netId : OwnActorNear(&r.pos);
        if (!looter) {
            log_("building panel: " + world_.TemplateName(r.sid) + ": no character of ours to name, the host is not asked");
            continue;
        }
        if (!ClientMaySend(Msg::ContainerPeek, looter, "look at a building's inventory")) continue;
        ContainerPeek m;
        m.looterNetId = looter;
        m.sid = r.sid;
        m.pos = r.pos;
        Writer w;
        Encode(w, m);
        SendReliable(net_.serverPeer(), w);
        log_("building panel: " + world_.TemplateName(r.sid) + "'s items asked of the host (nobody walks)");
    }
    scratchPeeks_.clear();
}

void Session::HoldMovesForWalk(uint32_t container, std::vector<InvOp>& ops, const std::vector<uint32_t>& involved, double now) {
    auto c = entities_.find(container);
    if (c == entities_.end()) return;
    // who walks: our character in these moves (the other side of the take or put), else our nearest one
    uint32_t looter = 0;
    for (const auto& op : ops)
        for (uint32_t id : {op.fromNetId, op.toNetId}) {
            auto it = entities_.find(id);
            const Entity* w = it != entities_.end() ? Wearer(it->second) : nullptr;
            if (!looter && w && !w->container && w->squad && w->owner == localId_) looter = w->netId;
        }
    if (!looter) looter = OwnActorNear(&c->second.containerPos);
    std::string sid;
    const bool named = world_.ContainerKind(c->second.handle, sid);
    auto undo = [&](const std::string& logged, const std::string& shown) {
        for (uint32_t id : involved)
            if (auto it = entities_.find(id); it != entities_.end()) {
                it->second.invDirty = true;
                it->second.invRetry = 0;
                it->second.invUnmatchedSince = 0;
            }
        AddChat("* " + shown, logged);
    };
    if (!looter || !named) {
        undo("building panel: a take or put not sent (" + std::string(!looter ? "no character of ours to walk there" : "the building is not here") + ")",
             "Objet non déplacé : aucun de tes personnages ne peut aller jusqu'à ce bâtiment. Rien n'a changé.");
        return;
    }
    if (!ClientMaySend(Msg::ContainerOpen, looter, "walk to a building")) {
        undo("building panel: a take or put not sent (not our character)", "Objet non déplacé : rien n'a changé.");
        return;
    }
    if (heldMoves_.size() >= kMaxHeldMoves) {
        undo("building panel: too many takes and puts waiting", "Objet non déplacé : attends que ton personnage soit arrivé.");
        return;
    }
    ContainerOpen m;
    m.looterNetId = looter;
    m.sid = sid;
    m.pos = c->second.containerPos;
    Writer w;
    Encode(w, m);
    SendReliable(net_.serverPeer(), w);
    HeldMoves h;
    h.container = container;
    h.looter = looter;
    h.ops = std::move(ops);
    h.involved = involved;
    h.until = now + kHeldWalk;
    for (uint32_t id : involved)
        if (auto it = entities_.find(id); it != entities_.end()) {
            it->second.invPendingUntil = h.until + 5.0;   // ours as the player left it, until the host answers
            it->second.invUnmatchedSince = 0;
        }
    log_("building panel: " + std::to_string(h.ops.size()) + " move(s) with " + world_.TemplateName(sid) +
         " wait for our character to get there (it walks, as in the game)");
    AddChat("* Ton personnage va jusqu'à " + world_.TemplateName(sid) + " pour prendre ou poser l'objet.", "our character walks to the building first");
    heldMoves_.push_back(std::move(h));
}

void Session::ReleaseHeldMoves(uint32_t container, uint32_t looter) {
    const double now = clock_();
    for (auto it = heldMoves_.begin(); it != heldMoves_.end();) {
        if (it->container != container) { ++it; continue; }
        for (const InvOp& op : it->ops) {
            log_("inventory move asked of the host: " + op.item.templateSid + " x" + std::to_string(op.item.quantity) + " " + op.item.section + " -> " +
                 op.toSection + " " + std::to_string(op.toX) + "," + std::to_string(op.toY) + " (our character " + std::to_string(looter) +
                 " got to the building)");
            Writer w;
            Encode(w, op);
            SendReliable(net_.serverPeer(), w);
        }
        for (uint32_t id : it->involved)
            if (auto e = entities_.find(id); e != entities_.end()) e->second.invPendingUntil = now + 3.0;   // wait for the host's answer
        it = heldMoves_.erase(it);
    }
}

void Session::ExpireHeldMoves(double now) {
    for (auto it = heldMoves_.begin(); it != heldMoves_.end();) {
        if (now <= it->until) { ++it; continue; }
        for (uint32_t id : it->involved)
            if (auto e = entities_.find(id); e != entities_.end()) {
                e->second.invDirty = true;   // back to the host's state: nothing moved there
                e->second.invRetry = 0;
                e->second.invPendingUntil = 0;
                e->second.invUnmatchedSince = 0;
            }
        AddChat("* Objet non déplacé : ton personnage n'a pas pu aller jusqu'au bâtiment. Rien n'a changé.",
                "building panel: held move(s) dropped, our character never got there");
        it = heldMoves_.erase(it);
    }
}

// ---------------------------------------------------------------- farm inputs

void Session::SendFarmInputs(std::vector<FarmInput>& farms, bool full) {
    FarmInputsMsg m;
    m.full = full;
    for (FarmInput& f : farms) {
        for (float& v : f.stored) v = std::max(0.0f, Quantize(v));
        const std::string key = MachineKey(f.sid, f.pos);
        auto prev = farmInputsSent_.find(key);
        const bool changed = prev == farmInputsSent_.end() || prev->second != f.stored;
        if (changed && farmInputsSent_.size() < 4096) farmInputsSent_[key] = f.stored;
        if (changed || full) m.farms.push_back(f);
    }
    for (size_t i = 0; i < m.farms.size(); i += kMaxFarmInputFarms) {
        FarmInputsMsg part;
        part.full = m.full;
        part.farms.assign(m.farms.begin() + ptrdiff_t(i), m.farms.begin() + ptrdiff_t(std::min(m.farms.size(), i + kMaxFarmInputFarms)));
        Writer w(2048);
        Encode(w, part);
        BroadcastReliable(w, true);
    }
}

void Session::ClientFarmInputsPacket(Reader& r) {
    FarmInputsMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    for (auto& f : m.farms) {
        const std::string key = MachineKey(f.sid, f.pos);
        if (farmInputs_.size() > 4096) farmInputs_.clear();
        ClientFarm& c = farmInputs_[key];
        if (!(c.f == f) || !c.found) c.dirty = true;
        c.f = std::move(f);
    }
}

void Session::ApplyFarmInputs(bool all) {
    for (auto& [key, c] : farmInputs_) {
        if (!all && !c.dirty) continue;
        const bool was = c.found;
        c.found = world_.ApplyFarmInputs(c.f, LocalBuildingOf(c.f.sid, c.f.pos));
        c.dirty = false;
        if (c.found && !was)
            log_("farm: " + world_.TemplateName(c.f.sid) + " holds the host's water now (" + (c.f.stored.empty() ? std::string("no input") : std::to_string(c.f.stored[0])) + ")");
    }
}

} // namespace kc
