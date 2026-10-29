#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/usage.hpp"
#include "charts.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// ---------------------------------------------------------------- readings in time

/* Readings of one quantity: (seconds since the epoch, value)
- Two readings less than half a second apart are one: nvidia-smi gives the GPUs of a node one line each
- An hour is kept: the longest the widest zoom shows
*/
struct Series {
    std::vector<std::pair<double, double>> points;

    void add(double at, double value) {
        if (!points.empty() && at - points.back().first < 0.5) points.back() = {points.back().first, value};
        else points.emplace_back(at, value);
        if (points.size() > 64 && at - points.front().first > 3600)
            points.erase(points.begin(), points.begin() + static_cast<long>(points.size() / 8));
    }
    bool empty() const { return points.empty(); }
    double last() const { return points.empty() ? -1 : points.back().second; }
    double peak(double since = 0) const {
        double p = 0;
        for (const auto& [at, v] : points)
            if (at >= since) p = std::max(p, v);
        return p;
    }
};

/* The series as `n` values, one per `step` seconds, the newest at the right (at `now`), each divided by `scale`
- Several readings in one: their mean. None: the one before holds (sstat answers every 2 s)
- -1 before the first reading: nothing is drawn there
*/
inline std::vector<double> resample(const Series& s, double now, double step, int n, double scale) {
    std::vector<double> out(static_cast<size_t>(std::max(0, n)), -1);
    if (s.empty() || scale <= 0) return out;
    size_t k = 0;
    double held = -1;
    const double first = now - n * step;
    while (k < s.points.size() && s.points[k].first <= first) held = s.points[k++].second;
    for (int i = 0; i < n; i++) {
        const double end = first + (i + 1) * step;
        double sum = 0;
        int count = 0;
        while (k < s.points.size() && s.points[k].first <= end) {
            sum += s.points[k++].second;
            count++;
        }
        if (count > 0) held = sum / count;
        if (held >= 0) out[static_cast<size_t>(i)] = std::clamp(held / scale, 0.0, 1.0);
    }
    return out;
}

/* How fast a total grows (CPU seconds, bytes), from readings far enough apart
- sstat counts CPU time in whole seconds and doesn't say when it took it: over 2 s, a second more or less is 50%
  on 1 CPU. The base is a reading at least `window` seconds older: the curve is smooth.
- Less than before (a step ended): it starts again from this reading
*/
struct Rate {
    double window = 6;
    std::vector<std::pair<double, double>> readings;

    // The rate per second, -1 until there are two readings 1.5 s apart
    double add(double at, double total) {
        if (!readings.empty() && total < readings.back().second) readings.clear();
        readings.emplace_back(at, total);
        const std::pair<double, double>* base = nullptr;
        for (const auto& r : readings)
            if (at - r.first >= window || (!base && at - r.first >= 1.5)) base = &r;
        const double rate = base ? (total - base->second) / (at - base->first) : -1;
        while (readings.size() > 2 && at - readings[1].first >= window) readings.erase(readings.begin());
        return rate;
    }
};

// ---------------------------------------------------------------- the view

/* A running job, live (m), in two views
- 1, overview: the CPUs' curve and the GPUs' (or the memory's) under it, beside them a gauge for each and every GPU
  with its little curve; under them the memory, the disk reads and writes, and the job's steps
- 2, a task manager's Performance tab: the resources on the left with their little curves, the one picked on the
  right with its big chart, its scale and the time, and its figures
- sstat every 2 s (main.cpp, on a worker of its own); the GPUs from one srun --overlap step that runs
  nvidia-smi --loop-ms while the view is open
*/
struct MonitorView {
    static constexpr double ZOOMS[] = {1, 2, 5, 10, 30};   // seconds per column

    std::string title, job_id, state, partition, nodelist;
    int cpus = 1, gpus = 0, nodes = 1;
    double mem_limit_mib = 0;   // all its nodes (0: it asked for none)
    std::time_t started = 0;
    long limit = util::UNKNOWN;

    bool ended = false;         // not running any more: the curves stay
    std::string cpu_error, gpu_error;
    bool gpu_waiting = false;   // the GPU step is starting

    // The whole job
    Series cpu;                 // share of its CPUs busy
    Series mem;                 // MiB in use, all its steps
    Series reads, writes;       // bytes per second
    Series gpu, gpu_mem, gpu_power;
    Rate cpu_rate, read_rate{4, {}}, write_rate{4, {}};
    double cpu_seconds = -1, mem_mib = -1, max_task_mib = 0, read_total = 0, write_total = 0;
    std::string max_task_node;

    struct StepTrack {
        api::StepUse last;
        Series cores;           // how many cores it keeps busy
        Rate rate;
        double seen = 0;
    };
    std::map<std::string, StepTrack> steps;

    struct GpuTrack {
        api::GpuUse last;
        Series util, mem;
        double seen = 0;
    };
    std::map<std::pair<std::string, int>, GpuTrack> gpu_tracks;   // by node, then index

    int view = 0;               // 0 overview, 1 performance
    chart::Symbols symbols = chart::Symbols::Bars;    // the graphs of the first view (g: bars, braille, blocks)
    int zoom = 1;
    int selected = 0, side_offset = 0, item_count = 1;
    std::function<void()> reload;

    static double nowSeconds() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // sstat: each step's CPU time, memory and bytes; the job's are their sums
    void setSteps(const std::vector<api::StepUse>& list, double at, const std::string& error) {
        cpu_error = error;
        if (!error.empty()) return;
        double cpu_total = 0, mem_total = 0, read = 0, write = 0;
        max_task_mib = 0;
        for (const auto& s : list) {
            cpu_total += s.cpu_seconds;
            mem_total += s.mem_mib;
            read += s.read_bytes;
            write += s.write_bytes;
            if (s.max_rss_mib > max_task_mib) {
                max_task_mib = s.max_rss_mib;
                max_task_node = s.max_rss_node;
            }
            auto& t = steps[s.id];
            t.rate.window = std::clamp(20.0 / std::max(1, s.tasks), 6.0, 20.0);
            t.last = s;
            t.seen = at;
            const double cores = t.rate.add(at, s.cpu_seconds);
            if (cores >= 0) t.cores.add(at, cores);
        }
        for (auto it = steps.begin(); it != steps.end();) it = at - it->second.seen > 30 ? steps.erase(it) : std::next(it);
        cpu_rate.window = std::clamp(20.0 / std::max(1, cpus), 6.0, 20.0);
        const double busy = cpu_rate.add(at, cpu_total);
        if (busy >= 0) cpu.add(at, std::min(1.0, busy / cpus));
        cpu_seconds = cpu_total;
        mem_mib = mem_total;
        mem.add(at, mem_total);
        read_total = read;
        write_total = write;
        const double r = read_rate.add(at, read), w = write_rate.add(at, write);
        if (r >= 0) reads.add(at, r);
        if (w >= 0) writes.add(at, w);
    }

    // A line from nvidia-smi: that GPU's curves, and those of all of them together
    void addGpu(const api::GpuUse& g, double at) {
        gpu_waiting = false;
        gpu_error.clear();
        auto& t = gpu_tracks[{g.node, g.index}];
        t.last = g;
        t.seen = at;
        if (g.util >= 0) t.util.add(at, g.util / 100);
        if (g.mem_total_mib > 0) t.mem.add(at, g.mem_used_mib / g.mem_total_mib);

        double util = 0, used = 0, total = 0, power = 0;
        int n = 0;
        for (const auto& [key, other] : gpu_tracks) {
            if (at - other.seen > 15) continue;   // a node that stopped answering doesn't hold the mean
            if (other.last.util >= 0) {
                util += other.last.util / 100;
                n++;
            }
            used += other.last.mem_used_mib;
            total += other.last.mem_total_mib;
            if (other.last.power_w >= 0) power += other.last.power_w;
        }
        if (n > 0) gpu.add(at, util / n);
        if (total > 0) gpu_mem.add(at, used / total);
        gpu_power.add(at, power);
    }

    /* 1 2 or Tab: the views; + - zoom; g how the graphs are drawn; r reads again (and starts the GPU step again); q, Esc or m close
       The arrows or j k pick a resource (the second view) */
    bool onEvent(Event e, bool& close) {
        const int n = std::max(1, item_count);
        if (e == Event::Character('r')) {
            if (reload) reload();
        } else if (e == Event::Character('1')) {
            view = 0;
        } else if (e == Event::Character('2')) {
            view = 1;
        } else if (e == Event::Character('g')) {
            symbols = symbols == chart::Symbols::Bars ? chart::Symbols::Braille
                      : symbols == chart::Symbols::Braille ? chart::Symbols::Block : chart::Symbols::Bars;
        } else if (e == Event::Tab || e == Event::TabReverse) {
            view = 1 - view;
        } else if (e == Event::Character('+') || e == Event::Character('=')) {
            zoom = std::max(0, zoom - 1);
        } else if (e == Event::Character('-') || e == Event::Character('_')) {
            zoom = std::min(static_cast<int>(std::size(ZOOMS)) - 1, zoom + 1);
        } else if (e == Event::ArrowDown || e == Event::Character('j')) {
            view = 1;
            selected = (selected + 1) % n;
        } else if (e == Event::ArrowUp || e == Event::Character('k')) {
            view = 1;
            selected = (selected + n - 1) % n;
        } else if (e.is_mouse()) {
            if (view == 1 && e.mouse().button == Mouse::WheelDown) selected = std::min(n - 1, selected + 1);
            else if (view == 1 && e.mouse().button == Mouse::WheelUp) selected = std::max(0, selected - 1);
        } else {
            close = e == Event::Escape || e == Event::Character('q') || e == Event::Character('m');
        }
        return true;
    }

    // ---------------------------------------------------------------- pieces

    static std::string watts(double w) {
        if (w >= 1000) {
            const long tenths = std::lround(w / 100);
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " kW";
        }
        return std::to_string(static_cast<long>(w + 0.5)) + " W";
    }
    static std::string pct(double r) { return r < 0 ? "-" : std::to_string(static_cast<int>(std::lround(r * 100))) + "%"; }
    static std::string shortNode(const std::string& node) {
        const size_t dash = node.rfind('-');
        return dash == std::string::npos ? node : node.substr(dash + 1);
    }
    static std::string stepName(const std::string& id) {
        const size_t dot = id.find('.');
        return dot == std::string::npos ? id : id.substr(dot + 1);
    }
    double memScale() const { return mem_limit_mib > 0 ? mem_limit_mib : std::max(1.0, mem.peak() * 1.25); }

    // A box: its name and what it says now on the top border, between the corners
    static Element box(const std::string& name, Element right, Element content, Color frame, int width, int height) {
        Elements top = {text("") | size(WIDTH, EQUAL, 2), text(" " + name + " ") | bold | color(Color::RGB(0xee, 0xee, 0xee)), filler()};
        if (right) top.push_back(hbox({text(" "), right, text(" ")}));
        top.push_back(text("") | size(WIDTH, EQUAL, 2));
        return dbox({content | flex | borderStyled(ROUNDED, frame), vbox({hbox(std::move(top)), filler()})}) | clear_under |
               size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
    }

    // "CPU  ■■■■■■■■■■■■  91%"
    static Element gauge(const std::string& label, double ratio, int width, const chart::Gradient& g) {
        const int w = std::max(3, width - 5 - 5);
        return hbox({text(util::padRight(label, 5)) | bold, chart::meter(ratio, w, g),
                     text(util::padLeft(pct(ratio), 5)) | bold | color(g.at(std::max(0.0, ratio)))});
    }

    // The scale beside a graph: its top, its middle when there is room, and 0 at the bottom (the top at the bottom
    // for one hanging down, and no 0: it shares the one above it)
    static Element axis(int height, const std::string& top, const std::string& mid = "", bool hanging = false, int width = 6) {
        Elements rows;
        for (int r = 0; r < height; r++) {
            std::string label;
            if (r == (hanging ? height - 1 : 0)) label = top;
            else if (!hanging && r == height - 1 && height > 2) label = "0";
            else if (!mid.empty() && height >= 7 && r == (height - 1) / 2) label = mid;
            rows.push_back(text(util::padLeft(label, static_cast<size_t>(width - 1)) + " ") | color(chart::graphText()));
        }
        return vbox(std::move(rows));
    }

    // A line between two graphs: what the one under it shows, then a faint rule
    static Element rule(Elements what) {
        what.push_back(text(" "));
        what.push_back(separatorLight() | color(chart::faint()) | flex);
        return hbox(std::move(what));
    }

    // ---------------------------------------------------------------- 1: overview

    Element overview(int width, int height, double now) {
        const double step = ZOOMS[zoom];
        const bool has_gpus = gpus > 0, wide = width >= 100;
        const int cpu_h = std::clamp(height * 11 / 20, 10, std::max(10, height - 10));
        const int low_h = height - cpu_h;

        // cpu: the CPUs' curve, under it the GPUs' (else the memory's), each with its scale
        const int side_w = wide ? std::clamp(width * 2 / 5, 46, 66) : 0;
        const int left_w = width - 2 - (wide ? side_w + 3 : 0);
        const int graph_w = left_w - 6;
        const int rows = cpu_h - 2 - 2;   // a line above each graph
        const int up_h = std::max(2, rows * 11 / 20), down_h = std::max(2, rows - up_h);
        const double busy = cpu.last();
        const chart::Gradient under_g = has_gpus ? chart::gpuGradient() : chart::usedGradient();
        const double under_now = has_gpus ? gpu.last() : mem_mib < 0 ? -1 : mem_mib / memScale();

        Elements cpu_line = {text("CPUs ") | bold | color(chart::cpuGradient().low),
                             text(pct(busy)) | bold | color(chart::cpuGradient().at(std::max(0.0, busy)))};
        if (busy >= 0) cpu_line.push_back(text("  " + std::to_string(static_cast<long>(busy * cpus + 0.5)) + " of " + thousands(cpus) + " cores busy") |
                                          color(chart::graphText()));
        cpu_line.push_back(filler());
        if (started > 0) {
            const long ran = static_cast<long>(static_cast<std::time_t>(now) - started);
            cpu_line.push_back(text("up " + util::duration(ran) + (limit > 0 ? " · " + util::duration(std::max(0L, limit - ran)) + " left" : "")) |
                               color(chart::graphText()));
        }
        Elements under_line = {text(has_gpus ? "GPUs " : "memory ") | bold | color(under_g.mid),
                               text(pct(under_now)) | bold | color(under_g.at(std::max(0.0, under_now)))};
        if (has_gpus && gpu_power.last() > 0) under_line.push_back(text("  " + watts(gpu_power.last())) | color(chart::graphText()));
        if (!has_gpus && mem_mib >= 0) under_line.push_back(text("  " + util::mib(mem_mib) + " of " + util::mib(memScale())) | color(chart::graphText()));

        Element left = vbox({hbox(std::move(cpu_line)),
                             hbox({axis(up_h, "100%", "50%"), chart::graph(resample(cpu, now, step, graph_w, 1), graph_w, up_h, chart::cpuGradient(), false, symbols)}),
                             rule(std::move(under_line)),
                             hbox({axis(down_h, has_gpus ? "100%" : util::mib(memScale()), has_gpus ? "50%" : ""),
                                   chart::graph(resample(has_gpus ? gpu : mem, now, step, graph_w, has_gpus ? 1 : memScale()), graph_w, down_h, under_g,
                                                false, symbols)})}) |
                       size(WIDTH, EQUAL, left_w);
        Element cpu_content = left;

        if (wide) {
            // Beside it: a gauge for each, then each GPU (or each step) with its little curve
            const int w = side_w;
            Elements lines = {text(util::cut(nodelist, static_cast<size_t>(w))) | bold,
                              gauge("CPU", busy, w, chart::cpuGradient()),
                              gauge("MEM", mem_mib < 0 ? -1 : mem_mib / memScale(), w, chart::usedGradient())};
            if (has_gpus) {
                lines.push_back(gauge("GPU", gpu.last(), w, chart::gpuGradient()));
                lines.push_back(gauge("VRAM", gpu_mem.last(), w, chart::cachedGradient()));
            }
            lines.push_back(separatorLight() | color(chart::faint()));
            const int per_row = w >= 56 ? 2 : 1, cell_w = (w - (per_row - 1) * 2) / per_row;
            const int mini_w = std::max(4, cell_w - 7 - 5 - 5);
            const double mini_step = step * graph_w / mini_w;   // the same time as the big curve
            Elements cells;
            if (has_gpus) {
                for (const auto& [key, t] : gpu_tracks) {
                    const double u = t.last.util < 0 ? -1 : t.last.util / 100;
                    const double temp = t.last.temp_c;
                    Element cell = hbox({text(util::padRight(shortNode(key.first) + ":" + std::to_string(key.second), 7)),
                                         chart::sparkline(resample(t.util, now, mini_step, mini_w, 1), mini_w, chart::gpuGradient()),
                                         text(util::padLeft(pct(u), 5)) | bold | color(chart::gpuGradient().at(std::max(0.0, u))),
                                         text(temp < 0 ? "     " : util::padLeft(std::to_string(static_cast<int>(temp)) + "°C", 5)) |
                                             color(chart::tempGradient().at((temp - 30) / 60))});
                    cells.push_back(now - t.seen > 15 ? cell | dim : cell);
                }
                if (gpu_tracks.empty()) cells.push_back(text(gpu_waiting ? "starting nvidia-smi in the job..." : "no GPU answered") | dim);
            } else {
                for (const auto& [id, t] : steps) {
                    const double c = t.cores.last();
                    cells.push_back(hbox({text(util::padRight(util::cut(stepName(id), 6), 7)),
                                          chart::sparkline(resample(t.cores, now, mini_step, mini_w, std::max(1, cpus)), mini_w, chart::cpuGradient()),
                                          text(util::padLeft(c < 0 ? "-" : std::to_string(static_cast<long>(std::lround(c))), 5)) | bold,
                                          text(" core") | color(chart::graphText())}));
                }
            }
            for (size_t i = 0; i < cells.size(); i += static_cast<size_t>(per_row)) {
                Elements r;
                for (size_t k = i; k < std::min(cells.size(), i + static_cast<size_t>(per_row)); k++) {
                    if (k > i) r.push_back(text("  "));
                    r.push_back(cells[k] | size(WIDTH, EQUAL, cell_w));
                }
                lines.push_back(hbox(std::move(r)));
            }
            lines.push_back(filler());
            lines.push_back(hbox({text(cpu_seconds < 0 ? "" : util::duration(static_cast<long>(cpu_seconds)) + " of CPU time") | color(chart::graphText()),
                                  filler(), text(std::to_string(steps.size()) + (steps.size() == 1 ? " step" : " steps")) | color(chart::graphText())}));
            cpu_content = hbox({left, text(" "), separatorLight() | color(chart::faint()), text(" "), vbox(std::move(lines)) | size(WIDTH, EQUAL, side_w)});
        }
        Elements out = {box("cpu", text(thousands(cpus) + (cpus == 1 ? " CPU" : " CPUs") + " · " + std::to_string(nodes) + (nodes == 1 ? " node" : " nodes")) |
                                       color(chart::graphText()),
                            cpu_content, chart::cpuGradient().low, width, cpu_h)};

        // Under it: memory, disk and the steps side by side (the steps go when it is narrow)
        const int mem_w = wide ? width * 27 / 100 : width / 2;
        const int io_w = wide ? width * 33 / 100 : width - mem_w;
        const int steps_w = wide ? width - mem_w - io_w : 0;
        Elements bottom = {memBox(mem_w, low_h, now), ioBox(io_w, low_h, now)};
        if (steps_w > 0) bottom.push_back(stepsBox(steps_w, low_h, now));
        out.push_back(hbox(std::move(bottom)));
        return vbox(std::move(out));
    }

    // Memory: what it asked, what it uses with its curve, what is left, its largest task, the GPUs' memory
    Element memBox(int width, int height, double now) const {
        const int w = width - 2;
        const double step = ZOOMS[zoom], scale = memScale();
        const auto head = [](const std::string& label, const std::string& value, Color c) {
            return hbox({text(label) | color(chart::graphText()), filler(), text(value) | bold | color(c)});
        };
        const auto withPct = [w](Element e, double ratio) { return hbox({e | size(WIDTH, EQUAL, w - 5), text(util::padLeft(pct(ratio), 5)) | bold}); };
        const int fixed = 2 + (mem_limit_mib > 0 ? 2 : 0) + 2 + (gpus > 0 ? 2 : 0);
        const int graph_h = std::clamp(height - 2 - fixed, 1, 10);
        const double used = mem_mib < 0 ? -1 : mem_mib / scale;
        Elements lines = {head("asked", mem_limit_mib > 0 ? util::mib(mem_limit_mib) : "nothing", Color::RGB(0xee, 0xee, 0xee)),
                          head("in use", mem_mib < 0 ? "-" : util::mib(mem_mib), chart::usedGradient().high),
                          hbox({chart::graph(resample(mem, now, step * 2, w - 5, scale), w - 5, graph_h, chart::usedGradient(), false, symbols, true),
                                vbox({filler(), text(util::padLeft(pct(used), 5)) | bold})})};
        if (mem_limit_mib > 0) {
            const double free_ratio = used < 0 ? -1 : std::max(0.0, 1 - used);
            lines.push_back(head("free", mem_mib < 0 ? "-" : util::mib(std::max(0.0, mem_limit_mib - mem_mib)), chart::freeGradient().mid));
            lines.push_back(withPct(chart::meter(free_ratio, w - 6, chart::freeGradient()), free_ratio));
        }
        lines.push_back(head("largest task", max_task_mib > 0 ? util::mib(max_task_mib) : "-", chart::availableGradient().mid));
        lines.push_back(hbox({filler(), text(max_task_node.empty() ? "" : "on " + max_task_node) | color(chart::graphText())}));
        if (gpus > 0) {
            double gu = 0, gt = 0;
            for (const auto& [key, t] : gpu_tracks) {
                gu += t.last.mem_used_mib;
                gt += t.last.mem_total_mib;
            }
            lines.push_back(head("GPU memory", gt > 0 ? util::mib(gu) + " of " + util::mib(gt) : "-", chart::cachedGradient().mid));
            lines.push_back(withPct(chart::meter(gpu_mem.last(), w - 6, chart::cachedGradient()), gpu_mem.last()));
        }
        return box("memory", nullptr, vbox(std::move(lines)), chart::usedGradient().mid, width, height);
    }

    // Disk: the reads rising, the writes hanging under them on the same scale, each with its rate and total
    Element ioBox(int width, int height, double now) const {
        const double step = ZOOMS[zoom];
        const int gw = width - 2 - 10;
        const int gh = height - 2 - 2;
        const double top = chart::niceMax(std::max({reads.peak(now - step * gw), writes.peak(now - step * gw), 1e6}));
        const double r = reads.last(), w = writes.last();
        const std::string label = chart::bytes(top) + "/s";
        const auto line = [&](const std::string& what, double rate, double total, Color c) {
            return hbox({text(what) | bold | color(c), text(rate < 0 ? "  -" : "  " + chart::bytes(rate) + "/s") | bold, filler(),
                         text(chart::bytes(total) + " in all") | color(chart::graphText())});
        };
        Element content = vbox({line("▲ read ", r, read_total, chart::readGradient().high),
                                hbox({axis((gh + 1) / 2, label, "", false, 10),
                                      chart::graph(resample(reads, now, step, gw, top), gw, (gh + 1) / 2, chart::readGradient(), false, symbols)}),
                                hbox({axis(gh / 2, label, "", true, 10),
                                      chart::graph(resample(writes, now, step, gw, top), gw, gh / 2, chart::writeGradient(), true, symbols)}),
                                line("▼ write", w, write_total, chart::writeGradient().high)});
        return box("disk", text("all its steps, every file system") | color(chart::graphText()), content, chart::readGradient().mid, width, height);
    }

    // The job's steps, the busiest first, each with the curve of the cores it keeps busy
    Element stepsBox(int width, int height, double now) const {
        const double step = ZOOMS[zoom];
        const int w = width - 2;
        const int graph_w = std::max(4, w - (8 + 6 + 7 + 2 + 7 + 8 + 2 + 6));
        std::vector<const std::pair<const std::string, StepTrack>*> order;
        for (const auto& s : steps) order.push_back(&s);
        std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->second.cores.last() > b->second.cores.last(); });
        const auto col = [](const std::string& s, int n, bool right = true) {
            return text(right ? util::padLeft(s, static_cast<size_t>(n)) : util::padRight(s, static_cast<size_t>(n)));
        };
        Elements lines = {hbox({col("step", 8, false), col("tasks", 6), col("cores", 7), text("  "), text("") | size(WIDTH, EQUAL, graph_w),
                                col("memory", 7), col("max RSS", 8), text("  "), col("on", 6, false)}) |
                          color(chart::graphText())};
        for (const auto* s : order) {
            const auto& t = s->second;
            const double c = t.cores.last();
            lines.push_back(hbox({col(util::cut(stepName(s->first), 7), 8, false) | bold, col(std::to_string(t.last.tasks), 6),
                                  col(c < 0 ? "-" : std::to_string(static_cast<long>(std::lround(c))), 7) | bold | color(chart::cpuGradient().low),
                                  text("  "), chart::sparkline(resample(t.cores, now, step * 2, graph_w, std::max(1, cpus)), graph_w, chart::cpuGradient()),
                                  col(util::mib(t.last.mem_mib), 7), col(util::mib(t.last.max_rss_mib), 8), text("  "),
                                  col(util::cut(shortNode(t.last.max_rss_node), 6), 6, false) | color(chart::graphText())}));
        }
        if (steps.empty()) lines.push_back(text("reading sstat...") | dim);
        lines.push_back(filler());
        lines.push_back(text(util::cut("cores: CPU seconds per second · max RSS: its largest task, and where", static_cast<size_t>(w))) |
                        color(chart::graphText()));
        return box("steps", text(std::to_string(steps.size()) + (steps.size() == 1 ? " step" : " steps")) | color(chart::graphText()),
                   vbox(std::move(lines)), chart::cpuGradient().mid, width, height);
    }

    // ---------------------------------------------------------------- 2: a task manager's

    // One resource of the list on the left, and what its chart shows
    struct Item {
        struct Curve {
            const Series* series;
            double scale;
            Color color;
            std::string name;
        };
        std::string name, value, subtitle;
        const Series* mini = nullptr;   // its little curve in the list
        double mini_scale = 1;
        chart::Gradient g;
        std::vector<Curve> curves;
        std::function<std::string(double)> y_label;
        std::vector<std::pair<std::string, std::string>> stats;
    };

    std::vector<Item> items(double now) const {
        std::vector<Item> out;
        const std::time_t now_t = static_cast<std::time_t>(now);
        const auto percentLabel = [](double f) { return std::to_string(static_cast<int>(std::lround(f * 100))) + "%"; };
        {
            const double busy = cpu.last();
            Item it{"CPU", busy < 0 ? "reading..." : pct(busy) + " · " + std::to_string(static_cast<long>(busy * cpus + 0.5)) + " cores",
                    thousands(cpus) + (cpus == 1 ? " CPU" : " CPUs") + " on " + nodelist, &cpu, 1, chart::cpuGradient(),
                    {{&cpu, 1, chart::cpuGradient().low, "busy"}}, percentLabel, {}};
            it.stats = {{"Use", pct(busy)},
                        {"Cores busy", busy < 0 ? "-" : std::to_string(static_cast<long>(busy * cpus + 0.5)) + " / " + thousands(cpus)},
                        {"CPU time", cpu_seconds < 0 ? "-" : util::duration(static_cast<long>(cpu_seconds))},
                        {"Steps", std::to_string(steps.size())}};
            if (started > 0) {
                it.stats.push_back({"Running for", util::duration(static_cast<long>(now_t - started))});
                if (limit > 0) it.stats.push_back({"Time left", util::duration(std::max(0L, limit - static_cast<long>(now_t - started)))});
            }
            out.push_back(std::move(it));
        }
        {
            const double scale = memScale();
            Item it{"Memory",
                    mem_mib < 0 ? "reading..."
                                : util::mib(mem_mib) + (mem_limit_mib > 0 ? " / " + util::mib(mem_limit_mib) + " (" + pct(mem_mib / mem_limit_mib) + ")" : ""),
                    mem_limit_mib > 0 ? util::mib(mem_limit_mib) + " asked" : "nothing asked: scaled on its peak", &mem, scale, chart::usedGradient(),
                    {{&mem, scale, chart::usedGradient().mid, "in use"}}, [scale](double f) { return util::mib(f * scale); }, {}};
            it.stats = {{"In use", mem_mib < 0 ? "-" : util::mib(mem_mib)},
                        {"Asked", mem_limit_mib > 0 ? util::mib(mem_limit_mib) : "-"},
                        {"Free", mem_limit_mib > 0 && mem_mib >= 0 ? util::mib(std::max(0.0, mem_limit_mib - mem_mib)) : "-"},
                        {"Peak seen", mem.empty() ? "-" : util::mib(mem.peak())},
                        {"Largest task", max_task_mib > 0 ? util::mib(max_task_mib) : "-"},
                        {"Its node", max_task_node.empty() ? "-" : max_task_node}};
            out.push_back(std::move(it));
        }
        {
            const double top = chart::niceMax(std::max({reads.peak(), writes.peak(), 1e6}));
            Item it{"Disk",
                    "R " + (reads.empty() ? "-" : chart::bytes(reads.last()) + "/s") + " · W " + (writes.empty() ? "-" : chart::bytes(writes.last()) + "/s"),
                    "what its steps read and write, every file system", &reads, top, chart::readGradient(),
                    {{&reads, top, chart::readGradient().high, "read"}, {&writes, top, chart::writeGradient().high, "write"}},
                    [top](double f) { return chart::bytes(f * top) + "/s"; }, {}};
            it.stats = {{"Read", reads.empty() ? "-" : chart::bytes(reads.last()) + "/s"},
                        {"Write", writes.empty() ? "-" : chart::bytes(writes.last()) + "/s"},
                        {"Read in all", chart::bytes(read_total)},
                        {"Written in all", chart::bytes(write_total)},
                        {"Peak read", chart::bytes(reads.peak()) + "/s"},
                        {"Peak write", chart::bytes(writes.peak()) + "/s"}};
            out.push_back(std::move(it));
        }
        if (gpus <= 0) return out;

        double power = 0, hottest = -1;
        int idle = 0;
        std::string model;
        for (const auto& [key, t] : gpu_tracks) {
            if (t.last.power_w > 0) power += t.last.power_w;
            hottest = std::max(hottest, t.last.temp_c);
            idle += t.last.util >= 0 && t.last.util < 5;
            model = t.last.name;
        }
        {
            Item it{"GPUs", gpu.empty() ? (gpu_waiting ? "starting..." : "no answer") : pct(gpu.last()) + " · " + pct(gpu_mem.last()) + " memory",
                    std::to_string(gpus) + " × " + (model.empty() ? "GPU" : model), &gpu, 1, chart::gpuGradient(),
                    {{&gpu, 1, chart::gpuGradient().mid, "mean use"}, {&gpu_mem, 1, chart::cachedGradient().mid, "memory"}}, percentLabel, {}};
            it.stats = {{"Mean use", pct(gpu.last())},
                        {"Memory", pct(gpu_mem.last())},
                        {"Power", power > 0 ? watts(power) : "-"},
                        {"Hottest", hottest >= 0 ? std::to_string(static_cast<int>(hottest)) + " °C" : "-"},
                        {"Almost idle", std::to_string(idle) + " / " + std::to_string(gpu_tracks.size())}};
            out.push_back(std::move(it));
        }
        for (const auto& [key, t] : gpu_tracks) {
            const auto& g = t.last;
            const double use = g.util < 0 ? -1 : g.util / 100;
            Item it{"GPU " + shortNode(key.first) + ":" + std::to_string(key.second),
                    pct(use) + " · " + util::mib(g.mem_used_mib) + (g.temp_c >= 0 ? " · " + std::to_string(static_cast<int>(g.temp_c)) + "°C" : ""),
                    g.name + " on " + key.first, &t.util, 1, chart::gpuGradient(),
                    {{&t.util, 1, chart::gpuGradient().mid, "use"}, {&t.mem, 1, chart::cachedGradient().mid, "memory"}}, percentLabel, {}};
            it.stats = {{"Use", pct(use)},
                        {"Memory", util::mib(g.mem_used_mib) + " / " + util::mib(g.mem_total_mib)},
                        {"Temperature", g.temp_c >= 0 ? std::to_string(static_cast<int>(g.temp_c)) + " °C" : "-"},
                        {"Power", g.power_w < 0 ? "-" : watts(g.power_w) + (g.power_limit_w > 0 ? " / " + watts(g.power_limit_w) : "")}};
            if (now - t.seen > 15) it.stats.push_back({"Last answer", util::duration(static_cast<long>(now - t.seen)) + " ago"});
            out.push_back(std::move(it));
        }
        return out;
    }

    // A resource in the list: its little curve in a frame, its name and what it does now
    static Element sideItem(const Item& it, bool on, int width, double now, double window) {
        const int mini_w = 12;
        Element mini = chart::graph(resample(*it.mini, now, window / mini_w, mini_w, it.mini_scale), mini_w, 2, it.g, false, chart::Symbols::Bars, true) |
                       borderStyled(ROUNDED, on ? it.g.mid : chart::faint());
        const size_t text_w = static_cast<size_t>(std::max(4, width - mini_w - 4));
        Element name = text(util::cut(it.name, text_w)) | bold;
        if (!on) name = name | color(chart::graphText());
        Element value = text(util::cut(it.value, text_w));
        if (!on) value = value | color(chart::graphText());
        Element row = hbox({text(on ? "▌" : " ") | color(it.g.mid), mini, text(" "), vbox({text(""), name, value})}) | size(WIDTH, EQUAL, width);
        return on ? row | bgcolor(Color::RGB(0x22, 0x26, 0x2e)) : row;
    }

    Element performance(int width, int height, double now) {
        const auto list = items(now);
        item_count = static_cast<int>(list.size());
        selected = std::clamp(selected, 0, item_count - 1);
        const Item& it = list[static_cast<size_t>(selected)];
        const double step = ZOOMS[zoom];

        const bool side = width >= 90;
        const int side_w = side ? 40 : 0;
        const int right_w = width - side_w - (side ? 2 : 0);

        // The figures under the chart, in columns: label above, value under it
        const int col_w = 20, per_row = std::max(1, right_w / col_w);
        Elements stat_rows;
        for (size_t i = 0; i < it.stats.size(); i += static_cast<size_t>(per_row)) {
            Elements labels, figures;
            for (size_t k = i; k < std::min(it.stats.size(), i + static_cast<size_t>(per_row)); k++) {
                labels.push_back(text(util::cut(it.stats[k].first, col_w - 1)) | color(chart::graphText()) | size(WIDTH, EQUAL, col_w));
                figures.push_back(text(util::cut(it.stats[k].second, col_w - 1)) | bold | size(WIDTH, EQUAL, col_w));
            }
            stat_rows.push_back(hbox(std::move(labels)));
            stat_rows.push_back(hbox(std::move(figures)));
        }
        const int chart_h = std::max(6, height - 3 - static_cast<int>(stat_rows.size()));

        // The chart: each curve resampled to two values a column (the canvas' dots)
        const int cols = chart::lineColumns(right_w, chart_h, it.y_label);
        const double window = step * cols;
        std::vector<chart::Line> lines;
        Elements legend;
        for (const auto& c : it.curves) {
            lines.push_back({resample(*c.series, now, step / 2, cols * 2, c.scale), c.color, lines.empty()});
            legend.push_back(text("━ ") | bold | color(c.color));
            legend.push_back(text(c.name + "   ") | color(chart::graphText()));
        }
        const auto x_label = [step](int from_right) {
            const long s = std::lround(from_right * step);
            if (from_right == 0) return std::string("now");
            if (s < 60) return "-" + std::to_string(s) + "s";
            if (s % 60 == 0 || s >= 3600) return "-" + util::duration(s);
            return "-" + std::to_string(s / 60) + "m" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
        };
        // A grid line every round number of seconds, about six across
        double tick = 10;
        for (double t : {10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600})
            if ((tick = t) >= cols / 6.0 * step) break;
        Element big = chart::lineChart(lines, right_w, chart_h, it.y_label, x_label, std::max(4, static_cast<int>(tick / step)));

        const size_t sub_w = static_cast<size_t>(std::max(0, right_w - static_cast<int>(util::width(it.name)) - 2));
        const std::string where = side ? "" : "  " + std::to_string(selected + 1) + "/" + std::to_string(item_count) + " ↑↓";
        Element pane = vbox({hbox({text(it.name) | bold | color(it.g.high), text(where) | color(chart::graphText()), text("  "), text(util::cut(it.subtitle, sub_w)) | color(chart::graphText())}),
                             hbox({hbox(std::move(legend)), filler(), text(util::duration(static_cast<long>(window)) + " shown") | color(chart::graphText())}),
                             big, text(""), vbox(std::move(stat_rows)), filler()}) |
                       size(WIDTH, EQUAL, right_w);
        if (!side) return pane;

        const int fits = std::max(1, (height - 1) / 4);
        side_offset = std::clamp(side_offset, std::max(0, selected - fits + 1), selected);
        side_offset = std::clamp(side_offset, 0, std::max(0, item_count - fits));
        Elements rows;
        for (int i = side_offset; i < std::min(item_count, side_offset + fits); i++)
            rows.push_back(sideItem(list[static_cast<size_t>(i)], i == selected, side_w, now, window));
        rows.push_back(filler());
        if (item_count > fits)
            rows.push_back(text(" " + std::to_string(side_offset + 1) + "-" + std::to_string(std::min(item_count, side_offset + fits)) + " of " +
                                std::to_string(item_count) + " · ↑↓") |
                           color(chart::graphText()));
        return hbox({vbox(std::move(rows)) | size(WIDTH, EQUAL, side_w), separator() | color(chart::faint()), text(" "), pane});
    }

    // ---------------------------------------------------------------- the whole

    Element render(int max_width, int max_height) {
        const int inner_w = std::max(40, max_width - 2), inner_h = std::max(14, max_height - 2);
        const double now = nowSeconds();

        // The views as tabs, and the job
        const auto tab = [&](int i, const std::string& name) {
            Element t = text(" " + std::to_string(i + 1) + " " + name + " ");
            return view == i ? t | bold | inverted | color(theme::accent()) : t | color(chart::graphText());
        };
        Elements head = {tab(0, "overview"), tab(1, "performance"), text("  "), text(state) | bold | color(stateColor(state)),
                         text(" · " + partition) | color(chart::graphText()), filler()};
        if (!cpu_error.empty() || !gpu_error.empty())
            head.push_back(text(util::cut(cpu_error.empty() ? gpu_error : cpu_error, static_cast<size_t>(inner_w / 2))) | color(theme::red()));
        else if (ended)
            head.push_back(text("ended: the curves keep what it did") | color(theme::yellow()));

        const int body_h = inner_h - 2;
        Element body = view == 0 ? overview(inner_w, body_h, now) : performance(inner_w, body_h, now);

        Elements keys = keyHints({{"1 2", "view"}, {"+ -", "zoom"}});
        if (view == 0)
            keys.push_back(keyHint("g", symbols == chart::Symbols::Bars ? "braille" : symbols == chart::Symbols::Braille ? "blocks" : "bars"));
        if (view == 1) keys.push_back(keyHint("↑↓", "resource"));
        for (auto& k : keyHints({{"r", "read again"}, {"q", "close"}})) keys.push_back(k);
        const std::string when =
            std::to_string(static_cast<int>(ZOOMS[zoom])) + " s per column · sstat 2 s" + (gpus > 0 ? " · nvidia-smi 2 s" : "");
        Elements foot = {hbox(std::move(keys)), filler()};
        if (inner_w >= 110) foot.push_back(text(when) | color(chart::graphText()));
        return panel(title,
                     vbox({hbox(std::move(head)), body | size(HEIGHT, EQUAL, body_h), hbox(std::move(foot))}) |
                         size(WIDTH, EQUAL, inner_w) | size(HEIGHT, EQUAL, inner_h),
                     theme::accent(), text(ended ? "stopped" : "live") | bold | color(ended ? theme::yellow() : theme::green()));
    }
};

}  // namespace ui
