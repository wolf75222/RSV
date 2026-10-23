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

/* Partitions (p): what is free right now, from sinfo
- Read once when opened (it was read again at every frame), r reads again
- CPUs and GPUs are counted node by node, so a partition's GPUs in use are exact
*/
struct PartitionsView {
    std::vector<api::Partition> partitions;
    std::string error;
    bool loading = true;
    std::function<void()> reload;

    bool onEvent(Event e, bool& close) {
        if (e == Event::Character('r')) {
            loading = true;
            if (reload) reload();
            return true;
        }
        if (e.is_mouse()) return true;
        close = true;   // any other key closes
        return true;
    }

    Element render(int max_width) {
        const int width = std::min(max_width, 100);
        const int inner = width - 4;
        const bool show_gpu = inner >= 70, show_limit = inner >= 64;
        const int fixed = 11 + (show_limit ? 12 : 0) + 6 * 4 + 1 + 12 + (show_gpu ? 10 : 0);
        const int bar_w = std::clamp(inner - fixed, 6, 30);

        auto cell = [](const std::string& s, int w, bool right = false) {
            return text(right ? util::padLeft(s, static_cast<size_t>(w) - 1) + " " : util::padRight(s, static_cast<size_t>(w)));
        };
        Elements header = {cell("PARTITION", 11), show_limit ? cell("TIME LIMIT", 12) : text(""), cell("IDLE", 6, true) | color(theme::green()),
                           cell("MIX", 6, true) | color(theme::yellow()), cell("ALLOC", 6, true), cell("DOWN", 6, true),
                           text(" "), cell("CPUS IN USE", bar_w + 12)};
        if (show_gpu) header.push_back(cell("GPUS", 10, true));
        Elements rows = {hbox(std::move(header)) | dim};

        for (const auto& p : partitions) {
            const double used = p.cpus_total > 0 ? double(p.cpus_alloc) / p.cpus_total : 0;
            Elements row = {
                cell(p.name + (p.is_default ? "*" : ""), 11) | bold | color(p.avail == "up" ? theme::green() : theme::red()),
                show_limit ? cell(p.timelimit, 12) | dim : text(""),
                cell(std::to_string(p.idle), 6, true) | color(theme::green()),
                cell(std::to_string(p.mixed), 6, true) | color(theme::yellow()),
                cell(std::to_string(p.allocated), 6, true),
                cell(std::to_string(p.unavailable), 6, true) | (p.unavailable ? color(theme::red()) : Decorator(dim)),
                text(" "),
                bar(used, bar_w, usageColor(used)),
                cell(" " + percent(used), 6, true),
                cell(std::to_string(p.cpus_total), 6, true) | dim,
            };
            if (show_gpu)
                row.push_back(p.gpus_total > 0 ? cell(std::to_string(p.gpus_used) + "/" + std::to_string(p.gpus_total), 10, true)
                                               : cell("-", 10, true) | dim);
            rows.push_back(hbox(std::move(row)));
        }
        if (loading && partitions.empty()) rows.push_back(text("Reading sinfo...") | dim);
        if (!error.empty()) rows.push_back(paragraph(error) | color(theme::red()));

        rows.push_back(text(""));
        rows.push_back(text("* default partition. DOWN: down, drained or not responding.") | dim);
        rows.push_back(hbox(keyHints({{"r", "refresh"}, {"any key", "close"}})));
        return panel("Partitions", vbox(std::move(rows)) | size(WIDTH, EQUAL, width - 2));
    }
};

}  // namespace ui
