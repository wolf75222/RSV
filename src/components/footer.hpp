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
        text(":Aide") | dim,
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
            text("══════════════ AIDE RSV ══════════════") | bold | center | color(Color::Cyan),
            text(""),
            text("Navigation") | bold | color(Color::Yellow),
            hbox({text("  ↑ / ↓         ") | color(Color::Cyan), text("Naviguer dans la liste des jobs")}),
            hbox({text("  Molette       ") | color(Color::Cyan), text("Scroll dans les details")}),
            text(""),
            text("Actions") | bold | color(Color::Yellow),
            hbox({text("  r / R         ") | color(Color::Cyan), text("Rafraichir les jobs")}),
            hbox({text("  c / C         ") | color(Color::Cyan), text("Annuler le job selectionne (scancel)")}),
            text(""),
            text("Vues") | bold | color(Color::Yellow),
            hbox({text("  p / P         ") | color(Color::Cyan), text("Vue partitions (sinfo)")}),
            hbox({text("  d / D         ") | color(Color::Cyan), text("Vue debug (scontrol show job)")}),
            hbox({text("  l / L         ") | color(Color::Cyan), text("Voir les logs (stdout/stderr)")}),
            text(""),
            text("Autre") | bold | color(Color::Yellow),
            hbox({text("  h / ?         ") | color(Color::Cyan), text("Afficher cette aide")}),
            hbox({text("  q / Escape    ") | color(Color::Cyan), text("Quitter l'application")}),
            text(""),
            text("═══════════════════════════════════════") | color(Color::Cyan),
            text(""),
            text("Appuyez sur une touche pour fermer") | dim | center,
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
