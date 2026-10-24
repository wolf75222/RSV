#pragma once

#include <algorithm>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "../api/arrays.hpp"
#include "../api/slurmjobs.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// What the last column shows: time left for a running job, expected start for a pending one
inline std::string timeColumn(const api::Job& j, long used, std::time_t now) {
    if (j.active() && j.limit > 0 && used >= 0) return util::duration(std::max(0L, j.limit - used));
    if (j.pending() && j.start > now) return "~" + util::duration(static_cast<long>(std::difftime(j.start, now)));
    return "";
}

constexpr int MARK_WIDTH = 1;   // ▌ in front of the rows marked with Space
constexpr int STATE_WIDTH = 3;
constexpr int TIME_WIDTH = 7;

// The widths of the list's columns, the same for every row
struct ListColumns {
    int width = 40;   // the whole row
    int id = 5;
    int user = 0;     // 0: no user column (your own jobs)

    int name() const { return std::max(1, width - MARK_WIDTH - STATE_WIDTH - id - 1 - (user ? user + 1 : 0) - TIME_WIDTH - 1); }
};

/* An array in the list: all its tasks under one row with their progress, Enter shows them
- `tasks`: its records in squeue; `past_known`: sacct answered, the tasks ended long ago are counted
*/
struct JobArray {
    std::string id;
    std::vector<const api::Job*> tasks;
    api::ArrayProgress progress;
    bool past_known = false;

    const api::Job& first() const { return *tasks.front(); }
    std::string shownId() const { return id + "_*"; }

    // Running while a task runs, then pending, then how it ended (failed as soon as a task failed)
    std::string state() const {
        if (progress.running) return "RUNNING";
        if (progress.pending) return "PENDING";
        if (progress.failed) return "FAILED";
        if (progress.cancelled && !progress.done) return "CANCELLED";
        return "COMPLETED";
    }

    // The record of its tasks not started yet (812402_[3-40]), or nullptr
    const api::Job* waiting() const {
        for (const api::Job* t : tasks)
            if (t->pending() && t->id.find('[') != std::string::npos) return t;
        return nullptr;
    }
};

/* The tasks of an array as one bar: done, failed, cancelled, running, waiting
- Each part gets its share of the cells and at least one: a failed task never disappears in a big array
*/
inline Element progressBar(const api::ArrayProgress& p, int width) {
    const int total = p.total();
    const std::vector<std::pair<int, Color>> parts = {{p.done, theme::blue()},     {p.failed, theme::red()},
                                                      {p.cancelled, theme::magenta()}, {p.running, theme::green()},
                                                      {p.pending, theme::gray()}};
    if (width < static_cast<int>(parts.size()) || total <= 0) return text("");
    std::vector<int> cells(parts.size(), 0);
    int used = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        if (parts[i].first == 0) continue;
        cells[i] = std::max(1, static_cast<int>(double(parts[i].first) * width / total + 0.5));
        used += cells[i];
    }
    for (; used > width; used--) {
        auto largest = std::max_element(cells.begin(), cells.end());
        if (*largest <= 1) break;
        (*largest)--;
    }
    for (; used < width; used++) (*std::max_element(cells.begin(), cells.end()))++;
    Elements out;
    for (size_t i = 0; i < parts.size(); i++) {
        std::string s;
        for (int k = 0; k < cells[i]; k++) s += i + 1 == parts.size() ? "░" : "█";
        if (!s.empty()) out.push_back(text(s) | color(parts[i].second));
    }
    return hbox(std::move(out));
}

// What the selection and the marks follow: the job's id; for the waiting tasks of an array, a name that stays
// when some of them start (812402_[159-200] becomes 812402_[161-200])
inline std::string rowKey(const api::Job& j) {
    return j.id.find("_[") != std::string::npos ? api::arrayOf(j.id) + "_[waiting]" : j.id;
}

inline Element markCell(bool marked) {
    return marked ? text("▌") | color(theme::yellow()) | bold : text(" ");
}

/* A job: state, id, (user), name, time. Cut by display width, so CJK or emoji names keep the columns aligned.
- `prefix`: ├ or └ for the tasks shown under their array
*/
inline Element jobRow(const api::Job& j, const ListColumns& c, long used, bool selected, std::time_t now, bool marked = false,
                      const std::string& prefix = "") {
    Elements cells = {
        markCell(marked),
        text(util::padRight(api::shortState(j.state), STATE_WIDTH)) | color(stateColor(j.state)) | bold,
        text(util::padRight(j.id, static_cast<size_t>(c.id)) + " "),
    };
    if (c.user) cells.push_back(text(util::padRight(j.user, static_cast<size_t>(c.user)) + " ") | color(theme::cyan()));
    const int name_w = c.name();
    if (!prefix.empty() && name_w > 4)
        cells.push_back(hbox({text(prefix) | dim, text(util::padRight(j.name, static_cast<size_t>(name_w - 2)) + " ")}));
    else
        cells.push_back(text(util::padRight(j.name, static_cast<size_t>(name_w)) + " "));
    cells.push_back(text(util::padLeft(timeColumn(j, used, now), TIME_WIDTH)) | (j.active() ? nothing : dim));
    Element row = hbox(std::move(cells));
    return selected ? row | inverted : row;
}

// An array: its state, 812402_*, its name, the bar of its tasks and how many have ended
inline Element arrayRow(const JobArray& a, const ListColumns& c, const std::string& time, bool open, bool selected, bool marked) {
    const api::ArrayProgress& p = a.progress;
    const std::string state = a.state();
    Elements cells = {
        markCell(marked),
        text(util::padRight(api::shortState(state), STATE_WIDTH)) | color(stateColor(state)) | bold,
        text(util::padRight(a.shownId(), static_cast<size_t>(c.id)) + " "),
    };
    if (c.user) cells.push_back(text(util::padRight(a.first().user, static_cast<size_t>(c.user)) + " ") | color(theme::cyan()));

    const int name_w = c.name();
    const std::string count = std::to_string(p.done + p.failed + p.cancelled) + "/" + std::to_string(p.total());
    const int count_w = static_cast<int>(util::width(count));
    const int bar_w = std::min(10, name_w - 2 - 8 - 1 - 1 - count_w);   // 8 columns of name at least
    const bool with_count = name_w >= 2 + 4 + 1 + count_w;
    const int text_w = std::max(1, name_w - 2 - (with_count ? count_w + 1 : 0) - (bar_w >= 5 ? bar_w + 1 : 0));
    Elements name = {text(open ? "▾ " : "▸ ") | dim, text(util::padRight(a.first().name, static_cast<size_t>(text_w)) + " ")};
    if (bar_w >= 5) name.push_back(hbox({progressBar(p, bar_w), text(" ")}));
    if (with_count) name.push_back(text(count) | (p.failed ? color(theme::red()) : nothing));
    cells.push_back(hbox(std::move(name)) | size(WIDTH, EQUAL, name_w + 1));
    cells.push_back(text(util::padLeft(time, TIME_WIDTH)) | (api::isActive(state) ? nothing : dim));
    Element row = hbox(std::move(cells));
    return selected ? row | inverted : row;
}

inline Element jobListHeader(const ListColumns& c) {
    Elements cells = {text(std::string(MARK_WIDTH, ' ')), text(util::padRight("ST", STATE_WIDTH)),
                      text(util::padRight("JOBID", static_cast<size_t>(c.id)) + " ")};
    if (c.user) cells.push_back(text(util::padRight("USER", static_cast<size_t>(c.user)) + " "));
    cells.push_back(text(util::padRight("NAME", static_cast<size_t>(c.name())) + " "));
    cells.push_back(text(util::padLeft("LEFT", TIME_WIDTH)));
    return hbox(std::move(cells)) | dim;
}

inline Element emptyList(bool filtered, const std::string& error) {
    Elements lines = {text("")};
    if (!error.empty()) {
        lines.push_back(paragraph(error) | color(theme::red()));
        lines.push_back(text(""));
        lines.push_back(text("r to try again") | dim);
    } else if (filtered) {
        lines.push_back(text("No job matches the filter") | dim);
        lines.push_back(text("Esc clears it") | dim);
    } else {
        lines.push_back(text("No jobs in the queue") | dim);
        lines.push_back(text(""));
        lines.push_back(hbox({text("a") | bold | color(theme::accent()), text(" history  ") | dim,
                              text("p") | bold | color(theme::accent()), text(" cluster") | dim}));
    }
    return vbox(std::move(lines)) | hcenter;
}

}  // namespace ui
