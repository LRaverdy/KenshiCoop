// Fix G5: the job list of the players' characters (the Tâches panel: follow, operate a machine...).
//
// The host's game runs every job. A client removing or moving a job in its panel asks the host (a
// Task command, see TaskVia::RemovePermajob); the host reads the job list of every squad member
// once a second and sends the lists that changed (all of them every 10 s). A client removes from
// its own copy the jobs the host no longer has, so a job removed on the host (or by its game) is
// gone there too and never comes back.
#include "kc/session.h"

#include <algorithm>

namespace kc {

namespace {
constexpr double kJobsInterval = 1.0;
constexpr double kJobsFull = 10.0;
constexpr double kJobsReapply = 2.0;
} // namespace

void Session::ResetJobs() {
    jobsSent_.clear();
    hostJobs_.clear();
    jobsDirty_.clear();
    nextJobs_ = jobsFullAt_ = jobsReapplyAt_ = 0;
}

void Session::HostJobs(double now) {
    if (now < nextJobs_) return;
    nextJobs_ = now + kJobsInterval;
    const bool full = now >= jobsFullAt_;
    if (full) jobsFullAt_ = now + kJobsFull;
    JobListMsg m;
    for (auto& [id, e] : entities_) {
        if (!e.squad || e.container) continue;
        std::vector<int32_t> jobs;
        if (!world_.ReadJobs(e.handle, jobs)) continue;
        if (jobs.size() > kMaxJobsPerCharacter) jobs.resize(kMaxJobsPerCharacter);
        auto it = jobsSent_.find(id);
        const bool changed = it == jobsSent_.end() || it->second != jobs;
        if (changed && it != jobsSent_.end())
            log_("jobs: " + world_.CharacterNameOf(e.handle) + " now has " + std::to_string(jobs.size()) + " job(s)");
        if (changed || full) m.entries.push_back({id, jobs});
        jobsSent_[id] = std::move(jobs);
    }
    for (auto it = jobsSent_.begin(); it != jobsSent_.end();) it = entities_.count(it->first) ? std::next(it) : jobsSent_.erase(it);
    for (size_t i = 0; i < m.entries.size(); i += kMaxJobLists) {
        JobListMsg part;
        part.entries.assign(m.entries.begin() + ptrdiff_t(i), m.entries.begin() + ptrdiff_t(std::min(m.entries.size(), i + size_t(kMaxJobLists))));
        Writer w;
        Encode(w, part);
        BroadcastReliable(w, true);
    }
    // the same lists with each job's target (JobState): a client builds the same list (adds too)
    JobStateMsg js;
    for (auto& [id, e] : entities_) {
        if (!e.squad || e.container) continue;
        std::vector<JobEntry> jobs;
        if (!world_.ReadJobList(e.handle, jobs)) continue;
        if (jobs.size() > kMaxJobsPerCharacter) jobs.resize(kMaxJobsPerCharacter);
        auto it = jobListSent_.find(id);
        const bool changed = it == jobListSent_.end() || it->second != jobs;
        if (changed || full) js.chars.push_back({id, jobs});
        jobListSent_[id] = std::move(jobs);
    }
    for (auto it = jobListSent_.begin(); it != jobListSent_.end();) it = entities_.count(it->first) ? std::next(it) : jobListSent_.erase(it);
    for (size_t i = 0; i < js.chars.size(); i += kMaxJobLists) {
        JobStateMsg part;
        part.chars.assign(js.chars.begin() + ptrdiff_t(i), js.chars.begin() + ptrdiff_t(std::min(js.chars.size(), i + size_t(kMaxJobLists))));
        Writer w(4096);
        Encode(w, part);
        BroadcastReliable(w, true);
    }
}

void Session::ClientJobsPacket(Reader& r) {
    JobListMsg m;
    if (state_ != SessionState::Connected || !Decode(r, m) || haveJobState_) return;   // JobState says it all
    for (auto& e : m.entries) {
        jobsDirty_.insert(e.netId);
        hostJobs_[e.netId] = std::move(e.jobs);
    }
}

void Session::ClientJobs(double now) {
    const bool reapply = now >= jobsReapplyAt_;
    if (reapply) jobsReapplyAt_ = now + kJobsReapply;
    for (auto it = hostJobs_.begin(); it != hostJobs_.end();) {
        auto e = entities_.find(it->first);
        if (e == entities_.end()) { jobsDirty_.erase(it->first); it = hostJobs_.erase(it); continue; }
        if (e->second.present && (reapply || jobsDirty_.count(it->first))) {
            world_.ApplyJobs(e->second.handle, it->second);
            jobsDirty_.erase(it->first);
        }
        ++it;
    }
    for (auto it = hostJobLists_.begin(); it != hostJobLists_.end();) {
        auto e = entities_.find(it->first);
        if (e == entities_.end()) { jobListDirty_.erase(it->first); it = hostJobLists_.erase(it); continue; }
        if (e->second.present && (reapply || jobListDirty_.count(it->first))) {
            world_.ApplyJobList(e->second.handle, it->second);
            jobListDirty_.erase(it->first);
        }
        ++it;
    }
}

} // namespace kc
