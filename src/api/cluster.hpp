#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <sys/statvfs.h>

#include "slurmjobs.hpp"

/* The whole cluster: nodes, partitions, everyone's queue, reservations, the controller and the scheduler
- On ROMEO every user sees all of it (PrivateData = none)
- The cheap commands are read at each refresh of the cluster view, sdiag and the partition limits when it opens
*/
namespace api {

enum class NodeUse { Idle, Mixed, Full, Down };

// sinfo's compact states: idle, mix, alloc, comp, drng, drain, down, maint, resv, plnd...; '*' means not responding
inline NodeUse nodeUse(const std::string& state) {
    if (state.find('*') != std::string::npos || util::startsWith(state, "down") || util::startsWith(state, "drain") ||
        util::startsWith(state, "fail") || util::startsWith(state, "maint") || util::startsWith(state, "inval") ||
        util::startsWith(state, "resv") || util::startsWith(state, "boot") || util::startsWith(state, "unk") ||
        util::startsWith(state, "pow") || util::startsWith(state, "drng"))
        return NodeUse::Down;
    if (util::startsWith(state, "idle")) return NodeUse::Idle;
    if (util::startsWith(state, "mix")) return NodeUse::Mixed;
    return NodeUse::Full;
}

struct ClusterNode {
    std::string name, features, state;
    NodeUse use = NodeUse::Idle;
    long cpus_alloc = 0, cpus_total = 0;
    int gpus_used = 0, gpus_total = 0;
    double load = -1;                                    // CPULoad, -1 when unknown
    double mem_alloc_mib = 0, mem_free_mib = -1, mem_total_mib = 0;

    // Cores in use by the load, at most the allocated ones (a load of 64 on 64 reserved cores: all busy)
    double busyCores() const { return load < 0 ? double(cpus_alloc) : std::min(load, double(cpus_alloc)); }

    // Reserved by jobs but doing almost nothing: a quarter of its cores at least, a tenth of them busy at most
    bool reservedIdle() const {
        return use != NodeUse::Down && load >= 0 && cpus_alloc >= std::max(8L, cpus_total / 4) && load < 0.1 * cpus_alloc;
    }
};

// Nodes by use: idle, mixed (partly used), full, down (down, drained, not responding)
struct NodeCounts {
    int nodes = 0, idle = 0, mixed = 0, full = 0, down = 0;
    long cpus_alloc = 0, cpus_total = 0;
    int gpus_used = 0, gpus_total = 0;
    double busy = 0;                                     // cores busy (see ClusterNode::busyCores)
    double mem_alloc_mib = 0, mem_total_mib = 0;
    int reserved_idle = 0;

    void add(const ClusterNode& n) {
        nodes++;
        (n.use == NodeUse::Idle ? idle : n.use == NodeUse::Mixed ? mixed : n.use == NodeUse::Full ? full : down)++;
        cpus_alloc += n.cpus_alloc;
        cpus_total += n.cpus_total;
        gpus_used += n.gpus_used;
        gpus_total += n.gpus_total;
        busy += n.busyCores();
        mem_alloc_mib += n.mem_alloc_mib;
        mem_total_mib += n.mem_total_mib;
        reserved_idle += n.reservedIdle();
    }
};

struct Partition : NodeCounts {
    std::string name, avail, timelimit;
    bool is_default = false;
};

// Nodes of the same kind (their features: armgpu, x64cpu on ROMEO), each counted once
struct NodeType : NodeCounts {
    std::string features, prefix;
};

inline std::string sinfoFormat() {
    return "Partition:|,Available:|,Time:|,NodeList:|,StateCompact:|,CPUsState:|,Gres:|,GresUsed:|,Features:|,"
           "CPUsLoad:|,AllocMem:|,FreeMem:|,Memory:|";
}

struct Sinfo {
    std::vector<Partition> partitions;
    std::vector<ClusterNode> nodes;   // each node once, in sinfo's order
};

/* One line per node and partition (sinfo -N), summed by partition: exact GPU counts, which grouped lines don't give
- A node is in several partitions on ROMEO: `nodes` keeps it once, for the totals and the node map
*/
inline Sinfo parseSinfo(const std::string& out) {
    Sinfo r;
    std::map<std::string, size_t> part_index, node_index;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, '|');
        if (f.size() < 8) continue;
        std::string name = util::trim(f[0]);
        bool is_default = false;
        if (!name.empty() && name.back() == '*') {
            name.pop_back();
            is_default = true;
        }
        if (name.empty()) continue;

        ClusterNode n;
        n.name = util::trim(f[3]);
        n.state = util::trim(f[4]);
        n.use = nodeUse(n.state);
        const auto cpus = util::split(f[5], '/');   // allocated/idle/other/total
        if (cpus.size() == 4) {
            n.cpus_alloc = util::toLong(cpus[0]);
            n.cpus_total = util::toLong(cpus[3]);
        }
        n.gpus_total = parseGpus(f[6]).count;
        n.gpus_used = parseGpus(f[7]).count;
        n.features = f.size() > 8 ? util::trim(f[8]) : "";
        if (n.features == "(null)") n.features.clear();
        if (f.size() > 12) {
            n.load = util::toDouble(util::trim(f[9]), -1);
            n.mem_alloc_mib = util::toDouble(util::trim(f[10]));
            n.mem_free_mib = util::toDouble(util::trim(f[11]), -1);
            n.mem_total_mib = util::toDouble(util::trim(f[12]));
        }

        if (!part_index.count(name)) {
            part_index[name] = r.partitions.size();
            Partition p;
            p.name = name;
            p.avail = util::trim(f[1]);
            p.timelimit = util::trim(f[2]);
            r.partitions.push_back(p);
        }
        Partition& p = r.partitions[part_index[name]];
        p.is_default = p.is_default || is_default;
        p.add(n);

        if (!n.name.empty() && !node_index.count(n.name)) {
            node_index[n.name] = r.nodes.size();
            r.nodes.push_back(n);
        }
    }
    return r;
}

// Kept for the partitions alone
inline std::vector<Partition> parsePartitions(const std::string& out) { return parseSinfo(out).partitions; }

inline std::string nodePrefix(const std::string& name) {
    size_t end = name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1]))) end--;
    return name.substr(0, end);
}

// The nodes by kind, in the order they first appear: armgpu (romeo-a), x64cpu (romeo-c)
inline std::vector<NodeType> nodeTypes(const std::vector<ClusterNode>& nodes) {
    std::vector<NodeType> types;
    std::map<std::string, size_t> index;
    for (const auto& n : nodes) {
        const std::string key = n.features.empty() ? "other" : n.features;
        if (!index.count(key)) {
            index[key] = types.size();
            NodeType t;
            t.features = key;
            t.prefix = nodePrefix(n.name);
            types.push_back(t);
        }
        types[index[key]].add(n);
    }
    return types;
}

// ---------------------------------------------------------------- why nodes are down (sinfo -R)

struct DownReason {
    std::string reason, user, nodes, state;
    std::time_t since = 0;
};

inline std::string sinfoReasonFormat() { return "%E|%u|%H|%N|%T"; }

// The reason is first: it may contain '|', the other fields are read from the right
inline std::vector<DownReason> parseDownReasons(const std::string& out) {
    std::vector<DownReason> r;
    for (const auto& line : util::lines(out)) {
        auto f = util::split(line, '|');
        if (f.size() < 5) continue;
        DownReason d;
        d.state = util::trim(f[f.size() - 1]);
        d.nodes = util::trim(f[f.size() - 2]);
        d.since = util::parseTime(util::trim(f[f.size() - 3]));
        d.user = util::trim(f[f.size() - 4]);
        f.resize(f.size() - 4);
        for (size_t i = 0; i < f.size(); i++) d.reason += (i ? "|" : "") + f[i];
        d.reason = util::sanitize(util::trim(d.reason));
        if (!d.nodes.empty()) r.push_back(d);
    }
    std::sort(r.begin(), r.end(), [](const DownReason& a, const DownReason& b) { return a.since > b.since; });
    return r;
}

// ---------------------------------------------------------------- reservations (scontrol show reservation)

struct Reservation {
    std::string name, nodes, features, partition, flags, state;
    std::time_t start = 0, end = 0;
    int node_count = 0;
    long cores = 0;

    bool maintenance() const { return flags.find("MAINT") != std::string::npos; }
};

inline std::vector<Reservation> parseReservations(const std::string& out) {
    std::vector<std::map<std::string, std::string>> records;
    for (const auto& line : util::lines(out)) {
        if (util::startsWith(line, "ReservationName=")) records.emplace_back();
        if (!records.empty()) parseFields(line, records.back());
    }
    std::vector<Reservation> r;
    for (auto& f : records) {
        Reservation v;
        v.name = f["ReservationName"];
        v.start = util::parseTime(f["StartTime"]);
        v.end = util::parseTime(f["EndTime"]);
        v.nodes = f["Nodes"] == "(null)" ? "" : f["Nodes"];
        v.node_count = static_cast<int>(util::toLong(f["NodeCnt"]));
        v.cores = util::toLong(f["CoreCnt"]);
        v.features = f["Features"] == "(null)" ? "" : f["Features"];
        v.partition = f["PartitionName"] == "(null)" ? "" : f["PartitionName"];
        v.flags = f["Flags"];
        v.state = f["State"];
        if (!v.name.empty()) r.push_back(v);
    }
    std::sort(r.begin(), r.end(), [](const Reservation& a, const Reservation& b) { return a.start < b.start; });
    return r;
}

// ---------------------------------------------------------------- the controller (scontrol ping)

// "Slurmctld(primary) at admin1 is UP" -> "primary admin1 UP", one per controller
inline std::vector<std::string> parsePing(const std::string& out) {
    std::vector<std::string> r;
    for (const auto& line : util::lines(out)) {
        const size_t open = line.find('('), close = line.find(')'), at = line.find(" at "), is = line.rfind(" is ");
        if (open == std::string::npos || close == std::string::npos || at == std::string::npos || is == std::string::npos) continue;
        r.push_back(line.substr(open + 1, close - open - 1) + " " + line.substr(at + 4, is - at - 4) + " " +
                    util::trim(line.substr(is + 4)));
    }
    return r;
}

// "UP" when every controller that should answer does, else what isn't
inline std::string controllerState(const std::vector<std::string>& ping) {
    if (ping.empty()) return "";
    for (const auto& p : ping)
        if (util::startsWith(p, "primary") && p.find("UP") == std::string::npos) return "primary controller " + p.substr(8);
    return "UP";
}

// ---------------------------------------------------------------- the scheduler (sdiag)

struct Sdiag {
    std::time_t since = 0;
    long submitted = -1, started = -1, completed = -1, canceled = -1, failed = -1;
    long main_last_us = -1, main_mean_us = -1, main_max_us = -1, main_per_minute = -1;
    long bf_last_us = -1, bf_mean_us = -1, bf_max_us = -1, bf_jobs = -1;
};

inline Sdiag parseSdiag(const std::string& out) {
    Sdiag d;
    enum { NONE, MAIN, BACKFILL } section = NONE;
    for (const auto& raw : util::lines(out)) {
        const std::string line = util::trim(raw);
        if (util::startsWith(line, "Main schedule statistics")) section = MAIN;
        else if (util::startsWith(line, "Backfilling stats")) section = BACKFILL;
        else if (util::startsWith(line, "Main scheduler exit") || util::startsWith(line, "Remote Procedure")) section = NONE;
        // "Data since      Fri Oct 23 02:00:00 2026 (1792713600)": the date has colons, the epoch is in parentheses
        if (util::startsWith(line, "Data since")) {
            const size_t open = line.rfind('(');
            if (open != std::string::npos) d.since = static_cast<std::time_t>(util::toLong(line.substr(open + 1), 0));
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = util::trim(line.substr(0, colon));
        const std::string value = util::trim(line.substr(colon + 1));
        const long n = util::toLong(value, -1);
        if (key == "Jobs submitted") d.submitted = n;
        else if (key == "Jobs started") d.started = n;
        else if (key == "Jobs completed") d.completed = n;
        else if (key == "Jobs canceled") d.canceled = n;
        else if (key == "Jobs failed") d.failed = n;
        else if (section == MAIN && key == "Last cycle") d.main_last_us = n;
        else if (section == MAIN && key == "Max cycle") d.main_max_us = n;
        else if (section == MAIN && key == "Mean cycle") d.main_mean_us = n;
        else if (section == MAIN && key == "Cycles per minute") d.main_per_minute = n;
        else if (section == BACKFILL && key == "Last cycle") d.bf_last_us = n;
        else if (section == BACKFILL && key == "Max cycle") d.bf_max_us = n;
        else if (section == BACKFILL && key == "Mean cycle") d.bf_mean_us = n;
        else if (section == BACKFILL && util::startsWith(key, "Total backfilled jobs (since last slurm start)")) d.bf_jobs = n;
    }
    return d;
}

// ---------------------------------------------------------------- partition limits (scontrol show partition)

struct PartitionLimits {
    std::string name, max_time, max_nodes, def_mem, max_mem, allow_accounts, qos, state;
    long tier = -1;
};

inline std::map<std::string, PartitionLimits> parsePartitionLimits(const std::string& out) {
    std::map<std::string, PartitionLimits> r;
    for (const auto& line : util::lines(out)) {
        if (!util::startsWith(util::trim(line), "PartitionName=")) continue;
        std::map<std::string, std::string> f;
        parseFields(line, f);
        PartitionLimits p;
        p.name = f["PartitionName"];
        p.max_time = f["MaxTime"];
        p.max_nodes = f["MaxNodes"];
        // Default and largest memory: per node, or per CPU (then written "per CPU")
        p.def_mem = f.count("DefMemPerNode") ? f["DefMemPerNode"] : f.count("DefMemPerCPU") ? f["DefMemPerCPU"] + " per CPU" : "";
        p.max_mem = f.count("MaxMemPerNode") ? f["MaxMemPerNode"] : f.count("MaxMemPerCPU") ? f["MaxMemPerCPU"] + " per CPU" : "";
        p.allow_accounts = f["AllowAccounts"];
        p.qos = f["QoS"];
        p.state = f["State"];
        p.tier = util::toLong(f["PriorityTier"], -1);
        if (!p.name.empty()) r[p.name] = p;
    }
    return r;
}

// ---------------------------------------------------------------- storage (statvfs, like df)

struct Storage {
    std::string path;
    double total_bytes = 0, used_bytes = 0;
};

// The shared file system: /gpfs on ROMEO, RSV_STORAGE for another one (or "" to hide it)
inline Storage readStorage() {
    Storage s;
    const char* env = std::getenv("RSV_STORAGE");
    s.path = env ? env : "/gpfs";
    struct statvfs v{};
    if (s.path.empty() || statvfs(s.path.c_str(), &v) != 0 || v.f_blocks == 0) {
        s.path.clear();
        return s;
    }
    s.total_bytes = double(v.f_blocks) * v.f_frsize;
    s.used_bytes = s.total_bytes - double(v.f_bfree) * v.f_frsize;
    return s;
}

// ---------------------------------------------------------------- everyone's jobs (squeue without -u)

struct QueueCounts {
    int running = 0, pending = 0;
    long cpus_running = 0, cpus_pending = 0;   // in use, asked for
    int gpus_running = 0, gpus_pending = 0;
};

struct UserUse : QueueCounts {
    std::string name;   // a user or an account
};

struct Queue : QueueCounts {
    std::set<std::string> users;
    std::map<std::string, QueueCounts> by_partition;
    std::map<std::string, int> reasons;          // why the pending jobs wait
    std::map<std::string, QueueCounts> reason_use;   // and what they ask for
    std::vector<UserUse> by_user, by_account;    // the most CPUs in use first
};

inline const std::vector<std::string>& queueFields() {
    static const std::vector<std::string> fields = {"UserName", "Account", "Partition", "State",   "NumCPUs",
                                                    "tres-alloc", "Reason", "JobArrayID"};
    return fields;
}

inline std::string queueFormat() {
    std::string format;
    for (const auto& f : queueFields()) format += (format.empty() ? "" : ",") + f + ":" + SEP;
    return format;
}

inline Queue parseQueue(const std::string& out) {
    Queue q;
    std::map<std::string, UserUse> users, accounts;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, SEP);
        if (f.size() < queueFields().size()) continue;
        const std::string user = util::trim(f[0]);
        const std::string account = util::trim(f[1]);
        std::string partition = util::trim(f[2]);
        partition = partition.substr(0, partition.find(','));   // a pending job can ask for several: the first one
        const std::string state = util::trim(f[3]);
        const long cpus = util::toLong(util::trim(f[4]));
        const auto tres = parseTres(util::trim(f[5]));
        const int gpus = tres.count("gres/gpu") ? static_cast<int>(tres.at("gres/gpu")) : 0;
        const bool running = isActive(state);
        const bool pending = isPending(state);
        if (!running && !pending) continue;
        // 812402_[3-40] is 38 pending tasks
        int count = 1;
        const std::string id = util::trim(f[7]);
        const size_t open = id.find("_[");
        if (pending && open != std::string::npos) {
            std::string ranges = id.substr(open + 2, id.find(']', open) - open - 2);
            count = std::max(1, static_cast<int>(parseIdList(ranges.substr(0, ranges.find('%'))).size()));
        }
        const std::string reason = util::trim(f[6]).substr(0, util::trim(f[6]).find_first_of(",( "));

        std::vector<QueueCounts*> sums = {&q, &q.by_partition[partition], &users[user], &accounts[account]};
        if (pending) sums.push_back(&q.reason_use[reason]);
        for (QueueCounts* c : sums) {
            if (running) {
                c->running++;
                c->cpus_running += cpus;
                c->gpus_running += gpus;
            } else {
                c->pending += count;
                c->cpus_pending += cpus * count;
                c->gpus_pending += gpus * count;
            }
        }
        if (!user.empty()) q.users.insert(user);
        if (pending) q.reasons[reason] += count;
    }
    auto ranked = [](std::map<std::string, UserUse>& m) {
        std::vector<UserUse> v;
        for (auto& [name, u] : m) {
            u.name = name;
            v.push_back(u);
        }
        std::sort(v.begin(), v.end(), [](const UserUse& a, const UserUse& b) {
            if (a.cpus_running != b.cpus_running) return a.cpus_running > b.cpus_running;
            if (a.gpus_running != b.gpus_running) return a.gpus_running > b.gpus_running;
            return a.pending > b.pending;
        });
        return v;
    };
    q.by_user = ranked(users);
    q.by_account = ranked(accounts);
    return q;
}

// ---------------------------------------------------------------- all of it

struct Cluster {
    Sinfo sinfo;
    std::vector<NodeType> types;
    Queue queue;
    std::string queue_error;   // the nodes can be there without the queue
    std::vector<DownReason> down;
    std::vector<Reservation> reservations;
    std::vector<std::string> ping;
    Storage storage;
};

// Read at each refresh of the cluster view: all cheap for slurmctld
inline Fetched<Cluster> fetchCluster() {
    Fetched<Cluster> r;
    const RunResult info = run({"sinfo", "-h", "-N", "-O", sinfoFormat()}, 20);
    if (!info.ok()) {
        r.error = info.error("sinfo");
        const RunResult ping = run({"scontrol", "ping"}, 10);
        r.data.ping = parsePing(ping.out);
        return r;
    }
    r.data.sinfo = parseSinfo(info.out);
    r.data.types = nodeTypes(r.data.sinfo.nodes);

    const RunResult queue = run({"squeue", "-h", "-t", "R,PD,CG", "-O", queueFormat()}, 20);
    if (queue.ok()) r.data.queue = parseQueue(queue.out);
    else r.data.queue_error = queue.error("squeue");

    const RunResult reasons = run({"sinfo", "-R", "-h", "-o", sinfoReasonFormat()}, 15);
    if (reasons.ok()) r.data.down = parseDownReasons(reasons.out);
    const RunResult resv = run({"scontrol", "show", "reservation"}, 15);
    if (resv.ok()) r.data.reservations = parseReservations(resv.out);
    const RunResult ping = run({"scontrol", "ping"}, 10);
    r.data.ping = parsePing(ping.out);
    r.data.storage = readStorage();
    return r;
}

// Read when the cluster view opens: the scheduler's statistics and the partitions' rules change slowly
struct ClusterExtra {
    Sdiag sdiag;
    bool has_sdiag = false;
    std::map<std::string, PartitionLimits> limits;
};

inline Fetched<ClusterExtra> fetchClusterExtra() {
    Fetched<ClusterExtra> r;
    const RunResult d = run({"sdiag"}, 15);
    if (d.ok()) {
        r.data.sdiag = parseSdiag(d.out);
        r.data.has_sdiag = r.data.sdiag.submitted >= 0;
    }
    const RunResult p = run({"scontrol", "-o", "show", "partition"}, 15);
    if (p.ok()) r.data.limits = parsePartitionLimits(p.out);
    return r;
}

// When squeue fails: is it the controller? ("" when it answers)
inline std::string controllerProblem() {
    const RunResult ping = run({"scontrol", "ping"}, 10);
    if (!ping.started) return "";
    const std::string state = controllerState(parsePing(ping.out));
    if (state.empty()) return ping.timed_out ? "the Slurm controller doesn't answer" : "";
    return state == "UP" ? "" : state;
}

}  // namespace api
