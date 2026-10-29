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
inline Element footer(int width, const std::string& message, bool message_is_error, int marked = 0) {
    if (!message.empty())
        return hbox({text(" "), text(util::cut(message, static_cast<size_t>(std::max(1, width - 2)))) |
                                    color(message_is_error ? theme::red() : theme::green())});
    // Rows marked with Space: what can be done with them
    if (marked > 0) {
        Elements hints = {text(" " + std::to_string(marked) + " marked  ") | bold | color(theme::yellow())};
        for (auto& h : keyHints({{"c", "cancel"}, {"Space", "mark"}, {"Esc", "unmark"}})) hints.push_back(h);
        return hbox(std::move(hints));
    }

    static const std::vector<std::pair<std::string, std::string>> keys = {
        {"?", "help"},    {"q", "quit"},       {"l", "log"},     {"p", "cluster"}, {"c", "cancel"},   {"/", "filter"},
        {"a", "history"}, {"o", "project"},    {"b", "script"},  {"Space", "mark"}, {"t", "start time"}, {"u", "limits"},
        {"d", "details"}, {"n", "GPUs"},       {"m", "live"},    {"i", "model server"}, {"s", "sort"},    {"y", "copy id"},  {"r", "refresh"}};
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

/* Help (?): every key, in two columns when the screen is wide enough
- One column on a narrow screen: what doesn't fit is cut, the essentials come first
*/
inline Element helpView(int max_width, int max_height, int refresh_seconds, int& offset) {
    const bool two = max_width >= 136;
    auto row = [](const std::string& keys, const std::string& what) {
        return hbox({text("  " + util::padRight(keys, 12)) | bold | color(theme::accent()), text(what)});
    };
    auto title = [](const std::string& s) { return text(s) | bold; };
    auto legend = [](const std::string& code, const std::string& state, const std::string& label) {
        return hbox({text(code) | bold | color(stateColor(state)), text(" " + label + "  ") | dim});
    };

    Elements left = {
        title("Jobs"),
        row("j k  ↑ ↓", "move (PgUp PgDn: a page, g G: first, last)"),
        row("/", "filter by name, id, state, partition, account, user"),
        row("s", "sort: state, id, name, time left"),
        row("J K", "scroll the details (or the mouse wheel over them)"),
        row("Space", "mark: c then cancels all marked (Esc unmarks)"),
        row("Enter  → ←", "an array: show its tasks under it, or fold them"),
        row("o", "your project: everyone's jobs in your accounts"),
        text(""),
        title("Selected job"),
        row("l", "its log, followed (/ search, e next error)"),
        row("b", "the script it was submitted with"),
        row("d", "everything scontrol says about it"),
        row("n", "its GPUs in depth: processes, SM activity, clocks, links"),
        row("m", "live: overview (1) or performance (2), CPUs, memory, disk, GPUs"),
        row("i", "the server of models it runs (vLLM, SGLang...): tokens, latency, cache"),
        row("c", "cancel it, or a whole array (asks first)"),
        row("H", "hold or release it (ROMEO refuses both to users)"),
        row("y  Y", "copy its id, or its output path"),
    };
    Elements right = {
        title("Views"),
        row("p", "the cluster: overview, queue, nodes (Enter: zoom), hardware"),
        row("t", "when would a job start? (sbatch --test-only)"),
        row("a", "the last 7 days: CPU and GPU hours, what was wasted"),
        row("u", "your accounts: limits, fairshare, disk quotas"),
        text(""),
        title("Other"),
        row("r", "refresh now (every " + std::to_string(refresh_seconds) + " s, slower if Slurm is)"),
        row("q  Ctrl-C", "quit"),
        text(""),
        title("Legend"),
        hbox({text("  "), legend("R", "RUNNING", "running"), legend("PD", "PENDING", "pending"),
              legend("CD", "COMPLETED", "completed"), legend("F", "FAILED", "failed"), legend("CA", "CANCELLED", "cancelled")}),
        hbox({text("  Cores ") | dim, text("■■") | color(theme::green()), text(" the job's  ") | dim, text(".."),
              text(" the others") | dim}),
        hbox({text("  GPUs  ") | dim, text("●") | color(theme::yellow()), text(" the job's  ") | dim, text("○"), text(" the others") | dim}),
        hbox({text("  Array ") | dim, text("██") | color(theme::blue()), text("█") | color(theme::red()), text("█") | color(theme::green()),
              text("░░") | color(theme::gray()), text("  done, failed, running, waiting") | dim}),
        hbox({text("  LEFT  ") | dim, text("time left, or ~ when a pending job should start") | dim}),
        text(""),
        text("y copies through the terminal (OSC 52), over SSH too.") | dim,
        text("Paths are links to VS Code (Remote-SSH): Cmd or Ctrl-click.") | dim,
    };
    // As rows, so that a short screen scrolls them (↑ ↓): side by side when wide enough, one after the other else
    Elements rows;
    if (two) {
        for (size_t i = 0; i < std::max(left.size(), right.size()); i++)
            rows.push_back(hbox({(i < left.size() ? left[i] : text("")) | size(WIDTH, EQUAL, 66), text("  "),
                                 i < right.size() ? right[i] : text("")}));
    } else {
        rows = std::move(left);
        rows.push_back(text(""));
        for (auto& e : right) rows.push_back(std::move(e));
    }
    const int page = std::max(1, max_height - 2);
    return panel("RSV help", fitOrScroll(rows, offset, page) | size(WIDTH, EQUAL, std::min(max_width, two ? 136 : 78) - 2));
}

}  // namespace ui
