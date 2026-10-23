// Parsing of Slurm output as printed on ROMEO (Slurm 23.11), names and accounts made up.
// Build with cmake -DRSV_TESTS=ON, run with ctest (or ./parse_test).

#include <cstdio>
#include <string>

#include "api/cluster.hpp"
#include "api/slurmjobs.hpp"
#include "api/usage.hpp"
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
        "/gpfs/home/demo/run/slurm-900101.out|/gpfs/home/demo/run|my job ^ with a bar|\n"
        "900200_1|900202|RUNNING|long|proj-b|normal|1|16|cpu=16,mem=64G,node=1,billing=16|1-09:22:51|7-00:00:00|5-14:37:09|"
        "2026-10-22T13:14:02|2026-10-29T12:14:02|2026-10-22T13:14:01|None|romeo-c002|(null)|/home/demo/a.out|/home/demo|sim|\n"
        "812402_[3-40%4]|812402|PENDING|instant|proj-b|normal|1|8|cpu=8,mem=16G,node=1,billing=8|0:00|01:00:00|01:00:00|"
        "N/A|N/A|2026-10-22T13:14:01|QOSMaxJobsPerUserLimit||afterok:1(unfulfilled)|/home/demo/s.out|/home/demo|sweep|\n"
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
    CHECK_EQ(ui::taskCount(jobs[2].id), 38);

    // A '|' in a path no longer shifts the fields after it
    std::string line = sep("1|1|RUNNING|short|p|normal|1|1|cpu=1|0:01|1:00|0:59|N/A|N/A|N/A|None|n1|(null)|/w/a.out|");
    line += "/home/demo/a|b";
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
    assocs();
    cluster_extras();
    usage();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
