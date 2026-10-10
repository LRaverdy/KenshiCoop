// Actor safety and authority: one check for every client->host message, before its handler.
//
// A client must never make a character it does not own act. Every request names its actor (a
// Command its netId, a container request its looter, new looks their character); the host checks,
// in one place, that the actor is a squad member assigned to that very player, and runs the request
// on exactly that character (the plugin then also makes the game's selection exactly that character,
// see kenshi::WithSelection). Anything else is refused: logged in English ("auth: ... refused" /
// "refused: actor N not owned by player P"), counted per player and rule (decaying, for the logs),
// and answered with a Result that carries a French text for the player.
#include <cmath>

#include "kc/session.h"

namespace kc {

namespace {
constexpr double kRefusalHalfLife = 60.0;   // seconds
const char* kNotYours = "Action refusée : ce personnage n'est pas le tien (chacun ne commande que ses propres personnages).";
}

Session::ActorVerdict Session::CheckActor(uint8_t player, uint32_t actorNetId) const {
    if (!actorNetId) return ActorVerdict::Missing;
    auto it = entities_.find(actorNetId);
    if (it == entities_.end() || it->second.container) return ActorVerdict::Unknown;
    if (!it->second.squad) return ActorVerdict::NotSquad;
    if (player == 0 || it->second.owner != player || (isHost() && player == hostId_)) return ActorVerdict::NotOwned;
    return ActorVerdict::Ok;
}

const char* Session::VerdictText(ActorVerdict v) {
    switch (v) {
    case ActorVerdict::Ok: return "ok";
    case ActorVerdict::Missing: return "no actor named";
    case ActorVerdict::Unknown: return "unknown character";
    case ActorVerdict::NotSquad: return "not a squad member";
    case ActorVerdict::NotOwned: return "belongs to someone else";
    }
    return "?";
}

double Session::recentRefusals(uint8_t player) const {
    const double now = clock_();
    double total = 0;
    for (const auto& [key, d] : refusals_)
        if (key.first == player) total += d.value * std::exp2(-(now - d.at) / kRefusalHalfLife);
    return total;
}

void Session::SendResult(uint8_t player, const Result& m) {
    auto pl = players_.find(player);
    if (pl == players_.end()) return;
    Writer w;
    Encode(w, m);
    SendReliable(pl->second.peer, w);
}

void Session::Refuse(uint8_t player, Msg type, uint32_t seq, uint32_t netId, ResultReason reason, const std::string& rule, const std::string& detail,
                     const std::string& french) {
    ++actorRefusals_;
    const double now = clock_();
    Decaying& d = refusals_[{player, rule}];
    d.value = d.value * std::exp2(-(now - d.at) / kRefusalHalfLife) + 1.0;
    d.at = now;
    const std::string who = players_.count(player) ? players_[player].name : "player " + std::to_string(player);
    char recent[32];
    snprintf(recent, sizeof(recent), "%.1f", d.value);
    const MessageRule* r = MessageRuleFor(type);
    log_("auth: [" + who + "] " + (r ? r->name : "message") + " refused: " + rule + " (" + detail + "; " + recent + " recent of this rule)");
    Result m;
    m.request = type;
    m.seq = seq;
    m.netId = netId;
    m.state = ResultState::Rejected;
    m.reason = reason;
    m.text = french;
    SendResult(player, m);
}

bool Session::AdmitActor(uint8_t player, uint32_t actorNetId, const std::string& request, Msg type, uint32_t seq) {
    const ActorVerdict v = CheckActor(player, actorNetId);
    if (v == ActorVerdict::Ok) return true;
    std::string owner;
    if (auto it = entities_.find(actorNetId); it != entities_.end() && it->second.squad)
        owner = it->second.owner == hostId_ ? ", it is the host's" : ", it is player " + std::to_string(it->second.owner) + "'s";
    log_("refused: actor " + std::to_string(actorNetId) + " not owned by player " + std::to_string(player) + " (" + request + ": " + VerdictText(v) +
         owner + ")");
    Refuse(player, type, seq, actorNetId, v == ActorVerdict::Missing || v == ActorVerdict::Unknown ? ResultReason::NoActor : ResultReason::NotYourCharacter,
           "own character", std::string("actor ") + std::to_string(actorNetId) + " " + VerdictText(v) + owner, kNotYours);
    return false;
}

bool Session::Authorize(RemotePlayer& pl, Msg type, Reader r) {
    const MessageRule* rule = MessageRuleFor(type);
    if (!rule || rule->role == AuthRole::HostOnly || rule->role == AuthRole::Handshake) {   // host->client only, or a second Hello
        const char* n = MsgName(type);
        Refuse(pl.id, type, 0, 0, ResultReason::NotAllowed, "message not accepted from a client",
               "type " + std::to_string(int(type)) + (n ? std::string(" (") + n + ")" : std::string()), {});
        return false;
    }
    if (rule->role == AuthRole::InGame && !pl.inGame) {
        Refuse(pl.id, type, 0, 0, ResultReason::NotAllowed, "in the game", "the player is still joining", {});
        return false;
    }
    if (rule->role == AuthRole::Joining && pl.inGame) return false;   // a second Ready: nothing to say
    if (rule->minInterval > 0) {   // too often (map pings): dropped quietly, the player only clicked fast
        const double now = clock_();
        auto [last, first] = lastAccepted_.try_emplace({pl.id, type}, now);
        if (!first) {
            if (now - last->second < rule->minInterval) { ++rateLimited_; return false; }
            last->second = now;
        }
    }
    auto malformed = [&] {
        Refuse(pl.id, type, 0, 0, ResultReason::NoActor, "well-formed request naming its actor", "could not be read (no actor, or invalid fields)",
               "Action refusée : demande invalide (aucun personnage désigné).");
        return false;
    };
    switch (rule->subject) {
    case AuthSubject::None: return true;
    case AuthSubject::OwnCharacter: {
        uint32_t actor = 0, seq = 0;
        if (type == Msg::Command) {
            Command c;
            if (!Decode(r, c)) return malformed();   // no actor named (netId 0), or not a valid order
            actor = c.netId;
            seq = c.seq;
            // AI settings (squad bar, Tâches panel) of a character nobody owns: any player's
            if (IsSettingsCommand(c) && MaySetSettings(pl.id, actor)) return true;
        } else if (type == Msg::SquadRequest) {
            SquadRequest q;
            if (!Decode(r, q)) return malformed();
            actor = q.actor;
            seq = q.seq;
        } else if (type == Msg::ContainerOpen) {
            ContainerOpen m;
            if (!Decode(r, m)) return malformed();
            actor = m.looterNetId;
        } else if (type == Msg::Appearance) {
            AppearanceMsg m;
            if (!Decode(r, m)) return malformed();
            actor = m.netId;
        }
        return AdmitActor(pl.id, actor, rule->name, type, seq);
    }
    case AuthSubject::OwnConversation: {
        DialogReply a;
        if (!Decode(r, a)) return malformed();
        auto o = dialogOwner_.find(a.dialogId);
        if (o != dialogOwner_.end() && o->second == pl.id) return true;
        Refuse(pl.id, type, 0, 0, ResultReason::NotYourCharacter, "own conversation",
               "conversation " + std::to_string(a.dialogId) + (o == dialogOwner_.end() ? " unknown" : " is player " + std::to_string(o->second) + "'s"), {});
        return false;
    }
    case AuthSubject::Inventory: {
        InvOp op;
        if (!Decode(r, op)) return malformed();
        if (op.kind != InvOpKind::Drop) return true;   // moves: HostInvOp's rules (own, opened, bodies)
        // the character that drops it: the source itself (or a worn backpack's wearer), else the one the
        // request names (a chest it has open, a body: HostInvOp also checks it stands by it)
        return AdmitActor(pl.id, DropRequestActor(op), "drop an item", type, 0);
    }
    }
    return false;
}

bool Session::InjectForTest(uint8_t playerId, const Writer& w) {
    auto pl = players_.find(playerId);
    if (!isHost() || pl == players_.end()) return false;
    Reader r(w.data(), w.size());
    const auto type = PeekType(r);
    if (!type) return false;
    HostPacket(pl->second.peer, *type, r);
    return true;
}

// ---- client
bool Session::ClientMaySend(Msg type, uint32_t netId, const char* what) {
    const MessageRule* rule = MessageRuleFor(type);
    if (!rule || rule->role == AuthRole::HostOnly || rule->role == AuthRole::Handshake) return false;
    if (rule->subject != AuthSubject::OwnCharacter && !(rule->subject == AuthSubject::Inventory && netId)) return true;
    const ActorVerdict v = CheckActor(localId_, netId);
    if (v == ActorVerdict::Ok) return true;
    std::string owner;
    if (auto it = entities_.find(netId); it != entities_.end() && it->second.squad)
        owner = it->second.owner == hostId_ ? " (the host's character)" : " (another player's character)";
    log_(std::string("refused locally: ") + what + " for a character this player does not own: " + VerdictText(v) + owner);
    const double now = clock_();
    if (now >= nextResultNote_) {
        nextResultNote_ = now + 2.0;
        AddChat("* Action refusée : ce personnage n'est pas le tien.", "local request refused: not this player's character");
    }
    return false;
}

bool Session::SendRawCommandForTest(Command c) {
    if (state_ != SessionState::Connected) return false;
    c.seq = ++cmdSeq_;
    Writer w;
    Encode(w, c);
    SendReliable(net_.serverPeer(), w);
    log_("test: forged order sent as is (actor " + std::to_string(c.netId) + ", task " + std::to_string(c.task) + ")");
    return true;
}

void Session::OnResult(const Result& m) {
    results_.push_back(m);
    if (m.request == Msg::SquadRequest) OnSquadResult(m);
    if (results_.size() > 64) results_.pop_front();
    auto sent = m.request == Msg::Command ? sentOrders_.find(m.seq) : sentOrders_.end();
    if (m.state != ResultState::Rejected) {
        if (sent != sentOrders_.end()) sentOrders_.erase(sent);
        return;
    }
    ++rejected_;
    log_(std::string("host rejected our request (") + ToString(m.reason) + ", seq " + std::to_string(m.seq) + ")" + (m.text.empty() ? "" : ": " + m.text));
    if (sent != sentOrders_.end()) {
        world_.OrderRejected(sent->second.first, sent->second.second);   // undo what we predicted (a job shown at once)
        sentOrders_.erase(sent);
    }
    const double now = clock_();
    if (!m.text.empty() && m.reason != ResultReason::Failed && now >= nextResultNote_) {
        nextResultNote_ = now + 2.0;
        AddChat("* " + m.text, std::string("host rejected a request: ") + ToString(m.reason));
    }
}

} // namespace kc
