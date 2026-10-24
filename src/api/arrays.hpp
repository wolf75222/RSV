#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "slurmjobs.hpp"

/* Job arrays: one row for all the tasks of an array, with its progress
- squeue knows the tasks running or waiting, and those ended less than MinJobAge ago (5 min on ROMEO); sacct knows
  the others. Both are put together, squeue's word first: it is the fresher.
- The tasks not started yet are one record: 812402_[3-40%4] is 38 tasks, 4 at a time at most
*/
namespace api {

// "812402_7" -> "812402", "812402_[3-40]" -> "812402"; "" for a job that isn't in an array
inline std::string arrayOf(const std::string& id) {
    const size_t bar = id.find('_');
    if (bar == std::string::npos || bar == 0) return "";
    for (size_t i = 0; i < bar; i++)
        if (!std::isdigit(static_cast<unsigned char>(id[i]))) return "";
    return id.substr(0, bar);
}

// The tasks a record stands for: "812402_[3-40%4]" -> 38, "812402_[1,3,5-7]" -> 5, "812402_7" -> 1
inline int taskCount(const std::string& id) {
    const size_t open = id.find("_[");
    if (open == std::string::npos) return 1;
    std::string ranges = id.substr(open + 2, id.find(']', open) - open - 2);
    ranges = ranges.substr(0, ranges.find('%'));
    return std::max(1, static_cast<int>(parseIdList(ranges).size()));
}

// "812402_[3-40%4]" -> 4; 0 without a limit
inline int throttleOf(const std::string& id) {
    const size_t percent = id.find('%');
    return percent == std::string::npos ? 0 : static_cast<int>(util::toLong(id.substr(percent + 1), 0));
}

// "812402_17" -> 17, for the order of the tasks; -1 for a record of several
inline long taskIndex(const std::string& id) {
    const size_t bar = id.find('_');
    if (bar == std::string::npos || id.find('[') != std::string::npos) return -1;
    return util::toLong(id.substr(bar + 1), -1);
}

// A task as sacct remembers it
struct TaskState {
    std::string id;          // 812402_17, or 812402_[3-40] for the tasks not started
    std::string state;       // its first word: CANCELLED by 1234 -> CANCELLED
    std::string exit_code;   // 1:0
};

inline std::vector<TaskState> parseArrayTasks(const std::string& out) {
    std::vector<TaskState> tasks;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, SEP);
        if (f.size() < 3 || arrayOf(util::trim(f[0])).empty()) continue;
        TaskState t;
        t.id = util::trim(f[0]);
        t.state = util::trim(f[1]).substr(0, util::trim(f[1]).find(' '));
        t.exit_code = util::trim(f[2]);
        tasks.push_back(t);
    }
    return tasks;
}

/* Every task of these arrays, ended long ago too: sacct -j 812402,812500
- Each array asked has its entry, empty when sacct doesn't know it yet: it isn't asked again at once
- `all_users`: the project view's arrays are other people's too
*/
inline Fetched<std::map<std::string, std::vector<TaskState>>> fetchArrayTasks(const std::set<std::string>& arrays, bool all_users) {
    Fetched<std::map<std::string, std::vector<TaskState>>> r;
    if (arrays.empty()) return r;
    std::string ids;
    for (const auto& a : arrays) ids += (ids.empty() ? "" : ",") + a;
    std::vector<std::string> args = {"sacct", "-X", "-n", "-P", "--delimiter=" + std::string(1, SEP), "-o", "JobID,State,ExitCode", "-j", ids};
    if (all_users) args.push_back("--allusers");
    const RunResult res = run(args, 20);
    if (!res.ok()) {
        r.error = res.error("sacct");
        return r;
    }
    for (const auto& a : arrays) r.data[a];
    for (auto& t : parseArrayTasks(res.out)) r.data[arrayOf(t.id)].push_back(std::move(t));
    return r;
}

struct ArrayProgress {
    int done = 0, running = 0, pending = 0, failed = 0, cancelled = 0;
    int throttle = 0;                                 // tasks at a time, when the array has a limit (%4)
    std::vector<std::pair<std::string, std::string>> failures;   // task, what happened (FAILED 1:0, TIMEOUT...)
    int total() const { return done + running + pending + failed + cancelled; }

    // `n` tasks in this state (a record of tasks not started stands for several)
    void add(const std::string& id, const std::string& state, const std::string& exit_code, int n) {
        if (isActive(state)) running += n;
        else if (isPending(state)) pending += n;
        else if (state == "COMPLETED") done += n;
        else if (state == "CANCELLED") cancelled += n;
        else {
            failed += n;
            failures.push_back({id, state + (exit_code.empty() || exit_code == "0:0" ? "" : " " + exit_code)});
        }
    }
};

// "812402_[3-40%4]" -> "812402_[3-40]": a throttle isn't part of an id
inline std::string plainId(std::string id) {
    const size_t percent = id.find('%');
    const size_t bracket = id.find(']', percent == std::string::npos ? 0 : percent);
    if (percent != std::string::npos && bracket != std::string::npos) id.erase(percent, bracket - percent);
    return id;
}

/* The progress of one array
- `tasks`: its records in squeue (tasks, and the record of the tasks not started); `past`: sacct's, or nullptr
- sacct's record of the tasks not started is left out while they wait (squeue has the fresh one); cancelled before
  they started, only sacct remembers them once squeue has forgotten
- A task sacct saw running or waiting that squeue has forgotten has ended (sacct was read before, or failed since):
  left out until sacct says how
*/
inline ArrayProgress progressOf(const std::vector<const Job*>& tasks, const std::vector<TaskState>* past) {
    ArrayProgress p;
    std::set<std::string> ranges, queued;
    for (const Job* j : tasks) (j->id.find('[') != std::string::npos ? ranges : queued).insert(plainId(j->id));
    std::map<std::string, std::pair<std::string, std::string>> by_task;   // task -> state, exit code
    if (past)
        for (const auto& t : *past) {
            if (t.id.find('[') == std::string::npos) by_task[t.id] = {t.state, t.exit_code};
            else if (!isPending(t.state) && !ranges.count(plainId(t.id))) p.add(t.id, t.state, t.exit_code, taskCount(t.id));
        }
    for (const Job* j : tasks) {
        if (j->id.find('[') != std::string::npos) {
            p.throttle = std::max(p.throttle, throttleOf(j->id));
            p.add(j->id, j->state, "", taskCount(j->id));
            continue;
        }
        const auto known = by_task.find(j->id);
        by_task[j->id] = {j->state, known == by_task.end() ? "" : known->second.second};
    }
    for (const auto& [id, s] : by_task) {
        if ((isActive(s.first) || isPending(s.first)) && !queued.count(id)) continue;
        p.add(id, s.first, s.second, 1);
    }
    std::sort(p.failures.begin(), p.failures.end(),
              [](const auto& a, const auto& b) { return taskIndex(a.first) < taskIndex(b.first); });
    return p;
}

// The records of an array's waiting tasks, read now: 812402_[161-200] (and a task split from them, held alone)
inline std::vector<std::string> waitingRecords(const std::string& array) {
    std::vector<std::string> out;
    const RunResult res = run({"squeue", "-h", "-j", array, "-t", "PD", "-O", "JobArrayID:60"}, 15);
    for (const auto& line : util::lines(res.out)) {
        const std::string id = plainId(util::trim(line));
        if (arrayOf(id) == array) out.push_back(id);
    }
    return out;
}

}  // namespace api
