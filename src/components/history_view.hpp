#pragma once

#include <functional>
#include <string>
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
- Filtered on the screen (←/→): sacct runs once, not once per filter
*/
struct HistoryView {
    std::vector<api::HistoryJob> jobs;
    std::string error;
    bool loading = true;
    int days = 7;
    int filter = 0;
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

    std::vector<const api::HistoryJob*> shown() const {
        std::vector<const api::HistoryJob*> out;
        for (const auto& j : jobs)
            if (matches(j, filter)) out.push_back(&j);
        return out;
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
        page = std::max(1, height - 9);
        const auto rows = shown();
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(rows.size()) - 1));
        if (selected < offset) offset = selected;
        if (selected >= offset + page) offset = selected - page + 1;
        offset = std::clamp(offset, 0, std::max(0, static_cast<int>(rows.size()) - page));

        // Columns that fit, from the most useful
        size_t id_w = 6;
        for (const auto* j : rows) id_w = std::max(id_w, util::width(j->id));
        id_w = std::min<size_t>(id_w, 16);
        const bool show_end = inner >= 99, show_time = inner >= 84, show_exit = inner >= 76, show_mem = inner >= 66;
        int fixed = static_cast<int>(id_w) + 1 + 11 + 8 + 5;
        if (show_mem) fixed += 6;
        if (show_time) fixed += 6;
        if (show_exit) fixed += 6;
        if (show_end) fixed += 13;
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
        if (show_time) header.push_back(cell("TIME", 6, true));
        if (show_exit) header.push_back(cell("EXIT", 6, true));
        if (show_end) header.push_back(cell("END", 13, true));

        Elements lines = {hbox(std::move(header)) | dim};
        const std::time_t now = std::time(nullptr);
        for (int i = offset; i < std::min(static_cast<int>(rows.size()), offset + page); i++) {
            const auto& j = *rows[static_cast<size_t>(i)];
            Elements row = {cell(j.id, id_w + 1), cell(j.name, name_w),
                            cell(j.state == "OUT_OF_MEMORY" ? "OOM" : j.state, 11) | color(stateColor(j.state)), cell(util::duration(j.elapsed), 8, true),
                            eff(j.cpuEfficiency(), 5)};
            if (show_mem) row.push_back(eff(j.memEfficiency(), 6));
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

        // The selected job, in one line, with advice
        Element info = text("");
        Element tip = text("");
        if (!rows.empty()) {
            const auto& j = *rows[static_cast<size_t>(selected)];
            std::string s = j.id + "  " + j.state_detail + " · " + std::to_string(j.nodes) + "n " + std::to_string(j.cpus) + "c";
            if (!j.nodelist.empty() && j.nodelist != "None assigned") s += " on " + j.nodelist;
            if (j.max_rss_mib > 0) s += " · peak " + util::mib(j.max_rss_mib) + " of " + util::mib(j.mem_mib) + (j.nodes > 1 ? "/node" : "");
            if (j.limit > 0) s += " · limit " + util::duration(j.limit);
            info = text(util::cut(s, static_cast<size_t>(inner)));
            const std::string a = advice(j);
            if (!a.empty()) tip = text(util::cut("> " + a, static_cast<size_t>(inner))) | color(theme::yellow());
        }

        return panel("History · last " + std::to_string(days) + " days",
                     vbox({hbox(std::move(tabs)), separator() | dim, vbox(std::move(lines)) | flex, separator() | dim, info, tip,
                           hbox(keyHints({{"←/→", "filter"}, {"j/k", "move"}, {"r", "refresh"}, {"q", "close"}}))}) |
                         size(WIDTH, EQUAL, width - 2) | size(HEIGHT, EQUAL, height - 2),
                     theme::accent(), text(loading ? "reading..." : std::to_string(rows.size()) + " jobs"));
    }
};

}  // namespace ui
