#pragma once

#include <map>
#include <string>

namespace ui {

struct ReasonInfo {
    std::string description;
    std::string suggestion;
};

/* Why a job is pending, in plain words, with what to do about it
- Slurm adds details after a comma: "ReqNodeNotAvail,_UnavailableNodes:romeo-a[001-002]"
- Unknown QOS* and Assoc* limits are described from their name
*/
inline ReasonInfo decodeReason(const std::string& full) {
    const std::string reason = full.substr(0, full.find_first_of(",( "));

    static const std::map<std::string, ReasonInfo> reasons = {
        {"None", {"Waiting for the scheduler", "It runs every few seconds: wait a moment"}},
        {"Resources", {"Waiting for enough free nodes", "Fewer nodes, CPUs, memory or a shorter --time start sooner"}},
        {"Priority", {"Jobs with a higher priority are first", "A shorter --time helps: backfill can start it in a gap"}},
        {"Dependency", {"Waiting for the job it depends on", "Starts when the dependency is satisfied"}},
        {"DependencyNeverSatisfied", {"Its dependency failed: it will never start", "Cancel it (c) and resubmit"}},
        {"BeginTime", {"Its start time (--begin) isn't reached", ""}},
        {"JobHeldUser", {"Held by you", "H releases it (scontrol release <jobid>)"}},
        {"JobHeldAdmin", {"Held by an administrator", "Ask the support team"}},
        {"ReqNodeNotAvail", {"A requested node is down, drained or reserved", "Check --nodelist / --exclude, or wait"}},
        {"NodeDown", {"A node it needs is down", "Wait, or remove --nodelist"}},
        {"PartitionDown", {"The partition is down", "Wait, or use another partition"}},
        {"PartitionInactive", {"The partition is inactive", "Use another partition"}},
        {"PartitionTimeLimit", {"--time is over the partition's limit", "Lower --time or change partition (p shows their limits)"}},
        {"PartitionNodeLimit", {"Too many nodes for this partition", "Lower --nodes or change partition"}},
        {"PartitionConfig", {"Not allowed in this partition", "Check --partition and --account"}},
        {"BadConstraints", {"No node matches --constraint", "On ROMEO: --constraint=x64cpu or armgpu"}},
        {"InvalidAccount", {"Invalid account", "Check --account (u: your accounts)"}},
        {"InvalidQOS", {"Invalid QOS", "Check --qos"}},
        {"Reservation", {"Waiting for its reservation", "Check --reservation"}},
        {"Licenses", {"Waiting for a license", ""}},
        {"JobArrayTaskLimit", {"Array task limit reached (%N in --array)", "Starts when other tasks finish"}},
        {"QOSMaxJobsPerUserLimit", {"Too many running jobs for you in this QOS", "Starts when one of yours ends"}},
        {"QOSMaxSubmitJobPerUserLimit", {"Too many jobs submitted in this QOS", "Wait, or group tasks in an array"}},
        {"QOSMaxCpuPerUserLimit", {"Your CPUs in this QOS are at the limit", "Starts when your other jobs end"}},
        {"QOSMaxNodePerUserLimit", {"Your nodes in this QOS are at the limit", "Starts when your other jobs end"}},
        {"QOSMaxGRESPerUser", {"Your GPUs in this QOS are at the limit", "Starts when your other jobs end"}},
        {"QOSMaxWallDurationPerJobLimit", {"--time is over the QOS limit", "Lower --time"}},
        {"QOSGrpCpuLimit", {"All CPUs of this QOS are in use", "Wait"}},
        {"QOSGrpGRES", {"All GPUs of this QOS are in use", "Wait"}},
        {"QOSGrpNodeLimit", {"All nodes of this QOS are in use", "Wait"}},
        {"AssocGrpCpuLimit", {"Your account's CPU limit is reached", "Starts when jobs of the account end (u)"}},
        {"AssocGrpGRES", {"Your account's GPU limit is reached", "Starts when jobs of the account end (u)"}},
        {"AssocGrpNodeLimit", {"Your account's node limit is reached", "Starts when jobs of the account end (u)"}},
        {"AssocGrpMemLimit", {"Your account's memory limit is reached", "Lower --mem, or wait"}},
        {"AssocGrpJobsLimit", {"Your account's running jobs limit is reached", "Starts when a job ends"}},
        {"AssocMaxJobsLimit", {"Your running jobs limit is reached", "Starts when a job ends (u: limits)"}},
        {"AssocGrpSubmitJobsLimit", {"Your account's submitted jobs limit is reached", "Wait, or use a job array"}},
        {"AssocMaxWallDurationPerJobLimit", {"--time is over your account's limit", "Lower --time"}},
        {"AssocGrpCPUMinutesLimit", {"Your account's CPU hours are used up", "Ask for more hours, or another account"}},
        {"AssocGrpBillingMinutes", {"Your account's hours are used up", "Ask for more hours, or another account"}},
        {"AssocMaxCpuPerJobLimit", {"Too many CPUs for one job", "Lower the CPU count"}},
        {"AssocMaxNodePerJobLimit", {"Too many nodes for one job", "Lower --nodes"}},
        {"launch", {"The launch failed, the job is held", "Look at its log (l), then scontrol release <jobid>"}},
        {"Cleaning", {"The previous run is being cleaned up", "Wait"}},
        {"Prolog", {"The node is running its prolog", "Wait"}},
        {"SystemFailure", {"Slurm or node failure", "Wait, or ask the support team"}},
    };

    const auto it = reasons.find(reason);
    if (it != reasons.end()) return it->second;

    auto has = [&](const char* s) { return reason.find(s) != std::string::npos; };
    if (has("QOS") || has("Assoc")) {
        const std::string who = has("Assoc") ? "your account" : "the QOS";
        std::string what = has("Cpu") ? "CPU" : has("Node") ? "node" : has("GRES") ? "GPU" : has("Mem") ? "memory"
                         : has("Wall") || has("Time") ? "time" : has("Job") ? "job" : "resource";
        return {"A " + what + " limit of " + who + " is reached", "u shows your limits"};
    }
    return {full, "d shows everything Slurm knows about the job"};
}

}  // namespace ui
