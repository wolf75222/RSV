// Parsing of Slurm output as printed on ROMEO (Slurm 23.11), names and accounts made up.
// Build with cmake -DRSV_TESTS=ON, run with ctest (or ./parse_test).

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <string>

#include "api/arrays.hpp"
#include "api/cluster.hpp"
#include "api/slurmjobs.hpp"
#include "api/usage.hpp"
#include "components/cluster_view.hpp"
#include "components/log_view.hpp"
#include "components/monitor_view.hpp"
#include "components/quota_view.hpp"
#include "utils/format.hpp"

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto va = (a);                                                                        \
        const auto vb = (b);                                                                        \
        if (!(va == vb)) {                                                                          \
            std::printf("%s:%d: FAILED: %s == %s\n", __FILE__, __LINE__, #a, #b);                   \
            failures++;                                                                             \
        }                                                                                           \
    } while (0)

// The fixtures are written with '|' for reading: squeue and sacct are asked for the unit separator (0x1F).
// '^' stands for a real '|' (in a name).
static std::string sep(std::string s) {
    for (char& c : s) {
        if (c == '|') c = api::SEP;
        else if (c == '^') c = '|';
    }
    return s;
}

static void durations() {
    CHECK_EQ(util::parseDuration("45:10"), 45 * 60 + 10);
    CHECK_EQ(util::parseDuration("1:02:03"), 3723);
    CHECK_EQ(util::parseDuration("2-22:34:00"), 2 * 86400 + 22 * 3600 + 34 * 60);
    CHECK_EQ(util::parseDuration("30-00:00:00"), 30L * 86400);
    CHECK_EQ(util::parseDuration("2-12"), 2 * 86400 + 12 * 3600);
    CHECK_EQ(util::parseDuration("30"), 30 * 60);
    CHECK_EQ(util::parseDuration("00:11.237"), 11);          // sacct TotalCPU: min:s.ms
    CHECK_EQ(util::parseDuration("1-12:42:14.123"), 86400 + 12 * 3600 + 42 * 60 + 14);
    CHECK_EQ(util::parseDuration("UNLIMITED"), util::UNLIMITED);
    CHECK_EQ(util::parseDuration("N/A"), util::UNKNOWN);
    CHECK_EQ(util::parseDuration("INVALID"), util::UNKNOWN);
    CHECK_EQ(util::parseDuration(""), util::UNKNOWN);
    CHECK_EQ(util::duration(45), std::string("45s"));
    CHECK_EQ(util::duration(3 * 3600 + 12 * 60), std::string("3h12m"));
    CHECK_EQ(util::duration(86400 + 5 * 3600), std::string("1d05h"));
}

static void widths() {
    CHECK_EQ(util::width("abc"), 3u);
    CHECK_EQ(util::width("数値"), 4u);
    CHECK_EQ(util::width(util::padRight("数値流体シミュレーション", 9)), 9u);   // never 1 column off
    CHECK_EQ(util::cut("数値流体", 5), std::string("数値…"));
    CHECK_EQ(util::width(util::padLeft("é", 4)), 4u);
    CHECK_EQ(util::cut("hello", 5), std::string("hello"));
    CHECK_EQ(util::cut("hello!", 5), std::string("hell…"));
    CHECK_EQ(util::sanitize("a\tb"), std::string("a       b"));
    CHECK_EQ(util::sanitize("\x1b[33mwarn\x1b[0m"), std::string("warn"));
    CHECK_EQ(util::sanitize("10%\r50%\r100%"), std::string("100%"));
    CHECK_EQ(util::sanitize(std::string("bad \xff byte")), std::string("bad ? byte"));
    CHECK_EQ(util::dropStart("abcdef", 2), std::string("cdef"));
    // Malformed UTF-8: each bad byte is one '?', so FTXUI and the terminal agree on the width
    CHECK_EQ(util::sanitize("A\xF5\x80\x80" "B"), std::string("A???B"));
    CHECK_EQ(util::sanitize("A\xE0\x81\x81" "B"), std::string("A???B"));   // overlong
    CHECK_EQ(util::sanitize("A\xED\xA0\x80" "B"), std::string("A???B"));   // surrogate
    CHECK_EQ(util::sanitize("A\xF4\x90\x80\x80" "B"), std::string("A????B"));   // past U+10FFFF
    CHECK_EQ(util::sanitize("\xF0\x9F\x9A\x80"), std::string("\xF0\x9F\x9A\x80"));   // a valid emoji stays
    CHECK_EQ(util::sanitize("a\xe2\x80\x8b" "b\xe2\x80\xae" "c\xef\xbb\xbf"), std::string("abc"));   // invisible: FTXUI would give them a column
    CHECK_EQ(util::sanitize("\u2764\ufe0f"), std::string("\u2764"));
    CHECK_EQ(util::width(util::sanitize("\U0001F468\u200D\U0001F469")), 4u);
}

static void hostlists() {
    const auto h = api::expandHostlist("romeo-a[031-032,040],romeo-c001");
    CHECK_EQ(h.size(), 4u);
    CHECK_EQ(h[0], std::string("romeo-a031"));
    CHECK_EQ(h[2], std::string("romeo-a040"));
    CHECK_EQ(h[3], std::string("romeo-c001"));
    CHECK_EQ(api::expandHostlist("r[1-2]n[01-02]").size(), 4u);
    CHECK_EQ(api::expandHostlist("(null)").size(), 0u);
    CHECK_EQ(api::parseIdList("0-15,72-87,144-159,216-231").size(), 64u);
    CHECK_EQ(api::parseIdList("garbage,3").size(), 1u);
    CHECK_EQ(api::parseIdList("99999999999999999999").size(), 0u);   // no exception, nothing
}

static void squeue() {
    const std::string out =
        "900101|900101|RUNNING|long|proj-a|normal|2|128|cpu=128,mem=1500G,node=2,billing=128,gres/gpu=8|1-11:56:54|"
        "2-22:34:00|1-10:37:06|2026-10-22T10:39:59|2026-10-25T08:13:59|2026-10-22T10:39:59|None|romeo-a[031-032]|(null)|"
        "/gpfs/home/demo/run/slurm-900101.out|/gpfs/home/demo/run|demo|my job ^ with a bar|\n"
        "900200_1|900202|RUNNING|long|proj-b|normal|1|16|cpu=16,mem=64G,node=1,billing=16|1-09:22:51|7-00:00:00|5-14:37:09|"
        "2026-10-22T13:14:02|2026-10-29T12:14:02|2026-10-22T13:14:01|None|romeo-c002|(null)|/home/demo/a.out|/home/demo|demo|sim|\n"
        "812402_[3-40%4]|812402|PENDING|instant|proj-b|normal|1|8|cpu=8,mem=16G,node=1,billing=8|0:00|01:00:00|01:00:00|"
        "N/A|N/A|2026-10-22T13:14:01|QOSMaxJobsPerUserLimit||afterok:1(unfulfilled)|/home/demo/s.out|/home/demo|alice|sweep|\n"
        "slurm_load_jobs error: something\n";
    const auto jobs = api::parseSqueue(sep(out));
    CHECK_EQ(jobs.size(), 3u);
    if (jobs.size() < 3) return;
    CHECK_EQ(jobs[0].name, std::string("my job | with a bar"));   // spaces and bars in names
    CHECK_EQ(jobs[0].gpus, 8);
    CHECK_EQ(jobs[0].nodes, 2);
    CHECK_EQ(jobs[0].cpus, 128);
    CHECK(jobs[0].active());
    CHECK_EQ(jobs[0].used, 86400 + 11 * 3600 + 56 * 60 + 54);
    CHECK_EQ(jobs[1].id, std::string("900200_1"));
    CHECK_EQ(jobs[1].job_id, std::string("900202"));
    CHECK(jobs[2].pending());
    CHECK_EQ(jobs[2].start, 0);
    CHECK_EQ(jobs[2].dependency, std::string("afterok:1(unfulfilled)"));
    CHECK_EQ(jobs[2].user, std::string("alice"));
    CHECK_EQ(api::taskCount(jobs[2].id), 38);

    // A '|' in a path no longer shifts the fields after it
    std::string line = sep("1|1|RUNNING|short|p|normal|1|1|cpu=1|0:01|1:00|0:59|N/A|N/A|N/A|None|n1|(null)|/w/a.out|");
    line += "/home/demo/a|b";
    line += api::SEP;
    line += "demo";
    line += api::SEP;
    line += "my job";
    line += api::SEP;
    const auto odd = api::parseSqueue(line);
    CHECK_EQ(odd.size(), 1u);
    if (!odd.empty()) {
        CHECK_EQ(odd[0].workdir, std::string("/home/demo/a|b"));
        CHECK_EQ(odd[0].name, std::string("my job"));
    }
}

static void details() {
    const std::string out =
        "JobId=900101 JobName=my job = test\n"
        "   UserId=demo(4242) GroupId=demo(4242) MCS_label=N/A\n"
        "   JobState=RUNNING Reason=None Dependency=(null)\n"
        "   RunTime=1-11:46:55 TimeLimit=2-22:34:00 TimeMin=N/A\n"
        "   Partition=long AllocNode:Sid=romeo1:139327\n"
        "   NodeList=romeo-a[031-033]\n"
        "   BatchHost=romeo-a031\n"
        "   NumNodes=3 NumCPUs=160 NumTasks=2 CPUs/Task=64 ReqB:S:C:T=0:0:*:*\n"
        "   JOB_GRES=gpu:h100:10\n"
        "     Nodes=romeo-a[031-032] CPU_IDs=0-15,72-87,144-159,216-231 Mem=768000 GRES=gpu:h100:4(IDX:0-3)\n"
        "     Nodes=romeo-a033 CPU_IDs=0-31 Mem=1000 GRES=gpu:h100:2(IDX:1,3)\n"
        "   Command=/home/demo/run.sh --opt=1\n"
        "   WorkDir=/home/demo/my dir\n"
        "   StdOut=/home/demo/out-%x-%j-%4a.log\n"
        "   \n";
    const auto d = api::parseJobDetails(out, "900101");
    CHECK_EQ(d.get("JobName"), std::string("my job = test"));
    CHECK_EQ(d.get("WorkDir"), std::string("/home/demo/my dir"));
    CHECK_EQ(d.get("Command"), std::string("/home/demo/run.sh --opt=1"));
    CHECK_EQ(d.get("AllocNode:Sid"), std::string("romeo1:139327"));
    CHECK_EQ(d.alloc.size(), 2u);
    if (d.alloc.size() == 2) {
        // The CPU ids are those of EACH node of the group (RSV 1 split them between the nodes)
        CHECK_EQ(d.alloc[0].hosts.size(), 2u);
        CHECK_EQ(d.alloc[0].cpus.size(), 64u);
        CHECK_EQ(d.alloc[0].gpus.indices.size(), 4u);
        CHECK_EQ(d.alloc[1].cpus.size(), 32u);
        CHECK_EQ(d.alloc[1].gpus.indices.size(), 2u);
        if (d.alloc[1].gpus.indices.size() == 2) CHECK_EQ(d.alloc[1].gpus.indices[1], 3);
    }
    CHECK_EQ(api::expandSlurmPath(d.get("StdOut"), d), std::string("/home/demo/out-my job = test-900101-4294967294.log"));

    // An array: one record per task, the one asked for is kept
    const std::string arr = "JobId=11 ArrayJobId=10 ArrayTaskId=1 JobName=a\n   JobState=RUNNING\n\n"
                            "JobId=10 ArrayJobId=10 ArrayTaskId=2-5 JobName=a\n   JobState=PENDING\n   StdOut=x-%A_%3a.out\n"
                            "   WorkDir=/w\n";
    const auto a = api::parseJobDetails(arr, "10");
    CHECK_EQ(a.get("JobState"), std::string("PENDING"));
    CHECK_EQ(api::expandSlurmPath(a.get("StdOut"), a), std::string("/w/x-10_2-5.out"));
}

static void nodes() {
    const std::string out =
        "NodeName=romeo-a031 Arch=aarch64 CoresPerSocket=72  CPUAlloc=64 CPUEfctv=288 CPUTot=288 CPULoad=3.00 "
        "AvailableFeatures=armgpu ActiveFeatures=armgpu Gres=gpu:h100:4 NodeAddr=romeo-a031 "
        "OS=Linux 5.14.0-570.141.1.el9_6.aarch64+64k #1 SMP PREEMPT_DYNAMIC Thu Sep 10 02:14:15 UTC 2026  "
        "RealMemory=820802 AllocMem=768000 FreeMem=175420 Sockets=4 Boards=1 State=MIXED ThreadsPerCore=1 "
        "AllocTRES=cpu=64,mem=750G,gres/gpu=4\n"
        "NodeName=romeo-c001 Arch=x86_64 CoresPerSocket=96 CPUAlloc=148 CPUTot=192 CPULoad=N/A Gres=(null) "
        "RealMemory=1160629 Sockets=2 State=DOWN* Reason=Not responding [slurm@2026-10-20T10:00:00]\n";
    const auto n = api::parseNodes(out);
    CHECK_EQ(n.size(), 2u);
    if (n.size() < 2) return;
    CHECK_EQ(n[0].cpus, 288);
    CHECK_EQ(n[0].sockets, 4);
    CHECK_EQ(n[0].cores, 72);
    CHECK_EQ(n[0].gpus.count, 4);
    CHECK_EQ(n[0].gpu_alloc, 4);
    CHECK_EQ(n[0].state, std::string("MIXED"));
    CHECK_EQ(n[1].gpus.count, 0);
    CHECK(n[1].load < 0);
    CHECK_EQ(n[1].reason, std::string("Not responding [slurm@2026-10-20T10:00:00]"));
}

static void partitions() {
    const std::string out =
        "instant*|up|1:00:00|romeo-a001|idle|0/288/0/288|gpu:h100:4|gpu:h100:0(IDX:N/A)|armgpu|\n"
        "instant*|up|1:00:00|romeo-a002|mix|128/160/0/288|gpu:h100:4|gpu:h100:2(IDX:0-1)|armgpu|\n"
        "instant*|up|1:00:00|romeo-c001|drain*|0/0/192/192|(null)|gpu:0|x64cpu|\n"
        "long|up|30-00:00:00|romeo-c002|alloc|192/0/0/192|(null)|gpu:0|x64cpu|\n"
        "long|up|30-00:00:00|romeo-a002|mix|128/160/0/288|gpu:h100:4|gpu:h100:2(IDX:0-1)|armgpu|\n";
    const auto info = api::parseSinfo(out);
    const auto& p = info.partitions;
    CHECK_EQ(p.size(), 2u);
    if (p.size() < 2) return;
    CHECK_EQ(p[0].name, std::string("instant"));
    CHECK(p[0].is_default);
    CHECK_EQ(p[0].nodes, 3);
    CHECK_EQ(p[0].idle, 1);
    CHECK_EQ(p[0].mixed, 1);
    CHECK_EQ(p[0].down, 1);
    CHECK_EQ(p[0].gpus_total, 8);
    CHECK_EQ(p[0].gpus_used, 2);
    CHECK_EQ(p[0].cpus_total, 768);
    CHECK_EQ(p[1].full, 1);

    // romeo-a002 is in two partitions: once in the nodes, the kinds and the totals
    CHECK_EQ(info.nodes.size(), 4u);
    const auto types = api::nodeTypes(info.nodes);
    CHECK_EQ(types.size(), 2u);
    if (types.size() == 2) {
        CHECK_EQ(types[0].features, std::string("armgpu"));
        CHECK_EQ(types[0].prefix, std::string("romeo-a"));
        CHECK_EQ(types[0].nodes, 2);
        CHECK_EQ(types[0].gpus_used, 2);
        CHECK_EQ(types[1].down, 1);
        CHECK_EQ(types[1].cpus_alloc, 192);
    }
}

static void queue() {
    // Everyone's jobs: user, account, partition, state, CPUs, TRES, reason, id
    const std::string out =
        "alice|proj-a|short|RUNNING|128|cpu=128,mem=1500G,node=2,gres/gpu=8|None|1001|\n"
        "alice|proj-a|short|RUNNING|64|cpu=64,mem=10G,node=1|None|1002|\n"
        "bob|proj-b|long|RUNNING|192|cpu=192,mem=50G,node=1|None|1003|\n"
        "bob|proj-b|long,short|PENDING|16|cpu=16,mem=64G,node=1|Resources|1004|\n"
        "chen|proj-a|instant|PENDING|8|cpu=8,mem=16G,node=1,gres/gpu=1|QOSMaxJobsPerUserLimit|2000_[3-40%4]|\n"
        "dana|proj-c|instant|COMPLETED|8|cpu=8|None|1005|\n";
    const auto q = api::parseQueue(sep(out));
    CHECK_EQ(q.running, 3);
    CHECK_EQ(q.pending, 1 + 38);
    CHECK_EQ(q.cpus_running, 384);
    CHECK_EQ(q.gpus_running, 8);
    CHECK_EQ(q.cpus_pending, 16 + 38 * 8);
    CHECK_EQ(q.gpus_pending, 38);
    CHECK_EQ(q.users.size(), 3u);   // dana's job ended
    CHECK_EQ(q.by_partition.at("short").running, 2);
    CHECK_EQ(q.by_partition.at("long").pending, 1);   // the first partition it asked for
    CHECK_EQ(q.reasons.at("QOSMaxJobsPerUserLimit"), 38);
    CHECK_EQ(q.reason_use.at("Resources").cpus_pending, 16);
    // alice and bob both use 192 CPUs: alice's 8 GPUs put her first
    CHECK_EQ(q.by_user.size(), 3u);
    if (q.by_user.size() == 3) {
        CHECK_EQ(q.by_user[0].name, std::string("alice"));
        CHECK_EQ(q.by_user[0].cpus_running, 192);
        CHECK_EQ(q.by_user[2].name, std::string("chen"));
    }
    CHECK_EQ(q.by_account.size(), 2u);   // proj-a (alice and chen), proj-b
    if (!q.by_account.empty()) CHECK_EQ(q.by_account[0].pending, 38);
}

static void sacct() {
    const std::string out =
        "900300_12|COMPLETED|instant|proj-a|2026-10-23T10:00:00|2026-10-23T10:00:01|2026-10-23T10:00:14|00:00:13|01:00:00|1|1|8G|"
        "billing=1,cpu=1,mem=8G,node=1|00:11.237|13||0:0|romeo-c001|my ^ job\n"
        "900300_12.batch|COMPLETED||proj-a|2026-10-23T10:00:01|2026-10-23T10:00:01|2026-10-23T10:00:14|00:00:13||1|1||"
        "cpu=1,mem=8G,node=1|00:11.236|13|324M|0:0|romeo-c001|batch\n"
        "900300_12.extern|COMPLETED||proj-a|2026-10-23T10:00:01|2026-10-23T10:00:01|2026-10-23T10:00:14|00:00:13||1|1||"
        "cpu=1,mem=8G,node=1|00:00.001|13|0|0:0|romeo-c001|extern\n"
        "900310|CANCELLED by 4242|short|proj-a|2026-10-23T11:00:00|None|2026-10-23T11:05:00|00:00:00|02:00:00|1|16|64G|"
        "billing=16,cpu=16,mem=64G,node=1|00:00:00|0||0:0|None assigned|later\n";
    const auto h = api::parseSacct(sep(out));
    CHECK_EQ(h.size(), 2u);
    if (h.size() < 2) return;
    CHECK_EQ(h[0].id, std::string("900310"));   // newest first
    CHECK_EQ(h[0].state, std::string("CANCELLED"));
    const auto& j = h[1];
    CHECK_EQ(j.name, std::string("my | job"));
    CHECK_EQ(j.max_rss_mib, 324.0);   // from the batch step
    CHECK_EQ(j.mem_mib, 8192.0);
    CHECK(j.cpuEfficiency() > 0.8 && j.cpuEfficiency() < 0.9);
}

// What the jobs consumed: hours reserved, used, idle, GPU hours, --time asked and used (as on ROMEO, without -X:
// the job's line has the CPU time of its steps)
static void consumption() {
    const std::string out =
        "900400|COMPLETED|short|proj-a|2026-10-23T08:00:00|2026-10-23T08:00:10|2026-10-23T10:00:10|02:00:00|04:00:00|1|32|120G|"
        "billing=32,cpu=32,gres/gpu=2,mem=120G,node=1|16:00:00|230400||0:0|romeo-a027|train\n"
        "900401|RUNNING|short|proj-a|2026-10-23T09:00:00|2026-10-23T09:00:05|Unknown|01:00:00|12:00:00|2|128|256G|"
        "billing=128,cpu=128,gres/gpu=8,mem=512G,node=2|00:00:00|460800||0:0|romeo-a[012-013]|lbm\n"
        "900402|CANCELLED by 4242|short|proj-a|2026-10-23T07:00:00|None|2026-10-23T07:05:00|00:00:00|02:00:00|1|16|64G|"
        "billing=16,cpu=16,mem=64G,node=1|00:00:00|0||0:0|None assigned|later\n"
        "900403|FAILED|short|proj-a|2026-10-23T06:00:00|2026-10-23T06:00:01|2026-10-23T06:10:01|00:10:00|01:00:00|1|4|8G|"
        "billing=4,cpu=4,mem=8G,node=1|20:00.250|2400||1:0|romeo-c001|crash\n";
    const auto h = api::parseSacct(sep(out));
    CHECK_EQ(h.size(), 4u);
    auto find = [&](const std::string& id) -> const api::HistoryJob* {
        for (const auto& j : h)
            if (j.id == id) return &j;
        return nullptr;
    };
    const auto* done = find("900400");
    const auto* running = find("900401");
    const auto* cancelled = find("900402");
    const auto* failed = find("900403");
    CHECK(done && running && cancelled && failed);
    if (!done || !running || !cancelled || !failed) return;

    CHECK_EQ(done->gpus, 2);
    CHECK_EQ(done->cpuHours(), 64.0);       // 2 h × 32 CPUs
    CHECK_EQ(done->usedCpuHours(), 16.0);
    CHECK_EQ(done->idleCpuHours(), 48.0);
    CHECK_EQ(done->gpuHours(), 4.0);        // 2 h × 2 GPUs
    CHECK_EQ(done->unusedTime(), 7200L);    // 4 h asked, 2 h run
    CHECK_EQ(running->gpus, 8);
    CHECK(!running->ended());
    CHECK_EQ(running->cpuEfficiency(), -1.0);   // its CPU time is recorded when its steps end
    CHECK_EQ(running->idleCpuHours(), -1.0);
    CHECK_EQ(cancelled->cpuHours(), 0.0);
    CHECK_EQ(cancelled->unusedTime(), api::UNKNOWN);   // cancelled: its --time isn't too long
    CHECK_EQ(failed->usedCpuHours(), 1200.0 / 3600);   // 20:00.250, without the fraction

    std::vector<const api::HistoryJob*> all;
    for (const auto& j : h) all.push_back(&j);
    const auto c = api::consumption(all);
    CHECK_EQ(c.ended, 3);
    CHECK_EQ(c.running, 1);
    CHECK_EQ(c.gpu_jobs, 1);
    CHECK(c.cpu_h > 64.66 && c.cpu_h < 64.67);
    CHECK(c.idle_cpu_h > 48.33 && c.idle_cpu_h < 48.34);
    CHECK_EQ(c.idle_cpu_h_gpu_jobs, 48.0);   // the training job's
    CHECK_EQ(c.gpu_h, 4.0);
    CHECK_EQ(c.asked_h, 4.0);
    CHECK_EQ(c.ran_h, 2.0);
    CHECK(c.unusedShare() > 0.49 && c.unusedShare() < 0.51);
    CHECK_EQ(api::consumption({}).idleShare(), -1.0);
}

static void assocs() {
    const std::string out = "default|||cpu=1536,gres/gpu=60|||0|0|\n"
                            "proj-a||normal|cpu=1536,gres/gpu=60||||100|\n";
    const auto a = api::parseAssocs(out);
    CHECK_EQ(a.size(), 2u);
    if (a.size() < 2) return;
    CHECK_EQ(a[0].max_submit, 0);
    CHECK_EQ(a[1].max_submit, 100);
    CHECK_EQ(a[1].max_jobs, -1);
    CHECK_EQ(a[1].grp_tres.at("gres/gpu"), 60.0);
}

static void cluster_extras() {
    // sinfo -N with load and memory: a node reserved but idle
    const std::string out =
        "short|up|1-00:00:00|romeo-c026|mix|120/72/0/192|(null)|gpu:0|x64cpu|9.60|400000|700000|1160629|\n"
        "short|up|1-00:00:00|romeo-c011|alloc|192/0/0/192|(null)|gpu:0|x64cpu|185.00|900000|200000|1160629|\n";
    const auto info = api::parseSinfo(out);
    CHECK_EQ(info.nodes.size(), 2u);
    if (info.nodes.size() == 2) {
        CHECK(info.nodes[0].reservedIdle());
        CHECK(!info.nodes[1].reservedIdle());
        CHECK_EQ(info.nodes[1].busyCores(), 185.0);
        CHECK_EQ(info.nodes[0].mem_total_mib, 1160629.0);
    }

    const auto down = api::parseDownReasons("GPUACheck : Not responding|slurm|2026-09-30T14:59:53|romeo-a042|drained*\n"
                                            "a|b reason|root|2026-10-23T16:05:06|romeo-a[044-045]|drained\n");
    CHECK_EQ(down.size(), 2u);
    if (down.size() == 2) {
        CHECK_EQ(down[0].reason, std::string("a|b reason"));   // newest first, a bar in the reason kept
        CHECK_EQ(down[0].nodes, std::string("romeo-a[044-045]"));
        CHECK_EQ(down[1].user, std::string("slurm"));
    }

    const auto resv = api::parseReservations(
        "ReservationName=training StartTime=2026-11-05T12:30:00 EndTime=2026-11-05T17:30:00 Duration=05:00:00\n"
        "   Nodes=romeo-a[019,021-022] NodeCnt=3 CoreCnt=864 Features=armgpu PartitionName=instant Flags=FLEX\n"
        "   TRES=cpu=864\n   Users=(null) Groups=(null) Accounts=(null) Licenses=(null) State=INACTIVE BurstBuffer=(null)\n\n"
        "ReservationName=maint StartTime=2026-11-01T08:00:00 EndTime=2026-11-01T18:00:00 Duration=10:00:00\n"
        "   Nodes=ALL NodeCnt=102 CoreCnt=25152 Features=(null) PartitionName=(null) Flags=MAINT,IGNORE_JOBS\n");
    CHECK_EQ(resv.size(), 2u);
    if (resv.size() == 2) {
        CHECK_EQ(resv[0].name, std::string("maint"));   // the first to start first
        CHECK(resv[0].maintenance());
        CHECK_EQ(resv[1].node_count, 3);
        CHECK_EQ(resv[1].partition, std::string("instant"));
    }

    const auto ping = api::parsePing("Slurmctld(primary) at admin1 is UP\nSlurmctld(backup) at admin2 is DOWN\n");
    CHECK_EQ(ping.size(), 2u);
    CHECK_EQ(api::controllerState(ping), std::string("UP"));   // the backup down doesn't matter while the primary answers
    CHECK_EQ(api::controllerState(api::parsePing("Slurmctld(primary) at admin1 is DOWN\n")), std::string("primary controller admin1 DOWN"));

    const auto d = api::parseSdiag("Data since      Fri Oct 23 02:00:00 2026 (1792713600)\nJobs submitted: 1408\n"
                                   "Main schedule statistics (microseconds):\n\tLast cycle:   61\n\tMax cycle:    242873\n"
                                   "\tMean cycle:   1993\nMain scheduler exit:\n\tEnd of job queue:3245\nBackfilling stats\n"
                                   "\tLast cycle: 344\n\tMax cycle:  105366\n\tMean cycle: 1152\n");
    CHECK_EQ(d.since, static_cast<std::time_t>(1792713600));
    CHECK_EQ(d.submitted, 1408);
    CHECK_EQ(d.main_mean_us, 1993);
    CHECK_EQ(d.main_max_us, 242873);
    CHECK_EQ(d.bf_mean_us, 1152);

    const auto limits = api::parsePartitionLimits(
        "PartitionName=long AllowGroups=ALL AllowAccounts=ALL MaxNodes=UNLIMITED MaxTime=30-00:00:00 PriorityTier=1 "
        "State=UP DefMemPerNode=UNLIMITED MaxMemPerNode=UNLIMITED\n"
        "PartitionName=gpu AllowAccounts=proj-a,proj-b MaxNodes=4 MaxTime=1-00:00:00 PriorityTier=50 DefMemPerCPU=4000\n");
    CHECK_EQ(limits.size(), 2u);
    CHECK_EQ(limits.at("long").tier, 1);
    CHECK_EQ(limits.at("gpu").max_nodes, std::string("4"));
    CHECK_EQ(limits.at("gpu").def_mem, std::string("4000 per CPU"));
    CHECK_EQ(limits.at("gpu").allow_accounts, std::string("proj-a,proj-b"));
}

static void usage() {
    const auto live = api::parseSstat("900101.batch|1|307M|cpu=01:00:00,energy=0,fs/disk=1,mem=1G,pages=0,vmem=3G\n"
                                      "900101.0|2|102400M|cpu=1-00:00:00,energy=0,mem=200G,pages=0\n");
    CHECK_EQ(live.steps, 2);
    CHECK_EQ(live.cpu_seconds, 3600.0 + 86400.0);
    CHECK_EQ(live.mem_mib, 1024.0 + 200 * 1024.0);
    CHECK_EQ(live.max_task_mib, 102400.0);

    // Busy between two readings: 4 CPUs, 54 s of CPU time in 15 s
    api::LiveUse first, second, close, after_step;
    first.cpu_seconds = 35, first.at = 1000;
    second.cpu_seconds = 89, second.at = 1015;
    api::follow(second, first, 4);
    CHECK(second.busy > 0.899 && second.busy < 0.901);
    close.cpu_seconds = 90, close.at = 1017;   // 2 s later: the reading before stays the base
    api::follow(close, second, 4);
    CHECK_EQ(close.at, 1015.0);
    CHECK_EQ(close.cpu_seconds, 89.0);
    CHECK_EQ(close.busy, second.busy);
    after_step.cpu_seconds = 10, after_step.at = 1030;   // a step ended: its CPU time went with it
    api::follow(after_step, second, 4);
    CHECK_EQ(after_step.busy, -1.0);

    const auto prio = api::parseSprio("100|alice|short|80000|0|79999|0|0|0|1\n"
                                      "101|demo|short|30001|0|30000|0|0|0|1\n"
                                      "102|bob|short|50000|0|49999|0|0|0|1\n"
                                      "103|chen|long|99999|0|99998|0|0|0|1\n");
    const auto rank = api::rankOf(prio, "101");
    CHECK_EQ(rank.rank, 3);   // after alice and bob, chen is in another partition
    CHECK_EQ(rank.of, 3);
    CHECK_EQ(rank.mine.fairshare, 30000.0);
    CHECK_EQ(api::rankOf(prio, "999").of, 0);

    const auto shares = api::parseSshare("default|demo|0.001000|0|0.000000|1.000000\nproj-a|demo|0.040000|41234567|0.612000|0.081234\n");
    CHECK_EQ(shares.size(), 2u);
    if (shares.size() == 2) CHECK_EQ(shares[1].fairshare, 0.081234);

    const auto quotas = api::parseMmlsquota(
        "mmlsquota:user:HEADER:version:reserved:reserved:filesystemName:quotaType:id:name:blockUsage:blockQuota:blockLimit:"
        "blockInDoubt:blockGrace:filesUsage:filesQuota:filesLimit:filesInDoubt:filesGrace:remarks:fid:filesetname:\n"
        "mmlsquota:user:0:1:::gpfs:USR:4242:demo:3250000:104857600:110100480:0:none:98213:1000000:1500000:0:none:x:1:home:\n"
        "mmlsquota:user:0:1:::gpfs:USR:4242:demo:23068672:104857600:110100480:0:none:1204551:1000000:1500000:0:6 days:x:2:scratch:\n");
    CHECK_EQ(quotas.size(), 2u);
    if (quotas.size() == 2) {
        CHECK_EQ(quotas[0].fileset, std::string("home"));
        CHECK_EQ(quotas[0].quota_kib, 104857600.0);
        CHECK_EQ(quotas[1].files, 1204551.0);
        CHECK_EQ(quotas[1].files_grace, std::string("6 days"));
    }

    const auto ok = api::parseTestOnly("sbatch: Job 781504 to start at 2026-10-24T00:31:35 using 2 processors on nodes romeo-a[049-050] in partition short\n");
    CHECK(ok.ok);
    CHECK_EQ(ok.nodes, std::string("romeo-a[049-050]"));
    CHECK_EQ(ok.partition, std::string("short"));
    CHECK_EQ(ok.cpus, 2);
    CHECK(ok.start > 0);
    const auto no = api::parseTestOnly("sbatch: error: Constraint is missing: use option --constraint=<feature>\nallocation failure: Unspecified error\n");
    CHECK(!no.ok);
    CHECK_EQ(no.message, std::string("Constraint is missing: use option --constraint=<feature> · allocation failure: Unspecified error"));

    const auto gpus = api::parseNvidiaSmi("0: 0, NVIDIA GH200 120GB, 97, 70123, 97871\n1: 3, NVIDIA GH200 120GB, 2, 512, 97871\n",
                                          {"romeo-a012", "romeo-a013"});
    CHECK_EQ(gpus.size(), 2u);
    if (gpus.size() == 2) {
        CHECK_EQ(gpus[1].node, std::string("romeo-a013"));
        CHECK_EQ(gpus[1].index, 3);
        CHECK_EQ(gpus[0].util, 97.0);
        CHECK_EQ(gpus[0].mem_total_mib, 97871.0);
    }
}

// Log view: the lines drawn in red, and the search and error jumps (made-up lines)
static void logs() {
    for (const char* line : {
             "slurmstepd: error: *** JOB 812402 ON romeo-c010 CANCELLED AT 2026-10-20T14:02:11 ***",
             "Error: file not found", "ERROR: unable to open mesh.dat", "3 errors, 1 warning", "Errors: 2",
             "RuntimeError: CUDA out of memory. Tried to allocate 2.00 GiB",
             "ValueError: invalid literal for int() with base 10: 'x'", "Traceback (most recent call last):",
             "Segmentation fault (core dumped)", "[romeo-c010:41235] Signal: Segmentation fault (11)",
             "srun: error: romeo-c010: task 3: Out Of Memory", "slurmstepd: error: Detected 1 oom-kill event(s)",
             "OOM killer: process 4567", "terminate called after throwing an instance of 'std::bad_alloc'",
             "FATAL: cannot open restart file", "Exception in thread \"main\" java.lang.OutOfMemoryError: heap",
             "/var/spool/slurmd/job812345/slurm_script: line 12: 4567 Killed ./solver",
             "*** STEP 812345.0 ON romeo-a012 CANCELLED AT 2026-10-24T01:00:00 DUE TO TIME LIMIT ***",
             "MPI_ABORT was invoked on rank 3 in communicator MPI_COMM_WORLD", "Aborted (core dumped)",
             "error 404 from the license server"})
        CHECK(ui::isErrorLine(line));
    for (const char* line : {
             "step   2400  t=3.0000e-04  residual=1.234e-03  MLUPS= 2437.0", "L2 error: 0.004",
             "iteration 12  error=3.2e-05  error = -1.5", "Total errors: 0", "0 errors, 2 warnings",
             "No errors found", "no exception", "stderr goes to slurm-812345.err", "-Werror=format-security",
             "error_rate=0.05 top1_error=0.241", "plot errorbar", "an error-free run", "see error.log",
             "warning: GPU clock throttled", "Loading room 12, zoom 3", ""})
        CHECK(!ui::isErrorLine(line));

    ui::LogData d;
    for (int i = 0; i < 100; i++) d.lines.push_back("step " + std::to_string(i) + (i % 10 == 3 ? " residual" : ""));
    d.lines[42] = "RuntimeError: diverged";
    d.lines[77] = "Segmentation fault";
    d.errors = ui::errorLines(d.lines);
    CHECK(d.errors == std::vector<int>({42, 77}));

    ui::LogView v;
    v.page = 10;
    v.setData(d);   // following: at the end
    CHECK_EQ(v.offset, 90);
    bool close = false;
    auto key = [&](const std::string& k) { v.onEvent(ftxui::Event::Character(k), close); };
    for (const char* k : {"/", "R", "e", "S", "q"}) key(k);   // e and q are letters of the search while typing
    CHECK(v.prompting);
    v.onEvent(ftxui::Event::Backspace, close);
    v.onEvent(ftxui::Event::Return, close);
    CHECK(!v.prompting && !close);
    CHECK_EQ(v.query, std::string("ReS"));
    CHECK_EQ(v.matches.size(), 10u);   // any case: 3, 13 ... 93
    CHECK_EQ(v.match_line, 93);        // the nearest: on the screen
    CHECK(!v.follow);
    key("n");
    CHECK_EQ(v.match_line, 3);         // around the end
    CHECK_EQ(v.offset, 0);
    key("N");
    CHECK_EQ(v.match_line, 93);
    CHECK_EQ(v.offset, 90);
    key("e");                          // from the screen, around the end
    CHECK_EQ(v.error_line, 42);
    CHECK_EQ(v.offset, 39);            // a third of the way down
    key("e");
    CHECK_EQ(v.error_line, 77);
    key("E");
    CHECK_EQ(v.error_line, 42);
    v.offset = 0;                      // scrolled away: n goes from the top of the screen
    key("n");
    CHECK_EQ(v.match_line, 3);
    for (const char* k : {"/", "x"}) key(k);
    v.onEvent(ftxui::Event::Escape, close);   // cancelled: the search before stays
    CHECK(!v.prompting && !close);
    CHECK_EQ(v.query, std::string("ReS"));
    key("/");
    v.onEvent(ftxui::Event::Return, close);   // empty: searched again
    CHECK_EQ(v.matches.size(), 10u);
    key("G");
    CHECK(v.follow);
    d.lines.push_back("step 100 residual");
    v.setData(d);                      // a new reading: the matches follow it
    CHECK_EQ(v.matches.size(), 11u);
    CHECK_EQ(v.offset, 91);
    for (const char* k : {"/", "z", "z", "z"}) key(k);
    v.onEvent(ftxui::Event::Return, close);
    CHECK(v.matches.empty() && v.match_line == -1);
    key("n");                          // nothing to go to
    CHECK_EQ(v.match_line, -1);
    v.onEvent(ftxui::Event::Escape, close);
    CHECK(close);
}

// The hardware tab and the zoom on a node (scontrol -o show node, squeue -w), as on ROMEO
static void node_hardware() {
    CHECK_EQ(api::compressHostlist({"romeo-a001", "romeo-a002", "romeo-a003", "romeo-c101", "romeo-a005", "login"}),
             std::string("romeo-a[001-003,005],romeo-c101,login"));
    CHECK_EQ(api::compressHostlist({"n9", "n10", "n11"}), std::string("n9,n[10-11]"));
    CHECK_EQ(api::compressHostlist({}), std::string(""));

    const std::string a = "NodeName=romeo-a058 Arch=aarch64 CoresPerSocket=72  CPUAlloc=0 CPUEfctv=288 CPUTot=288 CPULoad=0.05 "
                          "AvailableFeatures=armgpu ActiveFeatures=armgpu Gres=gpu:h100:4 NodeAddr=romeo-a058 "
                          "OS=Linux 5.14.0-570.129.1.el9_6.aarch64+64k #1 SMP PREEMPT_DYNAMIC Thu Jul 16 02:09:23 UTC 2026  "
                          "RealMemory=820802 AllocMem=0 FreeMem=779581 Sockets=4 Boards=1 MemSpecLimit=15360 State=IDLE "
                          "ThreadsPerCore=1 TmpDisk=0 Weight=10 Owner=N/A MCS_label=N/A Partitions=instant  BootTime=2026-08-27T16:58:33\n";
    const std::string c = "NodeName=romeo-c001 Arch=x86_64 CoresPerSocket=96  CPUAlloc=192 CPUTot=192 CPULoad=126.92 "
                          "AvailableFeatures=x64cpu Gres=(null) RealMemory=1160629 AllocMem=893952 Sockets=2 MemSpecLimit=15360 "
                          "State=ALLOCATED ThreadsPerCore=1 TmpDisk=0 Partitions=long,short,instant  BootTime=None\n";
    const std::string fat = "NodeName=romeo-c101 Arch=x86_64 CoresPerSocket=96 CPUTot=192 AvailableFeatures=x64cpu Gres=(null) "
                            "RealMemory=1547701 Sockets=2 MemSpecLimit=15360 ThreadsPerCore=1 TmpDisk=0 Partitions=long,short\n";
    const auto hw = api::parseHardware(a + c + fat + c.substr(0, 18) + "2" + c.substr(19));   // and romeo-c002
    CHECK_EQ(hw.size(), 4u);
    CHECK_EQ(hw[0].cpus, 288);
    CHECK_EQ(hw[0].sockets, 4);
    CHECK_EQ(hw[0].cores, 72);
    CHECK_EQ(hw[0].gpus, 4);
    CHECK_EQ(hw[0].gpu_type, std::string("h100"));
    CHECK_EQ(hw[0].mem_mib, 820802.0);
    CHECK_EQ(hw[0].mem_spec_mib, 15360.0);
    CHECK_EQ(hw[0].partitions.size(), 1u);
    CHECK_EQ(hw[1].partitions.size(), 3u);
    // A node that doesn't answer has no Arch: still of its kind
    std::string silent = a;
    silent.replace(silent.find("romeo-a058"), 10, "romeo-a020");
    silent.replace(silent.find("Arch=aarch64"), 12, "Arch=");
    auto all = hw;
    all.push_back(api::parseHardware(silent).at(0));
    CHECK(all.back().arch.empty());
    CHECK_EQ(api::hardwareKinds(all).size(), 3u);
    CHECK_EQ(api::hardwareKinds(all)[0].nodes.size(), 2u);
    const auto kinds = api::hardwareKinds(hw);
    CHECK_EQ(kinds.size(), 3u);   // armgpu, x64cpu, the x64cpu node with more memory
    CHECK_EQ(api::compressHostlist(kinds[1].nodes), std::string("romeo-c[001-002]"));
    CHECK_EQ(kinds[1].partitions.at("long"), 2);
    CHECK_EQ(api::knownModel(hw[0]).gpu, std::string("NVIDIA GH200 120GB"));
    CHECK_EQ(api::knownModel(hw[1]).cpu, std::string("AMD EPYC 9654 (Zen 4)"));
    api::NodeHardware elsewhere = hw[1];
    elsewhere.name = "node001";   // another cluster: only what Slurm says
    CHECK(api::knownModel(elsewhere).cpu.empty());

    // squeue -w: the name last ('|' in it is kept), the biggest jobs first
    const auto jobs = api::parseNodeJobs(sep("810001|alice|proj-a|RUNNING|576|2|cpu=576,mem=1200G,node=2,gres/gpu=8|2026-10-23T22:35:00|1-21:00:00|md-run|\n"
                                             "812350|demo|proj-b|RUNNING|32|1|cpu=32,mem=120G,node=1,gres/gpu=2|2026-10-24T00:49:10|3:14:50|a^b|\n"
                                             "short|line\n"));
    CHECK_EQ(jobs.size(), 2u);
    CHECK_EQ(jobs[0].id, std::string("810001"));
    CHECK_EQ(jobs[0].gpus, 8);
    CHECK_EQ(jobs[0].nodes, 2);
    CHECK_EQ(jobs[1].name, std::string("a|b"));
    CHECK_EQ(jobs[1].left, 3L * 3600 + 14 * 60 + 50);

    std::string why, user;
    std::time_t since = 0;
    api::splitReason("GPUACheck : Not responding [slurm@2026-09-30T14:59:53]", why, user, since);
    CHECK_EQ(why, std::string("GPUACheck : Not responding"));
    CHECK_EQ(user, std::string("slurm"));
    CHECK(since > 0);
    api::splitReason("disk [replaced]", why, user, since);
    CHECK_EQ(why, std::string("disk [replaced]"));
    CHECK(user.empty());

    // The curve: 100 readings from 0 to 100% in 10 columns, averaged by groups of 10; 3 readings stay 3 columns
    auto drawn = [](const std::vector<double>& values, int width) {
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(1));
        ftxui::Render(screen, ui::ClusterView::sparkline(values, width));
        std::string out;
        for (int x = 0; x < width; x++) out += screen.PixelAt(x, 0).character;
        return out;
    };
    std::vector<double> ramp;
    for (int i = 0; i < 100; i++) ramp.push_back(i / 99.0);
    CHECK_EQ(drawn(ramp, 10), std::string("▁▂▂▃▄▅▆▇▇█"));
    CHECK_EQ(drawn({0.5, 0.5, 1.0}, 6), std::string("▅▅█   "));
}

// Arrays: squeue's records and sacct's put together (made-up ids)
static void arrays() {
    CHECK_EQ(api::arrayOf("812402_7"), std::string("812402"));
    CHECK_EQ(api::arrayOf("812402_[159-200%8]"), std::string("812402"));
    CHECK_EQ(api::arrayOf("812345"), std::string(""));
    CHECK_EQ(api::arrayOf("812402+1"), std::string(""));
    CHECK_EQ(api::taskCount("812402_[159-200%8]"), 42);
    CHECK_EQ(api::taskCount("812402_[1,3,5-7]"), 5);
    CHECK_EQ(api::taskCount("812402_7"), 1);
    CHECK_EQ(api::throttleOf("812402_[159-200%8]"), 8);
    CHECK_EQ(api::plainId("812402_[159-200%8]"), std::string("812402_[159-200]"));
    CHECK_EQ(api::taskIndex("812402_17"), 17L);
    CHECK_EQ(api::taskIndex("812402_[3-4]"), -1L);

    // squeue: 8 running, the 42 waiting, the last 2 ended; sacct: the 148 before, 5 of them failed
    std::vector<api::Job> queue;
    auto task = [&](const std::string& id, const std::string& state) {
        api::Job j;
        j.id = id;
        j.state = state;
        queue.push_back(j);
    };
    for (int t = 151; t <= 158; t++) task("812402_" + std::to_string(t), "RUNNING");
    task("812402_[159-200%8]", "PENDING");
    task("812402_149", "COMPLETED");
    task("812402_150", "COMPLETED");
    std::string out;
    for (int t = 1; t <= 158; t++) {
        const bool failed = t == 17 || t == 43 || t == 98, oom = t == 77, timeout = t == 121;
        out += "812402_" + std::to_string(t) + "|" +
               (t >= 151 ? "RUNNING|0:0" : failed ? "FAILED|1:0" : oom ? "OUT_OF_MEMORY|0:125" : timeout ? "TIMEOUT|0:0" : "COMPLETED|0:0") + "\n";
    }
    out += "812402_[159-200%8]|PENDING|0:0\n";
    const auto past = api::parseArrayTasks(sep(out));
    CHECK_EQ(past.size(), 159u);
    std::vector<const api::Job*> tasks;
    for (const auto& j : queue) tasks.push_back(&j);
    auto p = api::progressOf(tasks, &past);
    CHECK_EQ(p.done, 145);
    CHECK_EQ(p.running, 8);
    CHECK_EQ(p.pending, 42);
    CHECK_EQ(p.failed, 5);
    CHECK_EQ(p.total(), 200);
    CHECK_EQ(p.throttle, 8);
    CHECK(!p.failures.empty() && p.failures.front().first == "812402_17" && p.failures.front().second == "FAILED 1:0");

    // The waiting ones cancelled before they started: squeue's record, then sacct's alone once squeue forgot it
    queue[8].state = "CANCELLED";
    tasks.clear();
    for (const auto& j : queue) tasks.push_back(&j);
    std::string cancelled = out.substr(0, out.rfind("812402_[")) + "812402_[159-200%8]|CANCELLED by 4242|0:0\n";
    const auto past2 = api::parseArrayTasks(sep(cancelled));
    p = api::progressOf(tasks, &past2);
    CHECK_EQ(p.cancelled, 42);
    CHECK_EQ(p.total(), 200);
    tasks.erase(tasks.begin() + 8);
    p = api::progressOf(tasks, &past2);
    CHECK_EQ(p.cancelled, 42);
    CHECK_EQ(p.total(), 200);

    // sacct read before task 151 ended, squeue forgot it since: not counted as running any more
    tasks.erase(tasks.begin());
    p = api::progressOf(tasks, &past2);
    CHECK_EQ(p.running, 7);
    CHECK_EQ(p.total(), 199);
}

// Live view (m): nvidia-smi's longer lines, the curves in time, the busy share from sstat, a stream of lines
static void monitor() {
    api::GpuUse g;
    CHECK(api::parseNvidiaSmiLine("1: 2, NVIDIA GH200 120GB, 88, 60000, 97871, 64, 512.40, [N/A]", {"romeo-a012", "romeo-a013"}, g));
    CHECK_EQ(g.node, std::string("romeo-a013"));
    CHECK_EQ(g.index, 2);
    CHECK_EQ(g.temp_c, 64.0);
    CHECK_EQ(g.power_w, 512.4);
    CHECK_EQ(g.power_limit_w, -1.0);
    CHECK(!api::parseNvidiaSmiLine("srun: error: something", {"romeo-a012"}, g));

    // Resampled: nothing before the first reading, the mean of a dot's readings, the one before holds after them
    ui::Series s;
    s.add(100, 0.2);
    s.add(101, 0.6);
    s.add(101.2, 0.4);   // less than 0.5 s after: replaces it
    const auto v = ui::resample(s, 104, 1, 6, 1);
    CHECK_EQ(v.size(), 6u);
    if (v.size() == 6) {
        CHECK_EQ(v[0], -1.0);   // (98, 99]
        CHECK_EQ(v[1], 0.2);    // (99, 100]
        CHECK_EQ(v[2], 0.4);    // (100, 101]: the reading at 101 became 0.4
        CHECK_EQ(v[5], 0.4);    // held
    }
    CHECK_EQ(ui::resample(s, 104, 1, 6, 2)[2], 0.2);   // divided by the scale

    // The curve: a line from the bottom to the top, the cells under it tinted
    {
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(3), ftxui::Dimension::Fixed(2));
        ftxui::Render(screen, ui::curve({0, 0, 1, 1, 1, 1}, 2, ui::theme::cyan(), 0));
        CHECK_EQ(screen.PixelAt(0, 1).character, std::string("⣸"));   // along the bottom, then up (Bresenham)
        CHECK_EQ(screen.PixelAt(1, 0).character, std::string("⡏"));
        CHECK_EQ(screen.PixelAt(2, 0).character, std::string("⠉"));   // along the top
        CHECK(screen.PixelAt(2, 1).background_color == screen.PixelAt(1, 1).background_color);   // under it: tinted
        CHECK(!(screen.PixelAt(2, 1).background_color == ftxui::Color()));
    }

    // sstat: the CPU time between two readings at least 6 s apart (2 CPUs here: 10 s), memory as it is
    ui::MonitorView m;
    m.cpus = 2;
    api::LiveUse a;
    a.at = 1000;
    a.cpu_seconds = 100;
    a.mem_mib = 2048;
    m.setLive(a, "");
    CHECK(m.cpu.empty());   // one reading: nothing to compare with
    a.at = 1012;
    a.cpu_seconds = 112;    // 12 s of CPU over 12 s on 2 CPUs
    m.setLive(a, "");
    CHECK_EQ(m.cpu.last(), 0.5);
    CHECK_EQ(m.mem.last(), 2048.0);
    a.at = 1014;
    a.cpu_seconds = 5;      // a step ended: start again from here
    m.setLive(a, "");
    CHECK_EQ(m.cpu.last(), 0.5);
    m.setLive(a, "sstat: error");
    CHECK_EQ(m.cpu_error, std::string("sstat: error"));

    // GPUs together: the mean use and the memory of all, from each one's last line
    api::GpuUse g0, g1;
    g0.node = g1.node = "romeo-a012";
    g1.index = 1;
    g0.util = 80;
    g1.util = 20;
    g0.mem_used_mib = g1.mem_used_mib = 25;
    g0.mem_total_mib = g1.mem_total_mib = 100;
    m.addGpu(g0, 2000);
    m.addGpu(g1, 2000.1);
    CHECK_EQ(m.gpu_tracks.size(), 2u);
    CHECK_EQ(m.gpu.last(), 0.5);
    CHECK_EQ(m.gpu_mem.last(), 0.25);
    CHECK_EQ(m.gpu.points.size(), 1u);   // the lines of one answer are one point

    // A stream: each line as it comes, then how it ended; stop() ends one that keeps going
    {
        std::vector<std::string> lines;
        std::string end = "?";
        std::mutex mu;
        api::LineStream stream;
        stream.start({"sh", "-c", "echo one; echo two; exit 3"}, [&](const std::string& l) { std::lock_guard<std::mutex> lock(mu); lines.push_back(l); },
                     [&](const std::string& e) { std::lock_guard<std::mutex> lock(mu); end = e; });
        for (int i = 0; i < 300 && stream.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stream.stop();
        CHECK_EQ(lines.size(), 2u);
        if (lines.size() == 2) CHECK_EQ(lines[1], std::string("two"));
        CHECK(end.find("exit status 3") != std::string::npos);

        end = "?";
        const auto t0 = std::chrono::steady_clock::now();
        stream.start({"sh", "-c", "while :; do echo tick; sleep 0.05; done"}, [](const std::string&) {},
                     [&](const std::string& e) { std::lock_guard<std::mutex> lock(mu); end = e; });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(stream.running());
        stream.stop();
        CHECK(!stream.running());
        CHECK_EQ(end, std::string(""));   // stopped: no error
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    }
}

int main() {
    durations();
    widths();
    hostlists();
    squeue();
    details();
    nodes();
    partitions();
    queue();
    sacct();
    consumption();
    assocs();
    cluster_extras();
    usage();
    logs();
    node_hardware();
    arrays();
    monitor();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
