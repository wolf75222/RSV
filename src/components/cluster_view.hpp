#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include "../api/slurmjobs.hpp"

namespace ui {
using namespace ftxui;

inline Element renderPartitionBar(const api::PartitionInfo& p) {
    int total = p.nodes_total;
    if (total == 0) total = 1;

    // Create visual bar
    const int bar_width = 30;
    int idle_w = (p.nodes_idle * bar_width) / total;
    int mix_w = (p.nodes_mix * bar_width) / total;
    int alloc_w = (p.nodes_alloc * bar_width) / total;
    int down_w = bar_width - idle_w - mix_w - alloc_w;
    if (down_w < 0) down_w = 0;

    std::vector<Element> bar_parts;
    if (idle_w > 0) bar_parts.push_back(text(std::string(idle_w, '=')) | color(Color::Green));
    if (mix_w > 0) bar_parts.push_back(text(std::string(mix_w, '=')) | color(Color::Yellow));
    if (alloc_w > 0) bar_parts.push_back(text(std::string(alloc_w, '=')) | color(Color::Red));
    if (down_w > 0) bar_parts.push_back(text(std::string(down_w, 'x')) | color(Color::GrayDark));

    return hbox(bar_parts);
}

inline Component clusterView() {
    return Renderer([] {
        auto partitions = api::slurm::getPartitions();

        std::vector<Element> rows;

        // Header
        rows.push_back(
            hbox({
                text("PARTITION") | bold | size(WIDTH, EQUAL, 15),
                text("NODES") | bold | size(WIDTH, EQUAL, 8),
                text("IDLE") | bold | color(Color::Green) | size(WIDTH, EQUAL, 6),
                text("MIX") | bold | color(Color::Yellow) | size(WIDTH, EQUAL, 6),
                text("ALLOC") | bold | color(Color::Red) | size(WIDTH, EQUAL, 6),
                text("DOWN") | bold | color(Color::GrayDark) | size(WIDTH, EQUAL, 6),
                text("LIMIT") | bold | size(WIDTH, EQUAL, 12),
                text("USAGE") | bold,
            })
        );
        rows.push_back(separator());

        for (const auto& p : partitions) {
            Color state_color = (p.state == "up") ? Color::Green : Color::Red;

            rows.push_back(hbox({
                text(p.name) | color(state_color) | size(WIDTH, EQUAL, 15),
                text(std::to_string(p.nodes_total)) | size(WIDTH, EQUAL, 8),
                text(std::to_string(p.nodes_idle)) | color(Color::Green) | size(WIDTH, EQUAL, 6),
                text(std::to_string(p.nodes_mix)) | color(Color::Yellow) | size(WIDTH, EQUAL, 6),
                text(std::to_string(p.nodes_alloc)) | color(Color::Red) | size(WIDTH, EQUAL, 6),
                text(std::to_string(p.nodes_down)) | color(Color::GrayDark) | size(WIDTH, EQUAL, 6),
                text(p.timelimit) | dim | size(WIDTH, EQUAL, 12),
                renderPartitionBar(p),
            }));
        }

        // Legend
        rows.push_back(text(""));
        rows.push_back(hbox({
            text("Legend: ") | dim,
            text("=") | color(Color::Green), text(" idle  ") | dim,
            text("=") | color(Color::Yellow), text(" mix  ") | dim,
            text("=") | color(Color::Red), text(" alloc  ") | dim,
            text("x") | color(Color::GrayDark), text(" down") | dim,
        }));

        return vbox(rows) | border;
    });
}

}
