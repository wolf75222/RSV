#pragma once

#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../api/usage.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

/* When would it start? (t): a job described in a few fields, given to sbatch --test-only
- Nothing is submitted: Slurm answers with the time it would start, and where, or why it can't
- On ROMEO a job needs --account, --constraint and --mem: the fields start with what fits
*/
struct PlanView {
    struct Field {
        std::string label, value;
        std::vector<std::string> choices;   // ←/→ cycle through them; empty: typed (numbers, sizes, times)
        bool number = false;
    };

    std::vector<Field> fields;
    int focus = 0;
    bool running = false, tried = false;
    api::TestRun result;
    std::function<void(const api::TestRequest&)> run;

    // Starts from the user's accounts, the partitions and the kinds of nodes known
    void setChoices(const std::vector<std::string>& accounts, const std::vector<std::string>& partitions,
                    const std::vector<std::string>& features) {
        auto keep = [&](size_t i, const std::vector<std::string>& choices, const std::string& fallback) {
            if (fields.size() <= i) return;
            fields[i].choices = choices;
            if (fields[i].value.empty() || (!choices.empty() && std::find(choices.begin(), choices.end(), fields[i].value) == choices.end()))
                fields[i].value = choices.empty() ? fallback : choices.front();
        };
        if (fields.empty()) {
            fields = {{"Account", "", {}, false},       {"Partition", "", {}, false}, {"Constraint", "", {}, false},
                      {"Nodes", "1", {}, true},         {"Cores per node", "1", {}, true}, {"GPUs per node", "0", {}, true},
                      {"Memory per node", "4G", {}, false}, {"Time", "01:00:00", {}, false}};
        }
        keep(0, accounts, "");
        keep(1, partitions, "");
        keep(2, features, "");
        // The default partition first: short when it exists, as most jobs run there
        if (std::find(partitions.begin(), partitions.end(), "short") != partitions.end() && !tried) fields[1].value = "short";
    }

    api::TestRequest request() const {
        api::TestRequest q;
        if (fields.size() < 8) return q;
        q.account = fields[0].value;
        q.partition = fields[1].value;
        q.constraint = fields[2].value;
        q.nodes = static_cast<int>(util::toLong(fields[3].value, 1));
        q.cores = static_cast<int>(util::toLong(fields[4].value, 1));
        q.gpus = static_cast<int>(util::toLong(fields[5].value, 0));
        q.mem = fields[6].value;
        q.time = fields[7].value;
        return q;
    }

    bool onEvent(Event e, bool& close) {
        if (fields.empty()) {
            close = e == Event::Escape || e == Event::Character('q');
            return true;
        }
        Field& f = fields[static_cast<size_t>(focus)];
        if (e == Event::Escape) {
            close = true;
        } else if (e == Event::Return) {
            if (!running && run) {
                running = true;
                tried = true;
                run(request());
            }
        } else if (e == Event::ArrowDown || e == Event::Tab) {
            focus = (focus + 1) % static_cast<int>(fields.size());
        } else if (e == Event::ArrowUp || e == Event::TabReverse) {
            focus = (focus + static_cast<int>(fields.size()) - 1) % static_cast<int>(fields.size());
        } else if (e == Event::ArrowRight || e == Event::ArrowLeft) {
            const int step = e == Event::ArrowRight ? 1 : -1;
            if (!f.choices.empty()) {
                auto it = std::find(f.choices.begin(), f.choices.end(), f.value);
                long i = it == f.choices.end() ? 0 : it - f.choices.begin();
                i = (i + step + static_cast<long>(f.choices.size())) % static_cast<long>(f.choices.size());
                f.value = f.choices[static_cast<size_t>(i)];
            } else if (f.number) {
                f.value = std::to_string(std::max(0L, util::toLong(f.value, 0) + step));
            }
        } else if (e == Event::Backspace) {
            if (f.choices.empty() && !f.value.empty()) f.value.pop_back();
        } else if (e.is_character() && f.choices.empty()) {
            const std::string c = e.character();
            const bool ok = c.size() == 1 && (f.number ? std::isdigit(static_cast<unsigned char>(c[0]))
                                                       : std::isalnum(static_cast<unsigned char>(c[0])) || c == ":" || c == "-" || c == ".");
            if (ok && f.value.size() < 16) f.value += c;
        } else if (e.is_mouse()) {
            return true;
        } else {
            return true;
        }
        return true;
    }

    // "Requested node configuration is not available": the plain words for sbatch's main refusals
    static std::string explain(const std::string& message) {
        if (message.find("node configuration is not available") != std::string::npos)
            return "No node of this partition and constraint can ever run it: fewer nodes, cores, GPUs or memory, or another partition";
        if (message.find("Constraint is missing") != std::string::npos) return "ROMEO needs a constraint: armgpu or x64cpu";
        if (message.find("Memory resource is missing") != std::string::npos) return "ROMEO needs the memory per node (--mem)";
        if (message.find("time limit") != std::string::npos || message.find("TimeLimit") != std::string::npos)
            return "The time is over the partition's limit";
        if (message.find("Invalid account") != std::string::npos) return "Invalid account for this partition";
        return message;
    }

    Element render(int max_width) {
        const int width = std::min(max_width, 90);
        Elements lines = {text("Nothing is submitted: Slurm says when such a job would start (sbatch --test-only).") | dim, text("")};
        for (size_t i = 0; i < fields.size(); i++) {
            const Field& f = fields[i];
            const bool on = static_cast<int>(i) == focus;
            std::string shown = f.value.empty() ? "-" : f.value;
            if (!f.choices.empty()) shown = "< " + shown + " >";
            Element value = text(util::padRight(shown + (on && f.choices.empty() ? "_" : ""), 28));
            lines.push_back(hbox({text(util::padRight(f.label, 17)) | (on ? bold : dim), on ? value | inverted : value}));
        }
        lines.push_back(text(""));

        if (running) {
            lines.push_back(text("Asking Slurm...") | dim);
        } else if (tried) {
            const std::time_t now = std::time(nullptr);
            if (result.ok) {
                const bool soon = result.start <= now + 60;
                lines.push_back(hbox({text("Would start ") | bold,
                                      text(soon ? "now" : util::relative(result.start, now) + " (" + util::clock(result.start, now) + ")") |
                                          bold | color(soon ? theme::green() : theme::yellow())}));
                lines.push_back(text("on " + result.nodes + " · " + result.partition + " · " + std::to_string(result.cpus) +
                                     (result.cpus == 1 ? " CPU" : " CPUs")) | dim);
                lines.push_back(text("An estimate: jobs that end early, or start before it, change it") | dim);
            } else {
                lines.push_back(paragraph(explain(result.message)) | color(theme::red()));
                if (explain(result.message) != result.message) lines.push_back(paragraph(result.message) | dim);
            }
        }
        if (!fields.empty()) {
            std::string cmd;
            for (const auto& a : api::testOnlyArgs(request()))
                if (a != "--test-only" && a != "--wrap=true") cmd += (cmd.empty() ? "" : " ") + a;
            lines.push_back(text(""));
            lines.push_back(paragraph(cmd + " job.sh") | color(theme::cyan()));
        }
        lines.push_back(text(""));
        lines.push_back(hbox(keyHints({{"↑/↓", "field"}, {"←/→", "change"}, {"Enter", "ask Slurm"}, {"Esc", "close"}})));
        return panel("When would it start?", vbox(std::move(lines)) | size(WIDTH, EQUAL, width - 2));
    }
};

}  // namespace ui
