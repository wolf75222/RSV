#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pwd.h>
#include <unistd.h>

#include "../utils/format.hpp"
#include "run.hpp"

/* Slurm commands and the parsing of their output
- Parsing is done by plain functions on text (tested in tests/parse_test.cpp), the fetch* functions run the commands
- Nothing here throws: a field that doesn't parse is left empty or at its default
*/
namespace api {

using util::UNKNOWN;
using util::UNLIMITED;

template <typename T>
struct Fetched {
    T data{};
    std::string error;   // empty when it worked
};

inline std::string currentUser() {
    if (const char* user = std::getenv("USER"); user && *user) return user;
    if (const passwd* pw = getpwuid(getuid())) return pw->pw_name;
    return "";
}

// ---------------------------------------------------------------- states

// RUNNING, COMPLETING... (not PENDING, not finished)
inline bool isActive(const std::string& state) {
    static const std::set<std::string> active = {"RUNNING", "COMPLETING", "CONFIGURING", "SUSPENDED", "RESIZING",
                                                 "SIGNALING", "STAGE_OUT", "STOPPED"};
    return active.count(state) > 0;
}

inline bool isPending(const std::string& state) {
    return state == "PENDING" || state == "REQUEUED" || state == "REQUEUE_HOLD" || state == "REQUEUE_FED" ||
           state == "RESV_DEL_HOLD";
}

inline bool isFailure(const std::string& state) {
    return state == "FAILED" || state == "TIMEOUT" || state == "OUT_OF_MEMORY" || state == "NODE_FAIL" ||
           state == "BOOT_FAIL" || state == "DEADLINE" || state == "PREEMPTED";
}

// Two letters, like squeue's compact states
inline std::string shortState(const std::string& state) {
    static const std::map<std::string, std::string> codes = {
        {"RUNNING", "R"}, {"PENDING", "PD"}, {"COMPLETING", "CG"}, {"COMPLETED", "CD"}, {"FAILED", "F"},
        {"CANCELLED", "CA"}, {"TIMEOUT", "TO"}, {"OUT_OF_MEMORY", "OOM"}, {"NODE_FAIL", "NF"}, {"SUSPENDED", "S"},
        {"PREEMPTED", "PR"}, {"BOOT_FAIL", "BF"}, {"DEADLINE", "DL"}, {"REQUEUED", "RQ"}, {"CONFIGURING", "CF"},
        {"RESIZING", "RS"}, {"SIGNALING", "SI"}, {"STAGE_OUT", "SO"}, {"STOPPED", "ST"}, {"SPECIAL_EXIT", "SE"},
        {"REVOKED", "RV"}, {"REQUEUE_HOLD", "RH"}, {"REQUEUE_FED", "RF"}, {"RESV_DEL_HOLD", "RD"},
    };
    const auto it = codes.find(state);
    return it != codes.end() ? it->second : state.substr(0, 2);
}

// ---------------------------------------------------------------- small parsers

// "cpu=128,mem=1500G,node=2,gres/gpu=8" -> {cpu: 128, mem: 1536000 (MiB), node: 2, gres/gpu: 8}
inline std::map<std::string, double> parseTres(const std::string& tres) {
    std::map<std::string, double> out;
    for (const auto& item : util::split(tres, ',')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq);
        const std::string value = item.substr(eq + 1);
        out[key] = key == "mem" ? util::parseMiB(value) : util::toDouble(value);
    }
    return out;
}

// "0-15,72-87,144" -> 0..15, 72..87, 144
inline std::vector<int> parseIdList(const std::string& list) {
    std::vector<int> ids;
    for (const auto& part : util::split(list, ',')) {
        if (part.empty() || !std::isdigit(static_cast<unsigned char>(part[0]))) continue;
        const size_t dash = part.find('-');
        const long a = util::toLong(part.substr(0, dash), -1);
        const long b = dash == std::string::npos ? a : util::toLong(part.substr(dash + 1), -1);
        if (a < 0 || b < a || b - a > 65536 || ids.size() > 65536) continue;
        for (long i = a; i <= b; i++) ids.push_back(static_cast<int>(i));
    }
    return ids;
}

// Splits on `sep`, except inside brackets or parentheses: "a[1-2,4],b" -> "a[1-2,4]", "b"
inline std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '[' || c == '(') depth++;
        if ((c == ']' || c == ')') && depth > 0) depth--;
        if (c == sep && depth == 0) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/* Expands a Slurm host list, like `scontrol show hostnames` but without running it
- "romeo-a[031-032,040],romeo-c001" -> romeo-a031, romeo-a032, romeo-a040, romeo-c001
- Zero padding is kept, several bracket groups are combined ("r[1-2]n[01-02]"), at most 100 000 names
*/
inline std::vector<std::string> expandHostlist(const std::string& list) {
    std::vector<std::string> out;
    for (const auto& part : splitTop(util::trim(list), ',')) {
        const size_t open = part.find('[');
        const size_t close = part.find(']', open);
        if (open == std::string::npos || close == std::string::npos) {
            if (!part.empty() && part != "(null)") out.push_back(part);
            continue;
        }
        const std::string prefix = part.substr(0, open);
        const std::vector<std::string> tails = expandHostlist(part.substr(close + 1));
        for (const auto& range : util::split(part.substr(open + 1, close - open - 1), ',')) {
            const size_t dash = range.find('-');
            const std::string first = range.substr(0, dash);
            const long a = util::toLong(first, -1);
            const long b = dash == std::string::npos ? a : util::toLong(range.substr(dash + 1), -1);
            if (a < 0 || b < a) continue;
            for (long i = a; i <= b && out.size() < 100000; i++) {
                std::string num = std::to_string(i);
                if (num.size() < first.size()) num.insert(0, first.size() - num.size(), '0');
                if (tails.empty())
                    out.push_back(prefix + num);
                else
                    for (const auto& tail : tails) out.push_back(prefix + num + tail);
            }
        }
    }
    return out;
}

struct Gpus {
    int count = 0;
    std::string type;          // h100
    std::vector<int> indices;  // from (IDX:0-3), when Slurm prints them
};

// "gpu:h100:4(IDX:0-3)", "gpu:4", "gpu:h100:2(S:0-1)", "gpu:a100:1(IDX:0),gpu:v100:1(IDX:1)", "(null)"
inline Gpus parseGpus(const std::string& gres) {
    Gpus g;
    for (const auto& item : splitTop(gres, ',')) {
        if (!util::startsWith(item, "gpu")) continue;
        const size_t paren = item.find('(');
        const auto parts = util::split(item.substr(0, paren), ':');
        if (parts.size() >= 3 && g.type.empty()) g.type = parts[1];
        if (parts.size() >= 2) g.count += static_cast<int>(util::toLong(parts.back(), 0));
        const size_t idx = item.find("IDX:");
        if (idx != std::string::npos) {
            const size_t end = item.find(')', idx);
            for (int i : parseIdList(item.substr(idx + 4, end == std::string::npos ? end : end - idx - 4)))
                g.indices.push_back(i);
        }
    }
    return g;
}

/* "Key=Value Key2=Value2 ..." as printed by scontrol, added to `fields`
- A word without '=' belongs to the value before it: OS=Linux 5.14.0 #1 SMP, Reason=Not responding [slurm@...]
- The keys in `to_end` take the rest of the line, '=' and spaces included (names, paths, commands)
*/
inline void parseFields(const std::string& line, std::map<std::string, std::string>& fields) {
    static const std::set<std::string> to_end = {"JobName", "Command", "WorkDir", "StdErr", "StdIn", "StdOut",
                                                 "Comment", "AdminComment", "SystemComment"};
    auto is_key = [](const std::string& k) {
        if (k.empty() || !std::isalpha(static_cast<unsigned char>(k[0]))) return false;
        return std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '/' || c == ':'; });
    };

    std::string key;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') i++;
        if (i >= line.size()) break;
        size_t end = line.find(' ', i);
        if (end == std::string::npos) end = line.size();
        const std::string word = line.substr(i, end - i);
        const size_t eq = word.find('=');
        if (eq != std::string::npos && is_key(word.substr(0, eq))) {
            key = word.substr(0, eq);
            if (to_end.count(key)) {
                std::string rest = line.substr(i + eq + 1);
                while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r')) rest.pop_back();
                fields[key] = rest;
                return;
            }
            fields[key] = word.substr(eq + 1);
        } else if (!key.empty()) {
            fields[key] += " " + word;
        }
        i = end;
    }
}

// ---------------------------------------------------------------- jobs (squeue)

struct Job {
    std::string id;           // as squeue shows it: 812345, 812402_1, 812402_[3-40]
    std::string job_id;       // Slurm's own number (each array task has one), for scontrol
    std::string state;        // RUNNING, PENDING, COMPLETED...
    std::string name, partition, account, qos, reason, nodelist, dependency, stdout_path, workdir;
    int nodes = 0, cpus = 0, gpus = 0;
    double mem_mib = 0;       // all nodes
    long used = UNKNOWN, limit = UNKNOWN, left = UNKNOWN;   // seconds
    std::time_t start = 0;    // for a pending job: when Slurm expects it to start (0: no idea yet)
    std::time_t end = 0, submit = 0;

    bool active() const { return isActive(state); }
    bool pending() const { return isPending(state); }
    bool finished() const { return !active() && !pending(); }
};

// The fields asked to squeue -O, in this order. The name is last: it may contain '|'.
inline const std::vector<std::string>& squeueFields() {
    static const std::vector<std::string> fields = {
        "JobArrayID", "JobID", "State", "Partition", "Account", "QOS", "NumNodes", "NumCPUs", "tres-alloc",
        "TimeUsed", "TimeLimit", "TimeLeft", "StartTime", "EndTime", "SubmitTime", "Reason", "NodeList",
        "Dependency", "STDOUT", "WorkDir", "Name"};
    return fields;
}

inline std::string squeueFormat() {
    std::string format;
    for (const auto& f : squeueFields()) format += (format.empty() ? "" : ",") + f + ":|";
    return format;
}

inline std::vector<Job> parseSqueue(const std::string& out) {
    std::vector<Job> jobs;
    const size_t n = squeueFields().size();
    for (std::string line : util::lines(out)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (util::trim(line).empty()) continue;

        std::vector<std::string> f;
        size_t start = 0;
        while (f.size() + 1 < n) {
            const size_t bar = line.find('|', start);
            if (bar == std::string::npos) break;
            f.push_back(util::trim(line.substr(start, bar - start)));
            start = bar + 1;
        }
        if (f.size() + 1 < n) continue;   // not one of our lines (a warning...)
        std::string name = line.substr(start);
        if (!name.empty() && name.back() == '|') name.pop_back();
        f.push_back(name);

        Job j;
        j.id = f[0];
        j.job_id = f[1].empty() ? f[0] : f[1];
        j.state = f[2].substr(0, f[2].find(' '));
        j.partition = f[3];
        j.account = f[4];
        j.qos = f[5];
        j.nodes = static_cast<int>(util::toLong(f[6]));
        j.cpus = static_cast<int>(util::toLong(f[7]));
        const auto tres = parseTres(f[8]);
        if (tres.count("gres/gpu")) j.gpus = static_cast<int>(tres.at("gres/gpu"));
        if (tres.count("mem")) j.mem_mib = tres.at("mem");
        j.used = util::parseDuration(f[9]);
        j.limit = util::parseDuration(f[10]);
        j.left = util::parseDuration(f[11]);
        j.start = util::parseTime(f[12]);
        j.end = util::parseTime(f[13]);
        j.submit = util::parseTime(f[14]);
        j.reason = util::sanitize(f[15]);
        j.nodelist = f[16];
        j.dependency = f[17] == "(null)" ? "" : f[17];
        j.stdout_path = f[18];
        j.workdir = f[19];
        j.name = util::sanitize(f[20]);   // shown only: no escape sequence or invisible character reaches the screen
        if (!j.id.empty()) jobs.push_back(j);
    }
    return jobs;
}

inline Fetched<std::vector<Job>> fetchJobs(const std::string& user) {
    Fetched<std::vector<Job>> r;
    // -t all: finished jobs too, while Slurm still knows them (MinJobAge, 5 min on ROMEO)
    const RunResult res = run({"squeue", "-u", user, "-t", "all", "-h", "-O", squeueFormat()}, 15);
    if (!res.ok()) {
        r.error = res.error("squeue");
        return r;
    }
    r.data = parseSqueue(res.out);
    return r;
}

// ---------------------------------------------------------------- job details (scontrol -dd)

struct NodeAlloc {
    std::string nodes;               // as scontrol prints them: romeo-a[031-032]
    std::vector<std::string> hosts;  // each node
    std::vector<int> cpus;           // the CPU ids used on each of these nodes
    Gpus gpus;                       // the GPUs used on each node
    double mem_mib = 0;              // on each node
};

struct JobDetails {
    std::string job_id;
    std::map<std::string, std::string> fields;
    std::vector<NodeAlloc> alloc;
    std::string raw;                 // the record as scontrol printed it

    std::string get(const std::string& key) const {
        const auto it = fields.find(key);
        return it == fields.end() ? "" : it->second;
    }
};

/* scontrol show job -dd output
- Asking for an array (or its pending tasks) prints one record per task: the one of `job_id` is kept
- "  Nodes=romeo-a[031-032] CPU_IDs=0-15,72-87 Mem=768000 GRES=gpu:h100:4(IDX:0-3)": the CPUs and GPUs used
  on EACH node of the list (they aren't split between the nodes)
*/
inline JobDetails parseJobDetails(const std::string& text, const std::string& job_id) {
    std::vector<std::vector<std::string>> records;
    for (const auto& line : util::lines(text)) {
        if (util::startsWith(line, "JobId=")) records.emplace_back();
        if (!records.empty() && !util::trim(line).empty()) records.back().push_back(line);
    }

    JobDetails best;
    for (const auto& record : records) {
        JobDetails d;
        for (const auto& line : record) {
            d.raw += line + "\n";
            const std::string t = util::trim(line);
            if (util::startsWith(t, "Nodes=") && t.find("CPU_IDs=") != std::string::npos) {
                std::map<std::string, std::string> f;
                parseFields(t, f);
                NodeAlloc a;
                a.nodes = f["Nodes"];
                a.hosts = expandHostlist(a.nodes);
                a.cpus = parseIdList(f["CPU_IDs"]);
                a.gpus = parseGpus(f["GRES"]);
                a.mem_mib = util::toDouble(f["Mem"]);
                d.alloc.push_back(a);
            } else {
                parseFields(line, d.fields);
            }
        }
        d.job_id = d.get("JobId");
        if (best.raw.empty() || d.job_id == job_id) best = d;
        if (d.job_id == job_id) break;
    }
    return best;
}

inline Fetched<JobDetails> fetchDetails(const std::string& job_id) {
    Fetched<JobDetails> r;
    const RunResult res = run({"scontrol", "show", "job", "-dd", job_id}, 15);
    if (!res.ok()) {
        r.error = res.error("scontrol");
        return r;
    }
    r.data = parseJobDetails(res.out, job_id);
    if (r.data.raw.empty()) r.error = "scontrol: no record for job " + job_id;
    return r;
}

// ---------------------------------------------------------------- nodes (scontrol -o show node)

struct NodeInfo {
    std::string name, arch, state, features, reason;
    Gpus gpus;
    int cpus = 0, sockets = 1, cores = 0, threads = 1;   // cores per socket
    int cpu_alloc = 0, gpu_alloc = 0;                    // by every job on the node
    double mem_mib = 0, mem_alloc_mib = 0, load = -1;
};

inline std::vector<NodeInfo> parseNodes(const std::string& text) {
    std::vector<std::map<std::string, std::string>> records;
    for (const auto& line : util::lines(text)) {
        if (util::startsWith(util::trim(line), "NodeName=")) records.emplace_back();
        if (!records.empty()) parseFields(line, records.back());
    }

    std::vector<NodeInfo> nodes;
    for (auto& f : records) {
        NodeInfo n;
        n.name = f["NodeName"];
        n.arch = f["Arch"];
        n.state = f["State"];
        n.features = f["AvailableFeatures"] == "(null)" ? "" : f["AvailableFeatures"];
        n.reason = f["Reason"];
        n.cpus = static_cast<int>(util::toLong(f["CPUTot"]));
        n.sockets = std::max(1, static_cast<int>(util::toLong(f["Sockets"], 1)));
        n.cores = static_cast<int>(util::toLong(f["CoresPerSocket"]));
        n.threads = std::max(1, static_cast<int>(util::toLong(f["ThreadsPerCore"], 1)));
        n.cpu_alloc = static_cast<int>(util::toLong(f["CPUAlloc"]));
        n.gpus = parseGpus(f["Gres"]);
        const auto alloc = parseTres(f["AllocTRES"]);
        if (alloc.count("gres/gpu")) n.gpu_alloc = static_cast<int>(alloc.at("gres/gpu"));
        n.mem_mib = util::toDouble(f["RealMemory"]);
        n.mem_alloc_mib = util::toDouble(f["AllocMem"]);
        n.load = util::toDouble(f["CPULoad"], -1);
        if (!n.name.empty()) nodes.push_back(n);
    }
    return nodes;
}

inline Fetched<std::vector<NodeInfo>> fetchNodes(const std::string& hostlist) {
    Fetched<std::vector<NodeInfo>> r;
    const RunResult res = run({"scontrol", "-o", "show", "node", hostlist}, 15);
    if (!res.ok()) {
        r.error = res.error("scontrol");
        return r;
    }
    r.data = parseNodes(res.out);
    return r;
}

// ---------------------------------------------------------------- partitions (sinfo -N)

struct Partition {
    std::string name, avail, timelimit;
    bool is_default = false;
    int nodes = 0, idle = 0, mixed = 0, allocated = 0, unavailable = 0;
    long cpus_alloc = 0, cpus_idle = 0, cpus_total = 0;
    int gpus_total = 0, gpus_used = 0;
};

inline std::string sinfoFormat() {
    return "Partition:|,Available:|,Time:|,NodeList:|,StateCompact:|,CPUsState:|,Gres:|,GresUsed:|";
}

// One line per node and partition (sinfo -N), summed by partition: exact GPU counts, which grouped lines don't give
inline std::vector<Partition> parsePartitions(const std::string& out) {
    std::vector<Partition> parts;
    std::map<std::string, size_t> index;
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
        if (!index.count(name)) {
            index[name] = parts.size();
            Partition p;
            p.name = name;
            p.avail = util::trim(f[1]);
            p.timelimit = util::trim(f[2]);
            parts.push_back(p);
        }
        Partition& p = parts[index[name]];
        p.is_default = p.is_default || is_default;
        p.nodes++;

        // idle, mix, alloc, comp, drng, drain, down, maint, resv, plnd...; '*' means not responding
        const std::string state = util::trim(f[4]);
        if (state.find('*') != std::string::npos || util::startsWith(state, "down") || util::startsWith(state, "drain") ||
            util::startsWith(state, "fail") || util::startsWith(state, "maint") || util::startsWith(state, "inval") ||
            util::startsWith(state, "resv") || util::startsWith(state, "boot") || util::startsWith(state, "unk") ||
            util::startsWith(state, "pow") || util::startsWith(state, "drng"))
            p.unavailable++;
        else if (util::startsWith(state, "idle"))
            p.idle++;
        else if (util::startsWith(state, "mix"))
            p.mixed++;
        else
            p.allocated++;

        const auto cpus = util::split(f[5], '/');   // allocated/idle/other/total
        if (cpus.size() == 4) {
            p.cpus_alloc += util::toLong(cpus[0]);
            p.cpus_idle += util::toLong(cpus[1]);
            p.cpus_total += util::toLong(cpus[3]);
        }
        p.gpus_total += parseGpus(f[6]).count;
        p.gpus_used += parseGpus(f[7]).count;
    }
    return parts;
}

inline Fetched<std::vector<Partition>> fetchPartitions() {
    Fetched<std::vector<Partition>> r;
    const RunResult res = run({"sinfo", "-h", "-N", "-O", sinfoFormat()}, 20);
    if (!res.ok()) {
        r.error = res.error("sinfo");
        return r;
    }
    r.data = parsePartitions(res.out);
    return r;
}

// ---------------------------------------------------------------- history (sacct)

struct HistoryJob {
    std::string id, name, state, state_detail, partition, account, exit_code, nodelist;
    std::time_t submit = 0, start = 0, end = 0;
    long elapsed = UNKNOWN, limit = UNKNOWN;
    int nodes = 0, cpus = 0;
    double mem_mib = 0;        // allocated, on each node
    double max_rss_mib = 0;    // largest task (sacct can't tell more), over all steps
    long total_cpu = UNKNOWN;  // CPU time used, in seconds
    long cpu_time = UNKNOWN;   // CPU time allocated: elapsed × CPUs

    // 0..1 (or more), -1 when unknown
    double cpuEfficiency() const { return total_cpu >= 0 && cpu_time > 0 ? double(total_cpu) / cpu_time : -1; }
    double memEfficiency() const { return max_rss_mib > 0 && mem_mib > 0 ? max_rss_mib / mem_mib : -1; }
    double timeUsed() const { return elapsed >= 0 && limit > 0 ? double(elapsed) / limit : -1; }
};

inline const std::vector<std::string>& sacctFields() {
    static const std::vector<std::string> fields = {
        "JobID", "State", "Partition", "Account", "Submit", "Start", "End", "Elapsed", "Timelimit", "NNodes",
        "NCPUs", "ReqMem", "AllocTRES", "TotalCPU", "CPUTimeRAW", "MaxRSS", "ExitCode", "NodeList", "JobName"};
    return fields;
}

/* sacct -P output, one line per job and per step (812301, 812301.batch, 812301.extern, 812301.0...)
- MaxRSS is only known for the steps: the largest one is kept
- Newest first
*/
inline std::vector<HistoryJob> parseSacct(const std::string& out) {
    std::vector<HistoryJob> jobs;
    std::map<std::string, size_t> index;
    std::map<std::string, double> rss;
    const size_t n = sacctFields().size();

    for (const auto& line : util::lines(out)) {
        std::vector<std::string> f;
        size_t start = 0;
        while (f.size() + 1 < n) {
            const size_t bar = line.find('|', start);
            if (bar == std::string::npos) break;
            f.push_back(line.substr(start, bar - start));
            start = bar + 1;
        }
        if (f.size() + 1 < n) continue;
        f.push_back(line.substr(start));

        const std::string id = f[0];
        const size_t dot = id.find('.');
        if (dot != std::string::npos) {
            const std::string base = id.substr(0, dot);
            rss[base] = std::max(rss[base], util::parseMiB(f[15], 1.0 / 1024 / 1024));
            continue;
        }

        HistoryJob h;
        h.id = id;
        h.state_detail = f[1];
        h.state = f[1].substr(0, f[1].find(' '));   // CANCELLED by 4242
        h.partition = f[2];
        h.account = f[3];
        h.submit = util::parseTime(f[4]);
        h.start = util::parseTime(f[5]);
        h.end = util::parseTime(f[6]);
        h.elapsed = util::parseDuration(f[7]);
        h.limit = util::parseDuration(f[8]);
        h.nodes = static_cast<int>(util::toLong(f[9]));
        h.cpus = static_cast<int>(util::toLong(f[10]));
        const auto tres = parseTres(f[12]);
        if (tres.count("mem") && h.nodes > 0)
            h.mem_mib = tres.at("mem") / h.nodes;
        else if (!f[11].empty()) {
            // Older Slurm: 4000Mc (per CPU) or 4000Mn (per node)
            const double v = util::parseMiB(f[11]);
            h.mem_mib = f[11].back() == 'c' && h.nodes > 0 ? v * h.cpus / h.nodes : v;
        }
        h.total_cpu = util::parseDuration(f[13]);
        h.cpu_time = f[14].empty() ? UNKNOWN : util::toLong(f[14], UNKNOWN);
        h.max_rss_mib = util::parseMiB(f[15], 1.0 / 1024 / 1024);
        h.exit_code = f[16];
        h.nodelist = f[17];
        h.name = util::sanitize(f[18]);
        index[id] = jobs.size();
        jobs.push_back(h);
    }
    for (auto& [id, value] : rss)
        if (index.count(id)) jobs[index[id]].max_rss_mib = std::max(jobs[index[id]].max_rss_mib, value);

    std::stable_sort(jobs.begin(), jobs.end(), [](const HistoryJob& a, const HistoryJob& b) {
        const std::time_t ta = a.end ? a.end : a.start ? a.start : a.submit;
        const std::time_t tb = b.end ? b.end : b.start ? b.start : b.submit;
        return ta > tb;
    });
    return jobs;
}

inline Fetched<std::vector<HistoryJob>> fetchHistory(const std::string& user, int days) {
    Fetched<std::vector<HistoryJob>> r;
    std::string format;
    for (const auto& f : sacctFields()) format += (format.empty() ? "" : ",") + f;
    const RunResult res = run({"sacct", "-u", user, "-S", "now-" + std::to_string(days) + "days", "-n", "-P",
                               "--format=" + format}, 30);
    if (!res.ok()) {
        r.error = res.error("sacct");
        return r;
    }
    r.data = parseSacct(res.out);
    return r;
}

// ---------------------------------------------------------------- limits (sacctmgr)

struct Assoc {
    std::string account, partition, qos;
    std::map<std::string, double> grp_tres, max_tres;   // cpu, gres/gpu, node, mem (MiB)
    long grp_jobs = -1, max_jobs = -1, max_submit = -1;  // -1: no limit
    long max_wall = UNKNOWN;
};

inline std::vector<Assoc> parseAssocs(const std::string& out) {
    std::vector<Assoc> assocs;
    auto limit = [](const std::string& s) { return util::trim(s).empty() ? -1L : util::toLong(s, -1); };
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, '|');
        if (f.size() < 9 || util::trim(f[0]).empty()) continue;
        Assoc a;
        a.account = f[0];
        a.partition = f[1];
        a.qos = f[2];
        a.grp_tres = parseTres(f[3]);
        a.max_tres = parseTres(f[4]);
        a.grp_jobs = limit(f[5]);
        a.max_jobs = limit(f[6]);
        a.max_submit = limit(f[7]);
        a.max_wall = util::trim(f[8]).empty() ? UNKNOWN : util::parseDuration(f[8]);
        assocs.push_back(a);
    }
    return assocs;
}

inline Fetched<std::vector<Assoc>> fetchAssocs(const std::string& user) {
    Fetched<std::vector<Assoc>> r;
    const RunResult res = run({"sacctmgr", "-n", "-P", "show", "assoc", "where", "user=" + user,
                               "format=Account,Partition,QOS,GrpTRES,MaxTRES,GrpJobs,MaxJobs,MaxSubmit,MaxWall"}, 15);
    if (!res.ok()) {
        r.error = res.error("sacctmgr");
        return r;
    }
    r.data = parseAssocs(res.out);
    return r;
}

// ---------------------------------------------------------------- cancel (scancel)

// "" when it worked, else what scancel said
inline std::string cancelJob(const std::string& id) {
    const RunResult res = run({"scancel", id}, 15);
    if (res.ok() && res.err.find("error") == std::string::npos) return "";
    std::string e = res.error("scancel");
    if (e.empty()) e = util::trim(res.err);
    return e.empty() ? "scancel failed" : e;
}

// ---------------------------------------------------------------- log files

/* The path of a job's output file, with Slurm's patterns replaced
- %A array master id, %a array task, %j job id, %J job id(.step), %u user, %x job name, %N first node, %% '%'
- A number after '%' pads with zeros: %4a -> 0007
- A relative path is in the job's working directory
*/
inline std::string expandSlurmPath(const std::string& pattern, const JobDetails& d) {
    if (pattern.empty() || pattern == "(null)") return "";
    const std::string job = d.get("JobId");
    const std::string array_job = d.get("ArrayJobId");
    const std::string task = d.get("ArrayTaskId");
    std::string user = d.get("UserId");
    user = user.substr(0, user.find('('));
    std::string node = d.get("BatchHost");
    if (node.empty()) {
        const auto hosts = expandHostlist(d.get("NodeList"));
        if (!hosts.empty()) node = hosts.front();
    }

    std::string out;
    for (size_t i = 0; i < pattern.size(); i++) {
        if (pattern[i] != '%' || i + 1 >= pattern.size()) {
            out += pattern[i];
            continue;
        }
        size_t k = i + 1;
        int pad = 0;
        while (k < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[k]))) pad = pad * 10 + (pattern[k++] - '0');
        if (k >= pattern.size()) {
            out += pattern.substr(i);
            break;
        }
        std::string value;
        switch (pattern[k]) {
            case '%': value = "%"; break;
            case 'A': value = array_job.empty() ? job : array_job; break;
            case 'a': value = task.empty() || task == "N/A" ? "4294967294" : task; break;
            case 'j': value = job; break;
            case 'J': value = job; break;
            case 'u': value = user; break;
            case 'x': value = d.get("JobName"); break;
            case 'N': value = node; break;
            case 'n': case 't': value = "0"; break;
            case 's': value = "batch"; break;
            default: value = pattern.substr(i, k - i + 1); break;
        }
        if (pad > 0 && value.size() < static_cast<size_t>(std::min(pad, 10)) &&
            std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
            value.insert(0, std::min(pad, 10) - value.size(), '0');
        out += value;
        i = k;
    }
    if (!out.empty() && out[0] != '/' && !d.get("WorkDir").empty()) out = d.get("WorkDir") + "/" + out;
    return out;
}

}  // namespace api
