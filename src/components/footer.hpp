#pragma once

#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "theme.hpp"

namespace ui {
using namespace ftxui;

/* The key line at the bottom: as many keys as fit, the most useful first
- A message (copied, cancelled, an error) takes its place for a few seconds
*/
inline Element footer(int width, const std::string& message, bool message_is_error) {
    if (!message.empty())
        return hbox({text(" "), text(util::cut(message, static_cast<size_t>(std::max(1, width - 2)))) |
                                    color(message_is_error ? theme::red() : theme::green())});

    static const std::vector<std::pair<std::string, std::string>> keys = {
        {"?", "help"}, {"q", "quit"}, {"l", "log"}, {"c", "cancel"}, {"/", "filter"}, {"a", "history"},
        {"d", "details"}, {"p", "partitions"}, {"u", "limits"}, {"s", "sort"}, {"y", "copy id"}, {"r", "refresh"}};
    Elements hints = {text(" ")};
    int used = 1;
    for (const auto& [k, l] : keys) {
        const int w = static_cast<int>(util::width(k) + 1 + util::width(l) + 2);
        if (used + w > width) break;
        hints.push_back(keyHint(k, l));
        used += w;
    }
    return hbox(std::move(hints));
}

inline Element helpView(int max_width, int refresh_seconds) {
    const int width = std::min(max_width, 78);
    auto row = [](const std::string& keys, const std::string& what) {
        return hbox({text("  " + util::padRight(keys, 14)) | bold | color(theme::accent()), text(what)});
    };
    auto title = [](const std::string& s) { return text(s) | bold; };
    auto legend = [](const std::string& code, const std::string& state, const std::string& label) {
        return hbox({text(code) | bold | color(stateColor(state)), text(" " + label + "  ") | dim});
    };

    Elements lines = {
        title("Jobs"),
        row("j k  ↑ ↓", "move (PgUp PgDn: a page, g G: first, last)"),
        row("/", "filter by name, id, state, partition or account"),
        row("s", "sort: state, id, name, time left"),
        row("J K", "scroll the details (or the mouse wheel over them)"),
        text(""),
        title("Selected job"),
        row("l", "its log, followed as it grows (Tab: errors)"),
        row("d", "everything scontrol says about it"),
        row("c", "cancel it (asks first)"),
        row("y  Y", "copy its id, or its output path"),
        text(""),
        title("Views"),
        row("a", "history of the last 7 days, with CPU and memory used"),
        row("p", "partitions: free nodes, CPUs and GPUs"),
        row("u", "your accounts and their limits"),
        text(""),
        title("Other"),
        row("r", "refresh now (every " + std::to_string(refresh_seconds) + " s otherwise)"),
        row("q  Ctrl-C", "quit"),
        text(""),
        title("Legend"),
        hbox({text("  "), legend("R", "RUNNING", "running"), legend("PD", "PENDING", "pending"),
              legend("CD", "COMPLETED", "completed"), legend("F", "FAILED", "failed"), legend("CA", "CANCELLED", "cancelled")}),
        hbox({text("  CPU ") | dim, text("███") | color(theme::green()), text("░░░") | color(theme::gray()) | dim,
              text("  yours / not yours, one bar per socket") | dim}),
        hbox({text("  GPU ") | dim, text("0 1 ") | bold | color(theme::green()), text("2 3") | dim,
              text("  yours in color") | dim}),
        hbox({text("  LEFT ") | dim, text("time left, or ~ when a pending job should start") | dim}),
        text(""),
        text("y copies through the terminal (OSC 52): over SSH too, in most terminals.") | dim,
    };
    return panel("RSV help", vbox(std::move(lines)) | size(WIDTH, EQUAL, width - 2));
}

}  // namespace ui
