#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
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
    double temp_c = -1, power_w = -1, power_limit_w = -1;   // only asked by the live monitor (-1: not given)
};

/* "0: 1, NVIDIA GH200 120GB, 97, 70123, 97871" (srun -l: the task number, one task per node, in the node order)
- The live monitor asks for 3 more: "..., 97871, 61, 412.33, 900.00" (temperature, power, its limit; "[N/A]" when unknown)
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
    return true;
}

inline std::vector<GpuUse> parseNvidiaSmi(const std::string& out, const std::vector<std::string>& hosts) {
    std::vector<GpuUse> r;
    GpuUse g;
    for (const auto& line : util::lines(out))
        if (parseNvidiaSmiLine(line, hosts, g)) r.push_back(g);
    return r;
}

/* One small step in the job itself (srun --overlap: it shares what the job already has, asks for nothing more)
- Run only when asked (a key): it starts a process on each node of the job
*/
inline Fetched<std::vector<GpuUse>> fetchGpus(const std::string& job_id, const std::string& nodelist, int gpus_per_node) {
    Fetched<std::vector<GpuUse>> r;
    const auto hosts = expandHostlist(nodelist);
    if (hosts.empty()) {
        r.error = "the job has no node";
        return r;
    }
    std::vector<std::string> args = {"srun", "--jobid=" + job_id, "--overlap", "--nodes=" + std::to_string(hosts.size()),
                                     "--ntasks-per-node=1", "-l", "--quiet"};
    if (gpus_per_node > 0) args.push_back("--gres=gpu:" + std::to_string(gpus_per_node));
    for (const char* a : {"nvidia-smi", "--query-gpu=index,name,utilization.gpu,memory.used,memory.total",
                          "--format=csv,noheader,nounits"})
        args.push_back(a);
    const RunResult res = run(args, 30);
    r.data = parseNvidiaSmi(res.out, hosts);
    if (r.data.empty()) r.error = res.error("srun").empty() ? "nvidia-smi showed no GPU" : res.error("srun");
    return r;
}

/* The same step, but it stays: nvidia-smi --loop-ms answers every `interval_ms` until it is stopped (live monitor)
- One step for as long as the monitor is open, instead of one every few seconds: sacct isn't filled with them
*/
inline std::vector<std::string> gpuStreamArgs(const std::string& job_id, size_t nodes, int gpus_per_node, int interval_ms) {
    std::vector<std::string> args = {"srun", "--jobid=" + job_id, "--overlap", "--nodes=" + std::to_string(nodes),
                                     "--ntasks-per-node=1", "-l", "--quiet"};
    if (gpus_per_node > 0) args.push_back("--gres=gpu:" + std::to_string(gpus_per_node));
    for (const char* a : {"nvidia-smi",
                          "--query-gpu=index,name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,power.limit",
                          "--format=csv,noheader,nounits"})
        args.push_back(a);
    args.push_back("--loop-ms=" + std::to_string(interval_ms));
    return args;
}

}  // namespace api
