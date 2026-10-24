#pragma once

#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "../api/slurmjobs.hpp"
#include "../api/usage.hpp"
#include "joblist.hpp"
#include "nodedetails.hpp"
#include "reason_decoder.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// /gpfs/home/demo/runs/x.out -> ~/runs/x.out (on ROMEO, Slurm shows homes under /gpfs)
inline std::string shortPath(const std::string& raw) {
    const std::string path = util::sanitize(raw);
    static const std::string home = [] {
        const char* h = std::getenv("HOME");
        return std::string(h ? h : "");
    }();
    if (home.size() > 1) {
        for (const std::string& prefix : {home, "/gpfs" + home}) {
            if (path == prefix) return "~";
            if (util::startsWith(path, prefix + "/")) return "~" + path.substr(prefix.size());
        }
    }
    return path;
}

// Where Slurm writes the job's output (and errors): from scontrol when it is there, else from squeue
inline std::pair<std::string, std::string> logPaths(const api::Job& j, const api::JobDetails* d) {
    std::string out = j.stdout_path, err;
    if (!d && out.find('%') != std::string::npos) {
        // Patterns left by squeue, before scontrol's details arrive: filled from what squeue gave
        api::JobDetails known;
        known.fields = {{"JobId", j.job_id}, {"JobName", j.name}, {"WorkDir", j.workdir}, {"UserId", api::currentUser()}};
        const size_t bar = j.id.find('_');
        if (bar != std::string::npos) {
            known.fields["ArrayJobId"] = j.id.substr(0, bar);
            known.fields["ArrayTaskId"] = j.id.substr(bar + 1);
        }
        out = api::expandSlurmPath(out, known);
    }
    if (d) {
        const std::string o = api::expandSlurmPath(d->get("StdOut"), *d);
        const std::string e = api::expandSlurmPath(d->get("StdErr"), *d);
        if (!o.empty()) out = o;
        if (!e.empty()) err = e;
    }
    if (err == out) err.clear();
    return {out, err};
}

inline std::string ordinal(int n) {
    const int last2 = n % 100, last = n % 10;
    const char* suffix = (last2 >= 11 && last2 <= 13) ? "th" : last == 1 ? "st" : last == 2 ? "nd" : last == 3 ? "rd" : "th";
    return std::to_string(n) + suffix;
}

/* The details panel, as lines (the panel scrolls)
- `used` is the run time now: squeue's value plus the time since it was read
- `live`: what the running steps use now (sstat), `rank`: the job's place among the pending jobs (sprio)
*/
inline Elements jobDetails(const api::Job& j, const api::JobDetails* d, const std::map<std::string, api::NodeInfo>& nodes,
                           long used, const api::LiveUse* live, const api::Rank* rank, int width,
                           const std::string& details_error) {
    const std::time_t now = std::time(nullptr);
    const int value_width = std::max(10, width - 11);
    auto value = [&](const std::string& s) { return text(util::cut(s, static_cast<size_t>(value_width))); };
    Elements out;

    out.push_back(text(util::cut(j.name, static_cast<size_t>(width))) | bold);
    out.push_back(text(""));

    // State, and how long
    {
        Elements line = {text(j.state) | bold | color(stateColor(j.state))};
        if (j.active() && used >= 0) {
            line.push_back(text("  for " + util::duration(used)));
            if (j.limit > 0) line.push_back(text(", " + util::duration(std::max(0L, j.limit - used)) + " left") | dim);
        } else if (j.pending() && j.submit > 0) {
            line.push_back(text("  for " + util::duration(static_cast<long>(std::difftime(now, j.submit)))) | dim);
        } else if (j.finished() && j.end > 0) {
            line.push_back(text("  ended " + util::duration(std::max(0L, static_cast<long>(std::difftime(now, j.end)))) + " ago") | dim);
        }
        out.push_back(field("State", hbox(std::move(line))));
    }

    if (j.active() && j.limit > 0 && used >= 0) {
        const double ratio = double(used) / j.limit;
        const int w = std::clamp(value_width - 22, 6, 40);
        out.push_back(field("", hbox({bar(ratio, w, ratio > 0.9 ? theme::yellow() : theme::green()),
                                      text("  " + util::duration(used) + " / " + util::duration(j.limit)) | dim})));
    }

    /* Live: how busy its CPUs are and the memory of the running steps
    - Busy since the reading before; for the first one, since the job started (until that reading, not until now:
      the CPU time doesn't move between readings, the clock does)
    */
    if (j.state == "RUNNING" && live && used > 0 && j.cpus > 0) {
        double cpu = live->busy;
        if (cpu < 0) {
            const double until_reading = std::max(1.0, double(used) - (double(now) - live->at));
            cpu = std::min(1.0, live->cpu_seconds / (until_reading * j.cpus));
        }
        Elements line = {text("CPU ") | dim, text(percent(cpu)) | bold | color(efficiencyColor(cpu)),
                         text(" busy · memory ") | dim, text(util::mib(live->mem_mib)) | bold};
        if (j.mem_mib > 0) line.push_back(text(" of " + util::mib(j.mem_mib)) | dim);
        if (live->max_task_mib > 0) line.push_back(text(" · max task " + util::mib(live->max_task_mib)) | dim);
        out.push_back(field("Now", hbox(std::move(line))));
    }

    if (j.pending()) {
        const ui::ReasonInfo info = decodeReason(j.reason);
        out.push_back(field("Reason", value(j.reason) | color(theme::yellow())));
        out.push_back(field("", value(info.description)));
        if (!info.suggestion.empty()) out.push_back(field("", text(util::cut("> " + info.suggestion, value_width)) | dim));
        if (j.start > now)
            out.push_back(field("Starts", hbox({text("~" + util::clock(j.start, now)), text("  " + util::relative(j.start, now)) | dim})));
        else
            out.push_back(field("Starts", text("not estimated yet") | dim));
        if (!j.dependency.empty()) out.push_back(field("Depends", value(j.dependency)));
        if (rank && rank->of > 0) {
            // Its place in its partition, and what its priority is made of (only the factors that count)
            std::string parts;
            auto add = [&](const char* name, double v) {
                if (v > 0) parts += (parts.empty() ? "" : " · ") + std::string(name) + " " + std::to_string(static_cast<long>(v + 0.5));
            };
            add("fairshare", rank->mine.fairshare);
            add("age", rank->mine.age);
            add("partition", rank->mine.partition_factor);
            add("size", rank->mine.jobsize);
            add("QOS", rank->mine.qos);
            add("site", rank->mine.site);
            out.push_back(field("Priority", hbox({text(std::to_string(static_cast<long>(rank->mine.priority + 0.5))) | bold,
                                                  text("  " + ordinal(rank->rank) + " of " + std::to_string(rank->of) + " waiting in " +
                                                       rank->mine.partition) | color(rank->rank <= 3 ? theme::green() : theme::yellow())})));
            if (!parts.empty()) out.push_back(field("", text(util::cut(parts, static_cast<size_t>(value_width))) | dim));
        }
    }

    // The constraint (armgpu, x64cpu on ROMEO) comes with scontrol's details
    const std::string features = d ? d->get("Features") : "";
    out.push_back(field("Partition", value(j.partition + (j.qos.empty() ? "" : " · QOS " + j.qos) +
                                           (j.account.empty() ? "" : " · " + j.account) +
                                           (features.empty() || features == "(null)" ? "" : " · " + features))));

    std::string res = std::to_string(j.nodes) + (j.nodes == 1 ? " node" : " nodes") + " · " + std::to_string(j.cpus) + (j.cpus == 1 ? " CPU" : " CPUs");
    if (j.gpus > 0) res += " · " + std::to_string(j.gpus) + (j.gpus == 1 ? " GPU" : " GPUs");
    if (j.mem_mib > 0) res += " · " + util::mib(j.mem_mib);
    out.push_back(field(j.pending() ? "Asks" : "Uses", value(res)));

    {
        std::string times = "submitted " + util::clock(j.submit, now);
        if (!j.pending() && j.start > 0) times += " · started " + util::clock(j.start, now);
        if (j.active() && j.limit > 0 && used >= 0) times += " · ends " + util::clock(now + std::max(0L, j.limit - used), now);
        else if (j.finished() && j.end > 0) times += " · ended " + util::clock(j.end, now);
        out.push_back(field("Times", value(times) | dim));
    }

    if (d && j.finished()) {
        const std::string code = d->get("ExitCode");
        if (!code.empty())
            out.push_back(field("Exit code", value(code) | color(code == "0:0" ? theme::green() : theme::red())));
    }

    const std::string dir = d && !d->get("WorkDir").empty() ? d->get("WorkDir") : j.workdir;
    if (!dir.empty()) out.push_back(field("Directory", text(util::cutMiddle(shortPath(dir), value_width))));
    const auto [log_out, log_err] = logPaths(j, d);
    if (!log_out.empty()) out.push_back(field("Output", text(util::cutMiddle(shortPath(log_out), value_width)) | fileLink(log_out)));
    if (!log_err.empty()) out.push_back(field("Errors", text(util::cutMiddle(shortPath(log_err), value_width)) | fileLink(log_err)));

    // Nodes
    if (d && !d->alloc.empty()) {
        for (auto& e : nodeDetails(*d, nodes, width)) out.push_back(e);
    } else if (j.active() && d == nullptr) {
        out.push_back(text(""));
        out.push_back(text(details_error.empty() ? "Reading the allocation..." : details_error) | dim);
    }
    return out;
}

// 151, 152, 153, 160 -> "151-153,160"
inline std::string indexRanges(std::vector<long> ids) {
    std::sort(ids.begin(), ids.end());
    std::string out;
    for (size_t i = 0; i < ids.size();) {
        size_t k = i;
        while (k + 1 < ids.size() && ids[k + 1] == ids[k] + 1) k++;
        out += (out.empty() ? "" : ",") + std::to_string(ids[i]) + (k > i ? "-" + std::to_string(ids[k]) : "");
        i = k + 1;
    }
    return out;
}

/* The details of an array: its progress, the tasks that failed, those running, why the others wait
- Each task has its own details and log: Enter shows them in the list
*/
inline Elements arrayDetails(const JobArray& a, int width, const std::string& past_error) {
    const std::time_t now = std::time(nullptr);
    const int value_width = std::max(10, width - 11);
    auto value = [&](const std::string& s) { return text(util::cut(s, static_cast<size_t>(value_width))); };
    const api::ArrayProgress& p = a.progress;
    const api::Job& f = a.first();
    Elements out = {text(util::cut(f.name, static_cast<size_t>(width))) | bold, text("")};

    std::string what = std::to_string(p.total()) + (p.total() == 1 ? " task" : " tasks");
    if (p.throttle) what += ", " + std::to_string(p.throttle) + " at a time";
    out.push_back(field("Array", hbox({text(a.state()) | bold | color(stateColor(a.state())), text("  " + what) | dim})));
    out.push_back(field("", progressBar(p, std::clamp(value_width, 10, 40))));
    Elements counts;
    auto count = [&](int n, const std::string& label, Color c) {
        if (n == 0) return;
        if (!counts.empty()) counts.push_back(text(" · ") | dim);
        counts.push_back(text(std::to_string(n)) | bold | color(c));
        counts.push_back(text(" " + label));
    };
    count(p.done, "done", theme::blue());
    count(p.running, "running", theme::green());
    count(p.pending, "waiting", theme::gray());
    count(p.failed, "failed", theme::red());
    count(p.cancelled, "cancelled", theme::magenta());
    out.push_back(field("", hbox(std::move(counts))));
    if (!a.past_known && !past_error.empty())
        out.push_back(field("", text(util::cut("Tasks ended over 5 min ago not counted: " + past_error, static_cast<size_t>(value_width))) | dim));

    if (!p.failures.empty()) {
        std::string list;
        size_t shown = 0;
        for (const auto& [id, how] : p.failures) {
            const std::string item = id.substr(id.find('_') + 1) + " " + how;
            if (shown > 0 && util::width(list + " · " + item) + 12 > static_cast<size_t>(value_width)) break;
            list += (list.empty() ? "" : " · ") + item;
            shown++;
        }
        if (shown < p.failures.size()) list += " +" + std::to_string(p.failures.size() - shown);
        out.push_back(field("Failed", value(list) | color(theme::red())));
    }

    std::vector<long> running;
    std::set<std::string> nodes;
    for (const api::Job* t : a.tasks)
        if (t->active()) {
            running.push_back(api::taskIndex(t->id));
            for (const auto& h : api::expandHostlist(t->nodelist)) nodes.insert(h);
        }
    if (!running.empty()) {
        std::string on;
        size_t shown = 0;
        for (const auto& n : nodes) {
            if (shown == 3) break;
            on += (on.empty() ? "" : ",") + n;
            shown++;
        }
        if (nodes.size() > shown) on += " +" + std::to_string(nodes.size() - shown);
        out.push_back(field("Running", value("tasks " + indexRanges(running) + (on.empty() ? "" : " on " + on))));
    }

    if (const api::Job* w = a.waiting()) {
        const ui::ReasonInfo info = decodeReason(w->reason);
        out.push_back(field("Waiting", value(std::to_string(api::taskCount(w->id)) + (api::taskCount(w->id) == 1 ? " task · " : " tasks · ") + w->reason) | color(theme::yellow())));
        out.push_back(field("", value(info.description)));
        if (!info.suggestion.empty()) out.push_back(field("", text(util::cut("> " + info.suggestion, static_cast<size_t>(value_width))) | dim));
        if (w->start > now)
            out.push_back(field("Starts", hbox({text("~" + util::clock(w->start, now)), text("  " + util::relative(w->start, now)) | dim})));
    }

    out.push_back(field("Partition", value(f.partition + (f.qos.empty() ? "" : " · QOS " + f.qos) +
                                           (f.account.empty() ? "" : " · " + f.account))));
    std::string res = std::to_string(f.nodes) + (f.nodes == 1 ? " node" : " nodes") + " · " + std::to_string(f.cpus) + (f.cpus == 1 ? " CPU" : " CPUs");
    if (f.gpus > 0) res += " · " + std::to_string(f.gpus) + (f.gpus == 1 ? " GPU" : " GPUs");
    if (f.mem_mib > 0) res += " · " + util::mib(f.mem_mib);
    if (f.limit > 0) res += " · " + util::duration(f.limit);
    out.push_back(field("Each task", value(res)));
    if (!f.workdir.empty()) out.push_back(field("Directory", text(util::cutMiddle(shortPath(f.workdir), static_cast<size_t>(value_width)))));
    out.push_back(text(""));
    out.push_back(hbox({text("Enter") | bold | color(theme::accent()), text(" its tasks, each with its log  ") | dim,
                        text("c") | bold | color(theme::accent()), text(" cancel all") | dim}));
    return out;
}

}  // namespace ui
