#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <fstream>
#include <sstream>
#include "../api/slurmjobs.hpp"

namespace ui {
using namespace ftxui;

inline std::string readLogFile(const std::string& path, int max_lines = 100) {
    if (path.empty() || path == "(null)") {
        return "[File not specified]";
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return "[Cannot open: " + path + "]";
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }

    // Keep only last max_lines
    std::ostringstream oss;
    int start = (lines.size() > (size_t)max_lines) ? lines.size() - max_lines : 0;
    for (size_t i = start; i < lines.size(); i++) {
        oss << lines[i] << "\n";
    }

    if (lines.empty()) {
        return "[Empty file]";
    }

    return oss.str();
}

inline Component logView(const std::string& job_id, std::function<void()> on_close) {
    auto paths = api::slurm::getJobLogPaths(job_id);
    auto stdout_path = std::make_shared<std::string>(paths.first);
    auto stderr_path = std::make_shared<std::string>(paths.second);
    auto stdout_content = std::make_shared<std::string>(readLogFile(*stdout_path));
    auto stderr_content = std::make_shared<std::string>(readLogFile(*stderr_path));
    auto show_stderr = std::make_shared<bool>(false);

    auto content = Renderer([=] {
        std::vector<Element> elements;

        elements.push_back(
            hbox({
                text("═══ LOGS: Job ") | color(Color::Cyan),
                text(job_id) | bold | color(Color::Magenta),
                text(" ═══") | color(Color::Cyan),
            }) | center
        );
        elements.push_back(separator());

        // Tab selector
        elements.push_back(hbox({
            text("  "),
            (*show_stderr ? text("[stdout]") | dim : text("[stdout]") | bold | color(Color::Green)),
            text("  |  "),
            (*show_stderr ? text("[stderr]") | bold | color(Color::Red) : text("[stderr]") | dim),
            text("  (Tab to switch)") | dim,
        }));
        elements.push_back(separator());

        // File path
        std::string current_path = *show_stderr ? *stderr_path : *stdout_path;
        elements.push_back(hbox({
            text("File: ") | dim,
            text(current_path) | color(Color::Cyan),
        }));
        elements.push_back(separator());

        // Content
        std::string& content_str = *show_stderr ? *stderr_content : *stdout_content;
        std::istringstream iss(content_str);
        std::string line;
        int line_num = 1;
        while (std::getline(iss, line)) {
            // Truncate long lines
            if (line.length() > 120) {
                line = line.substr(0, 117) + "...";
            }
            elements.push_back(hbox({
                text(std::to_string(line_num)) | dim | size(WIDTH, EQUAL, 5),
                text(" "),
                text(line),
            }));
            line_num++;
        }

        elements.push_back(separator());
        elements.push_back(hbox({
            text("Tab") | bold | color(Color::Yellow),
            text(": switch stdout/stderr  ") | dim,
            text("Esc/Enter") | bold | color(Color::Yellow),
            text(": close") | dim,
        }) | center);

        return vbox(elements) | border | size(HEIGHT, LESS_THAN, 40);
    });

    return CatchEvent(content, [=](Event e) {
        if (e == Event::Tab || e == Event::TabReverse) {
            *show_stderr = !*show_stderr;
            return true;
        }
        if (e.is_character() || e == Event::Escape || e == Event::Return) {
            on_close();
            return true;
        }
        return false;
    });
}

}
