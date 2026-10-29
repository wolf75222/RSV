#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/usage.hpp"
#include "charts.hpp"
#include "inference_view.hpp"
#include "live.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// ---------------------------------------------------------------- the view

/* A running job, live (m), in two views
- 1, overview: the CPUs' curve and the GPUs' (or the memory's) under it, beside them a gauge for each and every GPU
  with its little curve; under them the memory, the disk reads and writes, and the job's steps
- 2, a task manager's Performance tab: the resources on the left with their little curves, the one picked on the
  right with its big chart, its scale and the time, and its figures
- sstat every 2 s (main.cpp, on a worker of its own); the GPUs from one srun --overlap step that runs
  nvidia-smi --loop-ms while the view is open
*/
struct MonitorView : LivePieces {
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
        Series reads, writes;   // bytes per second
        Rate rate, read_rate{4, {}}, write_rate{4, {}};
        double seen = 0;
    };
    std::map<std::string, StepTrack> steps;
    std::map<std::string, std::string> step_names;   // squeue -s: what each step runs (python, lbm3d...)
    std::string step_selected;           // the one picked in the overview's steps (↑ ↓), its details on Enter
    bool step_detail = false;
    std::set<std::string> own_steps;     // the view's own steps (nvidia-smi...): not the job's
    std::set<std::string> known_steps;   // every step sstat showed: a new one is looked up by name

    struct GpuTrack {
        api::GpuUse last;
        Series util, mem;                    // shares, 0..1
        Series sm_active, sm_occupancy, tensor, dram, fp32;   // GPM, shares
        Series pcie_rx, pcie_tx, nvlink_rx, nvlink_tx;        // GPM, bytes per second
        Series power, temp;
        double seen = 0, gpm_seen = 0;
    };
    std::map<std::pair<std::string, int>, GpuTrack> gpu_tracks;   // by node, then index

    // The GPUs, in depth (3, or n): who uses each, why one is slow
    struct AppTrack {
        api::GpuApp app;
        double seen = 0;
    };
    std::map<std::pair<std::string, long>, AppTrack> apps;   // by node and pid
    api::GpuFacts facts;
    double facts_at = 0;
    bool facts_loading = false;
    std::string gpm_error, apps_error;
    bool gpm_waiting = false;
    int gpu_selected = 0;
    bool gpu_zoom = false;      // Enter: the selected GPU alone, the whole width

    // The server of models it runs, if it runs one (4, or i): found and read by main.cpp
    InferenceView inference;

    int view = 0;               // 0 overview, 1 performance, 2 GPUs, 3 inference
    chart::Symbols symbols = chart::Symbols::Bars;   // the graphs of the first view (v: areas, braille, blocks)
    int zoom = 1;
    int selected = 0, side_offset = 0, item_count = 1;
    std::function<void()> reload;

    // sstat: each step's CPU time, memory and bytes; the job's are their sums
    void setSteps(const std::vector<api::StepUse>& list, double at, const std::string& error) {
        cpu_error = error;
        if (!error.empty()) return;
        double cpu_total = 0, mem_total = 0, read = 0, write = 0;
        max_task_mib = 0;
        for (const auto& id : own_steps) steps.erase(id);
        for (const auto& s : list) {
            known_steps.insert(s.id);
            if (own_steps.count(s.id)) continue;
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
            const double sr = t.read_rate.add(at, s.read_bytes), sw = t.write_rate.add(at, s.write_bytes);
            if (sr >= 0) t.reads.add(at, sr);
            if (sw >= 0) t.writes.add(at, sw);
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
        if (g.power_w >= 0) t.power.add(at, g.power_w);
        if (g.temp_c >= 0) t.temp.add(at, g.temp_c);

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

    // dmon's GPM sample of one GPU: what its SMs, tensor cores, memory and links really do
    void addGpm(const api::GpmSample& g, double at) {
        gpm_waiting = false;
        gpm_error.clear();
        auto& t = gpu_tracks[{g.node, g.index}];
        t.gpm_seen = at;
        const auto put = [&](const char* key, Series& series, double scale) {
            const auto it = g.values.find(key);
            if (it != g.values.end() && it->second >= 0) series.add(at, it->second * scale);
        };
        put("smutil", t.sm_active, 0.01);
        put("smocc", t.sm_occupancy, 0.01);
        put("mmaact", t.tensor, 0.01);
        put("dram", t.dram, 0.01);
        put("fp32", t.fp32, 0.01);
        put("pcirx", t.pcie_rx, 1048576.0);
        put("pcitx", t.pcie_tx, 1048576.0);
        put("nvlrx", t.nvlink_rx, 1048576.0);
        put("nvltx", t.nvlink_tx, 1048576.0);
    }

    // A process nvidia-smi saw on a GPU; those not seen for 20 s are gone
    void addApp(const api::GpuApp& a, double at) {
        apps_error.clear();
        apps[{a.node, a.pid}] = {a, at};
        for (auto it = apps.begin(); it != apps.end();) it = at - it->second.seen > 20 ? apps.erase(it) : std::next(it);
    }

    // The processes on a GPU, by its uuid
    std::vector<const AppTrack*> appsOn(const std::string& uuid) const {
        std::vector<const AppTrack*> r;
        for (const auto& [key, a] : apps)
            if (!uuid.empty() && a.app.uuid == uuid) r.push_back(&a);
        return r;
    }

    // Slurm's pids again when a process they don't have shows up (it started since), at most every 30 s
    bool wantFacts(double now) const {
        if (facts_loading || gpus <= 0) return false;
        if (facts_at == 0) return true;
        if (now - facts_at < 30) return false;
        for (const auto& [key, a] : apps)
            if (!facts.pids.count(key)) return true;
        return false;
    }

    // The views it has: the GPUs' when it has some, the server's when one was found (or it is shown)
    std::vector<int> views() const {
        std::vector<int> v = {0, 1};
        if (gpus > 0) v.push_back(2);
        if (inference.any() || inference.hinted() || view == 3) v.push_back(3);
        return v;
    }

    /* Keys, as in the rest of RSV
- 1 2 3 4, Tab or ← → (h l): the views, as the cluster view's tabs
- ↑ ↓ (j k): the resource (2), the GPU (3) or the server (4); Enter: that GPU alone, Esc back
- + -: zoom in time; v: how the graphs are drawn; r: read again (and start the GPU steps again, look for a server)
- q or Esc close; the key that opened it closes it too: m from the first two views, n from the GPUs, i from the
  server (each goes to its view otherwise)
*/
    bool onEvent(Event e, bool& close) {
        const int n = std::max(1, item_count);
        const auto tabs = views();
        const auto at = std::find(tabs.begin(), tabs.end(), view);
        const int pos = at == tabs.end() ? 0 : static_cast<int>(at - tabs.begin());
        const int count = static_cast<int>(tabs.size());
        const int servers = static_cast<int>(inference.servers.size());
        if (e == Event::Character('r')) {
            if (reload) reload();
        } else if (e == Event::Character('1') || e == Event::Character('2') || (e == Event::Character('3') && gpus > 0) || e == Event::Character('4')) {
            view = e.character()[0] - '1';
        } else if (e == Event::Character('n') && gpus > 0) {
            if (view == 2) close = true;
            else view = 2;
        } else if (e == Event::Character('i')) {
            if (view == 3) close = true;
            else view = 3;
        } else if (e == Event::Character('m')) {
            if (view >= 2) view = 0;
            else close = true;
        } else if (e == Event::Character('v')) {
            symbols = symbols == chart::Symbols::Bars ? chart::Symbols::Braille
                      : symbols == chart::Symbols::Braille ? chart::Symbols::Block : chart::Symbols::Bars;
        } else if (e == Event::Tab || e == Event::ArrowRight || e == Event::Character('l')) {
            view = tabs[static_cast<size_t>((pos + 1) % count)];
        } else if (e == Event::TabReverse || e == Event::ArrowLeft || e == Event::Character('h')) {
            view = tabs[static_cast<size_t>((pos + count - 1) % count)];
        } else if (view == 2 && (e == Event::ArrowDown || e == Event::Character('j'))) {
            gpu_selected = std::min(static_cast<int>(gpu_tracks.size()) - 1, gpu_selected + 1);
        } else if (view == 2 && (e == Event::ArrowUp || e == Event::Character('k'))) {
            gpu_selected = std::max(0, gpu_selected - 1);
        } else if (view == 2 && e == Event::Return) {
            gpu_zoom = !gpu_zoom;
        } else if (view == 2 && gpu_zoom && e == Event::Escape) {
            gpu_zoom = false;
        } else if (view == 0 && (e == Event::ArrowDown || e == Event::Character('j') || e == Event::ArrowUp || e == Event::Character('k'))) {
            const auto order = stepOrder();
            int here = 0;
            for (size_t i = 0; i < order.size(); i++)
                if (order[i] == step_selected) here = static_cast<int>(i);
            const bool down = e == Event::ArrowDown || e == Event::Character('j');
            if (!order.empty()) step_selected = order[static_cast<size_t>(std::clamp(here + (down ? 1 : -1), 0, static_cast<int>(order.size()) - 1))];
        } else if (view == 0 && e == Event::Return) {
            step_detail = !step_detail;
        } else if (view == 0 && step_detail && e == Event::Escape) {
            step_detail = false;
        } else if (view == 3 && (e == Event::ArrowDown || e == Event::Character('j'))) {
            inference.selected = std::min(servers - 1, inference.selected + 1);
        } else if (view == 3 && (e == Event::ArrowUp || e == Event::Character('k'))) {
            inference.selected = std::max(0, inference.selected - 1);
        } else if (e == Event::Character('+') || e == Event::Character('=')) {
            zoom = std::max(0, zoom - 1);
        } else if (e == Event::Character('-') || e == Event::Character('_')) {
            zoom = std::min(static_cast<int>(std::size(ZOOMS)) - 1, zoom + 1);
        } else if (view == 1 && (e == Event::ArrowDown || e == Event::Character('j'))) {
            selected = (selected + 1) % n;
        } else if (view == 1 && (e == Event::ArrowUp || e == Event::Character('k'))) {
            selected = (selected + n - 1) % n;
        } else if (e.is_mouse()) {
            if (view == 1 && e.mouse().button == Mouse::WheelDown) selected = std::min(n - 1, selected + 1);
            else if (view == 1 && e.mouse().button == Mouse::WheelUp) selected = std::max(0, selected - 1);
            else if (view == 2 && e.mouse().button == Mouse::WheelDown)
                gpu_selected = std::min(static_cast<int>(gpu_tracks.size()) - 1, gpu_selected + 1);
            else if (view == 2 && e.mouse().button == Mouse::WheelUp) gpu_selected = std::max(0, gpu_selected - 1);
        } else {
            close = e == Event::Escape || e == Event::Character('q');
        }
        return true;
    }

    // ---------------------------------------------------------------- pieces

    static std::string stepName(const std::string& id) {
        const size_t dot = id.find('.');
        return dot == std::string::npos ? id : id.substr(dot + 1);
    }
    double memScale() const { return mem_limit_mib > 0 ? mem_limit_mib : std::max(1.0, mem.peak() * 1.25); }

    // ---------------------------------------------------------------- 1: overview, as btop's boxes

    // A series as the graph drawn with `symbols` wants it: one reading per column, or two (btop's braille)
    std::vector<double> curve(const Series& s, double now, double step, int width, double scale, chart::Symbols how) const {
        const int k = chart::perColumn(how);
        return resample(s, now, step / k, width * k, scale);
    }

    // The steps, the busiest first (the order ↑ ↓ go through)
    std::vector<std::string> stepOrder() const {
        std::vector<std::pair<double, std::string>> v;
        for (const auto& [id, t] : steps) v.push_back({-t.cores.last(), id});
        std::sort(v.begin(), v.end());
        std::vector<std::string> out;
        for (const auto& [c, id] : v) out.push_back(id);
        return out;
    }

    /* cpu: the CPUs' graph, under it the GPUs' (one beside the other, as btop's, when there is room) or the memory's;
       beside them, in a box of its own, a meter for each and every GPU with its little graph, as btop's cores */
    Element cpuBox(int width, int height, double now) {
        const double step = ZOOMS[zoom];
        const bool has_gpus = gpus > 0, wide = width >= 100;
        const int side_w = wide ? std::clamp(width * 2 / 5, 46, 66) : 0;
        const int left_w = width - 2 - (wide ? side_w + 1 : 0);
        const int graph_w = left_w - 6;
        const int rows = height - 2 - 2;
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

        // Under it, each GPU its own graph, side by side with its name, when each gets 10 columns (btop's gpu-totals)
        Element under;
        const int n_gpu = static_cast<int>(gpu_tracks.size());
        if (has_gpus && n_gpu > 1 && (graph_w - (n_gpu - 1)) / n_gpu >= 10) {
            const int each = (graph_w - (n_gpu - 1)) / n_gpu;
            Elements cols;
            int i = 0, used = 0;
            for (const auto& [key, t] : gpu_tracks) {
                const int w = i + 1 == n_gpu ? graph_w - used : each;
                const double u = t.last.util < 0 ? -1 : t.last.util / 100;
                Element label = hbox({text(shortNode(key.first) + ":" + std::to_string(key.second)) | color(chart::graphText()), text(" "),
                                      text(pct(u)) | bold | color(chart::gpuGradient().at(std::max(0.0, u)))});
                cols.push_back(dbox({chart::graph(curve(t.util, now, step * graph_w / w, w, 1, symbols), w, down_h, under_g, false, symbols),
                                     vbox({hbox({text(" "), label, filler()}), filler()})}) |
                               size(WIDTH, EQUAL, w));
                used += w;
                if (++i < n_gpu) {
                    cols.push_back(separatorLight() | color(chart::divColor()));
                    used += 1;
                }
            }
            under = hbox(std::move(cols));
        } else {
            under = chart::graph(curve(has_gpus ? gpu : mem, now, step, graph_w, has_gpus ? 1 : memScale(), symbols), graph_w, down_h, under_g, false, symbols);
        }
        Element left = vbox({hbox(std::move(cpu_line)),
                             hbox({axis(up_h, "100%", "50%"), chart::graph(curve(cpu, now, step, graph_w, 1, symbols), graph_w, up_h, chart::cpuGradient(), false, symbols)}),
                             rule(std::move(under_line)),
                             hbox({axis(down_h, has_gpus ? "100%" : util::mib(memScale()), has_gpus ? "50%" : ""), under})}) |
                       size(WIDTH, EQUAL, left_w);
        Element content = left;

        if (wide) {
            // btop's box in the box: a meter for each, then each GPU (or step) with its little graph
            const int w = side_w - 2;
            Elements lines = {gauge("CPU", busy, w, chart::cpuGradient()), gauge("MEM", mem_mib < 0 ? -1 : mem_mib / memScale(), w, chart::usedGradient())};
            if (has_gpus) {
                lines.push_back(gauge("GPU", gpu.last(), w, chart::gpuGradient()));
                lines.push_back(gauge("VRAM", gpu_mem.last(), w, chart::cachedGradient()));
            }
            lines.push_back(separatorLight() | color(chart::divColor()));
            const int per_row = w >= 56 ? 2 : 1, cell_w = (w - (per_row - 1) * 2) / per_row;
            const int mini_w = std::max(4, cell_w - 7 - 5 - 5);
            const double mini_step = step * graph_w / mini_w;   // the same time as the big graph
            Elements cells;
            if (has_gpus) {
                for (const auto& [key, t] : gpu_tracks) {
                    const double u = t.last.util < 0 ? -1 : t.last.util / 100;
                    const double temp = t.last.temp_c;
                    Element cell = hbox({text(util::padRight(shortNode(key.first) + ":" + std::to_string(key.second), 7)),
                                         chart::graph(resample(t.util, now, mini_step / 2, mini_w * 2, 1), mini_w, 1, chart::gpuGradient(), false, chart::Symbols::Braille),
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
                                          chart::graph(resample(t.cores, now, mini_step / 2, mini_w * 2, std::max(1, cpus)), mini_w, 1, chart::cpuGradient(), false,
                                                       chart::Symbols::Braille),
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
            content = hbox({left, text(" "), box(util::cut(nodelist, static_cast<size_t>(side_w - 8)), nullptr, vbox(std::move(lines)), chart::cpuBox(), side_w, height - 2)});
        }
        const std::string per_col = std::to_string(static_cast<int>(ZOOMS[zoom])) + "s";
        return box("cpu",
                   hbox({text(thousands(cpus) + (cpus == 1 ? " CPU" : " CPUs") + " · " + std::to_string(nodes) + (nodes == 1 ? " node" : " nodes")) |
                         color(chart::titleColor())}),
                   content, chart::cpuBox(), width, height, 1,
                   {tag(hbox({text("- ") | bold | color(chart::hiColor()), text(per_col + " per column") | color(chart::titleColor()),
                              text(" +") | bold | color(chart::hiColor())}),
                        chart::cpuBox(), true),
                    text(" "), keyTag("v", symbols == chart::Symbols::Bars ? "areas" : symbols == chart::Symbols::Braille ? "braille" : "blocks", chart::cpuBox(), true)});
    }

    Element overview(int width, int height, double now) {
        const int cpu_h = std::clamp(height * 11 / 20, 10, std::max(10, height - 10));
        const int low_h = height - cpu_h;
        const bool wide = width >= 100;
        const int mem_w = wide ? width * 25 / 100 : width / 2;
        const int io_w = wide ? width * 35 / 100 : width - mem_w;
        const int steps_w = wide ? width - mem_w - io_w : 0;
        Elements bottom = {memBox(mem_w, low_h, now), ioBox(io_w, low_h, now)};
        if (steps_w > 0) bottom.push_back(stepsBox(steps_w, low_h, now));
        return vbox({cpuBox(width, cpu_h, now), hbox(std::move(bottom))});
    }

    // mem, as btop's: what it asked; then each figure with its value on the right, its graph or meter under it
    Element memBox(int width, int height, double now) const {
        const int w = width - 2;
        const double step = ZOOMS[zoom], scale = memScale();
        const auto head = [](const std::string& label, const std::string& value, Color c) {
            return hbox({text(label + ":") | bold | color(chart::titleColor()), filler(), text(value) | bold | color(c)});
        };
        const auto withPct = [w](Element e, double ratio) { return hbox({e | size(WIDTH, EQUAL, w - 5), text(util::padLeft(pct(ratio), 5)) | bold}); };
        const auto divider = [] { return separatorLight() | color(chart::divColor()); };
        const int fixed = 1 + 2 + (mem_limit_mib > 0 ? 3 : 0) + 3 + (gpus > 0 ? 3 : 0);
        const int graph_h = std::clamp(height - 2 - fixed, 1, 10);
        const double used = mem_mib < 0 ? -1 : mem_mib / scale;
        Elements lines = {head("Asked", mem_limit_mib > 0 ? util::mib(mem_limit_mib) : "nothing", chart::titleColor()),
                          head("Used", mem_mib < 0 ? "-" : util::mib(mem_mib), chart::usedGradient().high),
                          withPct(chart::graph(curve(mem, now, step * 2, w - 5, scale, symbols), w - 5, graph_h, chart::usedGradient(), false, symbols, true), used)};
        if (mem_limit_mib > 0) {
            const double free_ratio = used < 0 ? -1 : std::max(0.0, 1 - used);
            lines.push_back(divider());
            lines.push_back(head("Free", mem_mib < 0 ? "-" : util::mib(std::max(0.0, mem_limit_mib - mem_mib)), chart::freeGradient().mid));
            lines.push_back(withPct(chart::meter(free_ratio, w - 6, chart::freeGradient()), free_ratio));
        }
        lines.push_back(divider());
        lines.push_back(head("Largest task", max_task_mib > 0 ? util::mib(max_task_mib) : "-", chart::availableGradient().mid));
        lines.push_back(hbox({filler(), text(max_task_node.empty() ? "" : "on " + max_task_node) | color(chart::graphText())}));
        if (gpus > 0) {
            double gu = 0, gt = 0;
            for (const auto& [key, t] : gpu_tracks) {
                gu += t.last.mem_used_mib;
                gt += t.last.mem_total_mib;
            }
            lines.push_back(divider());
            lines.push_back(head("GPU memory", gt > 0 ? util::mib(gu) + " / " + util::mib(gt) : "-", chart::cachedGradient().mid));
            lines.push_back(withPct(chart::meter(gpu_mem.last(), w - 6, chart::cachedGradient()), gpu_mem.last()));
        }
        return box("mem", nullptr, vbox(std::move(lines)), chart::memBox(), width, height, 2);
    }

    /* io, as btop's net: the reads rising, the writes hanging under them, each on its own scale written in its corner;
       beside them a box with each one's rate, its top and its total */
    Element ioBox(int width, int height, double now) const {
        const double step = ZOOMS[zoom];
        const bool side = width >= 46;
        const int sub_w = side ? 22 : 0;
        const int gw = width - 2 - (side ? sub_w + 1 : 0);
        const int gh = height - 2;
        const double window = step * gw;
        const double top_r = chart::niceMax(std::max(reads.peak(now - window), 1e6)), top_w = chart::niceMax(std::max(writes.peak(now - window), 1e6));
        const double r = reads.last(), w = writes.last();
        const auto scaled = [&](const Series& s, double top, int h, const chart::Gradient& g, bool hang) {
            Element label = text(" " + chart::bytes(top) + "/s ") | color(chart::graphText());
            return dbox({chart::graph(curve(s, now, step, gw, top, symbols), gw, h, g, hang, symbols),
                         hang ? vbox({filler(), hbox({label, filler()})}) : vbox({hbox({label, filler()}), filler()})});
        };
        Element graphs = vbox({scaled(reads, top_r, (gh + 1) / 2, chart::readGradient(), false), scaled(writes, top_w, gh / 2, chart::writeGradient(), true)}) |
                         size(WIDTH, EQUAL, gw);
        Element content = graphs;
        if (side) {
            const auto stats = [&](const char* arrow, const std::string& what, double rate, double peak, double total, Color c) {
                return vbox({hbox({text(std::string(arrow) + " ") | color(c), text(what) | bold | color(chart::titleColor()), filler(),
                                   text(rate < 0 ? "-" : chart::bytes(rate) + "/s") | bold | color(c)}),
                             hbox({text(std::string(arrow) + " Top:") | color(chart::graphText()), filler(), text(chart::bytes(peak) + "/s")}),
                             hbox({text(std::string(arrow) + " Total:") | color(chart::graphText()), filler(), text(chart::bytes(total))})});
            };
            Element sub = vbox({stats("▲", "read", r, reads.peak(), read_total, chart::readGradient().high), filler(),
                                stats("▼", "write", w, writes.peak(), write_total, chart::writeGradient().high)});
            content = hbox({graphs, text(" "), box("all steps", nullptr, sub, chart::netBox(), sub_w, height - 2)});
        }
        return box("io", text("every file system") | color(chart::graphText()), content, chart::netBox(), width, height, 3);
    }

    /* steps, as btop's processes: each with what it runs, its tasks, the cores it keeps busy and their graph, its memory;
       ↑ ↓ pick one, Enter shows it in depth above the list (its graph, its memory, its reads and writes) */
    Element stepsBox(int width, int height, double now) {
        const double step = ZOOMS[zoom];
        const int w = width - 2;
        const auto order = stepOrder();
        if (!order.empty() && !steps.count(step_selected)) step_selected = order.front();
        const auto col = [](const std::string& s, int n, bool right = true) {
            return text(right ? util::padLeft(s, static_cast<size_t>(n)) : util::padRight(s, static_cast<size_t>(n)));
        };
        const int name_w = std::clamp(w - (7 + 6 + 7 + 2 + 12 + 7 + 8), 6, 18);
        const int graph_w = std::max(4, w - (7 + name_w + 6 + 7 + 2 + 7 + 8));
        Elements lines;

        // The one picked, in depth
        if (step_detail && steps.count(step_selected)) {
            const auto& t = steps.at(step_selected);
            const int dh = std::clamp(height / 2 - 1, 4, 8);
            const int dg_w = std::max(10, w / 2);
            const double c = t.cores.last();
            const auto name = step_names.find(step_selected);
            const std::vector<std::pair<std::string, std::string>> figures = {
                {"Runs", name == step_names.end() ? "?" : name->second},
                {"Tasks", std::to_string(t.last.tasks)},
                {"Cores busy", c < 0 ? "-" : std::to_string(static_cast<long>(std::lround(c)))},
                {"Memory", util::mib(t.last.mem_mib)},
                {"Largest task", util::mib(t.last.max_rss_mib) + (t.last.max_rss_node.empty() ? "" : " on " + shortNode(t.last.max_rss_node))},
                {"Reads", t.reads.empty() ? "-" : chart::bytes(t.reads.last()) + "/s · " + chart::bytes(t.last.read_bytes)},
                {"Writes", t.writes.empty() ? "-" : chart::bytes(t.writes.last()) + "/s · " + chart::bytes(t.last.write_bytes)}};
            Elements right;
            for (const auto& [k, v] : figures)
                right.push_back(hbox({text(util::padRight(k, 13)) | color(chart::graphText()), text(util::cut(v, static_cast<size_t>(std::max(4, w - dg_w - 15)))) | bold}));
            const double top = std::max(1.0, chart::niceMax(t.cores.peak(now - step * dg_w)));
            lines.push_back(hbox({dbox({chart::graph(curve(t.cores, now, step, dg_w, top, symbols), dg_w, dh, chart::cpuGradient(), false, symbols),
                                        vbox({hbox({text(" " + fmt::count(top) + " cores ") | color(chart::graphText()), filler()}), filler()})}) |
                                      size(WIDTH, EQUAL, dg_w),
                                  text(" "), vbox(std::move(right))}) |
                            size(HEIGHT, EQUAL, dh));
            lines.push_back(separatorLight() | color(chart::procBox()));
        }
        lines.push_back(hbox({col("step", 7, false), col("runs", name_w, false), col("tasks", 6), col("cores", 7), text("  "),
                              text("") | size(WIDTH, EQUAL, graph_w), col("memory", 7), col("max RSS", 8)}) |
                        bold | color(chart::titleColor()));
        for (const auto& id : order) {
            const auto& t = steps.at(id);
            const double c = t.cores.last();
            const auto name = step_names.find(id);
            const bool on = id == step_selected;
            Element row = hbox({col(util::cut(stepName(id), 6), 7, false) | bold,
                                col(util::cut(name == step_names.end() ? "" : name->second, static_cast<size_t>(name_w - 1)), name_w, false) |
                                    color(on ? chart::titleColor() : chart::graphText()),
                                col(std::to_string(t.last.tasks), 6),
                                col(c < 0 ? "-" : std::to_string(static_cast<long>(std::lround(c))), 7) | bold | color(chart::cpuGradient().low), text("  "),
                                chart::graph(resample(t.cores, now, step, graph_w * 2, std::max(1, cpus)), graph_w, 1, chart::cpuGradient(), false, chart::Symbols::Braille),
                                col(util::mib(t.last.mem_mib), 7), col(util::mib(t.last.max_rss_mib), 8)});
            lines.push_back(on ? row | bgcolor(Color::RGB(0x6a, 0x2f, 0x2f)) : row);
        }
        if (steps.empty()) lines.push_back(text("reading sstat...") | dim);
        lines.push_back(filler());
        return box("steps", text(std::to_string(steps.size()) + (steps.size() == 1 ? " step" : " steps")) | color(chart::titleColor()), vbox(std::move(lines)),
                   chart::procBox(), width, height, 4,
                   {keyTag("↑↓", "select", chart::procBox(), true), text(" "), keyTag("Enter", step_detail ? "list" : "details", chart::procBox(), true)});
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

    // ---------------------------------------------------------------- 3: the GPUs, in depth

    // What is wrong with a GPU, the worst first: each one a line in the colour it deserves
    std::vector<std::pair<std::string, Color>> gpuWarnings(const GpuTrack& t, double now) const {
        std::vector<std::pair<std::string, Color>> w;
        const auto& g = t.last;
        if (now - t.seen > 15) w.push_back({"no answer for " + util::duration(static_cast<long>(now - t.seen)), theme::red()});
        if (g.ecc_uncorrected > 0) w.push_back({std::to_string(static_cast<long>(g.ecc_uncorrected)) + " uncorrected memory errors: tell the admins", theme::red()});
        const auto rows = facts.remapped.find(g.uuid);
        if (rows != facts.remapped.end() && rows->second.second) w.push_back({"memory row remapping failed: tell the admins", theme::red()});
        if (rows != facts.remapped.end() && rows->second.first) w.push_back({"memory rows wait for a GPU reset", theme::yellow()});
        if (!apps.empty() || facts_at > 0) {
            if (appsOn(g.uuid).empty() && !g.uuid.empty())
                w.push_back({"no process on it: reserved for nothing (CUDA_VISIBLE_DEVICES? one GPU per task?)", theme::yellow()});
        }
        const auto reasons = api::clockReasons(g.reasons);
        if (!reasons.empty()) {
            std::string r;
            for (const auto& x : reasons) r += (r.empty() ? "" : ", ") + x;
            w.push_back({"slowed down: " + r, theme::yellow()});
        }
        if (g.util >= 50 && !t.sm_active.empty() && t.sm_active.last() < 0.25)
            w.push_back({"busy " + pct(g.util / 100) + " of the time but only " + pct(t.sm_active.last()) + " of its SMs work: small kernels?", theme::yellow()});
        return w;
    }

    // One GPU of the list: its little curve, what it does, its warnings in one mark
    Element gpuRow(const std::pair<const std::pair<std::string, int>, GpuTrack>& e, bool on, int width, double now) const {
        const auto& t = e.second;
        const auto& g = t.last;
        const double u = g.util < 0 ? -1 : g.util / 100;
        const int spark_w = std::max(6, width - 4 - 5 - 9 - 7 - 6 - 7 - 3 - 2);
        const auto warn = gpuWarnings(t, now);
        Element mark = warn.empty() ? text(" ") : text("!") | bold | color(warn.front().second);
        const size_t n_apps = appsOn(g.uuid).size();
        Element row = hbox({text(on ? "▌" : " ") | color(chart::gpuGradient().mid), text(util::padLeft(std::to_string(e.first.second), 2) + " "),
                            chart::sparkline(resample(t.util, now, ZOOMS[zoom], spark_w, 1), spark_w, chart::gpuGradient()),
                            text(util::padLeft(pct(u), 5)) | bold | color(chart::gpuGradient().at(std::max(0.0, u))),
                            text(t.sm_active.empty() ? "         " : util::padLeft("SM " + pct(t.sm_active.last()), 9)) | color(chart::cachedGradient().mid),
                            text(util::padLeft(g.mem_total_mib > 0 ? pct(g.mem_used_mib / g.mem_total_mib) : "-", 5) + " m") | color(chart::graphText()),
                            text(g.temp_c < 0 ? "      " : util::padLeft(std::to_string(static_cast<int>(g.temp_c)) + "°C", 6)) |
                                color(chart::tempGradient().at((g.temp_c - 30) / 60)),
                            text(util::padLeft(g.power_w < 0 ? "" : watts(g.power_w), 7)) | color(chart::graphText()),
                            text(util::padLeft(n_apps == 0 ? "-" : std::to_string(n_apps), 3)) | color(n_apps == 0 ? theme::yellow() : Color::Default),
                            text(" "), mark});
        return on ? row | bgcolor(Color::RGB(0x22, 0x26, 0x2e)) : row;
    }

    // The list, grouped by node: a line for the node, then its GPUs
    Element gpuList(int width, int height, double now) {
        Elements lines = {hbox({text("    GPU") | color(chart::graphText()), filler(),
                                text("use      SM   mem  temp  power  procs") | color(chart::graphText())})};
        std::string node;
        int i = 0, selected_line = 0;
        for (const auto& e : gpu_tracks) {
            if (e.first.first != node) {
                node = e.first.first;
                int count = 0;
                size_t procs = 0;
                for (const auto& [k, t] : gpu_tracks)
                    if (k.first == node) {
                        count++;
                        procs += appsOn(t.last.uuid).size();
                    }
                lines.push_back(hbox({text(node) | bold, text("  " + std::to_string(count) + (count == 1 ? " GPU · " : " GPUs · ") +
                                                              std::to_string(procs) + (procs == 1 ? " process " : " processes ")) |
                                                         color(chart::graphText()),
                                      separatorLight() | color(chart::faint()) | flex}));
            }
            if (i == gpu_selected) selected_line = static_cast<int>(lines.size());
            lines.push_back(gpuRow(e, i == gpu_selected, width, now));
            i++;
        }
        if (gpu_tracks.empty()) lines.push_back(text(gpu_waiting ? "starting nvidia-smi in the job..." : "no GPU answered") | dim);
        int offset = std::max(0, selected_line - height + 2);
        return fitOrScroll(lines, offset, height) | size(WIDTH, EQUAL, width);
    }

    // The selected GPU: its warnings, its curves (what it does, its memory, its links), its figures and processes
    Element gpuDetail(int width, int height, double now) const {
        if (gpu_tracks.empty()) return text("");
        auto it = gpu_tracks.begin();
        std::advance(it, std::clamp(gpu_selected, 0, static_cast<int>(gpu_tracks.size()) - 1));
        const auto& [key, t] = *it;
        const auto& g = t.last;
        const double step = ZOOMS[zoom];
        const auto pctLabel = [](double f) { return std::to_string(static_cast<int>(std::lround(f * 100))) + "%"; };
        const auto x_label = [step](int from_right) {
            const long sec = std::lround(from_right * step);
            return from_right == 0 ? std::string("now") : "-" + util::duration(sec);
        };

        Elements out = {hbox({text("GPU " + std::to_string(key.second)) | bold | color(chart::gpuGradient().high),
                              text("  " + key.first + " · " + g.name + (g.pstate.empty() ? "" : " · " + g.pstate)) | color(chart::graphText())})};
        for (const auto& [msg, c] : gpuWarnings(t, now)) out.push_back(text("! " + msg) | color(c));
        if (!gpm_error.empty()) out.push_back(text(util::cut("SM, tensor and link figures: " + gpm_error, static_cast<size_t>(width))) | color(chart::graphText()));

        // Figures in columns
        std::vector<std::pair<std::string, std::string>> stats = {
            {"Use", pct(g.util < 0 ? -1 : g.util / 100)},
            {"SM active", t.sm_active.empty() ? "-" : pct(t.sm_active.last())},
            {"SM occupancy", t.sm_occupancy.empty() ? "-" : pct(t.sm_occupancy.last())},
            {"Tensor cores", t.tensor.empty() ? "-" : pct(t.tensor.last())},
            {"Memory", util::mib(g.mem_used_mib) + " / " + util::mib(g.mem_total_mib)},
            {"DRAM busy", t.dram.empty() ? "-" : pct(t.dram.last())},
            {"SM clock", g.sm_mhz < 0 ? "-" : std::to_string(static_cast<int>(g.sm_mhz)) + " / " + std::to_string(static_cast<int>(g.sm_max_mhz)) + " MHz"},
            {"Memory clock", g.mem_mhz < 0 ? "-" : std::to_string(static_cast<int>(g.mem_mhz)) + " / " + std::to_string(static_cast<int>(g.mem_max_mhz)) + " MHz"},
            {"Power", g.power_w < 0 ? "-" : watts(g.power_w) + (g.power_limit_w > 0 ? " / " + watts(g.power_limit_w) : "")},
            {"Temperature", g.temp_c < 0 ? "-" : std::to_string(static_cast<int>(g.temp_c)) + " °C"},
            {"ECC errors", g.ecc_corrected < 0 ? "-" : std::to_string(static_cast<long>(g.ecc_uncorrected)) + " uncorr. · " + std::to_string(static_cast<long>(g.ecc_corrected)) + " corr."},
            {"PCIe", t.pcie_rx.empty() ? "-" : chart::bytes(t.pcie_rx.last()) + "/s in · " + chart::bytes(t.pcie_tx.last()) + "/s out"},
            {"NVLink", t.nvlink_rx.empty() ? "-" : chart::bytes(t.nvlink_rx.last()) + "/s in · " + chart::bytes(t.nvlink_tx.last()) + "/s out"}};
        const int col_w = 27, per_row = std::max(1, width / col_w);
        Elements stat_rows;
        for (size_t i = 0; i < stats.size(); i += static_cast<size_t>(per_row)) {
            Elements labels, values;
            for (size_t k = i; k < std::min(stats.size(), i + static_cast<size_t>(per_row)); k++) {
                labels.push_back(text(stats[k].first) | color(chart::graphText()) | size(WIDTH, EQUAL, col_w));
                values.push_back(text(util::cut(stats[k].second, col_w - 1)) | bold | size(WIDTH, EQUAL, col_w));
            }
            stat_rows.push_back(hbox(std::move(labels)));
            stat_rows.push_back(hbox(std::move(values)));
        }

        // Its processes: which step and rank each one is
        Elements procs = {hbox({text(util::padRight("pid", 9)), text(util::padRight("process", static_cast<size_t>(std::max(10, width - 40)))),
                                text(util::padLeft("memory", 8)), text(util::padLeft("step", 9)), text(util::padLeft("rank", 6))}) |
                          color(chart::graphText())};
        for (const auto* a : appsOn(g.uuid)) {
            const auto sp = facts.pids.find({a->app.node, a->app.pid});
            procs.push_back(hbox({text(util::padRight(std::to_string(a->app.pid), 9)),
                                  text(util::padRight(util::cutMiddle(a->app.name, static_cast<size_t>(std::max(10, width - 41))),
                                                      static_cast<size_t>(std::max(10, width - 40)))) | bold,
                                  text(util::padLeft(util::mib(a->app.mem_mib), 8)),
                                  text(util::padLeft(sp == facts.pids.end() ? "?" : sp->second.step, 9)) | color(chart::cpuGradient().low),
                                  text(util::padLeft(sp == facts.pids.end() || sp->second.rank < 0 ? "-" : std::to_string(sp->second.rank), 6))}));
        }
        if (procs.size() == 1) procs.push_back(text(apps_error.empty() ? "no process on this GPU" : apps_error) | color(chart::graphText()));

        // The curves share what is left
        const int fixed = static_cast<int>(out.size()) + static_cast<int>(stat_rows.size()) + 1 + static_cast<int>(procs.size()) + 3 * 2;
        const int chart_h = std::max(4, (height - fixed) / 3);
        const auto chartOf = [&](const std::string& what, std::vector<std::pair<std::string, std::pair<const Series*, Color>>> curves,
                                 double scale, std::function<std::string(double)> y_label) {
            const int cols = chart::lineColumns(width, chart_h, y_label);
            std::vector<chart::Line> lines;
            Elements legend = {text(what) | bold, text("   ")};
            for (const auto& [name, sc] : curves) {
                lines.push_back({resample(*sc.first, now, step / 2, cols * 2, scale), sc.second, lines.empty()});
                legend.push_back(text("━ ") | bold | color(sc.second));
                legend.push_back(text(name + "  ") | color(chart::graphText()));
            }
            double tick = 10;
            for (double x : {10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600})
                if ((tick = x) >= cols / 5.0 * step) break;
            return vbox({hbox(std::move(legend)), chart::lineChart(lines, width, chart_h, y_label, x_label, std::max(4, static_cast<int>(tick / step)))});
        };
        out.push_back(chartOf("activity", {{"busy", {&t.util, chart::gpuGradient().mid}}, {"SM active", {&t.sm_active, chart::cachedGradient().mid}},
                                           {"SM occupancy", {&t.sm_occupancy, chart::availableGradient().mid}}, {"tensor", {&t.tensor, chart::writeGradient().high}}},
                              1, pctLabel));
        out.push_back(chartOf("memory", {{"in use", {&t.mem, chart::cachedGradient().mid}}, {"DRAM busy", {&t.dram, chart::usedGradient().mid}}}, 1, pctLabel));
        const double top = chart::niceMax(std::max({t.pcie_rx.peak(), t.pcie_tx.peak(), t.nvlink_rx.peak(), t.nvlink_tx.peak(), 1e6}));
        out.push_back(chartOf("links", {{"PCIe in", {&t.pcie_rx, chart::readGradient().high}}, {"PCIe out", {&t.pcie_tx, chart::writeGradient().high}},
                                        {"NVLink in", {&t.nvlink_rx, chart::freeGradient().mid}}, {"NVLink out", {&t.nvlink_tx, chart::tempGradient().high}}},
                          top, [top](double f) { return chart::bytes(f * top) + "/s"; }));
        out.push_back(vbox(std::move(stat_rows)));
        out.push_back(text(""));
        out.push_back(vbox(std::move(procs)));
        return vbox(std::move(out)) | size(WIDTH, EQUAL, width);
    }

    Element gpuView(int width, int height, double now) {
        gpu_selected = std::clamp(gpu_selected, 0, std::max(0, static_cast<int>(gpu_tracks.size()) - 1));
        if (gpu_zoom) return gpuDetail(width, height, now);
        if (width >= 130) {
            const int list_w = std::clamp(width * 2 / 5, 60, 80);
            return hbox({gpuList(list_w, height, now), text(" "), separatorLight() | color(chart::faint()), text(" "),
                         gpuDetail(width - list_w - 3, height, now)});
        }
        const int list_h = std::min(height / 3, static_cast<int>(gpu_tracks.size()) + 3);
        return vbox({gpuList(width, list_h, now), separatorLight() | color(chart::faint()), gpuDetail(width, height - list_h - 1, now)});
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
        Elements head;
        for (int v : views()) head.push_back(tab(v, v == 0 ? "overview" : v == 1 ? "performance" : v == 2 ? "gpus" : "inference"));
        for (auto& e : Elements{text("  "), text(state) | bold | color(stateColor(state)), text(" · " + partition) | color(chart::graphText()),
                                filler()})
            head.push_back(std::move(e));
        if (!cpu_error.empty() || !gpu_error.empty())
            head.push_back(text(util::cut(cpu_error.empty() ? gpu_error : cpu_error, static_cast<size_t>(inner_w / 2))) | color(theme::red()));
        else if (ended)
            head.push_back(text("ended: the curves keep what it did") | color(theme::yellow()));

        const int body_h = inner_h - 2;
        if (view == 2 && gpus <= 0) view = 0;
        Element body = view == 0   ? overview(inner_w, body_h, now)
                       : view == 1 ? performance(inner_w, body_h, now)
                       : view == 2 ? gpuView(inner_w, body_h, now)
                                   : inference.render(inner_w, body_h, now, ZOOMS[zoom], gpus > 0 ? &gpu : nullptr, gpus, job_id);

        std::string numbers;
        for (int v : views()) numbers += std::to_string(v + 1) + " ";
        Elements keys = keyHints({{numbers + "←→", "view"}, {"+ -", "zoom"}});
        if (view == 3 && inference.servers.size() > 1) keys.push_back(keyHint("↑↓", "server"));
        if (view == 2) {
            keys.push_back(keyHint("↑↓", "GPU"));
            keys.push_back(keyHint("Enter", gpu_zoom ? "list" : "zoom"));
        }
        if (view == 0) {
            keys.push_back(keyHint("↑↓", "step"));
            keys.push_back(keyHint("Enter", step_detail ? "list" : "details"));
        }
        if (view == 0)
            keys.push_back(keyHint("v", symbols == chart::Symbols::Bars ? "braille" : symbols == chart::Symbols::Braille ? "blocks" : "areas"));
        if (view == 1) keys.push_back(keyHint("↑↓", "resource"));
        for (auto& k : keyHints({{"r", "read again"}, {"q", "close"}})) keys.push_back(k);
        const std::string when =
            std::to_string(static_cast<int>(ZOOMS[zoom])) + " s per column · sstat 2 s" + (gpus > 0 ? " · nvidia-smi 2 s" : "") +
            (inference.any() ? " · metrics 2 s" : "");
        Elements foot = {hbox(std::move(keys)), filler()};
        if (inner_w >= 110) foot.push_back(text(when) | color(chart::graphText()));
        return panel(title,
                     vbox({hbox(std::move(head)), body | size(HEIGHT, EQUAL, body_h), hbox(std::move(foot))}) |
                         size(WIDTH, EQUAL, inner_w) | size(HEIGHT, EQUAL, inner_h),
                     theme::accent(), text(ended ? "stopped" : "live") | bold | color(ended ? theme::yellow() : theme::green()));
    }
};

}  // namespace ui
