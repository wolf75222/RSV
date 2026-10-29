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
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/usage.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// ---------------------------------------------------------------- curves

/* Readings of one quantity in time: (seconds since the epoch, value)
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
    double peak() const {
        double p = 0;
        for (const auto& [at, v] : points) p = std::max(p, v);
        return p;
    }
};

/* The series as `dots` values, one per `step` seconds, the newest at the right (at `now`), each divided by `scale`
- Several readings in a dot: their mean. None: the one before holds (sstat answers every 2 s, a dot can be 1 s)
- -1 before the first reading: nothing is drawn there
*/
inline std::vector<double> resample(const Series& s, double now, double step, int dots, double scale) {
    std::vector<double> out(static_cast<size_t>(std::max(0, dots)), -1);
    if (s.empty() || scale <= 0) return out;
    size_t k = 0;
    double held = -1;
    const double first = now - dots * step;
    while (k < s.points.size() && s.points[k].first <= first) held = s.points[k++].second;
    for (int i = 0; i < dots; i++) {
        const double end = first + (i + 1) * step;
        double sum = 0;
        int n = 0;
        while (k < s.points.size() && s.points[k].first <= end) {
            sum += s.points[k++].second;
            n++;
        }
        if (n > 0) held = sum / n;
        if (held >= 0) out[static_cast<size_t>(i)] = std::clamp(held / scale, 0.0, 1.0);
    }
    return out;
}

/* An area graph in braille, like btop: each character is 2 dots wide and 4 high, so `values` has 2 per column
- `heat`: the higher rows go yellow then red (memory: full is a danger); otherwise all in `base` (busy is good)
- `grid`: a faint line at each quarter of the height, where the curve doesn't cover it
*/
inline Element brailleGraph(const std::vector<double>& values, int height, Color base, bool heat, bool grid = false) {
    static const int left[4] = {0x40, 0x04, 0x02, 0x01}, right[4] = {0x80, 0x20, 0x10, 0x08};   // from the bottom
    height = std::max(1, height);
    const int columns = static_cast<int>(values.size() + 1) / 2;
    std::vector<int> dots(values.size());
    for (size_t i = 0; i < values.size(); i++) {
        const double v = values[i];
        dots[i] = v < 0 ? 0 : std::max(v > 0.004 ? 1 : 0, static_cast<int>(std::lround(v * height * 4)));
    }
    Elements rows;
    for (int r = 0; r < height; r++) {
        const int below = (height - 1 - r) * 4;   // dots under this row
        const double top = double(height - r) / height;
        const Color c = !heat ? base : top > 0.85 ? theme::red() : top > 0.6 ? theme::yellow() : base;
        const bool grid_row = grid && height >= 8 && r > 0 && r % (height / 4) == 0 && r < height - 1;
        Elements runs;
        std::string line;
        bool blank = false;   // what `line` holds: grid or curve
        auto flush = [&] {
            if (!line.empty()) runs.push_back(blank ? text(line) | color(theme::gray()) | dim : text(line) | color(c));
            line.clear();
        };
        for (int col = 0; col < columns; col++) {
            const size_t a = static_cast<size_t>(col) * 2, b = a + 1;
            const int la = std::clamp(dots[a] - below, 0, 4), lb = b < dots.size() ? std::clamp(dots[b] - below, 0, 4) : 0;
            int bits = 0;
            for (int d = 0; d < la; d++) bits |= left[d];
            for (int d = 0; d < lb; d++) bits |= right[d];
            if ((bits == 0) != blank) {
                flush();
                blank = bits == 0;
            }
            if (bits == 0) {
                line += grid_row ? "╌" : " ";
                continue;
            }
            const int cp = 0x2800 + bits;   // U+2800..U+28FF in UTF-8: 3 bytes
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        flush();
        rows.push_back(hbox(std::move(runs)));
    }
    return vbox(std::move(rows));
}

/* A curve like a task manager's, drawn on FTXUI's canvas: a line over a tinted area, a faint grid behind
- The canvas has 2 x 4 braille dots per character: `values` has 2 per column, -1 where there is no reading yet
- Its line (DrawPointLine) joins each value to the one before; the characters under it get a tinted background
- The grid: a line at each quarter of the height, and one every `grid_step` columns from the right (now)
*/
inline Element curve(const std::vector<double>& values, int height, Color c, int grid_step = 10) {
    height = std::max(1, height);
    const int columns = static_cast<int>(values.size() + 1) / 2;
    return canvas(columns * 2, height * 4, [values, height, columns, c, grid_step](Canvas& cv) {
        const int dots_h = height * 4;
        const Color tint = Color::Interpolate(0.22f, Color::RGB(0, 0, 0), c);
        auto dotY = [&](double v) { return dots_h - 1 - std::clamp(static_cast<int>(std::lround(v * (dots_h - 1))), 0, dots_h - 1); };

        // The grid first: the curve is drawn over it
        const auto faint = [](Cell& cell) {
            cell.foreground_color = theme::gray();
            cell.dim = true;
        };
        for (int r = 1; r < height - 1 && height >= 4; r++) {
            if (r % std::max(1, height / 4) != 0) continue;
            for (int col = 0; col < columns; col++) cv.DrawText(col * 2, r * 4, "╌", faint);
        }
        for (int col = columns - 1 - grid_step; grid_step > 0 && col > 0; col -= grid_step)
            for (int r = 0; r < height; r++) cv.DrawText(col * 2, r * 4, height >= 4 && r > 0 && r < height - 1 && r % std::max(1, height / 4) == 0 ? "┼" : "┆", faint);

        // The area under the line: each character whose bottom is below the lower of its two values
        for (int col = 0; col < columns; col++) {
            int lowest = -1;   // the largest dot row (the canvas counts from the top)
            for (int side = 0; side < 2; side++) {
                const size_t i = static_cast<size_t>(col * 2 + side);
                if (i < values.size() && values[i] >= 0) lowest = std::max(lowest, dotY(values[i]));
            }
            if (lowest < 0) continue;
            for (int r = lowest / 4 + (lowest % 4 == 3 ? 1 : 0); r < height; r++)
                cv.Style(col * 2, r * 4, [tint](Cell& cell) { cell.background_color = tint; });
        }

        // The line, from each value to the next
        const auto ink = [c](Cell& cell) {
            cell.foreground_color = c;
            cell.bold = true;
            cell.dim = false;
        };
        for (size_t i = 0; i < values.size(); i++) {
            if (values[i] < 0) continue;
            const int x = static_cast<int>(i), y = dotY(values[i]);
            if (i > 0 && values[i - 1] >= 0) cv.DrawPointLine(x - 1, dotY(values[i - 1]), x, y, ink);
            else cv.DrawPoint(x, y, true, ink);
        }
    });
}

// ---------------------------------------------------------------- the view

/* A running job, live (m), like btop: the curves of its CPUs, its memory and its GPUs since the view was opened
- sstat every 2 s (main.cpp, on a worker of its own: a slow squeue doesn't stop the curves)
- The GPUs: one srun --overlap step that stays, nvidia-smi --loop-ms in it; it ends when the view closes
- + and - change how much time a dot is: from 1 s (the last minutes) to 30 s (the last hour)
*/
struct MonitorView {
    static constexpr double ZOOMS[] = {1, 2, 5, 10, 30};

    std::string title, job_id, state, partition;
    int cpus = 1, gpus = 0, nodes = 1;
    double mem_limit_mib = 0;   // all its nodes (0: it asked for none)
    std::time_t started = 0;
    long limit = util::UNKNOWN;

    bool ended = false;         // it left the queue, or isn't running any more: the curves stay
    std::string cpu_error, gpu_error;
    bool gpu_waiting = false;   // the step is starting, nothing from nvidia-smi yet

    Series cpu, mem, gpu, gpu_mem, gpu_power;
    api::LiveUse live;
    bool has_live = false;

    struct GpuTrack {
        api::GpuUse last;
        Series util, mem;
        double seen = 0;
    };
    std::map<std::pair<std::string, int>, GpuTrack> gpu_tracks;   // by node, then index

    int zoom = 0;               // index in ZOOMS
    std::function<void()> reload;

    static double nowSeconds() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /* sstat: the memory as it is, and how busy its CPUs were since a reading far enough back
    - sstat counts CPU time in whole seconds, and doesn't say when it took it: over 2 s, a second more or less
      is 50% on 1 CPU. The base is a reading at least 6 s older (20/cpus s under 4 CPUs): the curve is smooth.
    - Less CPU time than before (a step ended): the readings start again from this one
    */
    std::vector<api::LiveUse> readings;

    void setLive(api::LiveUse now, const std::string& error) {
        cpu_error = error;
        if (!error.empty()) return;
        if (!readings.empty() && now.cpu_seconds < readings.back().cpu_seconds) readings.clear();
        readings.push_back(now);
        const double window = std::clamp(20.0 / std::max(1, cpus), 6.0, 20.0);
        const api::LiveUse* base = nullptr;
        for (const auto& r : readings)
            if (now.at - r.at >= window || (!base && now.at - r.at >= 1.5)) base = &r;
        if (base) {
            now.busy = std::clamp((now.cpu_seconds - base->cpu_seconds) / ((now.at - base->at) * cpus), 0.0, 1.0);
            cpu.add(now.at, now.busy);
        }
        while (readings.size() > 2 && now.at - readings[1].at >= window) readings.erase(readings.begin());
        mem.add(now.at, now.mem_mib);
        live = now;
        has_live = true;
    }

    // A line from nvidia-smi: that GPU's curves, and those of all of them together (mean use, memory, power)
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

    // ---------------------------------------------------------------- what it shows

    int gpu_offset = 0, gpu_page = 8;

    static std::string watts(double w) {
        if (w >= 1000) {
            const long tenths = std::lround(w / 100);
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " kW";
        }
        return std::to_string(static_cast<long>(w + 0.5)) + " W";
    }

    // q or Esc closes, + and - zoom, r reads again (and starts the GPU step again if it ended), j/k scroll the GPUs
    bool onEvent(Event e, bool& close) {
        if (e == Event::Character('r')) {
            if (reload) reload();
        } else if (e == Event::Character('+') || e == Event::Character('=')) {
            zoom = std::max(0, zoom - 1);
        } else if (e == Event::Character('-') || e == Event::Character('_')) {
            zoom = std::min(static_cast<int>(std::size(ZOOMS)) - 1, zoom + 1);
        } else if (!scrollKey(e, gpu_offset, gpu_page) && !e.is_mouse()) {
            close = e == Event::Escape || e == Event::Character('q') || e == Event::Character('m');
        }
        return true;
    }

    // "Used    ▕██████░░░░▏ 62%  320G": a gauge as btop draws them
    static Element meter(const std::string& label, double ratio, const std::string& value, int width, Color c) {
        const int bar_w = std::max(4, width - 10 - 6 - static_cast<int>(util::width(value)) - 1);
        return hbox({text(util::padRight(label, 10)) | dim, bar(std::clamp(ratio, 0.0, 1.0), bar_w, c),
                     text(util::padLeft(ratio < 0 ? "-" : percent(ratio), 5) + " ") | bold, text(value)});
    }

    static Element row(const std::string& label, const std::string& value) {
        return hbox({text(util::padRight(label, 10)) | dim, text(value) | bold});
    }

    // The mean and the highest of what the curve shows now
    static std::pair<double, double> meanPeak(const std::vector<double>& v) {
        double sum = 0, peak = -1;
        int n = 0;
        for (double x : v)
            if (x >= 0) {
                sum += x;
                peak = std::max(peak, x);
                n++;
            }
        return {n > 0 ? sum / n : -1, peak};
    }

    /* A curve with its scale, as a task manager shows it: what it is and its top above, its time span and 0 under
       (`height` counts those two lines) */
    Element plot(const std::vector<double>& values, const std::string& what, const std::string& top, Color c, int width,
                 int height, double window) const {
        const int h = std::max(1, height - 2);
        Element body = values.empty() || std::all_of(values.begin(), values.end(), [](double v) { return v < 0; })
                           ? vbox({text("waiting for a reading...") | dim, filler()}) | size(HEIGHT, EQUAL, h)
                           : curve(values, h, c, std::max(6, width / 6));
        return vbox({hbox({text(what) | dim, filler(), text(top) | dim}), body,
                     hbox({text(util::duration(static_cast<long>(window))) | dim, filler(), text("0") | dim})}) |
               size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
    }

    // A box of the dashboard, its name and figure on the top border
    static Element box(const std::string& name, const std::string& figure, Element content, Color c, int width, int height) {
        return panel(name, content | flex, c, text(figure) | bold) | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
    }

    Element render(int max_width, int max_height) {
        const int inner_w = std::max(40, max_width - 2), inner_h = std::max(12, max_height - 2);
        const double now = nowSeconds(), step = ZOOMS[zoom];
        const std::time_t now_t = static_cast<std::time_t>(now);
        const bool has_gpus = gpus > 0, wide = inner_w >= 100;

        // The job, in one line
        Elements head = {text(state) | bold | color(stateColor(state)),
                         text(" · " + std::to_string(nodes) + (nodes == 1 ? " node · " : " nodes · ") + thousands(cpus) +
                              (cpus == 1 ? " CPU" : " CPUs") + (has_gpus ? " · " + std::to_string(gpus) + (gpus == 1 ? " GPU" : " GPUs") : "")) |
                             dim};
        if (started > 0) {
            const long ran = static_cast<long>(now_t - started);
            head.push_back(text(" · run " + util::duration(ran)) | dim);
            if (limit > 0) head.push_back(text(" · " + util::duration(std::max(0L, limit - ran)) + " left") | dim);
        }
        if (!partition.empty()) head.push_back(text(" · " + partition) | dim);

        // Heights: the CPU on top, memory under it, the GPUs at the bottom (btop's cpu, mem and proc)
        const int body_h = inner_h - 2;
        const int cpu_h = has_gpus ? std::max(8, body_h * 3 / 10) : body_h / 2;
        const int mem_h = has_gpus ? std::max(8, body_h * 3 / 10) : body_h - cpu_h;
        const int gpu_h = has_gpus ? std::max(6, body_h - cpu_h - mem_h) : 0;

        // cpu: its curve, and beside it (btop's processor box) the figures
        const int info_w = wide ? 36 : 0;
        const int cpu_plot_w = inner_w - 2 - (wide ? info_w + 1 : 0);
        const double cpu_window = step * cpu_plot_w * 2;
        const auto cpu_values = resample(cpu, now, step, cpu_plot_w * 2, 1);
        const double busy = cpu.last();
        Element cpu_content = plot(cpu_values, "busy share of its CPUs", "100%", theme::cyan(), cpu_plot_w, cpu_h - 2, cpu_window);
        if (wide) {
            const auto [mean, peak] = meanPeak(cpu_values);
            Elements info = {meter("CPU", busy, "", info_w - 2, efficiencyColor(std::max(0.0, busy))),
                             row("cores", busy < 0 ? "-" : std::to_string(static_cast<long>(busy * cpus + 0.5)) + " / " + thousands(cpus) + " busy"),
                             row("mean", mean < 0 ? "-" : percent(mean) + "   peak " + percent(peak)),
                             row("CPU time", has_live ? util::duration(static_cast<long>(live.cpu_seconds)) : "-")};
            if (started > 0) info.push_back(row("run", util::duration(static_cast<long>(now_t - started)) +
                                                           (limit > 0 ? " · " + util::duration(std::max(0L, limit - static_cast<long>(now_t - started))) + " left" : "")));
            cpu_content = hbox({cpu_content, text(" "),
                                vbox(std::move(info)) | size(WIDTH, EQUAL, info_w - 2) | borderStyled(ROUNDED, theme::gray()) |
                                    size(HEIGHT, EQUAL, std::min(cpu_h - 2, 7))});
        }
        Elements boxes = {box("cpu", busy < 0 ? "" : percent(busy), cpu_content, theme::cyan(), inner_w, cpu_h)};

        // mem: btop's gauges, then the curve; GPU memory beside it the same way
        const int mem_w = has_gpus && wide ? inner_w / 2 : inner_w;
        const double mem_scale = mem_limit_mib > 0 ? mem_limit_mib : std::max(1.0, mem.peak() * 1.25);
        {
            const int w = mem_w - 2;
            const double used = has_live ? live.mem_mib : -1;
            Elements lines = {meter("used", used < 0 ? -1 : used / mem_scale, used < 0 ? "-" : util::mib(used), w, usageColor(used / mem_scale))};
            if (mem_h >= 10) {
                lines.push_back(row("asked", mem_limit_mib > 0 ? util::mib(mem_limit_mib) : "nothing: scaled on its peak"));
                lines.push_back(row("largest", has_live && live.max_task_mib > 0 ? util::mib(live.max_task_mib) + " (one task)" : "-"));
            }
            const int plot_h = mem_h - 2 - static_cast<int>(lines.size());
            lines.push_back(plot(resample(mem, now, step, w * 2, mem_scale), "memory in use", util::mib(mem_scale), theme::magenta(), w,
                                 plot_h, step * w * 2));
            Element mem_box = box("mem", used < 0 ? "" : util::mib(used), vbox(std::move(lines)), theme::magenta(), mem_w, mem_h);

            if (!has_gpus) {
                boxes.push_back(mem_box);
            } else {
                double gused = 0, gtotal = 0;
                for (const auto& [key, t] : gpu_tracks) {
                    gused += t.last.mem_used_mib;
                    gtotal += t.last.mem_total_mib;
                }
                const int gw = (wide ? inner_w - mem_w : inner_w) - 2;
                Elements glines = {meter("used", gtotal > 0 ? gused / gtotal : -1, gtotal > 0 ? util::mib(gused) : "-", gw,
                                         usageColor(gtotal > 0 ? gused / gtotal : 0))};
                if (mem_h >= 10) {
                    glines.push_back(row("total", gtotal > 0 ? util::mib(gtotal) : "-"));
                    glines.push_back(row("free", gtotal > 0 ? util::mib(gtotal - gused) : "-"));
                }
                const int gplot_h = mem_h - 2 - static_cast<int>(glines.size());
                glines.push_back(plot(resample(gpu_mem, now, step, gw * 2, 1), "GPU memory in use", "100%", theme::blue(), gw, gplot_h,
                                      step * gw * 2));
                Element gm_box = box("gpu mem", gtotal > 0 ? util::mib(gused) + " / " + util::mib(gtotal) : "", vbox(std::move(glines)),
                                     theme::blue(), wide ? inner_w - mem_w : inner_w, mem_h);
                if (wide) boxes.push_back(hbox({mem_box, gm_box}));
                else {
                    boxes.push_back(mem_box);
                    boxes.push_back(gm_box);
                }
            }
        }

        // gpu: the mean use of all, and beside it each GPU with its own little curve (btop's cores)
        if (has_gpus) {
            const int list_w = wide ? std::min(60, inner_w / 2) : 0;
            const int plot_w = inner_w - 2 - (wide ? list_w + 1 : 0);
            double power = 0;
            int idle = 0;
            for (const auto& [key, t] : gpu_tracks) {
                if (t.last.power_w > 0) power += t.last.power_w;
                idle += t.last.util >= 0 && t.last.util < 5;
            }
            Element content = plot(resample(gpu, now, step, plot_w * 2, 1), "mean use of its GPUs", "100%", theme::green(), plot_w,
                                   gpu_h - 2, step * plot_w * 2);
            if (wide) {
                Elements list;
                gpu_page = std::max(1, gpu_h - 3);
                gpu_offset = std::clamp(gpu_offset, 0, std::max(0, static_cast<int>(gpu_tracks.size()) - gpu_page));
                const int mini_w = std::max(4, list_w - 2 - 16 - 5 - 6 - 8);
                int i = 0;
                for (const auto& [key, t] : gpu_tracks) {
                    if (i++ < gpu_offset || static_cast<int>(list.size()) >= gpu_page) continue;
                    const auto& g = t.last;
                    const double use = g.util < 0 ? 0 : g.util / 100;
                    list.push_back(hbox({text(util::padRight(util::cut(key.first, 13) + ":" + std::to_string(key.second), 16)) | color(theme::cyan()),
                                         brailleGraph(resample(t.util, now, step * plot_w * 2 / (mini_w * 2), mini_w * 2, 1), 1,
                                                      efficiencyColor(use), false),
                                         text(g.util < 0 ? "    -" : util::padLeft(percent(use), 5)) | bold | color(efficiencyColor(use)),
                                         text(g.temp_c < 0 ? "      " : util::padLeft(std::to_string(static_cast<int>(g.temp_c)) + "°C", 6)) |
                                             color(g.temp_c >= 85 ? theme::red() : g.temp_c >= 75 ? theme::yellow() : theme::gray()),
                                         text(g.power_w < 0 ? "" : util::padLeft(watts(g.power_w), 8)) | dim}) |
                                   (now - t.seen > 15 ? dim : nothing));
                }
                if (gpu_tracks.empty()) list.push_back(text(gpu_waiting ? "starting nvidia-smi in the job..." : "no GPU answered") | dim);
                std::string foot = idle > 0 ? std::to_string(idle) + " almost idle: does the code use them?" : "";
                if (static_cast<int>(gpu_tracks.size()) > gpu_page)
                    foot += (foot.empty() ? "" : " · ") + std::to_string(gpu_offset + 1) + "-" +
                            std::to_string(std::min<int>(gpu_offset + gpu_page, static_cast<int>(gpu_tracks.size()))) + " of " +
                            std::to_string(gpu_tracks.size()) + " (j/k)";
                list.push_back(filler());
                list.push_back(text(foot) | color(idle > 0 ? theme::yellow() : theme::gray()));
                content = hbox({content, text(" "), vbox(std::move(list)) | size(WIDTH, EQUAL, list_w - 2) |
                                                        borderStyled(ROUNDED, theme::gray()) | size(HEIGHT, EQUAL, gpu_h - 2)});
            }
            std::string figure = gpu.empty() ? "" : percent(gpu.last()) + " mean";
            if (power > 0) figure += " · " + watts(power);
            boxes.push_back(box("gpu", figure, content, theme::green(), inner_w, gpu_h));
        }
        boxes.push_back(filler());

        // What went wrong, else the keys
        Element bottom;
        if (ended) bottom = text("The job isn't running any more: the curves keep what it did") | color(theme::yellow());
        else if (!cpu_error.empty() || !gpu_error.empty())
            bottom = text(util::cut(cpu_error.empty() ? gpu_error : gpu_error.empty() ? cpu_error : cpu_error + " · " + gpu_error,
                                    static_cast<size_t>(inner_w))) |
                     color(theme::red());
        else
            bottom = hbox({hbox(keyHints({{"+/-", "zoom"}, {"r", "read again"}, {"j/k", "GPUs"}, {"q", "close"}})), filler(),
                           text(std::to_string(static_cast<int>(step)) + " s per dot") | dim});

        const std::string when = std::string(ended ? "stopped" : "live") + " · sstat 2 s" + (has_gpus ? " · nvidia-smi 2 s" : "");
        return panel(title, vbox({hbox(std::move(head)), vbox(std::move(boxes)) | size(HEIGHT, EQUAL, body_h), bottom}) |
                                size(WIDTH, EQUAL, inner_w) | size(HEIGHT, EQUAL, inner_h),
                     theme::accent(), text(when) | dim);
    }
};

}  // namespace ui
