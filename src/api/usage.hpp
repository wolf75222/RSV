#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <grp.h>
#include <unistd.h>

#include "slurmjobs.hpp"

/* What your jobs and accounts use, and what Slurm would do with a new job
- sstat (running jobs, live), sprio (pending jobs: priority and rank), sshare (fairshare), mmlsquota (disk quotas),
  sbatch --test-only (when a job would start: nothing is submitted), nvidia-smi run inside a job with srun --overlap
*/
namespace api {

// ---------------------------------------------------------------- running jobs, live (sstat)

struct LiveUse {
    double cpu_seconds = 0;   // CPU time used by the running steps
    double mem_mib = 0;       // memory they use now, all tasks
    double max_task_mib = 0;  // the largest task's peak
    int steps = 0;
    double at = 0;            // when sstat answered, in seconds since the epoch
    double busy = -1;         // share of the job's CPUs busy since the reading before (-1: no reading before)
};

/* What the CPUs did between two readings of the same job: what it does now, not since it started
- sstat gives the CPU time in whole seconds: readings less than 5 s apart (r pressed twice) keep the one before
  as the base, and what it said
- Less CPU time than before (a step ended): no figure until the next reading
*/
inline void follow(LiveUse& now, const LiveUse& before, int cpus) {
    if (now.at - before.at < 5) {
        now.at = before.at;
        now.cpu_seconds = before.cpu_seconds;
        now.busy = before.busy;
        return;
    }
    const double cpu = now.cpu_seconds - before.cpu_seconds;
    now.busy = cpu >= 0 && cpus > 0 ? std::min(1.0, cpu / ((now.at - before.at) * cpus)) : -1;
}

// "cpu=01:02:03,energy=0,fs/disk=1234,mem=1.5G,pages=0,vmem=3G": the CPU time and the memory
inline void parseUsageTres(const std::string& tres, double& cpu_seconds, double& mem_mib) {
    for (const auto& item : util::split(tres, ',')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
        if (key == "cpu") cpu_seconds += std::max(0L, util::parseDuration(value));
        else if (key == "mem") mem_mib += util::parseMiB(value, 1.0 / 1024 / 1024);
    }
}

inline std::string sstatFormat() { return "JobID,NTasks,MaxRSS,TRESUsageInTot"; }

inline LiveUse parseSstat(const std::string& out) {
    LiveUse u;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, '|');
        if (f.size() < 4 || f[0].empty()) continue;
        u.steps++;
        u.max_task_mib = std::max(u.max_task_mib, util::parseMiB(f[2], 1.0 / 1024 / 1024));
        parseUsageTres(f[3], u.cpu_seconds, u.mem_mib);
    }
    return u;
}

inline Fetched<LiveUse> fetchLiveUse(const std::string& job_id) {
    Fetched<LiveUse> r;
    const RunResult res = run({"sstat", "-a", "-n", "-P", "--format=" + sstatFormat(), "-j", job_id}, 15);
    if (!res.ok()) {
        r.error = res.error("sstat");
        return r;
    }
    r.data = parseSstat(res.out);
    r.data.at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (r.data.steps == 0) r.error = "no step running";
    return r;
}

/* Each running step of a job, for the live view (m): what sstat says of it now
- TRESUsageInTot fs/disk: the bytes its tasks read; TRESUsageOutTot fs/disk: the bytes they wrote
- MaxRSSNode: the node of its largest task
*/
struct StepUse {
    std::string id;           // 812345.0, 812345.batch
    int tasks = 0;
    double cpu_seconds = 0, mem_mib = 0, max_rss_mib = 0;
    double read_bytes = 0, write_bytes = 0;
    std::string max_rss_node;
};

inline std::string stepsFormat() { return "JobID,NTasks,MaxRSS,MaxRSSNode,TRESUsageInTot,TRESUsageOutTot"; }

inline std::vector<StepUse> parseSteps(const std::string& out) {
    std::vector<StepUse> r;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, '|');
        if (f.size() < 6 || f[0].empty()) continue;
        StepUse s;
        s.id = util::trim(f[0]);
        s.tasks = static_cast<int>(util::toLong(f[1]));
        s.max_rss_mib = util::parseMiB(f[2], 1.0 / 1024 / 1024);
        s.max_rss_node = util::trim(f[3]);
        parseUsageTres(f[4], s.cpu_seconds, s.mem_mib);
        for (const auto& [field, sink] : {std::pair<const std::string*, double*>{&f[4], &s.read_bytes}, {&f[5], &s.write_bytes}})
            for (const auto& item : util::split(*field, ','))
                if (util::startsWith(item, "fs/disk=")) *sink = util::toDouble(item.substr(8));
        r.push_back(s);
    }
    return r;
}

inline Fetched<std::vector<StepUse>> fetchSteps(const std::string& job_id) {
    Fetched<std::vector<StepUse>> r;
    const RunResult res = run({"sstat", "-a", "-n", "-P", "--format=" + stepsFormat(), "-j", job_id}, 15);
    if (!res.ok()) {
        r.error = res.error("sstat");
        return r;
    }
    r.data = parseSteps(res.out);
    if (r.data.empty()) r.error = "no step running";
    return r;
}

// ---------------------------------------------------------------- pending jobs: priority and rank (sprio)

struct Priority {
    std::string job, user, partition;
    double priority = 0, age = 0, fairshare = 0, jobsize = 0, partition_factor = 0, qos = 0, site = 0;
};

inline std::string sprioFormat() { return "%i|%u|%r|%Y|%A|%F|%J|%P|%Q|%S"; }

inline std::vector<Priority> parseSprio(const std::string& out) {
    std::vector<Priority> r;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, '|');
        if (f.size() < 10) continue;
        Priority p;
        p.job = util::trim(f[0]);
        p.user = util::trim(f[1]);
        p.partition = util::trim(f[2]);
        p.priority = util::toDouble(util::trim(f[3]));
        p.age = util::toDouble(util::trim(f[4]));
        p.fairshare = util::toDouble(util::trim(f[5]));
        p.jobsize = util::toDouble(util::trim(f[6]));
        p.partition_factor = util::toDouble(util::trim(f[7]));
        p.qos = util::toDouble(util::trim(f[8]));
        p.site = util::toDouble(util::trim(f[9]));
        if (!p.job.empty()) r.push_back(p);
    }
    return r;
}

struct Rank {
    Priority mine;
    int rank = 0, of = 0;   // 1 = next to start in its partition
};

// The job's place among the pending jobs of the same partition, by priority
inline Rank rankOf(const std::vector<Priority>& all, const std::string& job_id) {
    Rank r;
    for (const auto& p : all)
        if (p.job == job_id) {
            r.mine = p;
            break;
        }
    if (r.mine.job.empty()) return r;
    for (const auto& p : all) {
        if (p.partition != r.mine.partition) continue;
        r.of++;
        if (p.priority > r.mine.priority) r.rank++;
    }
    r.rank++;
    return r;
}

inline Fetched<std::vector<Priority>> fetchPriorities() {
    Fetched<std::vector<Priority>> r;
    const RunResult res = run({"sprio", "-h", "-o", sprioFormat()}, 15);
    if (!res.ok()) {
        r.error = res.error("sprio");
        return r;
    }
    r.data = parseSprio(res.out);
    return r;
}

// ---------------------------------------------------------------- fairshare (sshare)

struct Share {
    std::string account;
    double norm_shares = -1, raw_usage = -1, effective_usage = -1, fairshare = -1;
};

inline std::vector<Share> parseSshare(const std::string& out) {
    std::vector<Share> r;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, '|');
        if (f.size() < 6 || util::trim(f[0]).empty()) continue;
        Share s;
        s.account = util::trim(f[0]);
        s.norm_shares = util::toDouble(f[2], -1);
        s.raw_usage = util::toDouble(f[3], -1);
        s.effective_usage = util::toDouble(f[4], -1);
        s.fairshare = util::toDouble(f[5], -1);
        r.push_back(s);
    }
    return r;
}

inline Fetched<std::vector<Share>> fetchShares(const std::string& user) {
    Fetched<std::vector<Share>> r;
    const RunResult res = run({"sshare", "-U", "-u", user, "-n", "-P", "-o", "Account,User,NormShares,RawUsage,EffectvUsage,FairShare"}, 15);
    if (!res.ok()) {
        r.error = res.error("sshare");
        return r;
    }
    r.data = parseSshare(res.out);
    return r;
}

// ---------------------------------------------------------------- disk quotas (mmlsquota, GPFS)

struct DiskQuota {
    std::string fileset, type, name;   // home, USR, demo / projet, GRP, r123456
    double used_kib = 0, quota_kib = 0, limit_kib = 0;
    double files = 0, files_quota = 0, files_limit = 0;
    std::string grace, files_grace;    // "none", or the time left once over the quota
};

/* mmlsquota -Y: one HEADER line naming the columns, then one line per fileset
- mmlsquota:user:HEADER:...:blockUsage:blockQuota:blockLimit:...:filesUsage:...:filesetname:
- Block values are in KiB
*/
inline std::vector<DiskQuota> parseMmlsquota(const std::string& out) {
    std::vector<DiskQuota> r;
    std::map<std::string, size_t> col;
    for (const auto& line : util::lines(out)) {
        const auto f = util::split(line, ':');
        if (f.size() > 2 && f[2] == "HEADER") {
            col.clear();
            for (size_t i = 0; i < f.size(); i++) col[f[i]] = i;
            continue;
        }
        auto get = [&](const char* name) { return col.count(name) && col[name] < f.size() ? f[col[name]] : std::string(); };
        if (col.empty() || get("filesetname").empty()) continue;
        DiskQuota q;
        q.fileset = get("filesetname");
        q.type = get("quotaType");
        q.name = get("name");
        q.used_kib = util::toDouble(get("blockUsage"));
        q.quota_kib = util::toDouble(get("blockQuota"));
        q.limit_kib = util::toDouble(get("blockLimit"));
        q.files = util::toDouble(get("filesUsage"));
        q.files_quota = util::toDouble(get("filesQuota"));
        q.files_limit = util::toDouble(get("filesLimit"));
        q.grace = get("blockGrace");
        q.files_grace = get("filesGrace");
        r.push_back(q);
    }
    return r;
}

// The mmlsquota in PATH (a fake one for the tests), else where GPFS installs it
inline std::string mmlsquotaPath() {
    if (const char* path = std::getenv("PATH"))
        for (const auto& dir : util::split(path, ':'))
            if (!dir.empty() && access((dir + "/mmlsquota").c_str(), X_OK) == 0) return dir + "/mmlsquota";
    return "/usr/lpp/mmfs/bin/mmlsquota";
}

// The user's quotas, and those of the user's project groups (r123456 on ROMEO): the filesets that have one
inline Fetched<std::vector<DiskQuota>> fetchDiskQuotas(const std::string& user) {
    Fetched<std::vector<DiskQuota>> r;
    const std::string bin = mmlsquotaPath();
    const RunResult mine = run({bin, "-Y", "-u", user, "gpfs"}, 10);
    if (!mine.ok()) {
        r.error = mine.error("mmlsquota");
        return r;
    }
    for (auto& q : parseMmlsquota(mine.out))
        if (q.quota_kib > 0 || q.files_quota > 0) r.data.push_back(q);

    std::vector<gid_t> gids(static_cast<size_t>(std::max(0, getgroups(0, nullptr))));
    if (!gids.empty()) gids.resize(static_cast<size_t>(std::max(0, getgroups(static_cast<int>(gids.size()), gids.data()))));
    for (gid_t g : gids) {
        const group* gr = getgrgid(g);
        if (!gr) continue;
        const std::string name = gr->gr_name;
        if (name.size() < 3 || name[0] != 'r' ||
            !std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        const RunResult res = run({bin, "-Y", "-g", name, "gpfs"}, 10);
        if (!res.ok()) continue;
        for (auto& q : parseMmlsquota(res.out))
            if (q.quota_kib > 0 || q.files_quota > 0) r.data.push_back(q);
    }
    return r;
}

// ---------------------------------------------------------------- when would a job start (sbatch --test-only)

struct TestRun {
    bool ok = false;
    std::time_t start = 0;
    std::string nodes, partition, message;   // message: what sbatch said when it can't
    long cpus = 0;
};

// "sbatch: Job 781504 to start at 2026-10-24T00:31:35 using 2 processors on nodes romeo-a[049-050] in partition short"
inline TestRun parseTestOnly(const std::string& out) {
    TestRun t;
    for (const auto& line : util::lines(out)) {
        const size_t at = line.find(" to start at ");
        if (at != std::string::npos) {
            t.ok = true;
            t.start = util::parseTime(line.substr(at + 13, 19));
            const size_t using_ = line.find(" using ");
            if (using_ != std::string::npos) t.cpus = util::toLong(line.substr(using_ + 7));
            const size_t on = line.find(" on nodes ");
            const size_t in = line.find(" in partition ");
            if (on != std::string::npos && in != std::string::npos && in > on) t.nodes = line.substr(on + 10, in - on - 10);
            if (in != std::string::npos) t.partition = util::trim(line.substr(in + 14));
            continue;
        }
        std::string l = util::trim(line);
        if (util::startsWith(l, "sbatch: error: ")) l = l.substr(15);
        if (!l.empty() && t.message.find(l) == std::string::npos) t.message += (t.message.empty() ? "" : " · ") + l;
    }
    if (t.ok) t.message.clear();
    return t;
}

struct TestRequest {
    std::string account, partition, constraint, mem, time;
    int nodes = 1, cores = 1, gpus = 0;
};

inline std::vector<std::string> testOnlyArgs(const TestRequest& q) {
    std::vector<std::string> a = {"sbatch", "--test-only"};
    if (!q.account.empty()) a.push_back("--account=" + q.account);
    if (!q.partition.empty()) a.push_back("--partition=" + q.partition);
    if (!q.constraint.empty()) a.push_back("--constraint=" + q.constraint);
    a.push_back("--nodes=" + std::to_string(std::max(1, q.nodes)));
    a.push_back("--ntasks-per-node=" + std::to_string(std::max(1, q.cores)));
    if (q.gpus > 0) a.push_back("--gpus-per-node=" + std::to_string(q.gpus));
    if (!q.mem.empty()) a.push_back("--mem=" + q.mem);
    if (!q.time.empty()) a.push_back("--time=" + q.time);
    a.push_back("--wrap=true");
    return a;
}

inline TestRun testOnly(const TestRequest& q) {
    const RunResult res = run(testOnlyArgs(q), 20);
    TestRun t = parseTestOnly(res.out + "\n" + res.err);
    if (!t.ok && t.message.empty()) t.message = res.error("sbatch");
    return t;
}

// ---------------------------------------------------------------- GPUs of a running job (nvidia-smi through srun)

struct GpuUse {
    std::string node, name;
    int index = 0;
    double util = -1, mem_used_mib = 0, mem_total_mib = 0;
    double temp_c = -1, power_w = -1, power_limit_w = -1;   // only asked by the live view (-1: not given)
    // The live view's longer query (-1 or empty: not given)
    std::string uuid, pstate;
    double sm_mhz = -1, sm_max_mhz = -1, mem_mhz = -1, mem_max_mhz = -1;
    unsigned long long reasons = 0;   // clocks_event_reasons.active: why its clocks are lowered (see clockReasons)
    double ecc_uncorrected = -1, ecc_corrected = -1;
};

/* What NVML's clock event reasons mean, the ones that slow a GPU down
- 0x1 (idle) and 0x2 (the clocks an application asked for) don't: they are left out
*/
inline std::vector<std::string> clockReasons(unsigned long long mask) {
    std::vector<std::string> r;
    if (mask & 0x4) r.push_back("power cap");
    if (mask & 0x8) r.push_back("hardware slowdown");
    if (mask & 0x10) r.push_back("sync boost");
    if (mask & 0x20) r.push_back("too hot");
    if (mask & 0x40) r.push_back("too hot (hardware)");
    if (mask & 0x80) r.push_back("power brake");
    return r;
}

/* "0: 1, NVIDIA GH200 120GB, 97, 70123, 97871" (srun -l: the task number, one task per node, in the node order)
- The live view asks for more: temperature, power and its limit, then its uuid, clocks (SM and memory, now and at most),
  why they are lowered (a hex mask), ECC errors (uncorrected, corrected) and its performance state; "[N/A]" when unknown
*/
inline bool parseNvidiaSmiLine(const std::string& line, const std::vector<std::string>& hosts, GpuUse& g) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    const long task = util::toLong(util::trim(line.substr(0, colon)), -1);
    const auto f = util::split(line.substr(colon + 1), ',');
    if (task < 0 || f.size() < 5) return false;
    g = GpuUse{};
    g.node = static_cast<size_t>(task) < hosts.size() ? hosts[static_cast<size_t>(task)] : "task " + std::to_string(task);
    g.index = static_cast<int>(util::toLong(util::trim(f[0])));
    g.name = util::trim(f[1]);
    g.util = util::toDouble(util::trim(f[2]), -1);
    g.mem_used_mib = util::toDouble(util::trim(f[3]));
    g.mem_total_mib = util::toDouble(util::trim(f[4]));
    if (f.size() >= 8) {
        g.temp_c = util::toDouble(util::trim(f[5]), -1);
        g.power_w = util::toDouble(util::trim(f[6]), -1);
        g.power_limit_w = util::toDouble(util::trim(f[7]), -1);
    }
    if (f.size() >= 17) {
        g.uuid = util::trim(f[8]);
        g.sm_mhz = util::toDouble(util::trim(f[9]), -1);
        g.sm_max_mhz = util::toDouble(util::trim(f[10]), -1);
        g.mem_mhz = util::toDouble(util::trim(f[11]), -1);
        g.mem_max_mhz = util::toDouble(util::trim(f[12]), -1);
        g.reasons = std::strtoull(util::trim(f[13]).c_str(), nullptr, 16);
        g.ecc_uncorrected = util::toDouble(util::trim(f[14]), -1);
        g.ecc_corrected = util::toDouble(util::trim(f[15]), -1);
        g.pstate = util::trim(f[16]);
    }
    return true;
}

inline std::vector<GpuUse> parseNvidiaSmi(const std::string& out, const std::vector<std::string>& hosts) {
    std::vector<GpuUse> r;
    GpuUse g;
    for (const auto& line : util::lines(out))
        if (parseNvidiaSmiLine(line, hosts, g)) r.push_back(g);
    return r;
}

// The name of the live view's own steps (rsv-live, rsv-live-gpm...): they are left out of the job's steps
inline std::string liveStepName() { return "rsv-live"; }

// The ids of the job's running steps whose name starts with `prefix` (squeue -s): the live view's own
// What each running step of a job runs, by its id (squeue -s: its name, the program srun started unless named)
inline std::map<std::string, std::string> stepNames(const std::string& job_id) {
    std::map<std::string, std::string> r;
    const RunResult res = run({"squeue", "-h", "-s", "-j", job_id, "-o", "%i|%j"}, 10);
    for (const auto& line : util::lines(res.out)) {
        const size_t bar = line.find('|');
        if (bar != std::string::npos) r[util::trim(line.substr(0, bar))] = util::sanitize(util::trim(line.substr(bar + 1)));
    }
    return r;
}

inline std::set<std::string> stepsNamed(const std::string& job_id, const std::string& prefix) {
    std::set<std::string> r;
    for (const auto& [id, name] : stepNames(job_id))
        if (util::startsWith(name, prefix)) r.insert(id);
    return r;
}

/* The same step, but it stays: nvidia-smi --loop-ms answers every `interval_ms` until it is stopped (live monitor)
- One step for as long as the monitor is open, instead of one every few seconds: sacct isn't filled with them
*/
inline std::vector<std::string> liveStep(const std::string& job_id, size_t nodes, int gpus_per_node, const std::string& name);

inline std::vector<std::string> gpuStreamArgs(const std::string& job_id, size_t nodes, int gpus_per_node, int interval_ms) {
    auto args = liveStep(job_id, nodes, gpus_per_node, liveStepName());
    for (const char* a : {"nvidia-smi",
                          "--query-gpu=index,name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,power.limit,"
                          "uuid,clocks.sm,clocks.max.sm,clocks.mem,clocks.max.mem,clocks_event_reasons.active,"
                          "ecc.errors.uncorrected.volatile.total,ecc.errors.corrected.volatile.total,pstate",
                          "--format=csv,noheader,nounits"})
        args.push_back(a);
    args.push_back("--loop-ms=" + std::to_string(interval_ms));
    return args;
}

// The start of every step of the live view: one task on each node of the job, its output lines labelled
inline std::vector<std::string> liveStep(const std::string& job_id, size_t nodes, int gpus_per_node, const std::string& name) {
    std::vector<std::string> args = {"srun", "--jobid=" + job_id, "--overlap", "--nodes=" + std::to_string(nodes),
                                     "--ntasks-per-node=1", "-l", "--quiet", "--job-name=" + name};
    if (gpus_per_node > 0) args.push_back("--gres=gpu:" + std::to_string(gpus_per_node));
    return args;
}

// ---------------------------------------------------------------- what the GPUs do inside (GPM, through nvidia-smi dmon)

/* Hopper's GPU performance metrics, what DCGM would give: dmon prints them every `interval_s` seconds
- 2 SM activity, 3 SM occupancy, 5 tensor activity, 10 DRAM activity, 12 FP32 activity (%), 20 21 PCIe TX RX,
  60 61 NVLink RX TX (MiB/s)
- The first two readings are "-": GPM needs two samples
- dmon starts threads: a job that already uses all the processes it may (8192 on ROMEO) can't run it
*/
inline std::vector<std::string> gpmStreamArgs(const std::string& job_id, size_t nodes, int gpus_per_node, int interval_s) {
    auto args = liveStep(job_id, nodes, gpus_per_node, liveStepName() + "-gpm");
    for (const auto& a : {std::string("nvidia-smi"), std::string("dmon"), std::string("--gpm-metrics"),
                                std::string("2,3,5,10,12,20,21,60,61"), std::string("-d"), std::to_string(interval_s)})
        args.push_back(a);
    return args;
}

struct GpmSample {
    std::string node;
    int index = -1;
    std::map<std::string, double> values;   // by dmon's column name: smutil, smocc, mmaact, dram, fp32, pcitx...
};

/* dmon's lines: per task (srun -l), a header naming the columns ("# gpu pwr gtemp ... smutil smocc ...") and rows
- A column without a value ("-") is left out of the sample
*/
struct DmonParser {
    std::map<long, std::vector<std::string>> columns;   // by task

    bool parse(const std::string& line, const std::vector<std::string>& hosts, GpmSample& out) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) return false;
        const long task = util::toLong(util::trim(line.substr(0, colon)), -1);
        if (task < 0) return false;
        std::string rest = util::trim(line.substr(colon + 1));
        std::vector<std::string> words;
        for (const auto& w : util::split(rest, ' '))
            if (!w.empty()) words.push_back(w);
        if (words.empty()) return false;
        if (words[0] == "#") {
            if (words.size() > 2 && words[1] == "gpu") columns[task] = std::vector<std::string>(words.begin() + 1, words.end());
            return false;
        }
        const auto it = columns.find(task);
        if (it == columns.end() || words.size() != it->second.size()) return false;
        out = GpmSample{};
        out.node = static_cast<size_t>(task) < hosts.size() ? hosts[static_cast<size_t>(task)] : "task " + std::to_string(task);
        out.index = static_cast<int>(util::toLong(words[0], -1));
        for (size_t i = 1; i < words.size(); i++)
            if (words[i] != "-") out.values[it->second[i]] = util::toDouble(words[i], -1);
        return out.index >= 0;
    }
};

// ---------------------------------------------------------------- who uses each GPU

struct GpuApp {
    std::string node, uuid, name;
    long pid = -1;
    double mem_mib = 0;
};

inline std::vector<std::string> appsStreamArgs(const std::string& job_id, size_t nodes, int gpus_per_node, int interval_ms) {
    auto args = liveStep(job_id, nodes, gpus_per_node, liveStepName() + "-apps");
    for (const auto& a : {std::string("nvidia-smi"), std::string("--query-compute-apps=gpu_uuid,pid,process_name,used_memory"),
                                std::string("--format=csv,noheader,nounits"), "--loop-ms=" + std::to_string(interval_ms)})
        args.push_back(a);
    return args;
}

// "0: GPU-101ef618-..., 3187803, ray::RayWorkerProc.run, 85408" (a name with commas keeps them)
inline bool parseGpuApp(const std::string& line, const std::vector<std::string>& hosts, GpuApp& a) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    const long task = util::toLong(util::trim(line.substr(0, colon)), -1);
    const auto f = util::split(line.substr(colon + 1), ',');
    if (task < 0 || f.size() < 4) return false;
    a = GpuApp{};
    a.node = static_cast<size_t>(task) < hosts.size() ? hosts[static_cast<size_t>(task)] : "task " + std::to_string(task);
    a.uuid = util::trim(f[0]);
    a.pid = util::toLong(util::trim(f[1]), -1);
    for (size_t i = 2; i + 1 < f.size(); i++) a.name += (i > 2 ? "," : "") + f[i];
    a.name = util::trim(a.name);
    a.mem_mib = util::toDouble(util::trim(f.back()));
    return a.pid > 0 && util::startsWith(a.uuid, "GPU-");
}

// ---------------------------------------------------------------- which step and rank each process belongs to

struct StepPid {
    std::string step;     // 0, batch, extern
    long rank = -1;       // its global task id when it is one of the step's tasks (-1: a child of one)
};

/* scontrol listpids on each node: "PID JOBID STEPID LOCALID GLOBALID", "-" for a task's children
- Keyed by node then pid: the pids of compute-apps are the node's own
*/
inline std::map<std::pair<std::string, long>, StepPid> parseListpids(const std::string& out, const std::vector<std::string>& hosts) {
    std::map<std::pair<std::string, long>, StepPid> r;
    for (const auto& line : util::lines(out)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const long task = util::toLong(util::trim(line.substr(0, colon)), -1);
        std::vector<std::string> w;
        for (const auto& x : util::split(line.substr(colon + 1), ' '))
            if (!x.empty()) w.push_back(util::trim(x));
        if (task < 0 || w.size() < 5) continue;
        const long pid = util::toLong(w[0], -1);
        if (pid <= 0) continue;
        const std::string node = static_cast<size_t>(task) < hosts.size() ? hosts[static_cast<size_t>(task)] : "task " + std::to_string(task);
        r[{node, pid}] = {w[2], w[4] == "-" ? -1 : util::toLong(w[4], -1)};
    }
    return r;
}

// Once (when the GPU view opens, and on r): the job's pids on each node, and the GPUs' remapped memory rows
struct GpuFacts {
    std::map<std::pair<std::string, long>, StepPid> pids;
    std::map<std::string, std::pair<bool, bool>> remapped;   // by uuid: rows waiting for a reset, a remapping that failed
    std::string error;
};

inline GpuFacts fetchGpuFacts(const std::string& job_id, const std::string& nodelist, int gpus_per_node) {
    GpuFacts f;
    const auto hosts = expandHostlist(nodelist);
    if (hosts.empty()) return f;
    auto pids = liveStep(job_id, hosts.size(), 0, liveStepName() + "-pids");
    for (const char* a : {"scontrol", "listpids"}) pids.push_back(a);
    const RunResult p = run(pids, 30);
    f.pids = parseListpids(p.out, hosts);
    if (f.pids.empty()) f.error = p.error("srun scontrol listpids");
    auto rows = liveStep(job_id, hosts.size(), gpus_per_node, liveStepName() + "-rows");
    for (const char* a : {"nvidia-smi", "--query-remapped-rows=gpu_uuid,remapped_rows.pending,remapped_rows.failure", "--format=csv,noheader"})
        rows.push_back(a);
    const RunResult m = run(rows, 30);
    for (const auto& line : util::lines(m.out)) {
        const size_t colon = line.find(':');
        const auto x = util::split(colon == std::string::npos ? line : line.substr(colon + 1), ',');
        if (x.size() >= 3 && util::startsWith(util::trim(x[0]), "GPU-"))
            f.remapped[util::trim(x[0])] = {util::trim(x[1]) == "Yes", util::trim(x[2]) == "Yes"};
    }
    return f;
}

}  // namespace api
