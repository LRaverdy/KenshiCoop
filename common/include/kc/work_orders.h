// Work orders given to players' characters (v0.3.4): what the host tells a player after one, the
// client's prediction of the squad bar's toggles (JOBS...), identical orders clicked again and
// again, and the order of a job list made like the host's. Pure logic, no game access.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kc {

// What the host saw of an order it ran for a client's character (a base work task: a machine, a
// farm, hauling, building, medic...). WorkOrderNote turns it into what the player is told.
struct WorkOrderFacts {
    int task = 0;
    bool permanent = false;      // asked as a permanent job (Shift + right click, the Tâches panel)
    int jobsBefore = 0, jobsAfter = 0;   // the character's job list (Tâches panel) before and after
    bool hadSameKind = false;    // a job of that kind was already in its list
    bool jobsModeOn = true;      // its JOBS toggle (standing order 15): off, it does none of its jobs
    // the subject, when it is a machine (UseableStuff: farm, bench, mine, research, generator...)
    bool machine = false;
    std::string machineName;
    bool atWorkThere = false;    // already one of the machine's operators
    bool noWorkNow = false;      // UseableStuff::dontNeedWorkRightNow
    bool farm = false;
    int operators = 0, maxOperators = 0;
};
// The French note for the player (empty: nothing worth saying, the result shows by itself).
std::string WorkOrderNote(const WorkOrderFacts& f);

// The squad bar's toggles (BLOCK 11, HOLD 12, PASSIVE 13, TAUNT 14, JOBS 15, RANGED 17) as ModeBits;
// 0 for any other standing order.
uint16_t ModeBitOfOrder(int order);

// Client: a toggle clicked here is set on our copy at once and kept, whatever the host's periodic
// modes say, for a short while: the host's state then wins (its answer to the order is in by then,
// a refused order is undone). A second click before the host's state arrived toggles back from
// what the player sees, not from the host's old value (0.3.3: two clicks on JOBS turned it off).
class ModePredictions {
public:
    static constexpr double kHold = 3.0;
    void Predict(const std::string& who, uint16_t bit, bool on, double now);
    // The host's modes with the toggles predicted here still held; expired predictions are dropped.
    uint16_t Filter(const std::string& who, uint16_t hostModes, double now);
    bool Pending(const std::string& who, uint16_t bit, double now, bool* on = nullptr) const;
    void Forget(const std::string& who) { map_.erase(who); }
    size_t size() const { return map_.size(); }

private:
    struct P { uint16_t bit; bool on; double until; };
    std::unordered_map<std::string, std::vector<P>> map_;
};

// Client: the same order clicked again and again (0.3.3: 60 "operate" clicks in 10 s on one machine)
// goes to the host once per window; the others are dropped here.
class OrderDebounce {
public:
    static constexpr double kWindow = 1.0;
    // True: send it. `key` names the order exactly (character, way, task, subject, flags).
    bool Admit(const std::string& key, double now);
    uint64_t dropped() const { return dropped_; }

private:
    std::unordered_map<std::string, double> last_;
    uint64_t dropped_ = 0;
};

// A job list made like the host's: rank[i] = the place in the host's list of our job i (jobs the
// host does not have, kept a moment, rank after all of them). The next Character::movePermajob(from,
// to) to make (remove at `from`, insert at `to`), or false when the order is right. Called again after
// each move; at most rank.size() moves in all.
bool NextJobMove(const std::vector<int>& rank, int& from, int& to);
// The game's movePermajob on a list (tests).
void ApplyJobMove(std::vector<int>& list, int from, int to);

} // namespace kc
