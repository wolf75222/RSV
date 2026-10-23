#pragma once

#include <set>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "theme.hpp"

namespace ui {
using namespace ftxui;

/* Raw view (d): everything scontrol says about the job, the important fields highlighted
- Scrolls, and long lines are cut to the window (it was drawn whole and cut by the screen)
*/
struct DebugView {
    std::string title;
    std::vector<std::string> lines;
    std::string error;
    bool loading = true;
    int offset = 0;
    int page = 20;

    void setText(const std::string& raw) {
        lines = util::lines(raw);
        loading = false;
    }

    bool onEvent(Event e, bool& close) {
        const int max = std::max(0, static_cast<int>(lines.size()) - page);
        if (e == Event::Escape || e == Event::Character('q') || e == Event::Character('d')) close = true;
        else if (e == Event::ArrowDown || e == Event::Character('j')) offset++;
        else if (e == Event::ArrowUp || e == Event::Character('k')) offset--;
        else if (e == Event::PageDown || e == Event::Character(' ')) offset += page - 1;
        else if (e == Event::PageUp || e == Event::Character('b')) offset -= page - 1;
        else if (e == Event::Home || e == Event::Character('g')) offset = 0;
        else if (e == Event::End || e == Event::Character('G')) offset = max;
        else if (e.is_mouse() && e.mouse().button == Mouse::WheelDown) offset += 3;
        else if (e.is_mouse() && e.mouse().button == Mouse::WheelUp) offset -= 3;
        else return false;
        offset = std::clamp(offset, 0, max);
        return true;
    }

    static Element highlight(const std::string& line, int width) {
        static const std::set<std::string> important = {
            "JobId", "JobName", "UserId", "Account", "QOS", "JobState", "Reason", "Dependency", "RunTime", "TimeLimit",
            "SubmitTime", "StartTime", "EndTime", "Partition", "NodeList", "NumNodes", "NumCPUs", "NumTasks",
            "CPUs/Task", "TRES", "ReqTRES", "AllocTRES", "MinMemoryNode", "MinMemoryCPU", "Features", "Nodes",
            "CPU_IDs", "GRES", "JOB_GRES", "Mem", "TresPerNode", "Command", "WorkDir", "StdOut", "StdErr", "ExitCode"};
        Elements parts;
        int used = 0;
        size_t i = 0;
        while (i < line.size() && used < width) {
            size_t end = line.find(' ', i);
            if (end == std::string::npos) end = line.size();
            const std::string word = line.substr(i, end - i + (end < line.size() ? 1 : 0));
            const std::string shown = util::cut(word, static_cast<size_t>(width - used));
            const size_t eq = shown.find('=');
            if (eq != std::string::npos && important.count(shown.substr(0, eq)))
                parts.push_back(hbox({text(shown.substr(0, eq)) | bold | color(theme::yellow()), text("="),
                                      text(shown.substr(eq + 1)) | color(theme::cyan())}));
            else if (eq != std::string::npos)
                parts.push_back(hbox({text(shown.substr(0, eq)) | dim, text(shown.substr(eq))}));
            else
                parts.push_back(text(shown));
            used += static_cast<int>(util::width(shown));
            i = end + 1;
        }
        return hbox(std::move(parts));
    }

    Element render(int width, int height) {
        page = std::max(1, height - 4);
        const int inner = std::max(10, width - 4);
        Elements all;
        if (loading) all.push_back(text("Reading...") | dim);
        if (!error.empty()) all.push_back(paragraph(error) | color(theme::red()));
        for (const auto& l : lines) all.push_back(highlight(util::sanitize(l), inner - 1));
        return panel(title, vbox({scrollWindow(all, offset, page) | flex, separator() | dim,
                                  hbox(keyHints({{"j/k", "scroll"}, {"g/G", "top/end"}, {"q", "close"}}))}) |
                                size(WIDTH, EQUAL, width - 2) | size(HEIGHT, EQUAL, height - 2));
    }
};

}  // namespace ui
