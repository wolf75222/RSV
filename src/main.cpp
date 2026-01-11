#include <ftxui/component/screen_interactive.hpp>
#include <algorithm>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>

#include "api/slurmjobs.hpp"
#include "components/jobdetails.hpp"
#include "components/nodedetails.hpp"
#include "components/title.hpp"
#include "components/footer.hpp"
#include "components/cluster_view.hpp"
#include "components/debug_view.hpp"
#include "components/log_view.hpp"
#include "components/history_view.hpp"
#include "components/quota_view.hpp"

using namespace ftxui;

int main() {
    auto jobs = std::make_shared<std::vector<api::Job>>(api::slurm::getUserJobs());
    if (jobs->empty()) {
        std::cout << "No jobs found for current user\n";
        return 0;
    }

    auto entries = std::make_shared<std::vector<std::string>>();
    for (const auto& job : *jobs)
        entries->push_back(job.name + " (" + job.id + ")");

    int selected = 0;
    bool show_help = false;
    bool show_partitions = false;
    bool show_debug = false;
    bool show_logs = false;
    auto log_show_stderr = std::make_shared<bool>(false);
    auto log_scroll_y = std::make_shared<float>(0.f);
    std::string status_message;

    // Filter state
    bool show_filter = false;
    std::string filter_text;

    // Sort state
    int sort_mode = 0;  // 0=none, 1=id, 2=name, 3=status

    // History state
    bool show_history = false;

    // Quota state
    bool show_quota = false;

    // Cancel confirmation state
    bool show_cancel_confirm = false;
    std::string cancel_job_id;
    std::string cancel_job_name;

    auto last_refresh = std::chrono::steady_clock::now();
    constexpr int AUTO_REFRESH_SECONDS = 30;

    auto current_job = std::make_shared<api::DetailedJob>(api::slurm::getJobDetails((*jobs)[selected].id));

    ScreenInteractive screen = ScreenInteractive::Fullscreen();

    // Refresh function
    auto refresh_jobs = [&]() {
        *jobs = api::slurm::getUserJobs();
        entries->clear();
        for (const auto& job : *jobs)
            entries->push_back(job.name + " (" + job.id + ")");

        if (jobs->empty()) {
            status_message = "No jobs";
            return;
        }

        if (selected >= (int)jobs->size()) {
            selected = jobs->size() - 1;
        }
        *current_job = api::slurm::getJobDetails((*jobs)[selected].id);
        last_refresh = std::chrono::steady_clock::now();
        status_message = "Refreshed!";
    };

    // Cancel job function
    auto cancel_selected_job = [&]() {
        if (jobs->empty()) return;
        std::string job_id = (*jobs)[selected].id;
        if (api::slurm::cancelJob(job_id)) {
            status_message = "Job " + job_id + " cancelled";
            refresh_jobs();
        } else {
            status_message = "Cancel error";
        }
    };

    Component job_info = Renderer([&] {
        return ui::jobdetails(*current_job)->Render();
    });

    Component job_nodes_content = Renderer([&] {
        return ui::nodedetails(*current_job, screen.dimx())->Render();
    });

    float scroll_y = 0.f;

    Component job_nodes_scrollable = Renderer(job_nodes_content, [&] {
        return job_nodes_content->Render()
               | focusPositionRelative(0.f, scroll_y)
               | frame
               | flex;
    });

    job_nodes_scrollable =
        CatchEvent(job_nodes_scrollable, [&](Event e) {
            constexpr float wheel_step = 0.05f;
            bool handled = false;

            if (e.is_mouse()) {
                if (e.mouse().button == Mouse::WheelDown) {
                    scroll_y += wheel_step;
                    handled = true;
                }
                if (e.mouse().button == Mouse::WheelUp) {
                    scroll_y -= wheel_step;
                    handled = true;
                }
            }

            if (handled) {
                scroll_y = std::clamp(scroll_y, 0.f, 1.f);
                return true;
            }

            return false;
        });

    Component job_nodes = Container::Horizontal({
        job_nodes_scrollable | flex,
    });

    MenuOption menu_opt;
    menu_opt.on_change = [&] {
        if (!jobs->empty() && selected < (int)jobs->size()) {
            *current_job = api::slurm::getJobDetails((*jobs)[selected].id);
            scroll_y = 0.f;
        }
    };

    Component sidebar =
        Menu(entries.get(), &selected, menu_opt)
        | size(WIDTH, EQUAL, 30);

    Component interface_job = Container::Vertical({
        job_info,
        job_nodes | flex,
    });

    Component interface_jobs = Container::Horizontal({
        sidebar,
        Renderer([] { return hbox({text("  "), separator(), text("  ")}); }),
        interface_job | flex,
    });

    // Status bar with last refresh time
    Component status_bar = Renderer([&] {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_refresh).count();
        int next_refresh = AUTO_REFRESH_SECONDS - elapsed;

        return hbox({
            text(" "),
            text(status_message) | color(Color::Green),
            filler(),
            text("Auto-refresh: " + std::to_string(next_refresh) + "s") | dim,
            text("  "),
        });
    });

    Component main_content = Container::Vertical({
        ui::title("Romeo Slurm Viewer (RSV) v1.0.0"),
        interface_jobs | flex,
        status_bar,
        Renderer([] { return ui::footer(); }),
    });

    // Help modal
    Component help = ui::helpModal([&] { show_help = false; });

    // Partition view
    Component partition_view = Renderer([&] {
        return vbox({
            text("══════════ CLUSTER PARTITIONS ══════════") | bold | center | color(Color::Cyan),
            text(""),
            ui::clusterView()->Render(),
            text(""),
            text("Press any key to close") | dim | center,
        }) | border | clear_under | center;
    });

    partition_view = CatchEvent(partition_view, [&](Event e) {
        if (e.is_character() || e == Event::Escape || e == Event::Return) {
            show_partitions = false;
            return true;
        }
        return false;
    });

    // Debug view (will be created dynamically based on selected job)
    auto debug_component = std::make_shared<Component>(
        ui::debugView(current_job->id, [&] { show_debug = false; })
    );

    // Log view (will be created dynamically based on selected job)
    auto log_component = std::make_shared<Component>(
        ui::logView(current_job->id, log_show_stderr, log_scroll_y, [&] { show_logs = false; })
    );

    // History view
    auto history_scroll_y = std::make_shared<float>(0.f);
    auto history_component = std::make_shared<Component>(
        ui::historyView(history_scroll_y, [&] { show_history = false; })
    );

    // Quota view
    auto quota_component = std::make_shared<Component>(
        ui::quotaView([&] { show_quota = false; })
    );

    Component interface = Container::Tab({main_content, help, partition_view}, nullptr);

    interface = Renderer(interface, [&] {
        Element base = main_content->Render();

        if (show_help) {
            return dbox({
                base,
                help->Render() | clear_under | center,
            });
        }
        if (show_partitions) {
            return dbox({
                base,
                partition_view->Render() | clear_under | center,
            });
        }
        if (show_debug) {
            return dbox({
                base,
                (*debug_component)->Render() | clear_under | center,
            });
        }
        if (show_logs) {
            return dbox({
                base,
                (*log_component)->Render() | clear_under | center,
            });
        }
        if (show_history) {
            return dbox({
                base,
                (*history_component)->Render() | clear_under | center,
            });
        }
        if (show_quota) {
            return dbox({
                base,
                (*quota_component)->Render() | clear_under | center,
            });
        }
        if (show_cancel_confirm) {
            return dbox({
                base,
                vbox({
                    text("") ,
                    text("  Cancel Job Confirmation  ") | bold | color(Color::Red) | center,
                    text(""),
                    separator(),
                    text(""),
                    hbox({
                        text("  Are you sure you want to cancel job "),
                        text(cancel_job_id) | bold | color(Color::Magenta),
                        text("?"),
                    }) | center,
                    text(""),
                    hbox({
                        text("  Name: "),
                        text(cancel_job_name) | color(Color::Cyan),
                    }) | center,
                    text(""),
                    separator(),
                    text(""),
                    hbox({
                        text("y") | bold | color(Color::Green),
                        text(": Yes, cancel  ") | dim,
                        text("n/Esc") | bold | color(Color::Red),
                        text(": No, keep job") | dim,
                    }) | center,
                    text(""),
                }) | border | clear_under | center,
            });
        }
        return base;
    });

    // Handle keyboard events
    interface = CatchEvent(interface, [&](Event e) {
        // Handle modals first
        if (show_help) {
            if (e.is_character() || e == Event::Escape || e == Event::Return) {
                show_help = false;
                return true;
            }
            return false;
        }
        if (show_partitions) {
            if (e.is_character() || e == Event::Escape || e == Event::Return) {
                show_partitions = false;
                return true;
            }
            return false;
        }
        if (show_debug) {
            if (e.is_character() || e == Event::Escape || e == Event::Return) {
                show_debug = false;
                return true;
            }
            return false;
        }
        if (show_logs) {
            // Let the log component handle all events (scrolling, tab, escape)
            return (*log_component)->OnEvent(e);
        }
        if (show_history) {
            // Let the history component handle all events (scrolling, escape)
            return (*history_component)->OnEvent(e);
        }
        if (show_quota) {
            // Let the quota component handle its events
            return (*quota_component)->OnEvent(e);
        }
        if (show_cancel_confirm) {
            // Handle cancel confirmation
            if (e == Event::Character('y') || e == Event::Character('Y')) {
                // Confirm cancel
                if (api::slurm::cancelJob(cancel_job_id)) {
                    status_message = "Job " + cancel_job_id + " cancelled";
                    refresh_jobs();
                } else {
                    status_message = "Cancel failed";
                }
                show_cancel_confirm = false;
                return true;
            }
            if (e == Event::Character('n') || e == Event::Character('N') ||
                e == Event::Escape || e == Event::Return) {
                // Abort cancel
                status_message = "Cancel aborted";
                show_cancel_confirm = false;
                return true;
            }
            return true;  // Consume all other keys
        }

        // Quit
        if (e == Event::Character('q') || e == Event::Character('Q') ||
            e == Event::Escape || e == Event::Character('\x03')) {
            screen.Exit();
            return true;
        }

        // Refresh
        if (e == Event::Character('r') || e == Event::Character('R')) {
            refresh_jobs();
            return true;
        }

        // Help
        if (e == Event::Character('h') || e == Event::Character('H') ||
            e == Event::Character('?')) {
            show_help = true;
            return true;
        }

        // Cancel job - show confirmation
        if (e == Event::Character('c') || e == Event::Character('C') ||
            e == Event::Delete) {
            if (!jobs->empty()) {
                cancel_job_id = (*jobs)[selected].id;
                cancel_job_name = (*jobs)[selected].name;
                show_cancel_confirm = true;
            }
            return true;
        }

        // Partitions view
        if (e == Event::Character('p') || e == Event::Character('P')) {
            show_partitions = true;
            return true;
        }

        // Debug view
        if (e == Event::Character('d') || e == Event::Character('D')) {
            if (!jobs->empty()) {
                *debug_component = ui::debugView((*jobs)[selected].id, [&] { show_debug = false; });
                show_debug = true;
            }
            return true;
        }

        // Logs view
        if (e == Event::Character('l') || e == Event::Character('L')) {
            if (!jobs->empty()) {
                *log_show_stderr = false;  // Reset to stdout
                *log_scroll_y = 0.f;       // Reset scroll
                *log_component = ui::logView((*jobs)[selected].id, log_show_stderr, log_scroll_y, [&] { show_logs = false; });
                show_logs = true;
            }
            return true;
        }

        // Copy job ID (y for yank)
        if (e == Event::Character('y') || e == Event::Character('Y')) {
            if (!jobs->empty()) {
                std::string job_id = (*jobs)[selected].id;
                // Use xclip/xsel on Linux, pbcopy on Mac, clip on Windows
                #ifdef _WIN32
                std::string cmd = "echo " + job_id + " | clip";
                #elif __APPLE__
                std::string cmd = "echo -n " + job_id + " | pbcopy";
                #else
                std::string cmd = "echo -n " + job_id + " | xclip -selection clipboard 2>/dev/null || echo -n " + job_id + " | xsel --clipboard 2>/dev/null";
                #endif
                system(cmd.c_str());
                status_message = "Copied: " + job_id;
            }
            return true;
        }

        // Sort jobs (s cycles through sort modes)
        if (e == Event::Character('s') || e == Event::Character('S')) {
            sort_mode = (sort_mode + 1) % 4;
            auto& j = *jobs;
            switch (sort_mode) {
                case 1:  // Sort by ID
                    std::sort(j.begin(), j.end(), [](const api::Job& a, const api::Job& b) {
                        return std::stoi(a.id) < std::stoi(b.id);
                    });
                    status_message = "Sort: ID";
                    break;
                case 2:  // Sort by name
                    std::sort(j.begin(), j.end(), [](const api::Job& a, const api::Job& b) {
                        return a.name < b.name;
                    });
                    status_message = "Sort: Name";
                    break;
                case 3:  // Sort by entry (original)
                    std::sort(j.begin(), j.end(), [](const api::Job& a, const api::Job& b) {
                        return a.entry_name < b.entry_name;
                    });
                    status_message = "Sort: Entry";
                    break;
                default:
                    refresh_jobs();  // Reset to original order
                    status_message = "Sort: Default";
                    break;
            }
            // Rebuild entries
            entries->clear();
            for (const auto& job : *jobs)
                entries->push_back(job.name + " (" + job.id + ")");
            if (!jobs->empty()) {
                selected = 0;
                *current_job = api::slurm::getJobDetails((*jobs)[selected].id);
            }
            return true;
        }

        // History view (a for archive/history)
        if (e == Event::Character('a') || e == Event::Character('A')) {
            *history_scroll_y = 0.f;
            *history_component = ui::historyView(history_scroll_y, [&] { show_history = false; });
            show_history = true;
            return true;
        }

        // Quota view (u for user quota)
        if (e == Event::Character('u') || e == Event::Character('U')) {
            *quota_component = ui::quotaView([&] { show_quota = false; });
            show_quota = true;
            return true;
        }

        return false;
    });

    // Auto-refresh loop using PostEvent
    std::atomic<bool> running{true};
    std::thread refresh_thread([&]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(AUTO_REFRESH_SECONDS));
            if (running) {
                screen.Post([&] { refresh_jobs(); });
                screen.Post(Event::Custom);
            }
        }
    });

    screen.Loop(interface);

    running = false;
    refresh_thread.join();

    return 0;
}
