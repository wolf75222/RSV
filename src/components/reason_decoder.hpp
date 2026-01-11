#pragma once

#include <string>
#include <map>

namespace ui {

struct ReasonInfo {
    std::string description;
    std::string suggestion;
};

inline ReasonInfo decodeReason(const std::string& reason) {
    static const std::map<std::string, ReasonInfo> reasons = {
        {"Resources", {
            "Ressources demandees non disponibles",
            "Attendez ou reduisez --nodes, --ntasks, -c, --mem"
        }},
        {"Priority", {
            "D'autres jobs ont une priorite plus elevee",
            "Attendez votre tour dans la file"
        }},
        {"PartitionTimeLimit", {
            "Temps demande depasse la limite de la partition",
            "Reduisez --time ou changez de partition"
        }},
        {"PartitionNodeLimit", {
            "Nombre de noeuds depasse la limite partition",
            "Reduisez --nodes"
        }},
        {"QOSMaxCpuPerUserLimit", {
            "Limite CPU par utilisateur atteinte",
            "Attendez la fin de vos autres jobs"
        }},
        {"QOSMaxNodePerUserLimit", {
            "Limite noeuds par utilisateur atteinte",
            "Attendez la fin de vos autres jobs"
        }},
        {"QOSMaxJobsPerUserLimit", {
            "Nombre max de jobs simultanes atteint",
            "Attendez la fin d'un job"
        }},
        {"AssocGrpCpuLimit", {
            "Limite CPU du groupe/compte atteinte",
            "Attendez ou contactez l'admin"
        }},
        {"AssocGrpNodeLimit", {
            "Limite noeuds du groupe/compte atteinte",
            "Attendez ou contactez l'admin"
        }},
        {"AssocGrpMemLimit", {
            "Limite memoire du groupe atteinte",
            "Reduisez --mem ou attendez"
        }},
        {"ReqNodeNotAvail", {
            "Noeuds demandes non disponibles (maintenance?)",
            "Verifiez sinfo ou retirez --nodelist"
        }},
        {"InvalidAccount", {
            "Compte invalide",
            "Verifiez --account (ex: r250127)"
        }},
        {"InvalidQOS", {
            "QOS invalide",
            "Verifiez votre QOS"
        }},
        {"Dependency", {
            "En attente d'un autre job (dependance)",
            "Le job demarre quand la dependance se termine"
        }},
        {"DependencyNeverSatisfied", {
            "Dependance ne peut pas etre satisfaite",
            "Le job dependant a echoue, annulez ce job"
        }},
        {"BeginTime", {
            "Heure de debut non atteinte",
            "Attendez l'heure specifiee par --begin"
        }},
        {"JobHeldUser", {
            "Job mis en pause par l'utilisateur",
            "Utilisez: scontrol release <jobid>"
        }},
        {"JobHeldAdmin", {
            "Job mis en pause par l'admin",
            "Contactez le support"
        }},
        {"Reservation", {
            "En attente d'une reservation",
            "Verifiez la reservation specifiee"
        }},
        {"NodeDown", {
            "Noeuds en panne",
            "Retirez --nodelist ou attendez"
        }},
        {"BadConstraints", {
            "Contraintes impossibles a satisfaire",
            "Verifiez --constraint (ex: x64cpu, armgpu)"
        }},
        {"None", {
            "Aucune raison specifiee",
            ""
        }},
    };

    auto it = reasons.find(reason);
    if (it != reasons.end()) {
        return it->second;
    }

    // Unknown reason
    return {reason, "Consultez: scontrol show job <id>"};
}

}
