#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <sstream>
#include <algorithm>
#include "../api/slurmjobs.hpp"

namespace ui {
using namespace ftxui;

struct HistoryJob {
    std::string id;
    std::string name;
    std::string state;
    std::string start;
    std::string end;
    std::string elapsed;
    std::string exit_code;
    std::string max_rss;      // Peak memory usage
    std::string cpu_time;     // Total CPU time
    std::string ncpus;        // Number of CPUs
    std::string nnodes;       // Number of nodes
    std::string partition;
    std::string account;
};

inline std::vector<HistoryJob> getJobHistory(const std::string& filter = "") {
    std::vector<HistoryJob> history;

    const char* user = std::getenv("USER");
    if (!user) user = "unknown";

    // Use sacct to get job history with more details
    std::string cmd = "sacct -u " + std::string(user) +
                      " --starttime=now-7days"
                      " --format=JobID,JobName%30,State,Start,End,Elapsed,ExitCode,MaxRSS,CPUTime,NCPUs,NNodes,Partition,Account"
                      " --noheader -P 2>/dev/null";

    // Add state filter if specified
    if (!filter.empty()) {
        cmd = "sacct -u " + std::string(user) +
              " --starttime=now-7days -s " + filter +
              " --format=JobID,JobName%30,State,Start,End,Elapsed,ExitCode,MaxRSS,CPUTime,NCPUs,NNodes,Partition,Account"
              " --noheader -P 2>/dev/null";
    }

    std::array<char, 512> buffer;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(
        popen(cmd.c_str(), "r"),
        static_cast<int(*)(FILE*)>(pclose)
    );

    if (!pipe) return history;

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        std::string line = buffer.data();
        if (line.empty() || line[0] == '\n') continue;

        if (!line.empty() && line.back() == '\n') line.pop_back();

        std::istringstream iss(line);
        std::string field;
        std::vector<std::string> fields;

        while (std::getline(iss, field, '|')) {
            fields.push_back(field);
        }

        if (fields.size() >= 7) {
            // Skip step entries (those with . in JobID like 12345.batch)
            if (fields[0].find('.') != std::string::npos) continue;

            HistoryJob job;
            job.id = fields[0];
            job.name = fields[1];
            job.state = fields[2];
            job.start = fields[3];
            job.end = fields[4];
            job.elapsed = fields[5];
            job.exit_code = fields[6];
            if (fields.size() > 7) job.max_rss = fields[7];
            if (fields.size() > 8) job.cpu_time = fields[8];
            if (fields.size() > 9) job.ncpus = fields[9];
            if (fields.size() > 10) job.nnodes = fields[10];
            if (fields.size() > 11) job.partition = fields[11];
            if (fields.size() > 12) job.account = fields[12];
            history.push_back(job);
        }
    }

    std::reverse(history.begin(), history.end());
    return history;
}

inline Color getStateColor(const std::string& state) {
    if (state.find("COMPLETED") != std::string::npos) return Color::Green;
    if (state.find("RUNNING") != std::string::npos) return Color::Cyan;
    if (state.find("PENDING") != std::string::npos) return Color::Yellow;
    if (state.find("FAILED") != std::string::npos) return Color::Red;
    if (state.find("CANCELLED") != std::string::npos) return Color::Magenta;
    if (state.find("TIMEOUT") != std::string::npos) return Color::Red;
    if (state.find("OUT_OF_ME") != std::string::npos) return Color::Red;  // OUT_OF_MEMORY
    return Color::White;
}

inline std::string formatMemory(const std::string& mem) {
    if (mem.empty()) return "-";
    // Already formatted like "1234K" or "512M"
    return mem;
}

inline Component historyView(std::shared_ptr<float> scroll_y, std::shared_ptr<int> filter_mode,
                             std::function<void()> on_close) {
    auto history = std::make_shared<std::vector<HistoryJob>>();
    auto selected_job = std::make_shared<int>(-1);  // -1 = no selection, show list

    // Filter options
    static const std::vector<std::pair<std::string, std::string>> filters = {
        {"", "ALL"},
        {"r", "RUNNING"},
        {"pd", "PENDING"},
        {"cd", "COMPLETED"},
        {"f", "FAILED"},
        {"ca", "CANCELLED"},
        {"to", "TIMEOUT"},
    };

    auto reload_history = [=]() {
        *history = getJobHistory(filters[*filter_mode].first);
    };

    reload_history();

    auto list_content = Renderer([=] {
        std::vector<Element> rows;

        // Header
        rows.push_back(
            hbox({
                text("ID") | bold | size(WIDTH, EQUAL, 10),
                text("NAME") | bold | size(WIDTH, EQUAL, 20),
                text("STATE") | bold | size(WIDTH, EQUAL, 12),
                text("ELAPSED") | bold | size(WIDTH, EQUAL, 10),
                text("CPU") | bold | size(WIDTH, EQUAL, 6),
                text("MEM") | bold | size(WIDTH, EQUAL, 10),
                text("EXIT") | bold | size(WIDTH, EQUAL, 6),
            })
        );
        rows.push_back(separator());

        for (const auto& job : *history) {
            std::string name = job.name;
            if (name.length() > 18) name = name.substr(0, 15) + "...";

            rows.push_back(hbox({
                text(job.id) | size(WIDTH, EQUAL, 10),
                text(name) | size(WIDTH, EQUAL, 20),
                text(job.state) | color(getStateColor(job.state)) | size(WIDTH, EQUAL, 12),
                text(job.elapsed) | color(Color::Cyan) | size(WIDTH, EQUAL, 10),
                text(job.ncpus.empty() ? "-" : job.ncpus) | size(WIDTH, EQUAL, 6),
                text(formatMemory(job.max_rss)) | color(Color::Yellow) | size(WIDTH, EQUAL, 10),
                text(job.exit_code) | (job.exit_code == "0:0" ? color(Color::Green) : color(Color::Red)) | size(WIDTH, EQUAL, 6),
            }));
        }

        return vbox(rows);
    });

    auto scrollable = Renderer(list_content, [=] {
        return list_content->Render()
               | focusPositionRelative(0.f, *scroll_y)
               | frame
               | size(HEIGHT, LESS_THAN, 20);
    });

    auto full_view = Renderer(scrollable, [=] {
        int total_jobs = history->size();
        int scroll_percent = (int)(*scroll_y * 100);

        // Filter tabs
        std::vector<Element> filter_tabs;
        for (size_t i = 0; i < filters.size(); i++) {
            if ((int)i == *filter_mode) {
                filter_tabs.push_back(text("[" + filters[i].second + "]") | bold | color(Color::Cyan));
            } else {
                filter_tabs.push_back(text(" " + filters[i].second + " ") | dim);
            }
        }

        return vbox({
            text("═══════════ JOB HISTORY (last 7 days) ═══════════") | bold | center | color(Color::Cyan),
            separator(),
            hbox(filter_tabs) | center,
            separator(),
            hbox({
                text(std::to_string(total_jobs) + " jobs") | dim,
                filler(),
                text(std::to_string(scroll_percent) + "%") | color(Color::Yellow),
            }),
            separator(),
            scrollable->Render() | flex,
            separator(),
            hbox({
                text("←→") | bold | color(Color::Yellow),
                text(": filter  ") | dim,
                text("↑↓") | bold | color(Color::Yellow),
                text(": scroll  ") | dim,
                text("r") | bold | color(Color::Yellow),
                text(": refresh  ") | dim,
                text("Esc") | bold | color(Color::Yellow),
                text(": close") | dim,
            }) | center,
        }) | border | size(WIDTH, LESS_THAN, 95);
    });

    return CatchEvent(full_view, [=](Event e) {
        constexpr float scroll_step = 0.05f;

        // Mouse wheel scrolling
        if (e.is_mouse()) {
            if (e.mouse().button == Mouse::WheelDown) {
                *scroll_y = std::min(*scroll_y + scroll_step, 1.f);
                return true;
            }
            if (e.mouse().button == Mouse::WheelUp) {
                *scroll_y = std::max(*scroll_y - scroll_step, 0.f);
                return true;
            }
        }

        // Arrow keys for scrolling
        if (e == Event::ArrowDown) {
            *scroll_y = std::min(*scroll_y + scroll_step, 1.f);
            return true;
        }
        if (e == Event::ArrowUp) {
            *scroll_y = std::max(*scroll_y - scroll_step, 0.f);
            return true;
        }

        // Arrow keys for filter cycling
        if (e == Event::ArrowRight) {
            *filter_mode = (*filter_mode + 1) % filters.size();
            *scroll_y = 0.f;
            reload_history();
            return true;
        }
        if (e == Event::ArrowLeft) {
            *filter_mode = (*filter_mode - 1 + filters.size()) % filters.size();
            *scroll_y = 0.f;
            reload_history();
            return true;
        }

        // Refresh
        if (e == Event::Character('r') || e == Event::Character('R')) {
            reload_history();
            return true;
        }

        // Close
        if (e == Event::Escape || e == Event::Return) {
            on_close();
            return true;
        }

        return false;
    });
}

// Overload for backward compatibility
inline Component historyView(std::shared_ptr<float> scroll_y, std::function<void()> on_close) {
    auto filter_mode = std::make_shared<int>(0);
    return historyView(scroll_y, filter_mode, on_close);
}

}
