#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace ui {
using namespace ftxui;

inline Element footer() {
    return hbox({
        text(" q") | bold | color(Color::Yellow),
        text(":Quitter") | dim,
        text("  "),
        text("r") | bold | color(Color::Yellow),
        text(":Refresh") | dim,
        text("  "),
        text("h/?") | bold | color(Color::Yellow),
        text(":Aide") | dim,
        text("  "),
        text("c") | bold | color(Color::Yellow),
        text(":Annuler") | dim,
        text("  "),
        text("↑↓") | bold | color(Color::Yellow),
        text(":Navigation") | dim,
        text("  "),
        filler(),
    }) | border;
}

inline Component helpModal(std::function<void()> on_close) {
    auto content = Renderer([&] {
        return vbox({
            text("══════════ AIDE RSV ══════════") | bold | center | color(Color::Cyan),
            text(""),
            hbox({text("  q, Q, Escape  ") | color(Color::Yellow), text("Quitter l'application")}),
            hbox({text("  r, R          ") | color(Color::Yellow), text("Rafraîchir les jobs")}),
            hbox({text("  h, ?          ") | color(Color::Yellow), text("Afficher cette aide")}),
            hbox({text("  c             ") | color(Color::Yellow), text("Annuler le job sélectionné")}),
            hbox({text("  ↑ / ↓         ") | color(Color::Yellow), text("Naviguer dans la liste")}),
            hbox({text("  Molette       ") | color(Color::Yellow), text("Scroll dans les détails")}),
            text(""),
            text("═══════════════════════════════") | color(Color::Cyan),
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
