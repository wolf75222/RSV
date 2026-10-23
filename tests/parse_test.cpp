// Parsing of Slurm output as printed on ROMEO (Slurm 23.11), names and accounts made up.
// Build with cmake -DRSV_TESTS=ON, run with ctest (or ./parse_test).

#include <cstdio>
#include <string>

#include "api/slurmjobs.hpp"
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
        "/gpfs/home/demo/run/slurm-900101.out|/gpfs/home/demo/run|my job | with a bar|\n"
        "900200_1|900202|RUNNING|long|proj-b|normal|1|16|cpu=16,mem=64G,node=1,billing=16|1-09:22:51|7-00:00:00|5-14:37:09|"
        "2026-10-22T13:14:02|2026-10-29T12:14:02|2026-10-22T13:14:01|None|romeo-c002|(null)|/home/demo/a.out|/home/demo|sim|\n"
        "812402_[3-40%4]|812402|PENDING|instant|proj-b|normal|1|8|cpu=8,mem=16G,node=1,billing=8|0:00|01:00:00|01:00:00|"
        "N/A|N/A|2026-10-22T13:14:01|QOSMaxJobsPerUserLimit||afterok:1(unfulfilled)|/home/demo/s.out|/home/demo|sweep|\n"
        "slurm_load_jobs error: something\n";
    const auto jobs = api::parseSqueue(out);
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
    CHECK_EQ(ui::taskCount(jobs[2].id), 38);
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
        "instant*|up|1:00:00|romeo-a001|idle|0/288/0/288|gpu:h100:4|gpu:h100:0(IDX:N/A)|\n"
        "instant*|up|1:00:00|romeo-a002|mix|128/160/0/288|gpu:h100:4|gpu:h100:2(IDX:0-1)|\n"
        "instant*|up|1:00:00|romeo-c001|drain*|0/0/192/192|(null)|gpu:0|\n"
        "long|up|30-00:00:00|romeo-c002|alloc|192/0/0/192|(null)|gpu:0|\n";
    const auto p = api::parsePartitions(out);
    CHECK_EQ(p.size(), 2u);
    if (p.size() < 2) return;
    CHECK_EQ(p[0].name, std::string("instant"));
    CHECK(p[0].is_default);
    CHECK_EQ(p[0].nodes, 3);
    CHECK_EQ(p[0].idle, 1);
    CHECK_EQ(p[0].mixed, 1);
    CHECK_EQ(p[0].unavailable, 1);
    CHECK_EQ(p[0].gpus_total, 8);
    CHECK_EQ(p[0].gpus_used, 2);
    CHECK_EQ(p[0].cpus_total, 768);
    CHECK_EQ(p[1].allocated, 1);
}

static void sacct() {
    const std::string out =
        "900300_12|COMPLETED|instant|proj-a|2026-10-23T10:00:00|2026-10-23T10:00:01|2026-10-23T10:00:14|00:00:13|01:00:00|1|1|8G|"
        "billing=1,cpu=1,mem=8G,node=1|00:11.237|13||0:0|romeo-c001|my | job\n"
        "900300_12.batch|COMPLETED||proj-a|2026-10-23T10:00:01|2026-10-23T10:00:01|2026-10-23T10:00:14|00:00:13||1|1||"
        "cpu=1,mem=8G,node=1|00:11.236|13|324M|0:0|romeo-c001|batch\n"
        "900300_12.extern|COMPLETED||proj-a|2026-10-23T10:00:01|2026-10-23T10:00:01|2026-10-23T10:00:14|00:00:13||1|1||"
        "cpu=1,mem=8G,node=1|00:00.001|13|0|0:0|romeo-c001|extern\n"
        "900310|CANCELLED by 4242|short|proj-a|2026-10-23T11:00:00|None|2026-10-23T11:05:00|00:00:00|02:00:00|1|16|64G|"
        "billing=16,cpu=16,mem=64G,node=1|00:00:00|0||0:0|None assigned|later\n";
    const auto h = api::parseSacct(out);
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

int main() {
    durations();
    widths();
    hostlists();
    squeue();
    details();
    nodes();
    partitions();
    sacct();
    assocs();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
