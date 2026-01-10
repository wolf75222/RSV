#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include "../api/slurmjobs.hpp"

namespace ui {
using namespace ftxui;

inline Component nodedetails(const api::DetailedJob& job, int width) {
    return Renderer([job, width] {
        std::vector<std::vector<Element>> rows;
        std::vector<Element> row;

        const int cell_width = 40;
        int nodes_per_row = std::max(1, width / cell_width);

        int count = 0;

        for (const auto& node : job.node_allocations) {
            std::string result;

            result += "" + node.node_name + "\n";

            const int cores_per_line = 20;
            int line_count = 0;
            result += "Coeurs : ";
            for (int i = 0; i < node.total_cores; ++i) {
                result += (std::find(node.allocated_cores.begin(), node.allocated_cores.end(), i) != node.allocated_cores.end()) ? "■" : ".";
                line_count++;

                if (line_count == cores_per_line) {
                    result += "\n         ";
                    line_count = 0;
                }
            }

            result += "\n \nGPUs   : ";
            for (int i = 0; i < node.total_gpus; ++i) {
                result += (i < node.allocated_gpus ? "● " : "○ ");
            }
            if(node.total_gpus == 0) {
                result += "None";
            }
            result += std::string(20 - node.total_gpus * 2, ' ');
            result += " \n";

            result += "\n";
            
            Element cell_content = vbox({
                paragraph(result)
            }) | border;

            row.push_back(cell_content);
            count++;

            if (count % nodes_per_row == 0) {
                rows.push_back(row);
                row.clear();
            }
        }

        if (!row.empty()) rows.push_back(row);

        return gridbox(rows);
    });
}


}