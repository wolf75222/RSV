#pragma once

#include <ctime>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

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

constexpr int STATE_WIDTH = 3;
constexpr int TIME_WIDTH = 7;

// Columns: state, id, name, time. Cut by display width, so CJK or emoji names keep the columns aligned.
inline Element jobRow(const api::Job& j, int width, int id_width, long used, bool selected, std::time_t now) {
    const int name_width = std::max(1, width - STATE_WIDTH - id_width - TIME_WIDTH - 2);
    Element row = hbox({
        text(util::padRight(api::shortState(j.state), STATE_WIDTH)) | color(stateColor(j.state)) | bold,
        text(util::padRight(j.id, static_cast<size_t>(id_width)) + " "),
        text(util::padRight(j.name, static_cast<size_t>(name_width)) + " "),
        text(util::padLeft(timeColumn(j, used, now), TIME_WIDTH)) | (j.active() ? nothing : dim),
    });
    return selected ? row | inverted : row;
}

inline int idWidth(const std::vector<const api::Job*>& jobs) {
    size_t w = 5;
    for (const auto* j : jobs) w = std::max(w, util::width(j->id));
    return static_cast<int>(std::min<size_t>(w, 16));
}

inline Element jobListHeader(int width, int id_width) {
    const int name_width = std::max(1, width - STATE_WIDTH - id_width - TIME_WIDTH - 2);
    return hbox({text(util::padRight("ST", STATE_WIDTH)), text(util::padRight("JOBID", static_cast<size_t>(id_width)) + " "),
                 text(util::padRight("NAME", static_cast<size_t>(name_width)) + " "), text(util::padLeft("LEFT", TIME_WIDTH))}) |
           dim;
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
