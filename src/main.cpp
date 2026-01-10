#include <ftxui/component/screen_interactive.hpp>
#include <algorithm>
#include <memory>

#include "api/slurmjobs.hpp"
#include "components/jobdetails.hpp"
#include "components/nodedetails.hpp"
#include "components/title.hpp"

using namespace ftxui;

int main() {
    std::vector<api::Job> jobs = api::slurm::getUserJobs();
    if (jobs.empty()) {
        std::cout << "No jobs found for current user\n";
        return 0;
    }

    std::vector<std::string> entries;
    for (const auto& job : jobs)
        entries.push_back(job.name + " (" + job.id + ")");

    int selected = 0;

    auto current_job = std::make_shared<api::DetailedJob>(api::slurm::getJobDetails(jobs[selected].id));

    ScreenInteractive screen = ScreenInteractive::Fullscreen();

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
        *current_job = api::slurm::getJobDetails(jobs[selected].id);
        scroll_y = 0.f;
    };

    Component sidebar =
        Menu(&entries, &selected, menu_opt)
        | border
        | size(WIDTH, EQUAL, 30);

    Component interface_job = Container::Vertical({
        job_info,
        job_nodes | flex,
    });

    Component interface_jobs = Container::Horizontal({
        sidebar,
        interface_job | flex,
    });

    Component interface = Container::Vertical({
        ui::title("Romeo Slurm Viewer (RSV) v1.0.0"),
        interface_jobs | flex,
    });

    screen.Loop(interface);

    return 0;
}
