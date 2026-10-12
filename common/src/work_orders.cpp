// Work orders given to players' characters: see kc/work_orders.h.
#include "kc/work_orders.h"

#include <algorithm>

#include "kc/protocol.h"

namespace kc {

namespace {
const char* const kHowPermanent = "Maj + clic droit (bouton JOBS allumé)";
}

std::string WorkOrderNote(const WorkOrderFacts& f) {
    const std::string name = f.machineName.empty() ? std::string("Cette machine") : f.machineName;
    const bool full = f.machine && f.maxOperators > 0 && f.operators >= f.maxOperators && !f.atWorkThere;
    const std::string places = std::to_string(f.operators) + "/" + std::to_string(f.maxOperators);
    if (f.permanent) {
        if (f.jobsAfter <= f.jobsBefore) {
            if (f.hadSameKind) return "Ce travail est déjà dans la liste de ton personnage (panneau Tâches) : rien d'ajouté.";
            return "Aucun travail permanent ajouté : le jeu n'en fait pas un travail permanent sur cette cible.";
        }
        if (!f.jobsModeOn)
            return "Travail permanent ajouté, mais le bouton JOBS de ce personnage est éteint : il ne fera aucun de ses travaux tant "
                   "qu'il n'est pas allumé.";
        if (full)
            return name + " : travail permanent ajouté. Toutes les places sont prises (" + places +
                   ") : ton personnage s'y mettra dès qu'une place se libère.";
        if (f.machine && f.noWorkNow && !f.atWorkThere) {
            if (f.farm) return name + " : travail permanent ajouté. Les cultures poussent : ton personnage y retournera pour la récolte.";
            return name + " : travail permanent ajouté. Rien à faire pour l'instant : ton personnage y retournera dès qu'il y aura du travail.";
        }
        return {};
    }
    if (!f.machine) return {};
    if (f.atWorkThere) return name + " : ton personnage y travaille déjà. Pour en faire un travail permanent : " + kHowPermanent + ".";
    if (f.noWorkNow) {
        if (f.farm)
            return name + " : les cultures poussent, il n'y a rien à récolter pour l'instant. Donne-lui plutôt ce travail en permanent (" +
                   kHowPermanent + ") : ton personnage y retournera pour la récolte.";
        return name + " n'a pas de travail pour l'instant (rien à produire, à fabriquer ou à rechercher). En travail permanent (" +
               kHowPermanent + "), ton personnage y retournera dès qu'il y en aura.";
    }
    if (full)
        return name + " : toutes les places sont prises (" + places + "). En travail permanent (" + kHowPermanent +
               "), ton personnage attendra qu'une place se libère.";
    return {};
}

uint16_t ModeBitOfOrder(int order) {
    switch (order) {
    case 11: return kModeDefensive;
    case 12: return kModeHold;
    case 13: return kModePassive;
    case 14: return kModeTaunt;
    case 15: return kModeChase;
    case 17: return kModeRanged;
    default: return 0;
    }
}

void ModePredictions::Predict(const std::string& who, uint16_t bit, bool on, double now) {
    if (!bit) return;
    auto& v = map_[who];
    v.erase(std::remove_if(v.begin(), v.end(), [&](const P& p) { return p.bit == bit; }), v.end());
    v.push_back({bit, on, now + kHold});
    if (map_.size() > 256) {   // characters gone: never more than a few predictions in flight
        for (auto it = map_.begin(); it != map_.end();) {
            auto& w = it->second;
            w.erase(std::remove_if(w.begin(), w.end(), [&](const P& p) { return p.until <= now; }), w.end());
            it = w.empty() ? map_.erase(it) : std::next(it);
        }
    }
}

uint16_t ModePredictions::Filter(const std::string& who, uint16_t hostModes, double now) {
    auto it = map_.find(who);
    if (it == map_.end()) return hostModes;
    auto& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const P& p) { return p.until <= now; }), v.end());
    for (const P& p : v) hostModes = uint16_t(p.on ? (hostModes | p.bit) : (hostModes & ~p.bit));
    if (v.empty()) map_.erase(it);
    return hostModes;
}

bool ModePredictions::Pending(const std::string& who, uint16_t bit, double now, bool* on) const {
    auto it = map_.find(who);
    if (it == map_.end()) return false;
    for (const P& p : it->second)
        if (p.bit == bit && p.until > now) {
            if (on) *on = p.on;
            return true;
        }
    return false;
}

bool OrderDebounce::Admit(const std::string& key, double now) {
    auto it = last_.find(key);
    if (it != last_.end() && now - it->second < kWindow && now >= it->second) {
        ++dropped_;
        return false;   // the window runs from the order sent, not from the last click
    }
    if (last_.size() > 512) {
        for (auto i = last_.begin(); i != last_.end();) i = now - i->second >= kWindow ? last_.erase(i) : std::next(i);
        if (last_.size() > 512) last_.clear();
    }
    last_[key] = now;
    return true;
}

bool NextJobMove(const std::vector<int>& rank, int& from, int& to) {
    const int n = int(rank.size());
    for (int p = 0; p < n; ++p) {
        int best = p;
        for (int j = p + 1; j < n; ++j)
            if (rank[size_t(j)] < rank[size_t(best)]) best = j;   // the first of the smallest: equal ranks keep their order
        if (best != p) {
            from = best;
            to = p;
            return true;
        }
    }
    return false;
}

void ApplyJobMove(std::vector<int>& list, int from, int to) {
    if (from < 0 || from >= int(list.size())) return;
    const int v = list[size_t(from)];
    list.erase(list.begin() + from);
    if (to < 0 || to >= int(list.size())) list.push_back(v);
    else list.insert(list.begin() + to, v);
}

} // namespace kc
