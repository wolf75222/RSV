#pragma once

#include <cstdint>
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

/* Cluster (p): what the whole of ROMEO is doing now (every user's jobs are visible there), in four tabs
- Overview: the totals (CPUs, GPUs, load, memory, storage, nodes, Slurm) with the CPUs and GPUs in use since the
  view was opened, reservations, partitions and their rules, kinds of nodes
- Queue: everyone's jobs by partition, why the pending ones wait and what they ask for, the main users or accounts
- Nodes: the nodes down and why, then every node as a map (■ busy, ■ reserved but idle, . free, x down) or a table
  with its load and memory (t). A cursor moves on them, Enter zooms on one: its jobs, load, memory, why it is down.
- Hardware: each kind of node, its processors, memory and GPUs
- Read again every refresh period while it is open; sdiag and the partition rules once, when it opens; the hardware
  the first time its tab is shown; the zoomed node when it opens, then at each refresh
*/
struct ClusterView {
    api::Cluster data;
    api::ClusterExtra extra;
    std::string error;
    bool loading = true;
    std::time_t updated = 0;
    int refresh_seconds = 15;
    int tab = 0;              // 0 overview, 1 queue, 2 nodes, 3 hardware
    int offset = 0;
    int page = 20;
    bool table = false;       // nodes as a table instead of a map
    bool by_account = false;  // main accounts instead of main users
    std::function<void()> reload;
    // Runs `work` on the Slurm thread, then what it returns on the UI thread (set by main.cpp)
    std::function<void(std::function<std::function<void()>()>)> async;

    // The CPUs and GPUs in use at each reading since the view was opened, for the curves of the overview
    struct Point {
        std::time_t at = 0;
        double cpus = 0, gpus = -1;   // shares in use; -1: no GPUs
    };
    std::vector<Point> history;

    // Hardware: read the first time its tab is shown (r reads it again)
    std::vector<api::NodeHardware> hardware;
    std::string hardware_error;
    bool hardware_loading = false, hardware_asked = false;

    // Nodes: the node under the cursor, and the zoom on it (Enter)
    std::string cursor;
    bool zoom_open = false, zoom_loading = false;
    std::string zoom_name, zoom_error;
    api::NodeZoom zoom;
    int zoom_offset = 0;
    uint64_t zoom_generation = 0;
    // From the last drawing of the Nodes tab: its nodes row by row, and the line of the cursor's row
    std::vector<std::vector<std::string>> map_rows;
    int cursor_line = -1;
    bool follow_cursor = false;

    // A new reading of the cluster (every refresh while the view is open): one more point on the curves
    void setData(api::Cluster d) {
        data = std::move(d);
        updated = std::time(nullptr);
        api::NodeCounts all;
        for (const auto& n : data.sinfo.nodes) all.add(n);
        if (all.cpus_total > 0) {
            if (history.size() >= 20000) history.erase(history.begin(), history.begin() + 10000);   // days at 15 s
            history.push_back({updated, double(all.cpus_alloc) / all.cpus_total,
                               all.gpus_total > 0 ? double(all.gpus_used) / all.gpus_total : -1});
        }
        if (zoom_open && !zoom_loading) openZoom(zoom_name);
    }

    // Opened again (p): the curves start again
    void opened() {
        history.clear();
        zoom_open = false;
        if (tab == 3 && !hardware_asked) requestHardware();
    }

    void requestHardware() {
        if (!async) return;
        hardware_asked = true;
        hardware_loading = true;
        async([this]() -> std::function<void()> {
            auto r = api::fetchHardware();   // on the Slurm thread: nothing of the view is read here
            return [this, r = std::move(r)]() mutable {
                hardware_loading = false;
                hardware_error = r.error;
                if (r.error.empty()) hardware = std::move(r.data);
            };
        });
    }

    // The zoom on a node: scontrol and squeue -w, on the Slurm thread; an answer for another node is dropped
    void openZoom(const std::string& name) {
        if (name.empty() || !async) return;
        if (name != zoom_name) {
            zoom = api::NodeZoom{};
            zoom_offset = 0;
        }
        zoom_open = true;
        zoom_name = name;
        zoom_loading = true;
        const uint64_t generation = ++zoom_generation;
        async([this, name, generation]() -> std::function<void()> {
            auto r = api::fetchNodeZoom(name);
            return [this, generation, r = std::move(r)]() mutable {
                if (generation != zoom_generation) return;
                zoom_loading = false;
                zoom_error = r.error;
                zoom = std::move(r.data);
            };
        });
    }

    static const std::vector<std::string>& tabs() {
        static const std::vector<std::string> t = {"Overview", "Queue", "Nodes", "Hardware"};
        return t;
    }

    void showTab(int t) {
        const int n = static_cast<int>(tabs().size());
        tab = (t % n + n) % n;
        offset = 0;
        follow_cursor = tab == 2;
        if (tab == 3 && !hardware_asked) requestHardware();
    }

    // Where the cursor's node is in the map's rows (the first node when it isn't there any more)
    std::pair<int, int> cursorPlace() const {
        for (size_t r = 0; r < map_rows.size(); r++)
            for (size_t c = 0; c < map_rows[r].size(); c++)
                if (map_rows[r][c] == cursor) return {static_cast<int>(r), static_cast<int>(c)};
        return {0, 0};
    }

    // Nodes tab: the arrows (or h/j/k/l) move the cursor over the map or the table, Enter zooms on its node
    bool mapEvent(const Event& e) {
        if (map_rows.empty()) return false;
        const auto place = cursorPlace();
        const int row = place.first, col = place.second;
        const int rows = static_cast<int>(map_rows.size());
        auto width = [&](int r) { return static_cast<int>(map_rows[static_cast<size_t>(r)].size()); };
        auto go = [&](int r, int c) {
            r = std::clamp(r, 0, rows - 1);
            cursor = map_rows[static_cast<size_t>(r)][static_cast<size_t>(std::clamp(c, 0, width(r) - 1))];
            follow_cursor = true;
        };
        if (e == Event::ArrowRight || e == Event::Character('l')) {
            if (col + 1 < width(row)) go(row, col + 1);
            else if (row + 1 < rows) go(row + 1, 0);
        } else if (e == Event::ArrowLeft || e == Event::Character('h')) {
            if (col > 0) go(row, col - 1);
            else if (row > 0) go(row - 1, width(row - 1) - 1);
        } else if (e == Event::ArrowDown || e == Event::Character('j')) {
            go(row + 1, col);
        } else if (e == Event::ArrowUp || e == Event::Character('k')) {
            go(row - 1, col);
        } else if (e == Event::Home || e == Event::Character('g')) {
            go(0, 0);
            follow_cursor = false;   // the top: the nodes down come first
            offset = 0;
        } else if (e == Event::End || e == Event::Character('G')) {
            go(rows - 1, width(rows - 1) - 1);
        } else if (e == Event::Return) {
            openZoom(map_rows[static_cast<size_t>(row)][static_cast<size_t>(col)]);
        } else {
            return false;
        }
        return true;
    }

    // The zoom: Esc goes back to the map, ←/→ to the node before or after it, p closes the view
    bool zoomEvent(Event e, bool& close) {
        const bool next = e == Event::ArrowRight || e == Event::Character('l');
        if (e == Event::Character('p')) {
            close = true;
        } else if (e == Event::Escape || e == Event::Character('q') || e == Event::Return || e == Event::Backspace) {
            zoom_open = false;
            follow_cursor = true;
        } else if (e == Event::Character('r')) {
            openZoom(zoom_name);
        } else if (next || e == Event::ArrowLeft || e == Event::Character('h')) {
            std::vector<std::string> all;
            for (const auto& r : map_rows) all.insert(all.end(), r.begin(), r.end());
            if (!all.empty()) {
                const auto it = std::find(all.begin(), all.end(), zoom_name);
                const long n = static_cast<long>(all.size());
                const long i = ((it == all.end() ? 0 : it - all.begin()) + (next ? 1 : -1) + n) % n;
                cursor = all[static_cast<size_t>(i)];
                openZoom(cursor);
            }
        } else if (e == Event::ArrowDown || e == Event::Character('j')) {
            zoom_offset++;
        } else if (e == Event::ArrowUp || e == Event::Character('k')) {
            zoom_offset--;
        } else if (e == Event::PageDown || e == Event::Character(' ')) {
            zoom_offset += page - 1;
        } else if (e == Event::PageUp || e == Event::Character('b')) {
            zoom_offset -= page - 1;
        } else if (e.is_mouse() && e.mouse().button == Mouse::WheelDown) {
            zoom_offset += 3;
        } else if (e.is_mouse() && e.mouse().button == Mouse::WheelUp) {
            zoom_offset -= 3;
        }
        zoom_offset = std::max(0, zoom_offset);
        return true;   // the zoom takes every key
    }

    bool onEvent(Event e, bool& close) {
        if (zoom_open) return zoomEvent(e, close);
        const int n = static_cast<int>(tabs().size());
        const std::string c = e.is_character() ? e.character() : "";
        if (e == Event::Escape || e == Event::Character('q') || e == Event::Character('p')) close = true;
        else if (e == Event::Character('r')) {
            loading = true;
            if (reload) reload();
            if (tab == 3) requestHardware();
        } else if (e == Event::Tab) showTab(tab + 1);
        else if (e == Event::TabReverse) showTab(tab - 1);
        else if (c.size() == 1 && c[0] >= '1' && c[0] < '1' + n) showTab(c[0] - '1');
        else if (tab == 2 && mapEvent(e)) return true;   // the arrows move the cursor there
        else if (e == Event::ArrowRight || e == Event::Character('l')) showTab(tab + 1);
        else if (e == Event::ArrowLeft || e == Event::Character('h')) showTab(tab - 1);
        else if (e == Event::Character('t')) {
            table = !table;
            follow_cursor = true;
        } else if (e == Event::Character('a')) by_account = !by_account;
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
    // In a narrow window the end of the line is cut, like a long line: the label, bar and numbers keep their width
    static Element gaugeLine(const std::string& label, double ratio, Elements after, int bar_w, Color fill) {
        return hbox({text(util::padRight(label, 8)) | bold, bar(ratio, bar_w, fill), text(" " + util::padLeft(percent(ratio), 4) + "  "),
                     hbox(std::move(after)) | xframe | xflex_shrink});
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
    - The number takes the node's state color, inverted when one of your jobs runs there; the cursor inverts the cell
    */
    static Element nodeCell(const api::ClusterNode& n, const std::set<std::string>& mine, int gpu_slots, bool at_cursor) {
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
        Element node = hbox(std::move(parts));
        return hbox({at_cursor ? node | inverted : node, text("  ")});
    }

    /* Under a gauge: its value at each reading since the view was opened, from ▁ (none) to █ (all)
    - More readings than columns: averaged by groups, the curve always covers the whole time
    */
    static Element sparkline(const std::vector<double>& values, int width) {
        static const char* levels[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        const int n = static_cast<int>(values.size());
        const int w = std::min(width, n);
        Elements cells;
        std::string run;
        Color run_color;
        for (int i = 0; i < w; i++) {
            const int from = i * n / w, to = std::max(from + 1, (i + 1) * n / w);
            double sum = 0;
            for (int k = from; k < to; k++) sum += values[static_cast<size_t>(k)];
            const double v = std::clamp(sum / (to - from), 0.0, 1.0);
            // Not the gauge's green, right above it: cyan, then yellow and red when nearly full
            const Color c = v >= 0.9 ? theme::red() : v >= 0.7 ? theme::yellow() : theme::cyan();
            if (!run.empty() && !(c == run_color)) {
                cells.push_back(text(run) | color(run_color));
                run.clear();
            }
            run += levels[std::min(7, static_cast<int>(v * 8))];
            run_color = c;
        }
        if (!run.empty()) cells.push_back(text(run) | color(run_color));
        if (w < width) cells.push_back(text(std::string(static_cast<size_t>(width - w), ' ')));
        return hbox(std::move(cells));
    }

    // The curve under the CPUs or GPUs gauge, with its lowest and highest values (and since when, for the CPUs)
    Element trendLine(bool gpus, int bar_w, std::time_t now) const {
        std::vector<double> values;
        for (const auto& p : history) values.push_back(std::max(0.0, gpus ? p.gpus : p.cpus));
        const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
        const int low = static_cast<int>(*lo * 100 + 0.5), high = static_cast<int>(*hi * 100 + 0.5);
        Elements line = {text(std::string(8, ' ')), sparkline(values, bar_w),
                         text("  " + (low == high ? std::to_string(low) : std::to_string(low) + "-" + std::to_string(high)) + "%") | dim};
        if (!gpus)
            line.push_back(text(" since " + util::clock(history.front().at, now) + " · " +
                                util::duration(static_cast<long>(now - history.front().at))) |
                           dim);
        return hbox(std::move(line));
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
        if (history.size() >= 2) lines.push_back(trendLine(false, gauge, now));

        if (all.gpus_total > 0) {
            const double gpu_ratio = double(all.gpus_used) / all.gpus_total;
            Elements gpu_after = {text(thousands(all.gpus_used)) | bold, text(" / " + thousands(all.gpus_total) + " in use") | dim};
            if (q.gpus_pending > 0) gpu_after.push_back(text(" · " + thousands(q.gpus_pending) + " asked by pending jobs") | color(theme::yellow()));
            lines.push_back(gaugeLine("GPUs", gpu_ratio, std::move(gpu_after), gauge, usageColor(gpu_ratio)));
            if (history.size() >= 2) lines.push_back(trendLine(true, gauge, now));
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

    // ---- 3 Nodes: the nodes down and why, then every node (a map, or a table with t), the cursor on one of them
    void nodeList(Elements& lines, const api::NodeCounts& all, int inner, const std::set<std::string>& mine, std::time_t now) {
        map_rows.clear();
        cursor_line = -1;
        // The cursor starts on a node of the user's jobs, or the first node
        const auto& nodes = data.sinfo.nodes;
        if (std::none_of(nodes.begin(), nodes.end(), [&](const api::ClusterNode& n) { return n.name == cursor; })) {
            const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const api::ClusterNode& n) { return mine.count(n.name) > 0; });
            cursor = it != nodes.end() ? it->name : nodes.empty() ? "" : nodes.front().name;
        }
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
                    if (n->name == cursor) cursor_line = static_cast<int>(lines.size());
                    map_rows.push_back({n->name});
                    Element row = hbox({
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
                    });
                    lines.push_back(n->name == cursor ? row | inverted : row);
                }
            } else {
                const int cell_w = 3 + 1 + 8 + (gpu_slots > 0 ? gpu_slots + 1 : 0) + 2;
                const int per_row = std::max(1, inner / cell_w);
                for (size_t i = 0; i < group.size(); i += static_cast<size_t>(per_row)) {
                    Elements row;
                    std::vector<std::string> names;
                    for (size_t k = i; k < std::min(group.size(), i + static_cast<size_t>(per_row)); k++) {
                        const bool at = group[k]->name == cursor;
                        if (at) cursor_line = static_cast<int>(lines.size());
                        row.push_back(nodeCell(*group[k], mine, gpu_slots, at));
                        names.push_back(group[k]->name);
                    }
                    map_rows.push_back(std::move(names));
                    lines.push_back(hbox(std::move(row)));
                }
            }
        }
    }

    // ---- 4 Hardware: each kind of node, its processors, memory, GPUs, and which partitions have it
    void hardwareList(Elements& lines, int inner) const {
        if (hardware.empty()) {
            if (hardware_loading) lines.push_back(text("Reading scontrol show node...") | dim);
            if (!hardware_error.empty()) lines.push_back(paragraph(hardware_error) | color(theme::red()));
            return;
        }
        long cores = 0;
        int gpus = 0;
        double mem = 0;
        for (const auto& n : hardware) {
            cores += n.cpus;
            gpus += n.gpus;
            mem += n.mem_mib;
        }
        lines.push_back(hbox({title(util::padRight("Total", 8)), text(std::to_string(hardware.size())) | bold, text(" nodes · "),
                              text(thousands(cores)) | bold, text(" cores · "),
                              text(gpus ? std::to_string(gpus) : "no") | bold, text(" GPUs · "),
                              text(util::mib(mem)) | bold, text(" of memory")}));

        // The partitions in sinfo's order, the others after them
        std::vector<std::string> order;
        for (const auto& p : data.sinfo.partitions) order.push_back(p.name);
        bool models = false;
        const int value_w = std::max(10, inner - 14);
        auto row = [&](const std::string& label, Element value) { return hbox({text("  " + util::padRight(label, 12)) | dim, value}); };
        for (const auto& k : api::hardwareKinds(hardware)) {
            const api::NodeHardware& n = k.like;
            const api::KnownModel m = api::knownModel(n);
            models = models || !m.cpu.empty() || !m.gpu.empty();
            lines.push_back(text(""));
            lines.push_back(hbox({text(n.features.empty() ? "other" : n.features) | bold | color(theme::cyan()),
                                  text("  " + std::to_string(k.nodes.size()) + (k.nodes.size() == 1 ? " node · " : " nodes · ")) | dim,
                                  text(util::cut(api::compressHostlist(k.nodes), static_cast<size_t>(std::max(10, inner - 24))))}));

            // 2 × 96 cores = 192, or 2 × 64 cores × 2 threads = 256 CPUs
            std::string topo = std::to_string(n.sockets) + " × " + std::to_string(n.cores) + " cores";
            if (n.threads > 1) topo += " × " + std::to_string(n.threads) + " threads = " + std::to_string(n.cpus) + " CPUs";
            else topo += " = " + std::to_string(n.cpus);
            Elements cpu;
            if (!m.cpu.empty()) cpu.push_back(text(m.cpu + " · ") | bold);
            cpu.push_back(text(topo + (n.arch.empty() ? "" : " · " + n.arch)));
            lines.push_back(row("CPU", hbox(std::move(cpu))));

            // MemSpecLimit: kept for the system, the jobs get the rest (when it shows at this precision)
            std::string memory = util::mib(n.mem_mib) + " per node";
            const std::string for_jobs = util::mib(n.mem_mib - n.mem_spec_mib);
            if (n.mem_spec_mib > 0 && n.mem_spec_mib < n.mem_mib && for_jobs != util::mib(n.mem_mib)) memory += ", " + for_jobs + " for jobs";
            lines.push_back(row("Memory", text(memory)));

            if (n.gpus == 0) {
                lines.push_back(row("GPUs", text("none") | dim));
            } else {
                std::string g = std::to_string(n.gpus) + " × " + (m.gpu.empty() ? (n.gpu_type.empty() ? "GPU" : n.gpu_type) : m.gpu);
                if (!m.gpu_memory.empty()) g += " · " + m.gpu_memory + " each";
                lines.push_back(row("GPUs", hbox({text(util::cut(g, static_cast<size_t>(value_w))) | bold,
                                                  text(n.gpu_type.empty() ? "" : "  gres gpu:" + n.gpu_type) | dim})));
            }
            lines.push_back(row("Local disk", n.tmp_disk_mib > 0 ? text(util::mib(n.tmp_disk_mib)) : text("none declared to Slurm") | dim));

            std::vector<std::string> names = order;
            for (const auto& [p, count] : k.partitions)
                if (std::find(names.begin(), names.end(), p) == names.end()) names.push_back(p);
            std::string parts;
            for (const auto& p : names) {
                const auto it = k.partitions.find(p);
                if (it != k.partitions.end()) parts += (parts.empty() ? "" : " · ") + p + " " + std::to_string(it->second);
            }
            lines.push_back(row("Partitions", text(util::cut(parts.empty() ? "-" : parts, static_cast<size_t>(value_w)))));
        }
        if (models) {
            lines.push_back(text(""));
            lines.push_back(text("Slurm gives the cores, memory and GPUs; the models come from ROMEO (lscpu, Spack, nvidia-smi)") | dim);
        }
    }

    // ---- The zoom on one node: its state and why it is down, its CPUs, memory and GPUs, the jobs on it
    void zoomLines(Elements& lines, int inner, int bar_w, std::time_t now) const {
        const auto& f = zoom.node;
        auto get = [&](const std::string& key) {
            const auto it = f.find(key);
            return it == f.end() ? std::string() : it->second;
        };
        if (f.empty()) {
            if (zoom_loading) lines.push_back(text("Reading scontrol show node " + zoom_name + "...") | dim);
            if (!zoom_error.empty()) lines.push_back(paragraph(zoom_error) | color(theme::red()));
            return;
        }

        // romeo-a044  armgpu · aarch64  MIXED+DRAIN
        const std::string state = get("State"), s = util::lower(state);
        const bool bad = s.find("down") != std::string::npos || s.find("drain") != std::string::npos ||
                         s.find("fail") != std::string::npos || s.find("not_responding") != std::string::npos ||
                         s.find("maint") != std::string::npos || s.find("reboot") != std::string::npos ||
                         s.find("power") != std::string::npos || s.find("unknown") != std::string::npos;
        const Color state_color = bad ? theme::red() : util::startsWith(s, "mix") ? theme::yellow()
                                                    : util::startsWith(s, "alloc") ? theme::green() : theme::gray();
        std::string kind = get("AvailableFeatures") == "(null)" ? "" : get("AvailableFeatures");
        if (!get("Arch").empty()) kind += (kind.empty() ? "" : " · ") + get("Arch");
        lines.push_back(hbox({text(zoom_name) | bold | color(theme::cyan()), text("  " + kind + "  ") | dim, text(state) | bold | color(state_color),
                              text(zoom_loading ? "  reading..." : "") | dim}));

        // Why it is down or drained: the reason, by whom, since when
        const std::string reason = get("Reason");
        if (!reason.empty() && reason != "(null)" && util::lower(reason) != "none") {
            std::string why, user;
            std::time_t since = 0;
            api::splitReason(reason, why, user, since);
            Elements r = {title(util::padRight("Reason", 8)), text(util::cut(why, static_cast<size_t>(std::max(10, inner - 46)))) | color(theme::red())};
            if (!user.empty()) r.push_back(text("  by " + user) | dim);
            if (since) r.push_back(text(" · since " + util::clock(since, now) + " (" + util::relative(since, now) + ")") | dim);
            lines.push_back(hbox(std::move(r)));
        }
        lines.push_back(text(""));

        // CPUs reserved, and how many of them really work (the load)
        const long cpus = util::toLong(get("CPUTot")), alloc = util::toLong(get("CPUAlloc"));
        const double load = util::toDouble(get("CPULoad"), -1);
        if (cpus > 0) {
            const double ratio = double(alloc) / cpus;
            Elements after = {text(std::to_string(alloc)) | bold, text(" / " + std::to_string(cpus) + " reserved") | dim};
            if (load >= 0) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.1f", load);
                after.push_back(text(std::string(" · load ") + buf) | dim);
                if (alloc > 0) {
                    const double busy = std::min(load, double(alloc)) / alloc;
                    after.push_back(text(", ") | dim);
                    after.push_back(text(percent(busy) + " of them busy") | color(efficiencyColor(busy)));
                }
            }
            lines.push_back(gaugeLine("CPUs", ratio, std::move(after), bar_w, usageColor(ratio)));
        }

        // Memory reserved by the jobs, and free on the node (what the system and the file cache leave)
        const double mem = util::toDouble(get("RealMemory")), mem_alloc = util::toDouble(get("AllocMem"));
        const double mem_free = util::toDouble(get("FreeMem"), -1);
        if (mem > 0) {
            const double ratio = mem_alloc / mem;
            Elements after = {text(util::mib(mem_alloc)) | bold, text(" / " + util::mib(mem) + " reserved") | dim};
            if (mem_free >= 0) after.push_back(text(" · " + util::mib(mem_free) + " free") | dim);
            lines.push_back(gaugeLine("Memory", ratio, std::move(after), bar_w, usageColor(ratio)));
        }

        // GPUs: ● in use, by their index when Slurm gives it (GresUsed, with -d)
        const api::Gpus total = api::parseGpus(get("Gres"));
        if (total.count > 0) {
            api::Gpus used = api::parseGpus(get("GresUsed"));
            if (f.find("GresUsed") == f.end()) {
                const auto tres = api::parseTres(get("AllocTRES"));
                used.count = tres.count("gres/gpu") ? static_cast<int>(tres.at("gres/gpu")) : 0;
            }
            Elements g = {title(util::padRight("GPUs", 8))};
            for (int i = 0; i < std::min(total.count, 16); i++) {
                const bool on = used.indices.empty() ? i < used.count
                                                     : std::find(used.indices.begin(), used.indices.end(), i) != used.indices.end();
                g.push_back(on ? text("●") | color(theme::yellow()) : text("○") | dim);
            }
            g.push_back(text("  " + std::to_string(used.count) + " / " + std::to_string(total.count) + " in use") | dim);
            lines.push_back(hbox(std::move(g)));
        }

        // The jobs on it (squeue -w): whose, how big, since when
        lines.push_back(text(""));
        if (!zoom.jobs_error.empty()) {
            lines.push_back(paragraph(zoom.jobs_error) | color(theme::red()));
            return;
        }
        if (zoom.jobs.empty()) {
            lines.push_back(text(zoom_loading ? "Reading squeue -w " + zoom_name + "..." : "No job on this node") | dim);
            return;
        }
        lines.push_back(hbox({title("Jobs  "), text(std::to_string(zoom.jobs.size()) + " on this node · CPUs and GPUs: the whole job's") | dim}));
        // Since when: the date and how long ago, or how long ago alone in a narrow window
        const bool wide = inner >= 100;
        const int since_w = wide ? 17 : 8;
        lines.push_back(hbox({cell("JOB", 13), cell("USER", 11), cell("CPUS", 6, true), cell("GPUS", 5, true), cell("NODES", 6, true),
                              text("  "), cell("SINCE", since_w), cell("LEFT", 7, true), text("  NAME")}) |
                        dim);
        const std::string me = api::currentUser();
        for (const auto& j : zoom.jobs) {
            const bool own = j.user == me;
            const std::string ago = util::duration(static_cast<long>(now - j.start));
            const std::string since = !j.start ? "-" : wide ? util::clock(j.start, now) + " " + ago : ago;
            lines.push_back(hbox({cell(util::cut(j.id, 12), 13) | (own ? color(theme::cyan()) : Decorator(nothing)),
                                  cell(util::cut(j.user, 10), 11) | (own ? Decorator(bold) : Decorator(nothing)),
                                  cell(thousands(j.cpus), 6, true), cell(j.gpus ? std::to_string(j.gpus) : "-", 5, true),
                                  cell(std::to_string(j.nodes), 6, true), text("  "), cell(since, since_w) | dim,
                                  cell(j.left >= 0 ? util::duration(j.left) : "-", 7, true) | dim,
                                  text("  " + util::cut(j.name, static_cast<size_t>(std::max(4, inner - 52 - since_w))))}));
        }
    }

    // `mine`: the nodes the user's jobs run on
    Element render(int width, int height, const std::set<std::string>& mine) {
        const int inner = std::max(40, width - 4);
        page = std::max(1, height - 6);
        const int bar_w = std::clamp(inner - 80, 6, 24);
        const std::time_t now = std::time(nullptr);

        Elements lines;
        if (zoom_open) {
            zoomLines(lines, inner, bar_w + 12, now);
        } else if (tab == 3) {
            hardwareList(lines, inner);
        } else {
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
        }
        // The cursor's row stays in view when it moves
        if (tab == 2 && !zoom_open && follow_cursor && cursor_line >= 0) {
            if (cursor_line < offset) offset = cursor_line;
            else if (cursor_line >= offset + page) offset = cursor_line - page + 1;
            follow_cursor = false;
        }
        int& shown = zoom_open ? zoom_offset : offset;
        shown = std::clamp(shown, 0, std::max(0, static_cast<int>(lines.size()) - page));

        std::string when = loading ? "reading..." : updated ? "updated " + (now - updated < 5 ? std::string("now") : util::duration(static_cast<long>(now - updated)) + " ago") : "";
        when += " · every " + std::to_string(refresh_seconds) + " s";
        Elements bar_tabs;
        for (int i = 0; i < static_cast<int>(tabs().size()); i++) {
            const std::string label = " " + std::to_string(i + 1) + " " + tabs()[static_cast<size_t>(i)] + " ";
            bar_tabs.push_back(i == tab ? text(label) | inverted | bold : text(label) | dim);
            bar_tabs.push_back(text(" "));
        }
        std::vector<std::pair<std::string, std::string>> hints;
        if (zoom_open) {
            hints = {{"Esc", "back to the map"}, {"←/→", "other node"}, {"j/k", "scroll"}, {"r", "refresh"}, {"p", "close"}};
        } else {
            hints = {{"Tab", "next"}};
            if (tab == 2) {
                hints.push_back({"↑↓←→", "move"});
                hints.push_back({"Enter", cursor.empty() ? "zoom" : "zoom on " + cursor});
                hints.push_back({"t", table ? "map" : "table"});
            } else {
                hints.push_back({"j/k", "scroll"});
            }
            if (tab == 1) hints.push_back({"a", by_account ? "users" : "accounts"});
            hints.push_back({"r", "refresh"});
            hints.push_back({"q", "close"});
        }
        return panel("Cluster", vbox({hbox(std::move(bar_tabs)), separator() | dim, scrollWindow(lines, shown, page) | flex,
                                      separator() | dim, hbox(keyHints(hints))}) |
                                    size(WIDTH, EQUAL, width - 2) | size(HEIGHT, EQUAL, height - 2),
                     theme::accent(), text(when));
    }
};

}  // namespace ui
