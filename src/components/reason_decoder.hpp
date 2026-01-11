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
            "Requested resources not available",
            "Wait or reduce --nodes, --ntasks, -c, --mem"
        }},
        {"Priority", {
            "Other jobs have higher priority",
            "Wait for your turn in the queue"
        }},
        {"PartitionTimeLimit", {
            "Requested time exceeds partition limit",
            "Reduce --time or change partition"
        }},
        {"PartitionNodeLimit", {
            "Node count exceeds partition limit",
            "Reduce --nodes"
        }},
        {"QOSMaxCpuPerUserLimit", {
            "CPU limit per user reached",
            "Wait for your other jobs to finish"
        }},
        {"QOSMaxNodePerUserLimit", {
            "Node limit per user reached",
            "Wait for your other jobs to finish"
        }},
        {"QOSMaxJobsPerUserLimit", {
            "Max concurrent jobs limit reached",
            "Wait for a job to finish"
        }},
        {"AssocGrpCpuLimit", {
            "Group/account CPU limit reached",
            "Wait or contact admin"
        }},
        {"AssocGrpNodeLimit", {
            "Group/account node limit reached",
            "Wait or contact admin"
        }},
        {"AssocGrpMemLimit", {
            "Group memory limit reached",
            "Reduce --mem or wait"
        }},
        {"ReqNodeNotAvail", {
            "Requested nodes unavailable (maintenance?)",
            "Check sinfo or remove --nodelist"
        }},
        {"InvalidAccount", {
            "Invalid account",
            "Check --account"
        }},
        {"InvalidQOS", {
            "Invalid QOS",
            "Check your QOS"
        }},
        {"Dependency", {
            "Waiting for another job (dependency)",
            "Job starts when dependency completes"
        }},
        {"DependencyNeverSatisfied", {
            "Dependency cannot be satisfied",
            "Dependent job failed, cancel this job"
        }},
        {"BeginTime", {
            "Start time not reached",
            "Wait for time specified by --begin"
        }},
        {"JobHeldUser", {
            "Job held by user",
            "Use: scontrol release <jobid>"
        }},
        {"JobHeldAdmin", {
            "Job held by admin",
            "Contact support"
        }},
        {"Reservation", {
            "Waiting for reservation",
            "Check the specified reservation"
        }},
        {"NodeDown", {
            "Nodes are down",
            "Remove --nodelist or wait"
        }},
        {"BadConstraints", {
            "Constraints cannot be satisfied",
            "Check --constraint"
        }},
        {"None", {
            "No reason specified",
            ""
        }},
    };

    auto it = reasons.find(reason);
    if (it != reasons.end()) {
        return it->second;
    }

    // Unknown reason
    return {reason, "See: scontrol show job <id>"};
}

}
