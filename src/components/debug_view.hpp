#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <sstream>
#include <set>
#include "../api/slurmjobs.hpp"

namespace ui {
using namespace ftxui;

// Important fields to highlight in debug view
inline std::set<std::string> important_fields = {
    "JobId", "JobName", "UserId", "Account", "Partition",
    "TimeLimit", "RunTime", "SubmitTime", "StartTime",
    "NumNodes", "NumCPUs", "NumTasks", "CPUs/Task",
    "MinMemoryNode", "ReqMem", "Features", "Gres",
    "StdOut", "StdErr", "WorkDir", "Command",
    "JobState", "Reason", "Priority"
};

inline Component debugView(const std::string& job_id, std::function<void()> on_close) {
    auto raw = std::make_shared<std::string>(api::slurm::getRawJobDetails(job_id));

    auto content = Renderer([raw, job_id] {
        std::vector<Element> lines;

        lines.push_back(
            hbox({
                text("═══ DEBUG: scontrol show job ") | color(Color::Cyan),
                text(job_id) | bold | color(Color::Magenta),
                text(" ═══") | color(Color::Cyan),
            }) | center
        );
        lines.push_back(separator());

        // Parse and display with highlighting
        std::istringstream iss(*raw);
        std::string line;
        while (std::getline(iss, line)) {
            std::vector<Element> line_elements;

            // Split by spaces and highlight key=value pairs
            std::istringstream lss(line);
            std::string token;
            while (lss >> token) {
                size_t eq_pos = token.find('=');
                if (eq_pos != std::string::npos) {
                    std::string key = token.substr(0, eq_pos);
                    std::string val = token.substr(eq_pos + 1);

                    bool is_important = important_fields.count(key) > 0;

                    if (is_important) {
                        line_elements.push_back(text(key) | bold | color(Color::Yellow));
                    } else {
                        line_elements.push_back(text(key) | dim);
                    }
                    line_elements.push_back(text("="));
                    if (is_important) {
                        line_elements.push_back(text(val) | color(Color::Cyan));
                    } else {
                        line_elements.push_back(text(val));
                    }
                    line_elements.push_back(text(" "));
                } else {
                    line_elements.push_back(text(token + " "));
                }
            }

            if (!line_elements.empty()) {
                lines.push_back(hbox(line_elements));
            }
        }

        lines.push_back(separator());
        lines.push_back(text("Press any key to close") | dim | center);

        return vbox(lines) | border | size(WIDTH, LESS_THAN, 100);
    });

    return CatchEvent(content, [on_close](Event e) {
        if (e.is_character() || e == Event::Escape || e == Event::Return) {
            on_close();
            return true;
        }
        return false;
    });
}

}
