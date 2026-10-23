#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/cluster.hpp"
#include "reason_decoder.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// Bytes in a short format: 534T, 2.8P
inline std::string storageSize(double bytes) {
    static const char* units[] = {"B", "K", "M", "G", "T", "P", "E"};
    int u = 0;
    while (bytes >= 1024 && u < 6) {
        bytes /= 1024;
        u++;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, bytes < 10 ? "%.1f%s" : "%.0f%s", bytes, units[u]);
    return buf;
}

// 1993 µs -> "2 ms", 242873 -> "243 ms", 3100000 -> "3.1 s"
inline std::string micros(long us) {
    if (us < 0) return "-";
    char buf[32];
    if (us < 1000) std::snprintf(buf, sizeof buf, "%ld µs", us);
    else if (us < 1000000) std::snprintf(buf, sizeof buf, "%.0f ms", us / 1000.0);
    else std::snprintf(buf, sizeof buf, "%.1f s", us / 1e6);
    return buf;
}

// What a pending reason says about the cluster (decodeReason speaks to the job's owner)
inline std::string reasonMeaning(const std::string& reason) {
    static const std::map<std::string, std::string> meanings = {
        {"Resources", "the free nodes aren't enough: the cluster is full for them"},
        {"Priority", "jobs with a higher priority go first"},
        {"Dependency", "waiting for another job"},
        {"BeginTime", "asked to start later"},
        {"JobHeldUser", "held by their owner"},
        {"JobHeldAdmin", "held by an administrator"},
        {"ReqNodeNotAvail", "a node they need is down, drained or reserved"},
        {"Reservation", "waiting for a reservation"},
        {"JobArrayTaskLimit", "the array's own limit of tasks at once"},
        {"None", "not looked at by the scheduler yet"},
    };
    const auto it = meanings.find(reason);
    if (it != meanings.end()) return it->second;
    if (reason.find("PerUser") != std::string::npos || reason.find("AssocMax") != std::string::npos) return "the user's own limit";
    if (reason.find("Assoc") != std::string::npos) return "a limit of their account";
    if (reason.find("QOS") != std::string::npos) return "a limit of the QOS";
    if (reason.find("Partition") != std::string::npos) return "a limit of the partition";
    return decodeReason(reason).description;
}

/* Cluster (p): what the whole of ROMEO is doing now (every user's jobs are visible there), in three tabs
- Overview: the totals (CPUs, GPUs, load, memory, storage, nodes, Slurm), reservations, partitions and their rules,
  kinds of nodes
- Queue: everyone's jobs by partition, why the pending ones wait and what they ask for, the main users or accounts
- Nodes: the nodes down and why, then every node as a map (■ busy, ■ reserved but idle, . free, x down) or a table
  with its load and memory (t)
- Read again every refresh period while it is open; sdiag and the partition rules once, when it opens
*/
struct ClusterView {
    api::Cluster data;
    api::ClusterExtra extra;
    std::string error;
    bool loading = true;
    std::time_t updated = 0;
    int refresh_seconds = 15;
    int tab = 0;              // 0 overview, 1 queue, 2 nodes
    int offset = 0;
    int page = 20;
    bool table = false;       // nodes as a table instead of a map
    bool by_account = false;  // main accounts instead of main users
    std::function<void()> reload;

    static const std::vector<std::string>& tabs() {
        static const std::vector<std::string> t = {"Overview", "Queue", "Nodes"};
        return t;
    }

    bool onEvent(Event e, bool& close) {
        const int n = static_cast<int>(tabs().size());
        if (e == Event::Escape || e == Event::Character('q') || e == Event::Character('p')) close = true;
        else if (e == Event::Character('r')) {
            loading = true;
            if (reload) reload();
        } else if (e == Event::Tab || e == Event::ArrowRight || e == Event::Character('l')) {
            tab = (tab + 1) % n;
            offset = 0;
        } else if (e == Event::TabReverse || e == Event::ArrowLeft || e == Event::Character('h')) {
            tab = (tab + n - 1) % n;
            offset = 0;
        } else if (e == Event::Character('1') || e == Event::Character('2') || e == Event::Character('3')) {
            tab = e.character()[0] - '1';
            offset = 0;
        } else if (e == Event::Character('t')) table = !table;
        else if (e == Event::Character('a')) by_account = !by_account;
        else if (e == Event::ArrowDown || e == Event::Character('j')) offset++;
        else if (e == Event::ArrowUp || e == Event::Character('k')) offset--;
        else if (e == Event::PageDown || e == Event::Character(' ') || e == Event::CtrlD) offset += page - 1;
        else if (e == Event::PageUp || e == Event::Character('b') || e == Event::CtrlU) offset -= page - 1;
        else if (e == Event::Home || e == Event::Character('g')) offset = 0;
        else if (e == Event::End || e == Event::Character('G')) offset = 1 << 20;
        else if (e.is_mouse() && e.mouse().button == Mouse::WheelDown) offset += 3;
        else if (e.is_mouse() && e.mouse().button == Mouse::WheelUp) offset -= 3;
        else return e.is_mouse();
        offset = std::max(0, offset);
        return true;
    }

    static Element cell(const std::string& s, int w, bool right = false) {
        return text(right ? util::padLeft(s, static_cast<size_t>(w) - 1) + " " : util::padRight(s, static_cast<size_t>(w)));
    }

    static Color useColor(api::NodeUse u) {
        switch (u) {
            case api::NodeUse::Idle: return theme::gray();
            case api::NodeUse::Mixed: return theme::yellow();
            case api::NodeUse::Full: return theme::green();
            default: return theme::red();
        }
    }

    static const char* useName(api::NodeUse u) {
        switch (u) {
            case api::NodeUse::Idle: return "idle";
            case api::NodeUse::Mixed: return "partly";
            case api::NodeUse::Full: return "full";
            default: return "down";
        }
    }

    static Element title(const std::string& s) { return text(s) | bold; }

    // CPUs    ████░░░░  27%  6 696 / 25 152 reserved · 1 200 asked by pending jobs
    static Element gaugeLine(const std::string& label, double ratio, Elements after, int bar_w, Color fill) {
        Elements line = {text(util::padRight(label, 8)) | bold, bar(ratio, bar_w, fill), text(" " + util::padLeft(percent(ratio), 4) + "  ")};
        for (auto& e : after) line.push_back(e);
        return hbox(std::move(line));
    }

    // Nodes columns shared by the partitions and node kinds tables
    static Elements countCells(const api::NodeCounts& c, int bar_w) {
        const double used = c.cpus_total > 0 ? double(c.cpus_alloc) / c.cpus_total : 0;
        return {
            cell(std::to_string(c.nodes), 6, true),
            cell(std::to_string(c.idle), 6, true) | color(theme::gray()),
            cell(std::to_string(c.mixed), 5, true) | color(theme::yellow()),
            cell(std::to_string(c.full), 5, true) | color(theme::green()),
            cell(std::to_string(c.down), 5, true) | (c.down ? color(theme::red()) : Decorator(dim)),
            text(" "),
            bar(used, bar_w, usageColor(used)),
            cell(percent(used), 5, true),
            c.gpus_total > 0 ? cell(std::to_string(c.gpus_used) + "/" + std::to_string(c.gpus_total), 9, true) : cell("-", 9, true) | dim,
        };
    }

    static Elements countHeader(const std::string& first, int first_w, int bar_w) {
        return {cell(first, first_w), cell("NODES", 6, true), cell("IDLE", 6, true), cell("PART", 5, true),
                cell("FULL", 5, true), cell("DOWN", 5, true), text(" "), cell("CPUS IN USE", bar_w + 5), cell("GPUS", 9, true)};
    }

    /* One node of the map: 012 ■■■■■■.. ●●○○
    - ■ green: cores busy (load), ■ yellow: reserved by a job but idle, . free, x down
    - The number takes the node's state color, inverted when one of your jobs runs there
    */
    static Element nodeCell(const api::ClusterNode& n, const std::set<std::string>& mine, int gpu_slots) {
        size_t end = n.name.size();
        while (end > 0 && std::isdigit(static_cast<unsigned char>(n.name[end - 1]))) end--;
        const std::string number = util::padLeft(n.name.substr(end), 3);
        Element num = mine.count(n.name) ? text(number) | bold | color(theme::cyan()) | inverted : text(number) | color(useColor(n.use));

        constexpr int CELLS = 8;
        Element cpus;
        if (n.use == api::NodeUse::Down) {
            cpus = text(std::string(CELLS, 'x')) | color(theme::red()) | dim;
        } else {
            auto cells = [&](double v) {
                int k = n.cpus_total > 0 ? static_cast<int>(v * CELLS / n.cpus_total + 0.5) : 0;
                if (v > 0 && k == 0) k = 1;   // one core still shows
                return std::clamp(k, 0, CELLS);
            };
            const int reserved = cells(double(n.cpus_alloc));
            // The busy part of the reserved cells: a node 96% busy shows all its reserved cells green
            const int busy = n.cpus_alloc > 0 ? std::clamp(static_cast<int>(reserved * n.busyCores() / n.cpus_alloc + 0.5), 0, reserved) : 0;
            std::string busy_s, idle_s;
            for (int i = 0; i < busy; i++) busy_s += "■";
            for (int i = busy; i < reserved; i++) idle_s += "■";
            cpus = hbox({text(busy_s) | color(theme::green()), text(idle_s) | color(theme::yellow()),
                         text(std::string(static_cast<size_t>(CELLS - reserved), '.')) | dim});
        }
        Elements parts = {num, text(" "), cpus};
        if (gpu_slots > 0) {
            std::string on, off;
            for (int i = 0; i < std::min(n.gpus_total, gpu_slots); i++) {
                if (i < n.gpus_used) on += "●";
                else off += "○";
            }
            parts.push_back(text(" "));
            parts.push_back(text(on) | color(theme::yellow()));
            parts.push_back(text(off) | dim);
            for (int i = std::min(n.gpus_total, gpu_slots); i < gpu_slots; i++) parts.push_back(text(" "));
        }
        parts.push_back(text("  "));
        return hbox(std::move(parts));
    }

    Element nodeCountsLine(const api::NodeCounts& all) const {
        return hbox({title(util::padRight("Nodes", 8)), text(std::to_string(all.nodes)) | bold, text(": "),
                     text(std::to_string(all.idle) + " idle") | color(theme::gray()), text(" · "),
                     text(std::to_string(all.mixed) + " partly used") | color(theme::yellow()), text(" · "),
                     text(std::to_string(all.full) + " full") | color(theme::green()), text(" · "),
                     text(std::to_string(all.down) + " down") | color(all.down ? theme::red() : theme::gray())});
    }

    Element jobsLine() const {
        const auto& q = data.queue;
        if (!data.queue_error.empty()) return hbox({title(util::padRight("Jobs", 8)), text(data.queue_error) | color(theme::red())});
        return hbox({title(util::padRight("Jobs", 8)), text(thousands(q.running)) | bold | color(theme::green()), text(" running · "),
                     text(thousands(q.pending)) | bold | color(q.pending ? theme::yellow() : theme::gray()), text(" pending · "),
                     text(std::to_string(q.users.size())) | bold, text(q.users.size() == 1 ? " user" : " users")});
    }

    // ---- 1 Overview: the totals, Slurm, reservations, partitions and their rules, kinds of nodes
    void overview(Elements& lines, const api::NodeCounts& all, int inner, int bar_w, bool wide, std::time_t now) const {
        const auto& q = data.queue;
        const int gauge = bar_w + 12;
        lines.push_back(jobsLine());

        const double cpu_ratio = all.cpus_total ? double(all.cpus_alloc) / all.cpus_total : 0;
        Elements cpu_after = {text(thousands(all.cpus_alloc)) | bold, text(" / " + thousands(all.cpus_total) + " reserved") | dim};
        if (q.cpus_pending > 0) cpu_after.push_back(text(" · " + thousands(q.cpus_pending) + " asked by pending jobs") | color(theme::yellow()));
        lines.push_back(gaugeLine("CPUs", cpu_ratio, std::move(cpu_after), gauge, usageColor(cpu_ratio)));

        if (all.gpus_total > 0) {
            const double gpu_ratio = double(all.gpus_used) / all.gpus_total;
            Elements gpu_after = {text(thousands(all.gpus_used)) | bold, text(" / " + thousands(all.gpus_total) + " in use") | dim};
            if (q.gpus_pending > 0) gpu_after.push_back(text(" · " + thousands(q.gpus_pending) + " asked by pending jobs") | color(theme::yellow()));
            lines.push_back(gaugeLine("GPUs", gpu_ratio, std::move(gpu_after), gauge, usageColor(gpu_ratio)));
        }

        // Load: the reserved cores that really work
        if (all.cpus_alloc > 0) {
            const double busy = all.busy / all.cpus_alloc;
            Elements after = {text(thousands(static_cast<long>(all.busy + 0.5))) | bold, text(" of the reserved cores busy") | dim};
            if (all.reserved_idle > 0)
                after.push_back(text(" · " + std::to_string(all.reserved_idle) +
                                     (all.reserved_idle == 1 ? " node reserved but idle" : " nodes reserved but idle")) |
                                color(theme::yellow()));
            lines.push_back(gaugeLine("Load", busy, std::move(after), gauge, efficiencyColor(busy)));
        }
        if (all.mem_total_mib > 0) {
            const double mem = all.mem_alloc_mib / all.mem_total_mib;
            lines.push_back(gaugeLine("Memory", mem,
                                      {text(util::mib(all.mem_alloc_mib)) | bold, text(" / " + util::mib(all.mem_total_mib) + " reserved") | dim},
                                      gauge, usageColor(mem)));
        }
        if (!data.storage.path.empty() && data.storage.total_bytes > 0) {
            const double used = data.storage.used_bytes / data.storage.total_bytes;
            lines.push_back(gaugeLine("Storage", used,
                                      {text(storageSize(data.storage.used_bytes)) | bold,
                                       text(" / " + storageSize(data.storage.total_bytes) + " used on " + data.storage.path) | dim},
                                      gauge, usageColor(used)));
        }
        lines.push_back(nodeCountsLine(all));

        // Slurm itself: the controller, and the scheduler's cycles (sdiag)
        {
            const std::string ctl = api::controllerState(data.ping);
            Elements s = {title(util::padRight("Slurm", 8))};
            if (ctl.empty()) s.push_back(text("the controller doesn't answer scontrol ping") | color(theme::yellow()));
            else if (ctl == "UP") s.push_back(text("controller up") | color(theme::green()));
            else s.push_back(text(ctl) | color(theme::red()));
            if (extra.has_sdiag) {
                const auto& d = extra.sdiag;
                std::string sched = " · scheduler cycle " + micros(d.main_mean_us) + " (max " + micros(d.main_max_us) + ")";
                if (d.bf_mean_us >= 0) sched += " · backfill " + micros(d.bf_mean_us) + " (max " + micros(d.bf_max_us) + ")";
                s.push_back(text(sched) | dim);
                if (wide && d.since > 0 && d.submitted >= 0)
                    s.push_back(text(" · " + thousands(d.submitted) + " jobs submitted since " + util::clock(d.since, now)) | dim);
            }
            lines.push_back(hbox(std::move(s)));
        }

        // Reservations to come (or running): a job still running when one starts waits for it
        std::vector<const api::Reservation*> resv;
        for (const auto& r : data.reservations)
            if (r.end == 0 || r.end > now) resv.push_back(&r);
        if (!resv.empty()) {
            lines.push_back(text(""));
            lines.push_back(hbox({title("Reservations  "),
                                  text("a job still running when one starts waits for it (ReqNodeNotAvail, Reservation)") | dim}));
            for (const auto* r : resv) {
                const bool active = r->start <= now;
                std::string what = std::to_string(r->node_count) + (r->node_count == 1 ? " node" : " nodes");
                if (!r->features.empty()) what += " " + r->features;
                if (!r->partition.empty()) what += " · " + r->partition;
                lines.push_back(hbox({
                    cell(r->name, 24) | bold | color(r->maintenance() ? theme::red() : theme::cyan()),
                    cell(r->maintenance() ? "maintenance" : active ? "now" : util::relative(r->start, now), 13) |
                        color(r->maintenance() ? theme::red() : active ? theme::yellow() : theme::gray()),
                    text(util::clock(r->start, now) + " → " + util::clock(r->end, now) + "  "),
                    text(util::cut(what, static_cast<size_t>(std::max(10, inner - 70)))) | dim,
                }));
            }
        }

        // Partitions, with their part of the queue and their rules
        lines.push_back(text(""));
        Elements h = countHeader("PARTITION", 11, bar_w);
        if (wide) h.insert(h.begin() + 1, cell("TIME LIMIT", 12));
        h.push_back(cell("RUN", 7, true));
        h.push_back(cell("PEND", 7, true));
        if (wide && !extra.limits.empty()) h.push_back(cell("TIER", 6, true));
        lines.push_back(hbox(std::move(h)) | dim);
        std::vector<std::string> rules;
        for (const auto& p : data.sinfo.partitions) {
            Elements row = {cell(p.name + (p.is_default ? "*" : ""), 11) | bold | color(p.avail == "up" ? theme::green() : theme::red())};
            if (wide) row.push_back(cell(p.timelimit, 12) | dim);
            for (auto& e : countCells(p, bar_w)) row.push_back(e);
            const auto it = q.by_partition.find(p.name);
            const api::QueueCounts c = it == q.by_partition.end() ? api::QueueCounts{} : it->second;
            row.push_back(cell(std::to_string(c.running), 7, true));
            row.push_back(cell(std::to_string(c.pending), 7, true) | (c.pending ? color(theme::yellow()) : Decorator(dim)));
            const auto lim = extra.limits.find(p.name);
            if (wide && lim != extra.limits.end())
                row.push_back(cell(lim->second.tier >= 0 ? std::to_string(lim->second.tier) : "-", 6, true) | dim);
            lines.push_back(hbox(std::move(row)));
            if (lim != extra.limits.end()) {
                // Only the rules that are set: max nodes, memory, accounts
                const auto& l = lim->second;
                std::string set;
                if (!l.max_nodes.empty() && l.max_nodes != "UNLIMITED") set += " · at most " + l.max_nodes + " nodes";
                if (!l.def_mem.empty() && l.def_mem != "UNLIMITED") set += " · default memory " + l.def_mem;
                if (!l.max_mem.empty() && l.max_mem != "UNLIMITED") set += " · at most " + l.max_mem + " per node";
                if (!l.allow_accounts.empty() && l.allow_accounts != "ALL") set += " · accounts " + l.allow_accounts;
                if (!set.empty()) rules.push_back(p.name + set);
            }
        }
        for (const auto& r : rules) lines.push_back(text("  " + util::cut(r, static_cast<size_t>(inner - 2))) | dim);
        if (wide && !extra.limits.empty()) lines.push_back(text("  TIER: the higher, the sooner its jobs are looked at") | dim);

        // Kinds of nodes
        lines.push_back(text(""));
        lines.push_back(hbox(countHeader("NODE KIND", wide ? 23 : 11, bar_w)) | dim);
        for (const auto& t : data.types) {
            const std::string label = wide ? util::padRight(t.features, 9) + " " + t.prefix : t.features;
            Elements row = {cell(label, wide ? 23 : 11) | bold};
            for (auto& e : countCells(t, bar_w)) row.push_back(e);
            lines.push_back(hbox(std::move(row)));
        }
    }

    // ---- 2 Queue: everyone's jobs by partition, why they wait, the main users or accounts
    void queue(Elements& lines, const api::NodeCounts& all, int inner) const {
        const auto& q = data.queue;
        lines.push_back(jobsLine());
        const double cpu_ratio = all.cpus_total ? double(all.cpus_alloc) / all.cpus_total : 0;
        lines.push_back(hbox({title(util::padRight("Asked", 8)),
                              text(thousands(q.cpus_pending) + " CPUs, " + std::to_string(q.gpus_pending) + " GPUs by the pending jobs") |
                                  color(q.pending ? theme::yellow() : theme::gray()),
                              text(" · " + percent(cpu_ratio) + " of the CPUs reserved now") | dim}));

        lines.push_back(text(""));
        lines.push_back(hbox({cell("PARTITION", 11), cell("RUNNING", 9, true), cell("CPUS", 9, true), cell("GPUS", 7, true),
                              cell("PENDING", 9, true), cell("CPUS ASKED", 12, true), cell("GPUS ASKED", 12, true)}) | dim);
        for (const auto& p : data.sinfo.partitions) {
            const auto it = q.by_partition.find(p.name);
            const api::QueueCounts c = it == q.by_partition.end() ? api::QueueCounts{} : it->second;
            lines.push_back(hbox({cell(p.name + (p.is_default ? "*" : ""), 11) | bold, cell(std::to_string(c.running), 9, true),
                                  cell(thousands(c.cpus_running), 9, true), cell(std::to_string(c.gpus_running), 7, true),
                                  cell(std::to_string(c.pending), 9, true) | (c.pending ? color(theme::yellow()) : Decorator(dim)),
                                  cell(thousands(c.cpus_pending), 12, true) | dim, cell(std::to_string(c.gpus_pending), 12, true) | dim}));
        }

        // Why the pending jobs wait: the cluster is full (Resources), or others go first (Priority)
        if (!q.reasons.empty()) {
            std::vector<std::pair<int, std::string>> reasons;
            for (const auto& [r, n] : q.reasons) reasons.emplace_back(n, r);
            std::sort(reasons.rbegin(), reasons.rend());
            lines.push_back(text(""));
            lines.push_back(hbox({cell("WAITING FOR", 26), cell("JOBS", 7, true), cell("CPUS", 9, true), cell("GPUS", 7, true),
                                  text("  meaning")}) | dim);
            for (const auto& [n, r] : reasons) {
                const auto it = q.reason_use.find(r);
                const api::QueueCounts c = it == q.reason_use.end() ? api::QueueCounts{} : it->second;
                lines.push_back(hbox({cell(r, 26) | color(theme::yellow()), cell(std::to_string(n), 7, true),
                                      cell(thousands(c.cpus_pending), 9, true), cell(std::to_string(c.gpus_pending), 7, true),
                                      text("  " + util::cut(reasonMeaning(r), static_cast<size_t>(std::max(10, inner - 51)))) | dim}));
            }
        } else if (data.queue_error.empty()) {
            lines.push_back(text(""));
            lines.push_back(text("No job is waiting: a job that fits in the free nodes starts at once") | color(theme::green()));
        }

        // Who uses the cluster most (users, or accounts with a)
        const auto& ranked = by_account ? q.by_account : q.by_user;
        if (!ranked.empty()) {
            lines.push_back(text(""));
            lines.push_back(hbox({cell(by_account ? "ACCOUNT" : "USER", 16), cell("RUNNING", 9, true), cell("PENDING", 9, true),
                                  cell("CPUS", 9, true), cell("GPUS", 7, true), text("  SHARE OF CPUS")}) | dim);
            const size_t top = std::min<size_t>(ranked.size(), 12);
            for (size_t i = 0; i < top; i++) {
                const auto& u = ranked[i];
                const double share = all.cpus_total > 0 ? double(u.cpus_running) / all.cpus_total : 0;
                lines.push_back(hbox({cell(u.name, 16) | bold, cell(std::to_string(u.running), 9, true),
                                      cell(std::to_string(u.pending), 9, true) | (u.pending ? color(theme::yellow()) : Decorator(dim)),
                                      cell(thousands(u.cpus_running), 9, true), cell(std::to_string(u.gpus_running), 7, true),
                                      text("  "), bar(share, 8, theme::accent()), cell(percent(share), 6, true)}));
            }
            if (ranked.size() > top) lines.push_back(text("+ " + std::to_string(ranked.size() - top) + " more") | dim);
        }
    }

    // ---- 3 Nodes: the nodes down and why, then every node (a map, or a table with t)
    void nodeList(Elements& lines, const api::NodeCounts& all, int inner, const std::set<std::string>& mine, std::time_t now) const {
        lines.push_back(nodeCountsLine(all));

        // Down or drained: why, since when, by whom (sinfo -R)
        std::map<std::string, const api::DownReason*> reason_of;
        for (const auto& d : data.down)
            for (const auto& n : api::expandHostlist(d.nodes)) reason_of[n] = &d;
        if (!data.down.empty()) {
            lines.push_back(text(""));
            lines.push_back(hbox({cell("DOWN OR DRAINED", 22), cell("SINCE", 14), cell("BY", 9), text("WHY")}) | dim);
            for (const auto& d : data.down)
                lines.push_back(hbox({cell(d.nodes, 22) | color(theme::red()), cell(d.since ? util::clock(d.since, now) : "-", 14) | dim,
                                      cell(d.user, 9) | dim, text(util::cut(d.reason, static_cast<size_t>(std::max(10, inner - 45))))}));
        }

        lines.push_back(text(""));
        if (!table)
            lines.push_back(hbox({title("Map  "), text("■") | color(theme::green()), text(" busy  ") | dim, text("■") | color(theme::yellow()),
                                  text(" reserved, idle  ") | dim, text(".") | dim, text(" free  ") | dim, text("●") | color(theme::yellow()),
                                  text(" GPU in use  ") | dim, text("x") | color(theme::red()), text(" down  ") | dim,
                                  text("012") | bold | color(theme::cyan()) | inverted, text(" your jobs") | dim}));
        for (const auto& t : data.types) {
            int gpu_slots = 0;
            std::vector<const api::ClusterNode*> group;
            for (const auto& n : data.sinfo.nodes)
                if ((n.features.empty() ? "other" : n.features) == t.features) {
                    group.push_back(&n);
                    gpu_slots = std::max(gpu_slots, std::min(n.gpus_total, 8));
                }
            lines.push_back(hbox({text(t.features) | bold, text("  " + t.prefix + " · " + std::to_string(group.size()) + " nodes") | dim}));
            if (table) {
                lines.push_back(hbox({cell("NODE", 12), cell("STATE", 8), cell("CPUS", 10, true), cell("LOAD", 8, true),
                                      cell("BUSY", 6, true), cell("MEMORY", 14, true), cell("FREE", 8, true), cell("GPUS", 6, true),
                                      text("  NOTE")}) |
                                dim);
                for (const auto* n : group) {
                    const double busy = n->cpus_alloc > 0 ? n->busyCores() / n->cpus_alloc : -1;
                    const auto why = reason_of.find(n->name);
                    std::string note = why != reason_of.end() ? why->second->reason : n->reservedIdle() ? "reserved but idle" : "";
                    if (mine.count(n->name)) note = "your jobs" + (note.empty() ? "" : " · " + note);
                    lines.push_back(hbox({
                        cell(n->name, 12) | (mine.count(n->name) ? color(theme::cyan()) : Decorator(nothing)),
                        cell(useName(n->use), 8) | color(useColor(n->use)),
                        cell(std::to_string(n->cpus_alloc) + "/" + std::to_string(n->cpus_total), 10, true),
                        cell(n->load < 0 ? "-" : std::to_string(static_cast<int>(n->load + 0.5)), 8, true),
                        busy < 0 ? cell("-", 6, true) | dim : cell(percent(busy), 6, true) | color(efficiencyColor(busy)),
                        cell(util::mib(n->mem_alloc_mib) + "/" + util::mib(n->mem_total_mib), 14, true),
                        cell(n->mem_free_mib < 0 ? "-" : util::mib(n->mem_free_mib), 8, true) | dim,
                        cell(n->gpus_total ? std::to_string(n->gpus_used) + "/" + std::to_string(n->gpus_total) : "-", 6, true),
                        text("  " + util::cut(note, static_cast<size_t>(std::max(4, inner - 74)))) |
                            (why != reason_of.end() ? color(theme::red()) : Decorator(dim)),
                    }));
                }
            } else {
                const int cell_w = 3 + 1 + 8 + (gpu_slots > 0 ? gpu_slots + 1 : 0) + 2;
                const int per_row = std::max(1, inner / cell_w);
                for (size_t i = 0; i < group.size(); i += static_cast<size_t>(per_row)) {
                    Elements row;
                    for (size_t k = i; k < std::min(group.size(), i + static_cast<size_t>(per_row)); k++)
                        row.push_back(nodeCell(*group[k], mine, gpu_slots));
                    lines.push_back(hbox(std::move(row)));
                }
            }
        }
    }

    // `mine`: the nodes the user's jobs run on
    Element render(int width, int height, const std::set<std::string>& mine) {
        const int inner = std::max(40, width - 4);
        page = std::max(1, height - 6);
        const int bar_w = std::clamp(inner - 80, 6, 24);
        const std::time_t now = std::time(nullptr);

        Elements lines;
        if (loading && data.sinfo.nodes.empty()) lines.push_back(text("Reading sinfo and squeue...") | dim);
        if (!error.empty()) {
            lines.push_back(paragraph(error) | color(theme::red()));
            const std::string ctl = api::controllerState(data.ping);
            if (!ctl.empty() && ctl != "UP") lines.push_back(text("Slurm: " + ctl) | color(theme::red()));
        }
        if (!data.sinfo.nodes.empty()) {
            // The totals count each node once (partitions share them)
            api::NodeCounts all;
            for (const auto& n : data.sinfo.nodes) all.add(n);
            if (tab == 0) overview(lines, all, inner, bar_w, inner >= 96, now);
            else if (tab == 1) queue(lines, all, inner);
            else nodeList(lines, all, inner, mine, now);
        }
        offset = std::clamp(offset, 0, std::max(0, static_cast<int>(lines.size()) - page));

        std::string when = loading ? "reading..." : updated ? "updated " + (now - updated < 5 ? std::string("now") : util::duration(static_cast<long>(now - updated)) + " ago") : "";
        when += " · every " + std::to_string(refresh_seconds) + " s";
        Elements bar_tabs;
        for (int i = 0; i < static_cast<int>(tabs().size()); i++) {
            const std::string label = " " + std::to_string(i + 1) + " " + tabs()[static_cast<size_t>(i)] + " ";
            bar_tabs.push_back(i == tab ? text(label) | inverted | bold : text(label) | dim);
            bar_tabs.push_back(text(" "));
        }
        std::vector<std::pair<std::string, std::string>> hints = {{"Tab", "next"}, {"j/k", "scroll"}};
        if (tab == 1) hints.push_back({"a", by_account ? "users" : "accounts"});
        if (tab == 2) hints.push_back({"t", table ? "map" : "table"});
        hints.push_back({"r", "refresh"});
        hints.push_back({"q", "close"});
        return panel("Cluster", vbox({hbox(std::move(bar_tabs)), separator() | dim, scrollWindow(lines, offset, page) | flex,
                                      separator() | dim, hbox(keyHints(hints))}) |
                                    size(WIDTH, EQUAL, width - 2) | size(HEIGHT, EQUAL, height - 2),
                     theme::accent(), text(when));
    }
};

}  // namespace ui
