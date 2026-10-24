#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/usage.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

/* GPUs of a running job (n): nvidia-smi run in the job itself, only when asked
- srun --overlap starts one small step per node, which shares what the job already has
*/
struct GpuView {
    std::string title;
    std::vector<api::GpuUse> gpus;
    std::string error;
    bool loading = true;
    std::function<void()> reload;
    int offset = 0;
    int page = 20;

    // n or r runs nvidia-smi again, the arrows scroll (a job on 16 nodes has 64 GPUs), q or Esc closes
    bool onEvent(Event e, bool& close) {
        if (e == Event::Character('r') || e == Event::Character('n')) {
            if (!loading && reload) {
                loading = true;
                reload();
            }
        } else if (!scrollKey(e, offset, page) && !e.is_mouse()) {
            close = e == Event::Escape || e == Event::Character('q');
        }
        return true;
    }

    Element render(int max_width, int max_height) {
        const int width = std::min(max_width, 96);
        const int bar_w = std::clamp(width - 70, 6, 20);
        auto cell = [](const std::string& s, int w, bool right = false) {
            return text(right ? util::padLeft(s, static_cast<size_t>(w) - 1) + " " : util::padRight(s, static_cast<size_t>(w)));
        };
        Elements lines;
        if (loading) lines.push_back(text("Running nvidia-smi in the job (srun --overlap)...") | dim);
        if (!error.empty()) lines.push_back(paragraph(error) | color(theme::red()));
        if (!gpus.empty()) {
            lines.push_back(hbox({cell("NODE", 12), cell("GPU", 4, true), text(" "), cell("USE", bar_w + 6), cell("MEMORY", 22, true),
                                  text("  MODEL")}) |
                            dim);
            for (const auto& g : gpus) {
                const double use = g.util < 0 ? 0 : g.util / 100;
                const double mem = g.mem_total_mib > 0 ? g.mem_used_mib / g.mem_total_mib : 0;
                lines.push_back(hbox({cell(g.node, 12) | color(theme::cyan()), cell(std::to_string(g.index), 4, true), text(" "),
                                      bar(use, bar_w, efficiencyColor(use)), cell(g.util < 0 ? "-" : percent(use), 6, true),
                                      cell(util::mib(g.mem_used_mib) + " / " + util::mib(g.mem_total_mib) + " " + percent(mem), 22, true),
                                      text("  " + util::cut(g.name, 24)) | dim}));
            }
            int idle = 0;
            for (const auto& g : gpus) idle += g.util >= 0 && g.util < 5;
            if (idle > 0)
                lines.push_back(text(std::to_string(idle) + (idle == 1 ? " GPU does" : " GPUs do") + " almost nothing: check that the code uses them") |
                                color(theme::yellow()));
        }
        lines.push_back(text(""));
        page = std::max(1, max_height - 3);
        Element keys = hbox(keyHints({{"n", "again"}, {"j/k", "scroll"}, {"q", "close"}}));
        return panel(title, vbox({fitOrScroll(lines, offset, page), keys}) | size(WIDTH, EQUAL, width - 2));
    }
};

}  // namespace ui
