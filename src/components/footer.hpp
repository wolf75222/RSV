#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace ui {
using namespace ftxui;

inline Element footer() {
    return hbox({
        text(" q") | bold | color(Color::Yellow),
        text(":Quit") | dim,
        text(" "),
        text("r") | bold | color(Color::Yellow),
        text(":Refresh") | dim,
        text(" "),
        text("h") | bold | color(Color::Yellow),
        text(":Help") | dim,
        text(" "),
        text("c") | bold | color(Color::Yellow),
        text(":Cancel") | dim,
        text(" "),
        text("p") | bold | color(Color::Yellow),
        text(":Partitions") | dim,
        text(" "),
        text("d") | bold | color(Color::Yellow),
        text(":Debug") | dim,
        text(" "),
        text("l") | bold | color(Color::Yellow),
        text(":Logs") | dim,
        text(" "),
        filler(),
    }) | border;
}

inline Component helpModal(std::function<void()> on_close) {
    auto content = Renderer([&] {
        return vbox({
            text("══════════════════ RSV HELP ══════════════════") | bold | center | color(Color::Cyan),
            text(""),
            text("Navigation") | bold | color(Color::Yellow),
            hbox({text("  Up/Down, j/k  ") | color(Color::Cyan), text("Navigate job list")}),
            hbox({text("  g / G         ") | color(Color::Cyan), text("Go to first / last job")}),
            hbox({text("  Home / End    ") | color(Color::Cyan), text("Go to first / last job")}),
            hbox({text("  1-9           ") | color(Color::Cyan), text("Quick select job 1-9")}),
            text(""),
            text("Scrolling") | bold | color(Color::Yellow),
            hbox({text("  Mouse wheel   ") | color(Color::Cyan), text("Scroll details")}),
            hbox({text("  PgUp / PgDown ") | color(Color::Cyan), text("Scroll details fast")}),
            hbox({text("  + / -         ") | color(Color::Cyan), text("Adjust scroll speed")}),
            text(""),
            text("Actions") | bold | color(Color::Yellow),
            hbox({text("  r / R         ") | color(Color::Cyan), text("Refresh jobs")}),
            hbox({text("  c / C         ") | color(Color::Cyan), text("Cancel selected job (scancel)")}),
            text(""),
            text("Views") | bold | color(Color::Yellow),
            hbox({text("  p / P         ") | color(Color::Cyan), text("Partitions view (sinfo)")}),
            hbox({text("  d / D         ") | color(Color::Cyan), text("Debug view (scontrol show job)")}),
            hbox({text("  l / L         ") | color(Color::Cyan), text("View logs (stdout/stderr)")}),
            text(""),
            text("Other") | bold | color(Color::Yellow),
            hbox({text("  h / ?         ") | color(Color::Cyan), text("Show this help")}),
            hbox({text("  q / Escape    ") | color(Color::Cyan), text("Quit application")}),
            text(""),
            text("═══════════════════════════════════════════════") | color(Color::Cyan),
            text(""),
            text("Press any key to close") | dim | center,
        }) | border | clear_under | center;
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
