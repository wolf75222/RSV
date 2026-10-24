#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/arrays.hpp"
#include "../api/slurmjobs.hpp"
#include "../api/usage.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

/* Limits (u): for each of your accounts, its limits (sacctmgr) and what your jobs use now (squeue)
- Every account is shown: on ROMEO "default" allows no job (MaxSubmit=0), --account is needed
*/
struct QuotaView {
    std::vector<api::Assoc> assocs;
    std::vector<api::Share> shares;
    std::vector<api::DiskQuota> disks;
    std::string user, error, disk_error;
    bool loading = true;
    std::function<void()> reload;

    bool onEvent(Event e, bool& close) {
        if (e == Event::Character('r')) {
            loading = true;
            if (reload) reload();
            return true;
        }
        if (e.is_mouse()) return true;
        close = true;
        return true;
    }

    Element render(int max_width, const std::vector<api::Job>& jobs) {
        const int width = std::min(max_width, 90);
        const int inner = width - 4;
        const int bar_w = std::clamp(inner - 56, 6, 24);

        struct Use {
            long cpus = 0, gpus = 0, nodes = 0, running = 0, submitted = 0;
        };
        std::map<std::string, Use> use;
        for (const auto& j : jobs) {
            Use& u = use[j.account];
            if (j.active()) {
                u.cpus += j.cpus;
                u.gpus += j.gpus;
                u.nodes += j.nodes;
                u.running++;
            }
            if (j.active() || j.pending()) u.submitted += api::taskCount(j.id);
        }

        auto gauge_line = [&](const std::string& label, long used, double limit, const std::string& unit) {
            const double ratio = limit > 0 ? used / limit : 0;
            return hbox({text("  " + util::padRight(label, 11)) | dim, bar(ratio, bar_w, usageColor(ratio)),
                         text(" " + util::padLeft(std::to_string(used), 5) + " / " + std::to_string(static_cast<long>(limit)) +
                              " " + unit)});
        };

        Elements rows;
        rows.push_back(hbox({text("User ") | dim, text(user) | bold}));
        for (const auto& a : assocs) {
            const Use& u = use.count(a.account) ? use.at(a.account) : Use{};
            rows.push_back(text(""));
            Elements head = {text(a.account) | bold | color(theme::cyan())};
            if (!a.partition.empty()) head.push_back(text("  partition " + a.partition) | dim);
            if (!a.qos.empty()) head.push_back(text("  QOS " + a.qos) | dim);
            rows.push_back(hbox(std::move(head)));

            bool any = false;
            if (a.max_submit == 0 || a.max_jobs == 0) {
                rows.push_back(text("  No job allowed with this account: give --account=<another one>") | color(theme::yellow()));
                continue;
            }
            const auto limit = [&](const char* key) {
                if (a.grp_tres.count(key)) return a.grp_tres.at(key);
                return 0.0;
            };
            if (limit("cpu") > 0) {
                rows.push_back(gauge_line("CPUs", u.cpus, limit("cpu"), "running"));
                any = true;
            }
            if (limit("gres/gpu") > 0) {
                rows.push_back(gauge_line("GPUs", u.gpus, limit("gres/gpu"), "running"));
                any = true;
            }
            if (limit("node") > 0) {
                rows.push_back(gauge_line("Nodes", u.nodes, limit("node"), "running"));
                any = true;
            }
            if (a.max_jobs > 0 || a.grp_jobs > 0) {
                rows.push_back(gauge_line("Jobs", u.running, a.max_jobs > 0 ? a.max_jobs : a.grp_jobs, "running"));
                any = true;
            }
            if (a.max_submit > 0) {
                rows.push_back(gauge_line("Submitted", u.submitted, a.max_submit, "max"));
                any = true;
            }
            if (a.max_wall > 0) {
                rows.push_back(hbox({text("  " + util::padRight("Time", 11)) | dim, text(util::duration(a.max_wall) + " per job at most")}));
                any = true;
            }
            if (!any) rows.push_back(text("  No limit set for you here (the QOS and partitions still have theirs)") | dim);

            // Fairshare: 1 when the account used less than its share, near 0 when much more (its jobs then wait longer)
            for (const auto& s : shares) {
                if (s.account != a.account || s.fairshare < 0) continue;
                const Color c = s.fairshare >= 0.5 ? theme::green() : s.fairshare >= 0.1 ? theme::yellow() : theme::red();
                Elements line = {text("  " + util::padRight("Fairshare", 11)) | dim, bar(s.fairshare, bar_w, c),
                                 text(" " + std::to_string(s.fairshare).substr(0, 5)) | bold};
                if (s.raw_usage >= 0) line.push_back(text("  " + thousands(static_cast<long>(s.raw_usage / 3600)) + " CPU-hours used (decaying)") | dim);
                rows.push_back(hbox(std::move(line)));
            }
        }

        // Disk quotas (GPFS): space and number of files, for you and your project groups
        if (!disks.empty() || !disk_error.empty()) {
            rows.push_back(text(""));
            rows.push_back(text("Disk") | bold | color(theme::cyan()));
            if (!disk_error.empty()) rows.push_back(text("  " + disk_error) | dim);
            for (const auto& q : disks) {
                const double space = q.quota_kib > 0 ? q.used_kib / q.quota_kib : 0;
                const double files = q.files_quota > 0 ? q.files / q.files_quota : 0;
                const bool over = (q.grace != "none" && !q.grace.empty()) || (q.files_grace != "none" && !q.files_grace.empty());
                std::string label = q.fileset + (q.type == "GRP" ? " " + q.name : "");
                Elements line = {text("  " + util::padRight(label, 18)) | (over ? color(theme::red()) : Decorator(dim)),
                                 bar(std::max(space, files), bar_w, usageColor(std::max(space, files))),
                                 text(" " + util::mib(q.used_kib / 1024) + (q.quota_kib > 0 ? " / " + util::mib(q.quota_kib / 1024) : "")),
                                 text(" · " + thousands(static_cast<long>(q.files)) +
                                      (q.files_quota > 0 ? " / " + thousands(static_cast<long>(q.files_quota)) : "") + " files") | dim};
                rows.push_back(hbox(std::move(line)));
                if (over)
                    rows.push_back(text("    over the quota: " + std::string(q.files_grace != "none" ? "files" : "space") + ", grace " +
                                        (q.files_grace != "none" ? q.files_grace : q.grace) + " (then nothing can be written)") |
                                   color(theme::red()));
            }
        }
        if (loading && assocs.empty()) rows.push_back(text("Reading sacctmgr...") | dim);
        if (!error.empty()) rows.push_back(paragraph(error) | color(theme::red()));
        if (!loading && error.empty() && assocs.empty()) rows.push_back(text("No account found for you in Slurm") | dim);

        rows.push_back(text(""));
        rows.push_back(text("Running counts come from your jobs in the queue now.") | dim);
        rows.push_back(hbox(keyHints({{"r", "refresh"}, {"any key", "close"}})));
        return panel("Accounts, limits and quotas", vbox(std::move(rows)) | size(WIDTH, EQUAL, width - 2));
    }
};

}  // namespace ui
