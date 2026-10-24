#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/slurmjobs.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

/* History (a): the jobs of the last days, from sacct, with what they really used
- CPU: CPU time used / CPU time allocated. MEM: largest memory used / memory per node. TIME: run time / --time.
- Low values mean the job asked for more than it needed: it waited longer in the queue for nothing
- On top, what the jobs shown consumed: CPU hours reserved, used and idle, GPU hours, --time asked and used
- Filtered and sorted on the screen (←/→, s): sacct runs once, not once per filter
*/
struct HistoryView {
    std::vector<api::HistoryJob> jobs;
    std::string error;
    bool loading = true;
    int days = 7;
    int filter = 0;
    int sort = 0;
    int selected = 0;
    int offset = 0;
    int page = 20;
    std::function<void()> reload;

    static const std::vector<std::string>& filters() {
        static const std::vector<std::string> f = {"all", "completed", "failed", "timeout", "cancelled", "active"};
        return f;
    }

    static bool matches(const api::HistoryJob& j, int filter) {
        switch (filter) {
            case 1: return j.state == "COMPLETED";
            case 2: return api::isFailure(j.state) && j.state != "TIMEOUT";
            case 3: return j.state == "TIMEOUT";
            case 4: return j.state == "CANCELLED";
            case 5: return api::isActive(j.state) || api::isPending(j.state);
            default: return true;
        }
    }

    static const std::vector<std::string>& sorts() {
        static const std::vector<std::string> s = {"newest", "idle CPU-h", "CPU-h", "GPU-h"};
        return s;
    }

    // The jobs of the filter, newest first (as sacct gave them) or the most hours first
    std::vector<const api::HistoryJob*> shown() const {
        std::vector<const api::HistoryJob*> out;
        for (const auto& j : jobs)
            if (matches(j, filter)) out.push_back(&j);
        auto key = [this](const api::HistoryJob* j) {
            return sort == 1 ? j->idleCpuHours() : sort == 2 ? j->cpuHours() : j->gpuHours();
        };
        if (sort > 0)
            std::stable_sort(out.begin(), out.end(),
                             [&](const api::HistoryJob* a, const api::HistoryJob* b) { return key(a) > key(b); });
        return out;
    }

    // 0.04 -> "<0.1 h", 2.43 -> "2.4 h", 1234.6 -> "1 235 h"; without the unit for the columns
    static std::string hours(double h, bool unit = true) {
        if (h < 0) return "-";
        std::string s;
        if (h < 0.05) {
            s = !unit ? "0.0" : h > 0 ? "<0.1" : "0";
        } else if (h < 10) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%.1f", h);
            s = buf;
        } else {
            s = thousands(static_cast<long>(h + 0.5));
        }
        return unit ? s + " h" : s;
    }

    // A line in parts of their own style: the first of `variants` that fits in `width`, else the last one cut
    using Part = std::pair<std::string, Decorator>;
    static Elements fitted(const std::vector<std::vector<Part>>& variants, int width) {
        for (size_t v = 0; v < variants.size(); v++) {
            size_t w = 0;
            for (const auto& part : variants[v]) w += util::width(part.first);
            if (static_cast<int>(w) > width && v + 1 < variants.size()) continue;
            Elements out;
            int left = width;
            for (const auto& [str, style] : variants[v]) {
                if (left <= 0) break;
                const std::string shown = util::cut(str, static_cast<size_t>(left));
                left -= static_cast<int>(util::width(shown));
                out.push_back(text(shown) | style);
            }
            return out;
        }
        return {};
    }

    /* Reserved, used, idle: the summary's lines, for the jobs of the filter
    - Shorter lines when the window is narrow, the bar only when there is room
    */
    static Elements summary(const api::Consumption& c, int width) {
        const Part cpu = {"CPU   ", dim}, gpu = {"GPU   ", dim}, time = {"Time  ", dim};
        Elements out;

        if (c.ended == 0) {
            out.push_back(hbox(fitted({{cpu, {c.running ? "no job has ended yet" : "no job", dim}}}, width)));
        } else {
            const double idle = std::max(0.0, c.idleShare());
            const Decorator shade = color(efficiencyColor(1 - idle));
            const Part reserved = {hours(c.cpu_h), bold}, idle_h = {hours(c.idle_cpu_h), bold | shade},
                       idle_share = {" idle (" + percent(idle) + ")", shade};
            const std::vector<Part> full = {cpu, reserved, {" reserved · ", dim}, {hours(c.used_cpu_h), nothing}, {" used · ", dim},
                                            idle_h, idle_share};
            const std::vector<Part> compact = {cpu, reserved, {" reserved · ", dim}, idle_h, idle_share};
            std::vector<std::vector<Part>> variants = {full, compact, {cpu, reserved, {" · ", dim}, idle_h, idle_share}};
            if (c.idle_cpu_h_gpu_jobs >= 0.05) {
                // The part of GPU jobs before the hours used: they are the reserved minus the idle ones
                const Part in_gpu_jobs = {", " + hours(c.idle_cpu_h_gpu_jobs) + " in GPU jobs", dim};
                std::vector<Part> a = full, b = compact;
                a.push_back(in_gpu_jobs);
                b.push_back(in_gpu_jobs);
                variants.insert(variants.begin(), {a, b});
            }
            Elements line = fitted(variants, width);
            Element row = hbox(line);
            row->ComputeRequirement();
            const int room = width - row->requirement().min_x - 3;
            if (room >= 8) line.push_back(hbox({text("   "), bar(1 - idle, std::min(room, 24), efficiencyColor(1 - idle))}));
            out.push_back(hbox(std::move(line)));
        }

        const std::string running = " · " + std::to_string(c.running) +
                                    (c.running == 1 ? " running job counts once ended" : " running jobs count once ended");
        if (c.gpu_jobs > 0) {
            const Part h = {hours(c.gpu_h), bold};
            const std::string by = std::to_string(c.gpu_jobs) + (c.gpu_jobs == 1 ? " job" : " jobs");
            std::vector<std::vector<Part>> variants = {{gpu, h, {" reserved by " + by, dim}}, {gpu, h, {" · " + by, dim}}};
            if (c.running > 0) variants.insert(variants.begin(), {gpu, h, {" reserved by " + by, dim}, {running, dim}});
            out.push_back(hbox(fitted(variants, width)));
        } else {
            std::vector<std::vector<Part>> variants = {{gpu, {"none", dim}}};
            if (c.running > 0) variants.insert(variants.begin(), {gpu, {"none", dim}, {running, dim}});
            out.push_back(hbox(fitted(variants, width)));
        }

        if (c.asked_h > 0) {
            const double unused = std::max(0.0, c.unusedShare());
            const Decorator shade = color(efficiencyColor(1 - unused));
            const Part not_used = {hours(c.asked_h - c.ran_h), bold | shade};
            const std::string share = " not used (" + percent(unused) + ")";
            out.push_back(hbox(fitted({{time, {hours(c.ran_h), nothing}, {" run of the ", dim}, {hours(c.asked_h), nothing},
                                        {" asked by completed jobs · ", dim}, not_used, {share, shade}},
                                       {time, not_used, {" of the " + hours(c.asked_h) + " asked" + share, shade}},
                                       {time, not_used, {share, shade}}},
                                      width)));
        } else {
            out.push_back(hbox(fitted({{time, {"no completed job", dim}}}, width)));
        }
        return out;
    }

    // The selected job's hours, in one line
    static std::string usage(const api::HistoryJob& j) {
        if (j.cpuHours() <= 0 && j.gpuHours() <= 0) return "";
        std::string s = "CPU " + hours(j.cpuHours()) + " reserved";
        if (j.usedCpuHours() >= 0) s += ", " + hours(j.usedCpuHours()) + " used, " + hours(j.idleCpuHours()) + " idle";
        else s += " so far";
        if (j.gpus > 0) s += " · GPU " + hours(j.gpuHours()) + (j.ended() ? "" : " so far");
        if (j.unusedTime() > 0)
            s += " · ran " + util::duration(j.elapsed) + " of " + util::duration(j.limit) + ", " + util::duration(j.unusedTime()) +
                 " not used";
        return s;
    }

    void setData(std::vector<api::HistoryJob> data, const std::string& err) {
        jobs = std::move(data);
        error = err;
        loading = false;
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(shown().size()) - 1));
    }

    bool onEvent(Event e, bool& close) {
        const int count = static_cast<int>(shown().size());
        if (e == Event::Escape || e == Event::Character('q') || e == Event::Character('a')) close = true;
        else if (e == Event::ArrowDown || e == Event::Character('j')) selected++;
        else if (e == Event::ArrowUp || e == Event::Character('k')) selected--;
        else if (e == Event::PageDown || e == Event::CtrlD) selected += page;
        else if (e == Event::PageUp || e == Event::CtrlU) selected -= page;
        else if (e == Event::Home || e == Event::Character('g')) selected = 0;
        else if (e == Event::End || e == Event::Character('G')) selected = count - 1;
        else if (e == Event::ArrowRight || e == Event::Character('l') || e == Event::Tab) {
            filter = (filter + 1) % static_cast<int>(filters().size());
            selected = offset = 0;
        } else if (e == Event::ArrowLeft || e == Event::Character('h') || e == Event::TabReverse) {
            filter = (filter + static_cast<int>(filters().size()) - 1) % static_cast<int>(filters().size());
            selected = offset = 0;
        } else if (e == Event::Character('s')) {
            sort = (sort + 1) % static_cast<int>(sorts().size());
            selected = offset = 0;
        } else if (e == Event::Character('r')) {
            loading = true;
            if (reload) reload();
        } else if (e.is_mouse() && e.mouse().button == Mouse::WheelDown) selected += 3;
        else if (e.is_mouse() && e.mouse().button == Mouse::WheelUp) selected -= 3;
        else return false;
        selected = std::clamp(selected, 0, std::max(0, count - 1));
        return true;
    }

    // Advice from what the job used, the most useful first
    static std::string advice(const api::HistoryJob& j) {
        if (j.state == "OUT_OF_MEMORY") return "Ran out of memory: ask for more (--mem)";
        if (j.state == "TIMEOUT") return "Reached its time limit: ask for more --time, or save checkpoints";
        if (api::isActive(j.state) || api::isPending(j.state)) return "";
        const double cpu = j.cpuEfficiency(), mem = j.memEfficiency(), t = j.timeUsed();
        if (j.elapsed >= 300 && cpu >= 0 && cpu < 0.5)
            return "Used " + percent(cpu) + " of its CPUs: fewer CPUs would do, or check the parallelism";
        if (j.elapsed >= 300 && mem >= 0 && mem < 0.25)
            return "Used " + percent(mem) + " of its memory: a smaller --mem starts sooner";
        if (j.state == "COMPLETED" && t >= 0 && t < 0.25 && j.limit >= 3600)
            return "Used " + percent(t) + " of its --time: a shorter one starts sooner (backfill)";
        return "";
    }

    Element render(int width, int height) {
        const int inner = std::max(20, width - 4);
        page = std::max(1, height - 14);
        const auto rows = shown();
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(rows.size()) - 1));
        if (selected < offset) offset = selected;
        if (selected >= offset + page) offset = selected - page + 1;
        offset = std::clamp(offset, 0, std::max(0, static_cast<int>(rows.size()) - page));

        // Columns that fit, from the most useful, while the name keeps 20 columns
        size_t id_w = 6;
        bool any_gpu = false;
        for (const auto* j : rows) {
            id_w = std::max(id_w, util::width(j->id));
            any_gpu = any_gpu || j->gpus > 0;
        }
        id_w = std::min<size_t>(id_w, 16);
        int fixed = static_cast<int>(id_w) + 1 + 11 + 8 + 5;
        auto fits = [&](int w, bool wanted = true) {
            if (!wanted || inner - fixed - w < 20) return false;
            fixed += w;
            return true;
        };
        const bool show_mem = fits(6), show_cpuh = fits(7), show_gpuh = fits(7, any_gpu), show_time = fits(6),
                   show_exit = fits(6), show_end = fits(13);
        const size_t name_w = static_cast<size_t>(std::max(6, inner - fixed));

        auto cell = [](const std::string& s, size_t w, bool right = false) {
            return text(right ? util::padLeft(s, w - 1) + " " : util::padRight(s, w));
        };
        auto eff = [&](double v, size_t w) {
            return v < 0 ? cell("-", w, true) | dim : cell(percent(v), w, true) | color(efficiencyColor(v));
        };

        Elements header = {cell("JOBID", id_w + 1), cell("NAME", name_w), cell("STATE", 11), cell("ELAPSED", 8, true),
                           cell("CPU", 5, true)};
        if (show_mem) header.push_back(cell("MEM", 6, true));
        if (show_cpuh) header.push_back(cell("CPU-H", 7, true));
        if (show_gpuh) header.push_back(cell("GPU-H", 7, true));
        if (show_time) header.push_back(cell("TIME", 6, true));
        if (show_exit) header.push_back(cell("EXIT", 6, true));
        if (show_end) header.push_back(cell("END", 13, true));

        Elements lines = {hbox(std::move(header)) | dim};
        const std::time_t now = std::time(nullptr);
        for (int i = offset; i < std::min(static_cast<int>(rows.size()), offset + page); i++) {
            const auto& j = *rows[static_cast<size_t>(i)];
            Elements row = {cell(j.id, id_w + 1), cell(util::cut(j.name, name_w - 1), name_w),
                            cell(j.state == "OUT_OF_MEMORY" ? "OOM" : j.state, 11) | color(stateColor(j.state)), cell(util::duration(j.elapsed), 8, true),
                            eff(j.cpuEfficiency(), 5)};
            if (show_mem) row.push_back(eff(j.memEfficiency(), 6));
            if (show_cpuh) row.push_back(j.cpuHours() > 0 ? cell(hours(j.cpuHours(), false), 7, true) : cell("-", 7, true) | dim);
            if (show_gpuh) row.push_back(j.gpuHours() > 0 ? cell(hours(j.gpuHours(), false), 7, true) : cell("-", 7, true) | dim);
            if (show_time) row.push_back(eff(j.timeUsed(), 6));
            if (show_exit)
                row.push_back(cell(j.exit_code, 6, true) | (j.exit_code == "0:0" ? Decorator(dim) : color(theme::red())));
            if (show_end) row.push_back(cell(j.end ? util::clock(j.end, now) : "-", 13, true) | dim);
            Element r = hbox(std::move(row));
            lines.push_back(i == selected ? r | inverted : r);
        }
        if (loading && jobs.empty()) lines.push_back(text("Reading sacct...") | dim);
        else if (!error.empty()) lines.push_back(paragraph(error) | color(theme::red()));
        else if (rows.empty()) lines.push_back(text("No job") | dim);
        while (static_cast<int>(lines.size()) < page + 1) lines.push_back(text(""));

        // Filters, and counts
        Elements tabs;
        for (int i = 0; i < static_cast<int>(filters().size()); i++) {
            int n = 0;
            for (const auto& j : jobs) n += matches(j, i);
            const std::string label = " " + filters()[static_cast<size_t>(i)] + " " + std::to_string(n) + " ";
            tabs.push_back(i == filter ? text(label) | inverted | bold : text(label) | dim);
        }

        // The selected job, in two lines, with advice
        Element info = text("");
        Element used = text("");
        Element tip = text("");
        if (!rows.empty()) {
            const auto& j = *rows[static_cast<size_t>(selected)];
            std::string s = j.id + "  " + j.state_detail + " · " + std::to_string(j.nodes) + "n " + std::to_string(j.cpus) + "c";
            if (j.gpus > 0) s += " " + std::to_string(j.gpus) + "g";
            if (!j.nodelist.empty() && j.nodelist != "None assigned") s += " on " + j.nodelist;
            if (j.max_rss_mib > 0) s += " · peak " + util::mib(j.max_rss_mib) + " of " + util::mib(j.mem_mib) + (j.nodes > 1 ? "/node" : "");
            if (j.limit > 0) s += " · limit " + util::duration(j.limit);
            if (!show_exit && !j.exit_code.empty() && j.exit_code != "0:0") s += " · exit " + j.exit_code;
            info = text(util::cut(s, static_cast<size_t>(inner)));
            used = text(util::cut(usage(j), static_cast<size_t>(inner))) | dim;
            const std::string a = advice(j);
            if (!a.empty()) tip = text(util::cut("> " + a, static_cast<size_t>(inner))) | color(theme::yellow());
        }

        std::string right = loading ? "reading..." : std::to_string(rows.size()) + " jobs";
        if (sort > 0 && !loading) right = "by " + sorts()[static_cast<size_t>(sort)] + " · " + right;
        return panel("History · last " + std::to_string(days) + " days",
                     vbox({hbox(std::move(tabs)), separator() | dim, vbox(summary(api::consumption(rows), inner)), separator() | dim,
                           vbox(std::move(lines)) | flex, separator() | dim, info, used, tip,
                           hbox(keyHints({{"←/→", "filter"}, {"s", "sort"}, {"j/k", "move"}, {"r", "refresh"}, {"q", "close"}}))}) |
                         size(WIDTH, EQUAL, width - 2) | size(HEIGHT, EQUAL, height - 2),
                     theme::accent(), text(right));
    }
};

}  // namespace ui
