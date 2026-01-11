#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <map>
#include <regex>
#include "../api/slurmjobs.hpp"

namespace ui {
using namespace ftxui;

// Extract APU prefix from node name (e.g., "romeo-a057" → "romeo-a")
inline std::string extractApuPrefix(const std::string& node_name) {
    std::regex re(R"(^([a-zA-Z]+-[a-zA-Z]+))");
    std::smatch match;
    if (std::regex_search(node_name, match, re)) {
        return match[1].str();
    }
    return "unknown";
}

// Convert APU prefix to readable name (auto-generated from prefix)
inline std::string getApuDisplayName(const std::string& prefix) {
    // Extract the letter suffix (e.g., "romeo-a" → "A")
    std::regex re(R"(-([a-zA-Z])$)");
    std::smatch match;
    if (std::regex_search(prefix, match, re)) {
        char letter = std::toupper(match[1].str()[0]);
        return "APU " + std::string(1, letter) + " (" + prefix + ")";
    }
    return "APU (" + prefix + ")";
}

// Group structure for APU statistics
struct ApuGroup {
    std::string prefix;
    std::string display_name;
    std::vector<const api::NodeAllocation*> nodes;
    int total_allocated_cores = 0;
    int total_cores = 0;
    int total_allocated_gpus = 0;
    int total_gpus = 0;
};

// Render a single node cell
inline Element renderNodeCell(const api::NodeAllocation& node) {
    Element title = text(node.node_name) | color(Color::BlueLight) | bold;

    const int cores_per_line = 20;
    std::vector<Element> core_lines;
    int line_count = 0;
    std::vector<Element> current_line;

    current_line.push_back(text("CPUs   : "));

    for (int i = 0; i < node.total_cores; ++i) {
        if (std::find(node.allocated_cores.begin(), node.allocated_cores.end(), i) != node.allocated_cores.end())
            current_line.push_back(text("■") | color(Color::Blue));
        else
            current_line.push_back(text("."));

        line_count++;
        if (line_count == cores_per_line) {
            core_lines.push_back(hbox(current_line));
            current_line.clear();
            current_line.push_back(text("         "));
            line_count = 0;
        }
    }

    if (!current_line.empty()) {
        while (line_count < cores_per_line) {
            current_line.push_back(text("."));
            line_count++;
        }
        core_lines.push_back(hbox(current_line));
    }

    Element cores_box = vbox(core_lines);

    std::vector<Element> gpu_line;
    gpu_line.push_back(text("GPUs   : "));
    for (int i = 0; i < node.total_gpus; ++i) {
        if (i < node.allocated_gpus)
            gpu_line.push_back(text("● ") | color(Color::Blue));
        else
            gpu_line.push_back(text("○ "));
    }

    if (node.total_gpus == 0) {
        gpu_line.push_back(text("None"));
    }

    Element gpu_box = hbox(gpu_line);

    return hbox({
        hbox({
            text("  "),
            vbox({
                title,
                cores_box,
                text(" "),
                gpu_box
            }),
            text("  ")
        }) | border,
        text("  ")
    });
}

// Render APU group header with statistics
inline Element renderApuHeader(const ApuGroup& group) {
    std::string summary = std::to_string(group.nodes.size()) + " noeud(s) | " +
                          std::to_string(group.total_allocated_cores) + "/" +
                          std::to_string(group.total_cores) + " CPUs | " +
                          std::to_string(group.total_allocated_gpus) + "/" +
                          std::to_string(group.total_gpus) + " GPUs";

    return vbox({
        text(""),
        hbox({
            text("━━━ ") | color(Color::Yellow),
            text(group.display_name) | bold | color(Color::Yellow),
            text(" ━━━ ") | color(Color::Yellow),
            text(summary) | dim,
            text(" ") | flex,
        }),
        text(""),
    });
}

inline Component nodedetails(const api::DetailedJob& job, int width) {
    using namespace ftxui;

    return Renderer([job, width] {
        // Group nodes by APU prefix
        std::map<std::string, ApuGroup> groups;

        for (const auto& node : job.node_allocations) {
            std::string prefix = extractApuPrefix(node.node_name);

            if (groups.find(prefix) == groups.end()) {
                groups[prefix] = ApuGroup{
                    prefix,
                    getApuDisplayName(prefix),
                    {},
                    0, 0, 0, 0
                };
            }

            auto& group = groups[prefix];
            group.nodes.push_back(&node);
            group.total_allocated_cores += node.allocated_cores.size();
            group.total_cores += node.total_cores;
            group.total_allocated_gpus += node.allocated_gpus;
            group.total_gpus += node.total_gpus;
        }

        // Build the display
        std::vector<Element> all_elements;
        const int cell_width = 44;
        int nodes_per_row = std::max(1, width / cell_width);

        for (auto& [prefix, group] : groups) {
            // Add APU header
            all_elements.push_back(renderApuHeader(group));

            // Add nodes in grid layout
            std::vector<std::vector<Element>> rows;
            std::vector<Element> row;
            int count = 0;

            for (const auto* node : group.nodes) {
                row.push_back(renderNodeCell(*node));
                count++;

                if (count % nodes_per_row == 0) {
                    rows.push_back(row);
                    row.clear();
                }
            }

            if (!row.empty()) rows.push_back(row);

            if (!rows.empty()) {
                all_elements.push_back(gridbox(rows));
            }
        }

        return vbox(all_elements);
    });
}

}