// 0.3.5, lot 5 (branch night-actions, kc/actions.h): what a client player's characters do in the host's
// game that the player must see or answer on their own screen.
//
// Slave shops (A_FAIRE 16/10 point 4). The slaver's dialogue actions 0x2A ("Je voudrais acheter des
// esclaves": its slaves) and 0x29 ("vendre des esclaves": the player character's squad mates near it)
// run in the host's game, which builds the offer and would open the game's slave window on the
// host's screen. The plugin keeps that window hidden and the offer aside (IWorld::TakeSlaveOffers,
// as the animal shops of protocol 36); the session sends it to the player whose character talked
// (SlaveShop); their game opens the same window with its copies and the host's prices. Their choice
// comes back (SlaveTrade, checked like any order: the character in the conversation must be theirs,
// every character must be in that very offer, and one sold must still be theirs), and the host's game
// runs the purchase or the sale with the offer it kept (its prices, the player faction's cats). A
// slave bought is freed by the game, follows the buyer and talks to them (that conversation goes to
// their player like any other); a character sold becomes the slaver's slave.
//
// Action progress (point 5). The host's game moves the bar of a timed task (ProgressBarTask: picking,
// cutting or forcing a lock, picking shackles...) and draws it above the character, on the host's
// screen only. For another player's character the progress goes to that player (ActionProgress, at
// most 4 times a second while it moves; "ended" once it has not moved for a second); their game
// draws the same bar above its copy.
//
// Disguise (point 13). The host's game works out each player character's disguise from the uniform it
// wears (AI vt 0x48, 0x5A51E0, every AI periodic update; at once after a client's item move touching
// a worn slot, see plugin/actions.cpp). Clients run no AI: their copies never had a disguise, hence no
// indicator. The host sends each player their characters' disguise when it changes; their copies take
// it (the game's own stealth panel shows "In Disguise").
//
// Inventories asked again (point 15). A client gives up rebuilding an inventory after three failed
// attempts (ApplyInventory false) and then follows it no more: nothing moved in it is seen, so after
// an admin teleport far away (the zone loading while the rebuilds ran) sales in a trade window never
// reached the host until a rejoin. The client asks for its own characters' (and their worn backpacks')
// inventories again (InvResend), at once when a trade window is about to open for them.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "kc/session.h"

namespace kc {

namespace {
constexpr double kSlaveOfferTimeout = 300.0;
constexpr double kInvResendSpacing = 10.0;     // per inventory
constexpr double kTradeSideWait = 6.0;         // a trade window waits that long for our inventories
constexpr double kProgressForget = 3.0;        // client: a bar the host stopped speaking of goes
constexpr double kDisguiseInterval = 1.0, kDisguiseReapply = 2.0;
const char* KindVerb(uint8_t kind) { return kind == kSlaveSell ? "sells" : "buys"; }
}

void Session::ResetActions() {
    for (const auto& [pid, o] : slaveOffers_) world_.DropSlaveOffer(o.key);
    slaveOffers_.clear();
    pendingSlaveTrades_.clear();
    scratchSlaveOffers_.clear();
    scratchSlaveChoices_.clear();
    slaveShop_ = ClientSlaveShop{};
    slavesTraded_ = 0;
    progressTracks_.clear();
    scratchProgress_.clear();
    progressSent_ = 0;
    for (const auto& [id, v] : progressShown_)
        if (auto it = entities_.find(id); it != entities_.end() && it->second.present) world_.ShowActionProgress(it->second.handle, -1.0f);
    progressShown_.clear();
    progressHeardAt_.clear();
    progressNoBar_ = false;
    disguises_.clear();
    nextDisguise_ = nextDisguiseApply_ = 0;
    invResendAt_.clear();
    invResendAsked_ = invResendSent_ = 0;
    nextInvResendCheck_ = 0;
    tradeSideWarned_ = false;
    tradeSpendSince_ = -1;
}

// ============================== slave shops ==============================

void Session::EndSlaveOffer(uint8_t player, const char* why) {
    auto it = slaveOffers_.find(player);
    if (it == slaveOffers_.end()) return;
    world_.DropSlaveOffer(it->second.key);
    const std::string who = players_.count(player) ? players_[player].name : "player " + std::to_string(player);
    log_("[" + who + "] slave shop offer " + std::to_string(it->second.id) + " dropped: " + why);
    slaveOffers_.erase(it);
}

void Session::HostSlaveTradePacket(uint8_t from, Reader& r) {
    SlaveTradeMsg m;
    auto pl = players_.find(from);
    if (pl == players_.end() || !pl->second.inGame || !Decode(r, m)) return;   // (Authorize checked the character is theirs)
    if (pendingSlaveTrades_.size() < 16) pendingSlaveTrades_.emplace_back(from, std::move(m));
}

void Session::HostSlaveShops(double now) {
    // the game's offers made in other players' conversations: each goes to that player
    world_.TakeSlaveOffers(scratchSlaveOffers_);
    for (const auto& o : scratchSlaveOffers_) {
        Entity* buyer = entityByHandle(o.buyer);
        auto pl = buyer ? players_.find(buyer->owner) : players_.end();
        const std::string seller = world_.CharacterNameOf(o.seller);
        const char* what = o.kind == kSlaveSell ? "slave sale" : "slave shop";
        if (!buyer || !buyer->squad || buyer->owner == hostId_ || pl == players_.end() || !pl->second.inGame) {
            log_(std::string(what) + " of " + seller + " dropped: " +
                 (!buyer ? "the character is not followed" : !buyer->squad ? "not a squad character" : "no player in game owns that character"));
            world_.DropSlaveOffer(o.key);
            continue;
        }
        const uint8_t pid = pl->first;
        const std::string who = pl->second.name;
        Handle own;
        if (auto sy = sync_.find(pid); sy != sync_.end()) own = sy->second.own;
        SlaveShopMsg m;
        m.offerId = nextSlaveOffer_++;
        if (!nextSlaveOffer_) nextSlaveOffer_ = 1;
        m.kind = o.kind;
        m.buyerNetId = buyer->netId;
        m.basePrice = std::max(o.basePrice, 0);
        Entity* sellerE = entityByHandle(o.seller);
        m.sellerNetId = sellerE ? sellerE->netId : 0;
        HostSlaveOffer off;
        off.key = o.key;
        off.id = m.offerId;
        off.kind = o.kind;
        off.buyer = buyer->netId;
        off.seller = o.seller;
        off.since = now;
        size_t notTheirs = 0;
        for (size_t i = 0; i < o.slaves.size(); ++i) {
            Entity* s = entityByHandle(o.slaves[i]);
            if (!s || s->container || off.slaves.count(s->netId)) continue;   // not followed: the player cannot see it
            if (o.kind == kSlaveBuy && s->squad) continue;                      // only the slaver's slaves are for sale
            // sold: only that player's own characters, never their main one (the game would sell any
            // squad mate standing near: the host's or another player's too)
            if (o.kind == kSlaveSell && (!s->squad || s->owner != pid || (own.valid() && s->handle == own))) { ++notTheirs; continue; }
            if (m.slaves.size() >= kMaxShopSlaves) break;
            m.slaves.push_back(s->netId);
            m.prices.push_back(std::max(i < o.prices.size() ? o.prices[i] : 0, 0));
            off.slaves[s->netId] = s->handle;
        }
        Writer w;
        if (!sellerE || m.slaves.empty()) {
            log_("[" + who + "] " + what + " of " + seller + " not sent: " +
                 (!sellerE ? std::string("the slaver is not followed")
                           : "none of its " + std::to_string(o.slaves.size()) + " character(s) can be offered" +
                                 (notTheirs ? " (" + std::to_string(notTheirs) + " not theirs to sell, or their own character)" : std::string())));
            m.slaves.clear();
            m.prices.clear();
            m.note = o.kind == kSlaveSell ? "Tu n'as ici aucun personnage à vendre comme esclave (seuls les tiens, près du marchand, et pas ton personnage principal)."
                                          : seller + " n'a aucun esclave à te vendre ici.";
            Encode(w, m);
            SendReliable(pl->second.peer, w);
            world_.DropSlaveOffer(o.key);
            continue;
        }
        EndSlaveOffer(pid, "a new offer replaces it");   // one window at a time
        slaveOffers_[pid] = off;
        Encode(w, m);
        SendReliable(pl->second.peer, w);
        log_("[" + who + "] " + what + " of " + seller + ": " + std::to_string(m.slaves.size()) + " character(s) offered (offer " + std::to_string(m.offerId) +
             ", base price " + std::to_string(m.basePrice) + "), window sent to that player" +
             (notTheirs ? " (" + std::to_string(notTheirs) + " left out: not theirs, or their own character)" : std::string()));
    }
    scratchSlaveOffers_.clear();

    // the players' answers: run by the host's game with the offer it kept
    for (auto& [from, b] : pendingSlaveTrades_) {
        auto pl = players_.find(from);
        if (pl == players_.end()) continue;
        const std::string who = pl->second.name;
        Result res;
        res.request = Msg::SlaveTrade;
        res.netId = b.buyerNetId;
        auto it = slaveOffers_.find(from);
        if (it == slaveOffers_.end() || it->second.id != b.offerId || it->second.buyer != b.buyerNetId || it->second.kind != b.kind) {
            log_("[" + who + "] slave deal refused: offer " + std::to_string(b.offerId) +
                 (it == slaveOffers_.end() ? " unknown (expired or replaced)" : " is not the one open for that character"));
            if (!b.slaves.empty()) {
                res.state = ResultState::Rejected;
                res.reason = ResultReason::WrongTarget;
                res.text = "Commerce d'esclaves refusé : cette offre n'est plus valable, redemande au marchand.";
                SendResult(from, res);
            }
            continue;
        }
        const HostSlaveOffer off = it->second;
        slaveOffers_.erase(it);
        if (b.slaves.empty()) {
            log_("[" + who + "] closes the slave window without a deal");
            world_.DropSlaveOffer(off.key);
            continue;
        }
        std::vector<Handle> want;
        bool offered = true, stillTheirs = true;
        for (uint32_t id : b.slaves) {
            auto s = off.slaves.find(id);
            if (s == off.slaves.end()) { offered = false; break; }
            if (std::find(want.begin(), want.end(), s->second) != want.end()) continue;
            if (off.kind == kSlaveSell) {   // still theirs now (given to someone else meanwhile?)
                auto e = entities_.find(id);
                if (e == entities_.end() || !e->second.squad || e->second.owner != from) { stillTheirs = false; break; }
            }
            want.push_back(s->second);
        }
        if (!offered || !stillTheirs) {
            world_.DropSlaveOffer(off.key);
            Refuse(from, Msg::SlaveTrade, 0, b.buyerNetId, !offered ? ResultReason::WrongTarget : ResultReason::NotYourCharacter, "characters of the offer",
                   !offered ? "a character that offer " + std::to_string(off.id) + " does not hold" : "a character no longer theirs",
                   !offered ? "Commerce d'esclaves refusé : ce personnage n'était pas proposé." : "Commerce d'esclaves refusé : ce personnage n'est plus le tien.");
            continue;
        }
        int32_t price = 0;
        std::string why;
        const bool ok = world_.TradeSlaves(off.key, want, price, why);
        world_.DropSlaveOffer(off.key);
        if (!ok) {
            log_("[" + who + "] slave deal not done by the game: " + why);
            res.state = ResultState::Rejected;
            res.reason = ResultReason::NotAllowed;
            res.text = std::string(off.kind == kSlaveSell ? "Vente" : "Achat") + " d'esclaves impossible : " + why;
            SendResult(from, res);
            continue;
        }
        slavesTraded_ += want.size();
        if (PlayerStats* st = StatsOf(from)) {   // player statistics: the cats of their trades
            if (price > 0) st->cashSpent += price;
            else st->cashEarned += -int64_t(price);
        }
        moneyAt_ = -1e9;
        nextProgress_ = 0;   // the new cats go out now
        log_("[" + who + "] " + KindVerb(off.kind) + " " + std::to_string(want.size()) + " slave(s) " + (off.kind == kSlaveSell ? "to " : "from ") +
             world_.CharacterNameOf(off.seller) + " for " +
             std::to_string(std::abs(price)) + " cats (their window showed " + std::to_string(b.shownPrice) + ")" +
             (off.kind == kSlaveBuy ? ": freed by the game, they follow the buyer and talk to them" : ": they are the slaver's slaves now"));
        res.state = ResultState::Done;
        SendResult(from, res);
    }
    pendingSlaveTrades_.clear();

    // offers left unanswered, or of a player gone
    for (auto it = slaveOffers_.begin(); it != slaveOffers_.end();) {
        const bool gone = !players_.count(it->first);
        if (!gone && now - it->second.since <= kSlaveOfferTimeout) { ++it; continue; }
        log_("slave shop offer " + std::to_string(it->second.id) + " dropped: " + (gone ? "its player left" : "no answer"));
        world_.DropSlaveOffer(it->second.key);
        it = slaveOffers_.erase(it);
    }
}

void Session::ClientSlaveShopPacket(Reader& r) {
    SlaveShopMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    if (m.slaves.empty()) {
        if (!m.note.empty()) AddChat("* " + m.note, "the host cannot open this slave window");
        return;
    }
    auto answerNone = [&](const std::string& why) {
        log_("slave shop " + std::to_string(m.offerId) + " cannot open here: " + why);
        AddChat("* La fenêtre des esclaves n'a pas pu s'ouvrir ici (" + why + ").", "slave shop not opened");
        SlaveTradeMsg b;
        b.offerId = m.offerId;
        b.kind = m.kind;
        b.buyerNetId = m.buyerNetId;
        if (!ClientMaySend(Msg::SlaveTrade, b.buyerNetId, "close a slave window")) return;
        Writer w;
        Encode(w, b);
        SendReliable(net_.serverPeer(), w);
    };
    auto seller = entities_.find(m.sellerNetId);
    auto buyer = entities_.find(m.buyerNetId);
    if (seller == entities_.end() || !seller->second.present) return answerNone("le marchand n'est pas là");
    if (buyer == entities_.end() || !buyer->second.present) return answerNone("ton personnage n'est pas là");
    std::vector<Handle> slaves;
    std::vector<int32_t> prices;
    for (size_t i = 0; i < m.slaves.size(); ++i) {
        auto s = entities_.find(m.slaves[i]);
        if (s == entities_.end() || !s->second.present) continue;
        slaves.push_back(s->second.handle);
        prices.push_back(m.prices[i]);
    }
    if (slaves.empty()) return answerNone("aucun des personnages proposés n'est là");
    if (!world_.OpenSlaveShop(m.kind, seller->second.handle, buyer->second.handle, slaves, prices, m.basePrice)) return answerNone("le jeu a refusé");
    slaveShop_.offerId = m.offerId;
    slaveShop_.kind = m.kind;
    slaveShop_.buyer = m.buyerNetId;
    log_(std::string(m.kind == kSlaveSell ? "slave sale " : "slave shop ") + std::to_string(m.offerId) + " open: " + std::to_string(slaves.size()) + " of the " +
         std::to_string(m.slaves.size()) + " character(s) offered are here");
}

void Session::ClientSlaveShops() {
    world_.TakeSlaveChoices(scratchSlaveChoices_);
    for (const auto& c : scratchSlaveChoices_) {
        if (!slaveShop_.offerId) {
            log_("slave window choice without an offer from the host: ignored");
            continue;
        }
        SlaveTradeMsg b;
        b.offerId = slaveShop_.offerId;
        b.kind = slaveShop_.kind;
        b.buyerNetId = slaveShop_.buyer;
        b.shownPrice = c.price;
        for (const Handle& h : c.slaves)
            if (Entity* e = entityByHandle(h); e && !e->container) b.slaves.push_back(e->netId);
        slaveShop_ = ClientSlaveShop{};
        if (!c.slaves.empty() && b.slaves.size() != c.slaves.size()) {
            log_("slave window: " + std::to_string(c.slaves.size() - b.slaves.size()) + " chosen character(s) unknown to the session: no deal");
            AddChat("* Commerce d'esclaves annulé : un des personnages choisis n'est plus suivi ici.", "slave deal cancelled here");
            b.slaves.clear();
        }
        if (!ClientMaySend(Msg::SlaveTrade, b.buyerNetId, "trade slaves")) continue;
        Writer w;
        Encode(w, b);
        SendReliable(net_.serverPeer(), w);
        log_(b.slaves.empty() ? std::string("slave window closed without a deal")
                              : std::string("slave ") + (b.kind == kSlaveSell ? "sale" : "purchase") + " asked of the host: " + std::to_string(b.slaves.size()) +
                                    " character(s), " + std::to_string(c.price) + " cats shown");
    }
    scratchSlaveChoices_.clear();
}

// ============================== action progress ==============================

void Session::HostActionProgress(double now) {
    world_.TakeActionProgress(scratchProgress_);
    for (const auto& p : scratchProgress_) {
        Entity* e = entityByHandle(p.who);
        if (!e || !e->squad || e->container || e->owner == hostId_ || !std::isfinite(p.progress)) continue;
        auto& t = progressTracks_[e->netId];
        t.value = p.progress;
        t.seenAt = now;
    }
    scratchProgress_.clear();
    if (progressTracks_.empty()) return;
    std::map<uint8_t, ActionProgressMsg> out;
    for (auto it = progressTracks_.begin(); it != progressTracks_.end();) {
        auto e = entities_.find(it->first);
        if (e == entities_.end() || !e->second.squad || e->second.owner == hostId_) { it = progressTracks_.erase(it); continue; }
        actions::ProgressTrack& t = it->second;
        const float v = actions::ProgressToSend(t, now);
        const uint8_t owner = e->second.owner;
        auto pl = players_.find(owner);
        if (v != -1.0f && pl != players_.end() && pl->second.inGame) {
            ActionProgressEntry en;
            en.netId = it->first;
            en.permille = v == actions::kSendEnded ? kProgressEnded : actions::ToPermille(v);
            auto& msg = out[owner];
            if (msg.entries.size() < kMaxProgressEntries) msg.entries.push_back(en);
        }
        // long over and told: forgotten
        if (t.sentValue < 0 && now - t.seenAt > 30.0) { it = progressTracks_.erase(it); continue; }
        ++it;
    }
    for (const auto& [pid, msg] : out) {
        auto pl = players_.find(pid);
        if (pl == players_.end() || msg.entries.empty()) continue;
        Writer w;
        Encode(w, msg);
        SendReliable(pl->second.peer, w);
        progressSent_ += msg.entries.size();
    }
}

void Session::ClientActionProgressPacket(Reader& r) {
    ActionProgressMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    const double now = clock_();
    for (const auto& e : m.entries) {
        auto it = entities_.find(e.netId);
        if (e.permille == kProgressEnded) {
            if (it != entities_.end() && it->second.present && progressShown_.count(e.netId)) world_.ShowActionProgress(it->second.handle, -1.0f);
            progressShown_.erase(e.netId);
            progressHeardAt_.erase(e.netId);
            continue;
        }
        if (it == entities_.end()) continue;
        if (!progressShown_.count(e.netId))
            log_("action progress of " + world_.CharacterNameOf(it->second.handle) + " shown here (the host's game runs the task)");
        progressShown_[e.netId] = float(e.permille) / 1000.0f;
        progressHeardAt_[e.netId] = now;
    }
}

void Session::ClientActionProgress(double now) {
    for (auto it = progressShown_.begin(); it != progressShown_.end();) {
        auto e = entities_.find(it->first);
        const auto heard = progressHeardAt_.find(it->first);
        const bool stale = heard == progressHeardAt_.end() || now - heard->second > kProgressForget;
        if (e == entities_.end() || stale) {
            if (e != entities_.end() && e->second.present) world_.ShowActionProgress(e->second.handle, -1.0f);
            progressHeardAt_.erase(it->first);
            it = progressShown_.erase(it);
            continue;
        }
        // every frame: the bar follows the character
        if (e->second.present && !world_.ShowActionProgress(e->second.handle, it->second) && !progressNoBar_) {
            progressNoBar_ = true;
            log_("action progress: the game's bar could not be made here: shown in the overlay instead");
        }
        ++it;
    }
}

// ============================== disguise ==============================

void Session::HostDisguises(double now) {
    if (now < nextDisguise_) return;
    nextDisguise_ = now + kDisguiseInterval;
    std::map<uint8_t, DisguiseMsg> out;
    for (auto it = disguises_.begin(); it != disguises_.end();) {   // characters gone, or their player out of the game
        auto e = entities_.find(it->first);
        auto pl = e != entities_.end() ? players_.find(e->second.owner) : players_.end();
        if (e == entities_.end() || !e->second.squad || pl == players_.end() || !pl->second.inGame) it = disguises_.erase(it);
        else ++it;
    }
    for (auto& [id, e] : entities_) {
        if (!e.squad || e.container || e.owner == hostId_) continue;
        auto pl = players_.find(e.owner);
        if (pl == players_.end() || !pl->second.inGame) continue;
        DisguiseInfo d;
        if (!world_.ReadDisguise(e.handle, d)) continue;
        auto prev = disguises_.find(id);
        const DisguiseInfo before = prev != disguises_.end() ? prev->second : DisguiseInfo{};
        if (prev != disguises_.end() && !actions::DisguiseChanged(before, d)) continue;
        // said in the log when it matters for the AI (A_FAIRE 16/10 point 13: next time we know what
        // the host's game thought of a client's uniform)
        const bool was = actions::Disguised(before), is = actions::Disguised(d);
        char lvl[32];
        snprintf(lvl, sizeof(lvl), "%.2f", double(d.level));
        const std::string name = world_.CharacterNameOf(e.handle);
        if (is && !was)
            log_("[" + pl->second.name + "] disguise: " + name + " now passes for " + d.factionName + " (uniform " + lvl + ")");
        else if (was && !is)
            log_("[" + pl->second.name + "] disguise: " + name + " no longer passes for " + before.factionName +
                 (d.blown > 0 ? " (seen through: blown for " + std::to_string(int(d.blown)) + " s)" : " (uniform " + std::string(lvl) + ")"));
        else if (prev == disguises_.end() && !d.factionSid.empty())
            log_("[" + pl->second.name + "] disguise: " + name + " wears a " + d.factionName + " uniform, not enough to pass for one (" + lvl + ")");
        disguises_[id] = d;
        auto& msg = out[e.owner];
        if (msg.entries.size() < kMaxDisguiseEntries) msg.entries.push_back({id, d});
    }
    for (const auto& [pid, msg] : out) {
        auto pl = players_.find(pid);
        if (pl == players_.end() || msg.entries.empty()) continue;
        Writer w;
        Encode(w, msg);
        SendReliable(pl->second.peer, w);
    }
}

void Session::ClientDisguisePacket(Reader& r) {
    DisguiseMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    for (const auto& en : m.entries) {
        auto it = entities_.find(en.netId);
        if (it == entities_.end()) continue;
        const DisguiseInfo before = disguises_.count(en.netId) ? disguises_[en.netId] : DisguiseInfo{};
        disguises_[en.netId] = en.d;
        if (actions::Disguised(en.d) != actions::Disguised(before))
            log_("disguise from the host: " + world_.CharacterNameOf(it->second.handle) +
                 (actions::Disguised(en.d) ? " passes for " + en.d.factionName : std::string(" no longer disguised")));
        if (it->second.present) world_.ApplyDisguise(it->second.handle, en.d);
    }
}

void Session::ClientDisguises(double now) {
    if (now < nextDisguiseApply_ || disguises_.empty()) return;
    nextDisguiseApply_ = now + kDisguiseReapply;
    for (auto it = disguises_.begin(); it != disguises_.end();) {
        auto e = entities_.find(it->first);
        if (e == entities_.end()) { it = disguises_.erase(it); continue; }
        if (e->second.present) world_.ApplyDisguise(e->second.handle, it->second);
        ++it;
    }
}

std::vector<std::string> Session::actionLines() const {
    std::vector<std::string> out;
    if (!isClient()) return out;
    if (progressNoBar_)
        for (const auto& [id, v] : progressShown_)
            if (auto e = entities_.find(id); e != entities_.end())
                out.push_back("Action en cours (" + world_.CharacterNameOf(e->second.handle) + ") : " + std::to_string(int(v * 100.0f + 0.5f)) + " %");
    for (const auto& [id, d] : disguises_) {
        auto e = entities_.find(id);
        if (e == entities_.end() || e->second.owner != localId_ || d.factionSid.empty()) continue;
        const std::string name = world_.CharacterNameOf(e->second.handle);
        if (actions::Disguised(d)) out.push_back("Déguisé en " + d.factionName + " : " + name);
        else if (d.blown > 0) out.push_back("Déguisement percé (" + d.factionName + ") : " + name);
    }
    return out;
}

// ============================== inventories asked again ==============================

bool Session::AskInventoriesAgain(const std::vector<uint32_t>& ids, double now, const char* why) {
    InvResendMsg m;
    for (uint32_t id : ids) {
        auto at = invResendAt_.find(id);
        if (at != invResendAt_.end() && now - at->second < kInvResendSpacing) continue;
        if (m.netIds.size() >= kMaxInvResend) break;
        m.netIds.push_back(id);
        invResendAt_[id] = now;
    }
    if (m.netIds.empty() || state_ != SessionState::Connected) return false;
    Writer w;
    Encode(w, m);
    SendReliable(net_.serverPeer(), w);
    invResendAsked_ += m.netIds.size();
    log_("inventories asked of the host again (" + std::to_string(m.netIds.size()) + "): " + why);
    return true;
}

void Session::ClientInvResend(double now) {
    // a window that counted cats no item move carried (point 15: the sales went nowhere): said, with
    // what our side looks like here, and our inventories asked again
    if (trade_.open && unsentSpend_ != 0) {
        if (tradeSpendSince_ < 0) tradeSpendSince_ = now;
        else if (now - tradeSpendSince_ > 4.0) {
            tradeSpendSince_ = -1;
            std::vector<uint32_t> ours;
            std::string state;
            for (const auto& [id, e] : entities_) {
                const bool looter = id == trade_.looter;
                const bool bag = e.bag && e.bagOwner == trade_.looter;
                if (!looter && !bag) continue;
                ours.push_back(id);
                state += std::string(looter ? " character " : " backpack ") + std::to_string(id) + (e.present ? "" : " not here") +
                         (e.haveInv ? "" : " (inventory not followed)") + (e.invDirty ? " (being rebuilt)" : "");
            }
            log_("trade: our window counted " + std::to_string(unsentSpend_) + " cats but no item move was seen for 4 s;" + state);
            AskInventoriesAgain(ours, now, "a trade our window counted went unseen");
        }
    } else {
        tradeSpendSince_ = -1;
    }
    if (now < nextInvResendCheck_) return;
    nextInvResendCheck_ = now + 5.0;
    std::vector<uint32_t> lost;
    for (const auto& [id, e] : entities_) {
        if (!e.present || e.haveInv || e.invFailures < 3) continue;
        const Entity* w = e.bag ? Wearer(e) : &e;
        if (!w || !w->squad || w->owner != localId_ || (!e.bag && e.container)) continue;
        lost.push_back(id);
    }
    if (!lost.empty()) AskInventoriesAgain(lost, now, "our rebuild of them was given up (3 failed attempts)");
}

bool Session::TradeSideFollowed(double now) {
    if (trade_.pendingSince == 0) tradeSideWarned_ = false;   // a new window
    std::vector<uint32_t> missing;
    for (const auto& [id, e] : entities_) {
        const bool looter = id == trade_.looter;
        const bool bag = e.bag && e.bagOwner == trade_.looter && e.present;
        if ((looter || bag) && !e.haveInv) missing.push_back(id);
    }
    if (missing.empty()) return true;
    AskInventoriesAgain(missing, now, "a trade window opens for that character");
    if (trade_.pendingSince > 0 && now - trade_.pendingSince > kTradeSideWait) {
        if (!tradeSideWarned_) {
            tradeSideWarned_ = true;
            AddChat("* Commerce : ton inventaire n'est pas encore à jour ici, une vente peut ne pas passer (rouvre la fenêtre si rien ne bouge).",
                    "trade window opened with our side not followed yet (inventories asked again)");
        }
        return true;
    }
    return false;
}

void Session::HostInvResendPacket(uint8_t from, Reader& r) {
    InvResendMsg m;
    auto pl = players_.find(from);
    if (pl == players_.end() || !pl->second.inGame || !Decode(r, m)) return;
    size_t sent = 0, refused = 0;
    for (uint32_t id : m.netIds) {
        auto it = entities_.find(id);
        const Entity* w = it != entities_.end() ? Wearer(it->second) : nullptr;
        // only the asking player's own characters and their worn backpacks
        if (!w || !w->squad || w->owner != from || (it->second.container && !it->second.bag)) { ++refused; continue; }
        InventoryMsg inv;
        inv.netId = id;
        if (!world_.ReadInventory(it->second.handle, inv.items)) { ++refused; continue; }
        Writer out(1024);
        Encode(out, inv);
        SendReliable(pl->second.peer, out);
        ++sent;
    }
    invResendSent_ += sent;
    log_("[" + pl->second.name + "] asked for " + std::to_string(m.netIds.size()) + " inventory(ies) again: " + std::to_string(sent) + " sent" +
         (refused ? ", " + std::to_string(refused) + " not theirs or unreadable" : std::string()));
}

} // namespace kc
