#pragma once

#include <cstdlib>
#include <ctime>
#include <map>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "../api/slurmjobs.hpp"
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

/* The details panel, as lines (the panel scrolls)
- `used` is the run time now: squeue's value plus the time since it was read
*/
inline Elements jobDetails(const api::Job& j, const api::JobDetails* d, const std::map<std::string, api::NodeInfo>& nodes,
                           long used, int width, const std::string& details_error) {
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
            line.push_back(text("  " + util::relative(j.end, now)) | dim);
        }
        out.push_back(field("State", hbox(std::move(line))));
    }

    if (j.active() && j.limit > 0 && used >= 0) {
        const double ratio = double(used) / j.limit;
        const int w = std::clamp(value_width - 22, 6, 40);
        out.push_back(field("", hbox({bar(ratio, w, ratio > 0.9 ? theme::yellow() : theme::green()),
                                      text("  " + util::duration(used) + " / " + util::duration(j.limit)) | dim})));
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
    }

    // The constraint (armgpu, x64cpu on ROMEO) comes with scontrol's details
    const std::string features = d ? d->get("Features") : "";
    out.push_back(field("Partition", value(j.partition + (j.qos.empty() ? "" : " · QOS " + j.qos) +
                                           (j.account.empty() ? "" : " · " + j.account) +
                                           (features.empty() || features == "(null)" ? "" : " · " + features))));

    std::string res = std::to_string(j.nodes) + (j.nodes == 1 ? " node" : " nodes") + " · " + std::to_string(j.cpus) + " CPUs";
    if (j.gpus > 0) res += " · " + std::to_string(j.gpus) + " GPUs";
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
    if (!log_out.empty()) out.push_back(field("Output", text(util::cutMiddle(shortPath(log_out), value_width))));
    if (!log_err.empty()) out.push_back(field("Errors", text(util::cutMiddle(shortPath(log_err), value_width))));

    // Nodes
    if (d && !d->alloc.empty()) {
        for (auto& e : nodeDetails(*d, nodes, width)) out.push_back(e);
    } else if (j.active() && d == nullptr) {
        out.push_back(text(""));
        out.push_back(text(details_error.empty() ? "Reading the allocation..." : details_error) | dim);
    }
    return out;
}

}  // namespace ui
