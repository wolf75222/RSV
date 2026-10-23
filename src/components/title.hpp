#pragma once

#include <ctime>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "../api/slurmjobs.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

/* The top line: who, how many jobs in each state, and how fresh the data is
- When squeue fails, its error stays here (in red) and the last list is kept
*/
inline Element title(const std::string& who, const std::vector<api::Job>& jobs, std::time_t updated, bool refreshing,
                     const std::string& error, int width) {
    int running = 0, pending = 0, done = 0;
    for (const auto& j : jobs) {
        if (j.active()) running++;
        else if (j.pending()) pending++;
        else done++;
    }
    Elements left = {text(" RSV ") | bold | inverted | color(theme::accent()), text(" " + who + "  ") | dim};
    if (width >= 60) {
        left.push_back(text(std::to_string(running) + " running") | color(theme::green()));
        left.push_back(text(" · ") | dim);
        left.push_back(text(std::to_string(pending) + " pending") | color(theme::yellow()));
        if (done) {
            left.push_back(text(" · ") | dim);
            left.push_back(text(std::to_string(done) + " ended") | dim);
        }
    }

    Element right;
    const std::time_t now = std::time(nullptr);
    if (!error.empty()) {
        const int room = std::max(10, width - 50);
        right = text(util::cut(error, static_cast<size_t>(room))) | color(theme::red());
    } else if (refreshing && updated == 0) {
        right = text("reading squeue...") | dim;
    } else if (updated > 0) {
        const long age = static_cast<long>(std::difftime(now, updated));
        right = text((refreshing ? "refreshing · " : "") + std::string("updated ") + (age < 5 ? "now" : util::duration(age) + " ago")) | dim;
    } else {
        right = text("");
    }
    return hbox({hbox(std::move(left)), filler(), right, text(" ")});
}

}  // namespace ui
