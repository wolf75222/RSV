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
    float scroll_step = 0.05f;  // Adjustable with +/-
    std::string status_message;
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
            bool handled = false;

            if (e.is_mouse()) {
                if (e.mouse().button == Mouse::WheelDown) {
                    scroll_y += scroll_step;
                    handled = true;
                }
                if (e.mouse().button == Mouse::WheelUp) {
                    scroll_y -= scroll_step;
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
        ui::logView(current_job->id, log_show_stderr, [&] { show_logs = false; })
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
            if (e == Event::Tab || e == Event::TabReverse) {
                *log_show_stderr = !*log_show_stderr;
                return true;
            }
            if (e.is_character() || e == Event::Escape || e == Event::Return) {
                show_logs = false;
                return true;
            }
            return false;
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

        // Cancel job
        if (e == Event::Character('c') || e == Event::Character('C') ||
            e == Event::Delete) {
            cancel_selected_job();
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
                *log_component = ui::logView((*jobs)[selected].id, log_show_stderr, [&] { show_logs = false; });
                show_logs = true;
            }
            return true;
        }

        // Helper to update job details after selection change
        auto update_selection = [&]() {
            if (!jobs->empty() && selected < (int)jobs->size()) {
                *current_job = api::slurm::getJobDetails((*jobs)[selected].id);
                scroll_y = 0.f;
            }
        };

        // Vim-style navigation (j/k)
        if (e == Event::Character('j')) {
            if (!jobs->empty() && selected < (int)jobs->size() - 1) {
                selected++;
                update_selection();
            }
            return true;
        }
        if (e == Event::Character('k')) {
            if (!jobs->empty() && selected > 0) {
                selected--;
                update_selection();
            }
            return true;
        }

        // Go to first/last job (g/G or Home/End)
        if (e == Event::Character('g') || e == Event::Home) {
            if (!jobs->empty()) {
                selected = 0;
                update_selection();
            }
            return true;
        }
        if (e == Event::Character('G') || e == Event::End) {
            if (!jobs->empty()) {
                selected = jobs->size() - 1;
                update_selection();
            }
            return true;
        }

        // Page Up/Down for details scrolling
        if (e == Event::PageUp) {
            scroll_y -= 0.25f;
            scroll_y = std::clamp(scroll_y, 0.f, 1.f);
            return true;
        }
        if (e == Event::PageDown) {
            scroll_y += 0.25f;
            scroll_y = std::clamp(scroll_y, 0.f, 1.f);
            return true;
        }

        // Zoom in/out (adjust scroll step)
        if (e == Event::Character('+') || e == Event::Character('=')) {
            scroll_step = std::min(scroll_step + 0.02f, 0.2f);
            status_message = "Scroll: " + std::to_string((int)(scroll_step * 100)) + "%";
            return true;
        }
        if (e == Event::Character('-') || e == Event::Character('_')) {
            scroll_step = std::max(scroll_step - 0.02f, 0.01f);
            status_message = "Scroll: " + std::to_string((int)(scroll_step * 100)) + "%";
            return true;
        }

        // Quick number selection (1-9 for first 9 jobs)
        for (char c = '1'; c <= '9'; c++) {
            if (e == Event::Character(c)) {
                int idx = c - '1';
                if (!jobs->empty() && idx < (int)jobs->size()) {
                    selected = idx;
                    update_selection();
                }
                return true;
            }
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
