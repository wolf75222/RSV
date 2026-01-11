#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <sstream>
#include <regex>
#include <cstdlib>
#include "../api/slurmjobs.hpp"

namespace ui {
using namespace ftxui;

struct UserQuota {
    std::string user;
    std::string account;
    int max_cpus = 0;
    int max_nodes = 0;
    int max_jobs = 0;
    int used_cpus = 0;
    int used_nodes = 0;
    int running_jobs = 0;
    int pending_jobs = 0;
};

inline std::string execCmd(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(
        popen(cmd.c_str(), "r"),
        static_cast<int(*)(FILE*)>(pclose)
    );
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

inline UserQuota getUserQuota() {
    UserQuota quota;

    const char* user = std::getenv("USER");
    if (!user) user = "unknown";
    quota.user = user;

    // Get association limits from sacctmgr
    std::string cmd = "sacctmgr show Association where user=" + std::string(user) +
                      " format=User,Account,GrpTRES,MaxTRES,GrpJobs,MaxJobs -P --noheader 2>/dev/null";
    std::string out = execCmd(cmd);

    // Parse output (format: user|account|grptres|maxtres|grpjobs|maxjobs)
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;

        std::vector<std::string> fields;
        std::istringstream lss(line);
        std::string field;
        while (std::getline(lss, field, '|')) {
            fields.push_back(field);
        }

        if (fields.size() >= 2) {
            quota.account = fields[1];
        }

        // Parse TRES limits (cpu=X,node=Y,...)
        auto parseTRES = [](const std::string& tres, int& cpus, int& nodes) {
            std::regex cpu_re(R"(cpu=(\d+))");
            std::regex node_re(R"(node=(\d+))");
            std::smatch m;
            if (std::regex_search(tres, m, cpu_re)) cpus = std::stoi(m[1]);
            if (std::regex_search(tres, m, node_re)) nodes = std::stoi(m[1]);
        };

        if (fields.size() >= 3) parseTRES(fields[2], quota.max_cpus, quota.max_nodes);
        if (fields.size() >= 4) parseTRES(fields[3], quota.max_cpus, quota.max_nodes);

        // Parse job limits
        if (fields.size() >= 5 && !fields[4].empty()) {
            try { quota.max_jobs = std::stoi(fields[4]); } catch (...) {}
        }
        if (fields.size() >= 6 && !fields[5].empty()) {
            try { quota.max_jobs = std::max(quota.max_jobs, std::stoi(fields[5])); } catch (...) {}
        }
    }

    // Get current usage from squeue
    cmd = "squeue -u " + std::string(user) + " -o \"%T %C\" --noheader 2>/dev/null";
    out = execCmd(cmd);

    std::istringstream iss2(out);
    while (std::getline(iss2, line)) {
        if (line.empty()) continue;

        std::istringstream lss(line);
        std::string state;
        int cpus = 0;
        lss >> state >> cpus;

        if (state == "RUNNING") {
            quota.used_cpus += cpus;
            quota.running_jobs++;
        } else if (state == "PENDING") {
            quota.pending_jobs++;
        }
    }

    // Get node count for running jobs
    cmd = "squeue -u " + std::string(user) + " -t RUNNING -o \"%D\" --noheader 2>/dev/null";
    out = execCmd(cmd);

    std::istringstream iss3(out);
    while (std::getline(iss3, line)) {
        if (line.empty()) continue;
        try { quota.used_nodes += std::stoi(line); } catch (...) {}
    }

    return quota;
}

inline Element renderQuotaBar(int used, int max, Color color) {
    if (max <= 0) return text("N/A") | dim;

    float ratio = (float)used / max;
    int bar_width = 30;
    int filled = (int)(ratio * bar_width);
    filled = std::min(filled, bar_width);

    std::string bar = "";
    for (int i = 0; i < filled; i++) bar += "█";
    for (int i = filled; i < bar_width; i++) bar += "░";

    Color bar_color = color;
    if (ratio >= 0.9) bar_color = Color::Red;
    else if (ratio >= 0.7) bar_color = Color::Yellow;

    return hbox({
        text(bar) | color(bar_color),
        text(" "),
        text(std::to_string(used) + "/" + std::to_string(max)) | bold,
        text(" (" + std::to_string((int)(ratio * 100)) + "%)") | dim,
    });
}

inline Component quotaView(std::function<void()> on_close) {
    auto quota = std::make_shared<UserQuota>(getUserQuota());

    auto content = Renderer([=] {
        int remaining_cpus = quota->max_cpus > 0 ? quota->max_cpus - quota->used_cpus : -1;
        int remaining_nodes = quota->max_nodes > 0 ? quota->max_nodes - quota->used_nodes : -1;

        std::vector<Element> elements;

        elements.push_back(
            text("═══════════ USER QUOTA ═══════════") | bold | center | color(Color::Cyan)
        );
        elements.push_back(separator());

        // User info
        elements.push_back(hbox({
            text("User: ") | dim,
            text(quota->user) | bold | color(Color::Magenta),
            text("  Account: ") | dim,
            text(quota->account.empty() ? "default" : quota->account) | color(Color::Cyan),
        }));
        elements.push_back(separator());

        // CPU quota
        elements.push_back(text("CPU Cores") | bold | color(Color::Yellow));
        if (quota->max_cpus > 0) {
            elements.push_back(hbox({
                text("  Used:      "),
                renderQuotaBar(quota->used_cpus, quota->max_cpus, Color::Green),
            }));
            elements.push_back(hbox({
                text("  Remaining: "),
                text(std::to_string(remaining_cpus)) | bold |
                    color(remaining_cpus > 0 ? Color::Green : Color::Red),
                text(" cores available") | dim,
            }));
        } else {
            elements.push_back(hbox({
                text("  "),
                text("No CPU limit configured") | dim,
                text(" (using " + std::to_string(quota->used_cpus) + " cores)") | color(Color::Cyan),
            }));
        }
        elements.push_back(text(""));

        // Node quota
        elements.push_back(text("Nodes") | bold | color(Color::Yellow));
        if (quota->max_nodes > 0) {
            elements.push_back(hbox({
                text("  Used:      "),
                renderQuotaBar(quota->used_nodes, quota->max_nodes, Color::Blue),
            }));
            elements.push_back(hbox({
                text("  Remaining: "),
                text(std::to_string(remaining_nodes)) | bold |
                    color(remaining_nodes > 0 ? Color::Green : Color::Red),
                text(" nodes available") | dim,
            }));
        } else {
            elements.push_back(hbox({
                text("  "),
                text("No node limit configured") | dim,
                text(" (using " + std::to_string(quota->used_nodes) + " nodes)") | color(Color::Cyan),
            }));
        }
        elements.push_back(text(""));

        // Job quota
        elements.push_back(text("Jobs") | bold | color(Color::Yellow));
        int total_jobs = quota->running_jobs + quota->pending_jobs;
        if (quota->max_jobs > 0) {
            elements.push_back(hbox({
                text("  Active:    "),
                renderQuotaBar(total_jobs, quota->max_jobs, Color::Magenta),
            }));
        }
        elements.push_back(hbox({
            text("  Running: ") | dim,
            text(std::to_string(quota->running_jobs)) | color(Color::Green),
            text("  Pending: ") | dim,
            text(std::to_string(quota->pending_jobs)) | color(Color::Yellow),
        }));

        // Warning if at limit
        if (remaining_cpus == 0 || remaining_nodes == 0) {
            elements.push_back(text(""));
            elements.push_back(
                hbox({
                    text("⚠ ") | color(Color::Red),
                    text("You are at your resource limit!") | bold | color(Color::Red),
                }) | center
            );
            elements.push_back(
                text("Jobs pending with AssocGrp*Limit will start when resources free up.") | dim | center
            );
        }

        elements.push_back(separator());
        elements.push_back(hbox({
            text("r") | bold | color(Color::Yellow),
            text(": refresh  ") | dim,
            text("Esc") | bold | color(Color::Yellow),
            text(": close") | dim,
        }) | center);

        return vbox(elements) | border | size(WIDTH, LESS_THAN, 70);
    });

    return CatchEvent(content, [=](Event e) {
        if (e == Event::Character('r') || e == Event::Character('R')) {
            *quota = getUserQuota();
            return true;
        }
        if (e == Event::Escape || e == Event::Return || e.is_character()) {
            on_close();
            return true;
        }
        return false;
    });
}

}
