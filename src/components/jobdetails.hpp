#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>

#include "../api/slurmjobs.hpp"

namespace ui {

inline ftxui::Component jobdetails(const api::DetailedJob& job) {
    using namespace ftxui;

    return Renderer([job] {
        auto lines = vbox({
            text("Job ID: " + job.id) | bold,
            text("Nom: " + job.name),
            text("Date de soumission: " + job.submitTime),
            text("Nombre de noeuds: " + std::to_string(job.nodes)),
            text("Durée MAX: " + job.maxTime),
            text("Partition: " + job.partition),
            text("Status: " + job.status),
            text("Contraintes: " + job.constraints)
        });

        return lines | border | flex | size(HEIGHT, EQUAL, 10);
    });
}

}
