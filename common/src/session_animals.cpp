// Animal shops: a farmer's "got animals for sale?" in a client player's conversation.
//
// The dialogue action (CHOOSE_ANIMALS_BUYING) runs in the host's game, which builds the offer (the
// seller's animals around it, a price for each) and would open its window on the host's screen. The
// plugin keeps that window hidden and the game's offer aside (IWorld::TakeAnimalOffers); the session
// sends the offer to the player whose character asked (AnimalShop), whose game opens the same window
// with its copies of the animals. The player's choice comes back (AnimalBuy, checked like any order:
// the buying character must be theirs, the animals must be in that very offer), and the host's game
// sells them with the offer it kept (its own prices, its own purchase: the cats are taken from the
// player faction, each animal is recruited). The newcomers are the buyer's (as a recruit from their
// conversation) and go into their squad.
#include "kc/session.h"

#include <algorithm>

namespace kc {

namespace {
constexpr double kAnimalOfferTimeout = 300.0;   // an offer nobody answered (window left open, answer lost)
constexpr size_t kMaxPendingRecruits = 32;
}

void Session::ResetAnimalShops() {
    for (const auto& [pid, o] : animalOffers_) world_.DropAnimalOffer(o.key);
    animalOffers_.clear();
    pendingAnimalBuys_.clear();
    scratchAnimalOffers_.clear();
    scratchAnimalChoices_.clear();
    animalShop_ = ClientAnimalShop{};
    animalsSold_ = 0;
}

void Session::EndAnimalOffer(uint8_t player, const char* why) {
    auto it = animalOffers_.find(player);
    if (it == animalOffers_.end()) return;
    world_.DropAnimalOffer(it->second.key);
    const std::string who = players_.count(player) ? players_[player].name : "player " + std::to_string(player);
    log_("[" + who + "] animal shop offer " + std::to_string(it->second.id) + " dropped: " + why);
    animalOffers_.erase(it);
}

void Session::AnimalFollowsOwner(const Handle& h, uint8_t playerId) {
    if (!world_.IsAnimal(h)) return;
    Handle leader;
    if (auto sy = sync_.find(playerId); playerId != hostId_ && sy != sync_.end() && entityByHandle(sy->second.own)) leader = sy->second.own;
    for (auto& [nid, o] : entities_) {
        if (leader.valid()) break;
        if (o.squad && !o.container && o.owner == playerId && !(o.handle == h) && !world_.IsAnimal(o.handle)) leader = o.handle;
    }
    if (leader.valid() && world_.JoinSquadOf(h, leader)) log_("the animal " + world_.CharacterNameOf(h) + " joins its owner's squad");
}

void Session::HostAnimalBuyPacket(uint8_t from, Reader& r) {
    AnimalBuyMsg m;
    auto pl = players_.find(from);
    if (pl == players_.end() || !pl->second.inGame || !Decode(r, m)) return;   // (Authorize checked the buyer is theirs)
    if (pendingAnimalBuys_.size() < 16) pendingAnimalBuys_.emplace_back(from, std::move(m));
}

void Session::HostAnimalShops(double now) {
    // the game's offers for other players' characters: each goes to that player
    world_.TakeAnimalOffers(scratchAnimalOffers_);
    for (const auto& o : scratchAnimalOffers_) {
        Entity* buyer = entityByHandle(o.buyer);
        auto pl = buyer ? players_.find(buyer->owner) : players_.end();
        const std::string seller = world_.CharacterNameOf(o.seller);
        if (!buyer || !buyer->squad || buyer->owner == hostId_ || pl == players_.end() || !pl->second.inGame) {
            log_("animal shop of " + seller + " dropped: " + (!buyer ? "the buyer is not followed" : !buyer->squad ? "the buyer is not a squad character" : "no player in game owns the buyer"));
            world_.DropAnimalOffer(o.key);
            continue;
        }
        const std::string who = pl->second.name;
        AnimalShopMsg m;
        m.offerId = nextAnimalOffer_++;
        if (!nextAnimalOffer_) nextAnimalOffer_ = 1;
        m.buyerNetId = buyer->netId;
        m.basePrice = std::max(o.basePrice, 0);
        Entity* sellerE = entityByHandle(o.seller);
        m.sellerNetId = sellerE ? sellerE->netId : 0;
        HostAnimalOffer off;
        off.key = o.key;
        off.id = m.offerId;
        off.buyer = buyer->netId;
        off.since = now;
        for (const Handle& h : o.animals) {
            Entity* a = entityByHandle(h);
            if (!a || a->squad || a->container || off.animals.count(a->netId)) continue;   // an animal the player cannot see: not offered
            if (m.animals.size() >= kMaxShopAnimals) break;
            m.animals.push_back(a->netId);
            off.animals[a->netId] = h;
        }
        Writer w;
        if (!sellerE || m.animals.empty()) {
            log_("[" + who + "] animal shop of " + seller + " not sent: " + (!sellerE ? "the seller is not followed" : "none of its " + std::to_string(o.animals.size()) + " animals is followed"));
            m.animals.clear();
            m.note = seller + " n'a aucun animal à te proposer ici.";
            Encode(w, m);
            SendReliable(pl->second.peer, w);
            world_.DropAnimalOffer(o.key);
            continue;
        }
        EndAnimalOffer(pl->first, "a new offer replaces it");   // one window at a time
        animalOffers_[pl->first] = off;
        Encode(w, m);
        SendReliable(pl->second.peer, w);
        log_("[" + who + "] animal shop of " + seller + ": " + std::to_string(m.animals.size()) + " animal(s) offered (offer " + std::to_string(m.offerId) +
             ", base price " + std::to_string(m.basePrice) + "), window sent to that player");
    }
    scratchAnimalOffers_.clear();

    // the players' answers: sold by the host's game, with the offer it kept
    for (auto& [from, b] : pendingAnimalBuys_) {
        auto pl = players_.find(from);
        if (pl == players_.end()) continue;
        const std::string who = pl->second.name;
        Result res;
        res.request = Msg::AnimalBuy;
        res.netId = b.buyerNetId;
        auto it = animalOffers_.find(from);
        if (it == animalOffers_.end() || it->second.id != b.offerId || it->second.buyer != b.buyerNetId) {
            log_("[" + who + "] animal purchase refused: offer " + std::to_string(b.offerId) + (it == animalOffers_.end() ? " unknown (expired or replaced)" : " is not the one open for that character"));
            if (!b.animals.empty()) {
                res.state = ResultState::Rejected;
                res.reason = ResultReason::WrongTarget;
                res.text = "Achat d'animaux refusé : cette offre n'est plus valable, redemande au vendeur.";
                SendResult(from, res);
            }
            continue;
        }
        const HostAnimalOffer off = it->second;
        animalOffers_.erase(it);
        if (b.animals.empty()) {
            log_("[" + who + "] closes the animal shop without buying");
            world_.DropAnimalOffer(off.key);
            continue;
        }
        std::vector<Handle> want;
        std::vector<std::pair<Handle, uint64_t>> before;   // their handles change when they join the player faction
        bool offered = true;
        for (uint32_t id : b.animals) {
            auto a = off.animals.find(id);
            if (a == off.animals.end()) { offered = false; break; }
            if (std::find(want.begin(), want.end(), a->second) != want.end()) continue;
            want.push_back(a->second);
            before.emplace_back(a->second, world_.Identity(a->second));
        }
        if (!offered) {
            world_.DropAnimalOffer(off.key);
            Refuse(from, Msg::AnimalBuy, 0, b.buyerNetId, ResultReason::WrongTarget, "animals of the offer", "an animal that offer " + std::to_string(off.id) + " does not hold",
                   "Achat d'animaux refusé : cet animal n'était pas proposé.");
            continue;
        }
        int32_t price = 0;
        std::string why;
        const bool ok = world_.SellAnimals(off.key, want, price, why);
        world_.DropAnimalOffer(off.key);
        if (!ok) {
            log_("[" + who + "] animal purchase not done by the game: " + why);
            res.state = ResultState::Rejected;
            res.reason = ResultReason::NotAllowed;
            res.text = "Achat d'animaux impossible : " + why;
            SendResult(from, res);
            continue;
        }
        // each newcomer is the buyer's player's (RecruitJoined), and goes into their squad
        for (const auto& [h, id] : before) {
            PendingRecruit p;
            p.player = from;
            p.identity = id;
            p.before = h;
            p.at = clock_();
            if (pendingRecruits_.size() < kMaxPendingRecruits) pendingRecruits_.push_back(p);
        }
        animalsSold_ += want.size();
        log_("[" + who + "] buys " + std::to_string(want.size()) + " animal(s) for " + std::to_string(price) + " cats (their window showed " +
             std::to_string(b.shownPrice) + ")");
        res.state = ResultState::Done;
        SendResult(from, res);
    }
    pendingAnimalBuys_.clear();

    // offers left unanswered, or of a player gone
    for (auto it = animalOffers_.begin(); it != animalOffers_.end();) {
        const bool gone = !players_.count(it->first);
        if (!gone && now - it->second.since <= kAnimalOfferTimeout) { ++it; continue; }
        log_("animal shop offer " + std::to_string(it->second.id) + " dropped: " + (gone ? "its player left" : "no answer"));
        world_.DropAnimalOffer(it->second.key);
        it = animalOffers_.erase(it);
    }
}

void Session::ClientAnimalShopPacket(Reader& r) {
    AnimalShopMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m)) return;
    if (m.animals.empty()) {
        if (!m.note.empty()) AddChat("* " + m.note, "the host cannot open this animal shop");
        return;
    }
    auto answerNone = [&](const std::string& why) {
        log_("animal shop " + std::to_string(m.offerId) + " cannot open here: " + why);
        AddChat("* La fenêtre d'achat d'animaux n'a pas pu s'ouvrir ici (" + why + ").", "animal shop not opened");
        AnimalBuyMsg b;
        b.offerId = m.offerId;
        b.buyerNetId = m.buyerNetId;
        if (!ClientMaySend(Msg::AnimalBuy, b.buyerNetId, "close an animal shop")) return;
        Writer w;
        Encode(w, b);
        SendReliable(net_.serverPeer(), w);
    };
    auto seller = entities_.find(m.sellerNetId);
    auto buyer = entities_.find(m.buyerNetId);
    if (seller == entities_.end() || !seller->second.present) return answerNone("le vendeur n'est pas là");
    if (buyer == entities_.end() || !buyer->second.present) return answerNone("ton personnage n'est pas là");
    std::vector<Handle> animals;
    for (uint32_t id : m.animals)
        if (auto a = entities_.find(id); a != entities_.end() && a->second.present && !a->second.squad) animals.push_back(a->second.handle);
    if (animals.empty()) return answerNone("aucun de ses animaux n'est là");
    if (!world_.OpenAnimalShop(seller->second.handle, buyer->second.handle, animals, m.basePrice)) return answerNone("le jeu a refusé");
    animalShop_.offerId = m.offerId;
    animalShop_.buyer = m.buyerNetId;
    log_("animal shop " + std::to_string(m.offerId) + " open: " + std::to_string(animals.size()) + " of the " + std::to_string(m.animals.size()) +
         " animal(s) offered are here");
}

void Session::ClientAnimalShops() {
    world_.TakeAnimalChoices(scratchAnimalChoices_);
    for (const auto& c : scratchAnimalChoices_) {
        if (!animalShop_.offerId) {
            log_("animal shop choice without an offer from the host: ignored");
            continue;
        }
        AnimalBuyMsg b;
        b.offerId = animalShop_.offerId;
        b.buyerNetId = animalShop_.buyer;
        b.shownPrice = c.price;
        for (const Handle& h : c.animals)
            if (Entity* e = entityByHandle(h); e && !e->squad) b.animals.push_back(e->netId);
        animalShop_ = ClientAnimalShop{};
        if (!c.animals.empty() && b.animals.size() != c.animals.size()) {
            log_("animal shop: " + std::to_string(c.animals.size() - b.animals.size()) + " chosen animal(s) unknown to the session: not bought");
            AddChat("* Achat d'animaux annulé : un des animaux choisis n'est plus suivi ici.", "animal purchase cancelled here");
            b.animals.clear();
        }
        if (!ClientMaySend(Msg::AnimalBuy, b.buyerNetId, "buy animals")) continue;
        Writer w;
        Encode(w, b);
        SendReliable(net_.serverPeer(), w);
        log_(b.animals.empty() ? std::string("animal shop closed without buying") :
                                 "animal purchase asked of the host: " + std::to_string(b.animals.size()) + " animal(s), " + std::to_string(c.price) + " cats shown");
    }
    scratchAnimalChoices_.clear();
}

} // namespace kc
