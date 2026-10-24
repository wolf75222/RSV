#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "../api/slurmjobs.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// Extract APU prefix from node name by removing trailing digits
// e.g., "romeo-a057" → "romeo-a", "romeo-gpu01" → "romeo-gpu"
inline std::string extractApuPrefix(const std::string& node_name) {
    size_t end = node_name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(node_name[end - 1]))) end--;
    return node_name.substr(0, end);
}

// Convert APU prefix to readable name (matching ROMEO cluster naming)
inline std::string getApuDisplayName(const std::string& prefix) {
    static const std::map<std::string, std::string> apu_names = {
        {"romeo-a",   "APU ARM (romeo-a)"},
        {"romeo-b",   "APU Standard (romeo-b)"},
        {"romeo-c",   "APU Compute (romeo-c)"},
        {"romeo-gpu", "APU GPU (romeo-gpu)"},
        {"romeo-fat", "APU Fat Nodes (romeo-fat)"},
        {"romeo-arm", "APU ARM (romeo-arm)"},
    };

    auto it = apu_names.find(prefix);
    if (it != apu_names.end()) {
        return it->second;
    }
    return "APU: " + prefix;
}

// One node of the job, with what the job uses on it
struct NodeUse {
    std::string host;
    const api::NodeAlloc* alloc = nullptr;
    const api::NodeInfo* info = nullptr;   // null when scontrol didn't answer
};

// Group structure for APU statistics
struct ApuGroup {
    std::string prefix;
    std::string display_name;
    std::vector<NodeUse> nodes;
    int total_allocated_cores = 0;
    int total_allocated_gpus = 0;
};

constexpr int CORES_PER_LINE = 20;
constexpr int CELL_WIDTH = 9 + CORES_PER_LINE + 4 + 2 + 2;   // label, cores, padding, border, gap

/* Render a single node cell
- ■ the cores of the job, . the others: the CPU ids are those of this node (RSV 1 split them between the nodes)
- ● the GPUs of the job, by their index (RSV 1 colored the first ones)
- The last line isn't padded with dots any more: they looked like free cores that don't exist
*/
inline Element renderNodeCell(const NodeUse& node) {
    Element title = text(node.host) | color(theme::cyan()) | bold;

    const std::vector<int>& cpus = node.alloc->cpus;
    int total_cores = node.info && node.info->cpus > 0 ? node.info->cpus : 0;
    for (int id : cpus) total_cores = std::max(total_cores, id + 1);
    std::vector<bool> mine(static_cast<size_t>(total_cores), false);
    for (int id : cpus)
        if (id >= 0 && id < total_cores) mine[static_cast<size_t>(id)] = true;

    std::vector<Element> core_lines;
    for (int start = 0; start < total_cores; start += CORES_PER_LINE) {
        // A run of the same kind is one text: a 288 cores node is 15 lines, not 288 elements
        std::vector<Element> line = {text(start == 0 ? "Cores  : " : "         ")};
        std::string run;
        bool run_mine = false;
        for (int i = start; i < std::min(total_cores, start + CORES_PER_LINE); i++) {
            const bool m = mine[static_cast<size_t>(i)];
            if (!run.empty() && m != run_mine) {
                line.push_back(run_mine ? text(run) | color(theme::green()) : text(run));
                run.clear();
            }
            run_mine = m;
            run += m ? "■" : ".";
        }
        if (!run.empty()) line.push_back(run_mine ? text(run) | color(theme::green()) : text(run));
        core_lines.push_back(hbox(std::move(line)));
    }
    if (core_lines.empty()) core_lines.push_back(text("Cores  : ?") | dim);

    const api::Gpus& gpus = node.alloc->gpus;
    int total_gpus = node.info ? node.info->gpus.count : 0;
    for (int i : gpus.indices) total_gpus = std::max(total_gpus, i + 1);
    std::vector<Element> gpu_line = {text("GPUs   : ")};
    for (int i = 0; i < total_gpus && i < 16; ++i) {
        const bool used = std::find(gpus.indices.begin(), gpus.indices.end(), i) != gpus.indices.end() ||
                          (gpus.indices.empty() && i < gpus.count);
        gpu_line.push_back(used ? text("● ") | color(theme::yellow()) : text("○ "));
    }
    if (total_gpus == 0) gpu_line.push_back(text("None"));

    return hbox({
        hbox({
            text("  "),
            vbox({
                title,
                vbox(std::move(core_lines)),
                text(" "),
                hbox(std::move(gpu_line)),
            }),
            text("  "),
        }) | border,
        text("  "),
    });
}

// Render APU group header with statistics (as wide as the panel, 60 columns at most)
inline Element renderApuHeader(const ApuGroup& group, int width) {
    std::string stats = "Nodes: " + std::to_string(group.nodes.size()) +
                        " | Cores allocated: " + std::to_string(group.total_allocated_cores) +
                        " | GPUs allocated: " + std::to_string(group.total_allocated_gpus);

    const int w = std::clamp(width, 10, 60);
    std::string line;
    for (int i = 0; i < w - 2; i++) line += "═";

    return vbox({
        text(""),
        text("╔" + line + "╗") | color(theme::magenta()) | bold,
        hbox({
            text("║ ") | color(theme::magenta()) | bold,
            text(util::cut(group.display_name, static_cast<size_t>(w - 2))) | bold,
        }),
        hbox({
            text("║ ") | color(theme::magenta()) | bold,
            text(util::cut(stats, static_cast<size_t>(w - 2))) | color(theme::cyan()),
        }),
        text("╚" + line + "╝") | color(theme::magenta()) | bold,
    });
}

// The nodes of the job, grouped by APU, as a grid of cells that fits `width`
inline Elements nodeDetails(const api::JobDetails& d, const std::map<std::string, api::NodeInfo>& nodes, int width) {
    std::map<std::string, ApuGroup> groups;
    for (const auto& a : d.alloc) {
        for (const auto& host : a.hosts) {
            const std::string prefix = extractApuPrefix(host);
            auto& group = groups[prefix];
            if (group.prefix.empty()) {
                group.prefix = prefix;
                group.display_name = getApuDisplayName(prefix);
            }
            const auto it = nodes.find(host);
            group.nodes.push_back({host, &a, it == nodes.end() ? nullptr : &it->second});
            group.total_allocated_cores += static_cast<int>(a.cpus.size());
            group.total_allocated_gpus += a.gpus.indices.empty() ? a.gpus.count : static_cast<int>(a.gpus.indices.size());
        }
    }

    Elements all_elements;
    const int nodes_per_row = std::max(1, width / CELL_WIDTH);
    for (auto& [prefix, group] : groups) {
        all_elements.push_back(renderApuHeader(group, width));

        std::vector<Elements> rows;
        Elements row;
        for (const auto& node : group.nodes) {
            row.push_back(renderNodeCell(node));
            if (static_cast<int>(row.size()) == nodes_per_row) {
                rows.push_back(row);
                row.clear();
            }
        }
        if (!row.empty()) rows.push_back(row);
        if (!rows.empty()) all_elements.push_back(gridbox(rows));
    }
    return all_elements;
}

}  // namespace ui
