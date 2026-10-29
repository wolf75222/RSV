#!/usr/bin/env python3
"""Fake Slurm commands for testing RSV anywhere, with made-up jobs (no real user, account or project).

bin/ holds one wrapper per command (squeue, scontrol, sinfo, sacct, sacctmgr, scancel): put it first in PATH.

    PATH=tests/fake-slurm/bin:$PATH USER=demo ./build/rsv

Environment:
    FAKE_SLURM_SCENARIO  demo (default), empty, many (300 jobs), unicode (CJK and emoji names)
    FAKE_SLURM_STATE     folder for logs and cancelled jobs (default: $TMPDIR/fake-slurm-$USER)
    FAKE_SLURM_HOME      folder shown as the user's home in paths (default: $FAKE_SLURM_STATE/home)
    FAKE_SLURM_DELAY     seconds to wait before answering (a slow or hung slurmctld); also read from
                         $FAKE_SLURM_STATE/delay when that file exists, to make Slurm slow while a test runs
    FAKE_SLURM_FAIL      commands that fail, comma separated (e.g. squeue,sacct)

scancel never cancels anything real: it marks the job as cancelled in the state folder and logs the call.
"""

import json
import math
import os
import re
import sys
import time

NOW = int(time.time())
CMD = sys.argv[1] if len(sys.argv) > 1 else ""
ARGS = sys.argv[2:]

USER = os.environ.get("USER", "demo")
STATE = os.environ.get("FAKE_SLURM_STATE") or os.path.join(os.environ.get("TMPDIR", "/tmp"), "fake-slurm-" + USER)
HOME = os.environ.get("FAKE_SLURM_HOME") or os.path.join(STATE, "home")
SCENARIO = os.environ.get("FAKE_SLURM_SCENARIO", "demo")

MIN = 60
HOUR = 3600
DAY = 86400


def fail(message, code=1):
    sys.stderr.write(message + "\n")
    sys.exit(code)


try:
    os.makedirs(STATE, exist_ok=True)
    with open(os.path.join(STATE, "calls.log"), "a") as calls:
        calls.write("%.3f %s %s\n" % (time.time(), CMD, " ".join(ARGS)))
except OSError:
    pass

if os.environ.get("FAKE_SLURM_DELAY"):
    time.sleep(float(os.environ["FAKE_SLURM_DELAY"]))
try:
    with open(os.path.join(STATE, "delay")) as delay:
        time.sleep(float(delay.read().strip() or 0))
except (OSError, ValueError):
    pass
if CMD in os.environ.get("FAKE_SLURM_FAIL", "").split(","):
    fail("%s: error: Socket timed out on send/recv operation" % CMD)


def anchor():
    """When this fake cluster was first asked: running jobs started before it, so their run and CPU times grow"""
    path = os.path.join(STATE, "anchor")
    try:
        with open(path) as f:
            return int(f.read())
    except (OSError, ValueError):
        try:
            with open(path, "w") as f:
                f.write(str(NOW))
        except OSError:
            pass
        return NOW


ANCHOR = anchor()


# ---------------------------------------------------------------- hostlists

def expand(hostlist):
    out = []
    for part in re.findall(r"[^,\[]+(?:\[[^\]]*\])?[^,]*", hostlist):
        m = re.match(r"^([^\[]*)\[([^\]]*)\](.*)$", part)
        if not m:
            out.append(part)
            continue
        prefix, ranges, suffix = m.groups()
        for r in ranges.split(","):
            if "-" in r:
                a, b = r.split("-")
                for i in range(int(a), int(b) + 1):
                    out.append("%s%0*d%s" % (prefix, len(a), i, suffix))
            else:
                out.append(prefix + r + suffix)
    return out


def compress(hosts):
    groups = {}
    order = []
    for h in hosts:
        m = re.match(r"^(.*?)(\d+)$", h)
        if not m:
            groups.setdefault((h, None), [])
            order.append((h, None))
            continue
        key = (m.group(1), len(m.group(2)))
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(int(m.group(2)))
    out = []
    for key in dict.fromkeys(order):
        prefix, width = key
        if width is None:
            out.append(prefix)
            continue
        nums = sorted(groups[key])
        if len(nums) == 1:
            out.append("%s%0*d" % (prefix, width, nums[0]))
            continue
        ranges = []
        start = prev = nums[0]
        for n in nums[1:] + [None]:
            if n is not None and n == prev + 1:
                prev = n
                continue
            ranges.append("%0*d" % (width, start) if start == prev else "%0*d-%0*d" % (width, start, width, prev))
            if n is not None:
                start = prev = n
        out.append("%s[%s]" % (prefix, ",".join(ranges)))
    return ",".join(out)


def cpu_ranges(ids):
    ids = sorted(ids)
    out = []
    i = 0
    while i < len(ids):
        j = i
        while j + 1 < len(ids) and ids[j + 1] == ids[j] + 1:
            j += 1
        out.append(str(ids[i]) if i == j else "%d-%d" % (ids[i], ids[j]))
        i = j + 1
    return ",".join(out)


# ---------------------------------------------------------------- the cluster

def make_nodes():
    nodes = {}
    for i in range(1, 59):
        nodes["romeo-a%03d" % i] = dict(arch="aarch64", cpus=288, sockets=4, cores=72, gpus=4, gpu_type="h100",
                                         mem=820802, features="armgpu", state="IDLE", load=0.0,
                                         others_cpus=0, others_gpus=0, others_mem=0)
    for i in list(range(1, 41)) + list(range(101, 105)):
        nodes["romeo-c%03d" % i] = dict(arch="x86_64", cpus=192, sockets=2, cores=96, gpus=0, gpu_type="",
                                         mem=1547701 if i > 100 else 1160629, features="x64cpu", state="IDLE", load=0.0,
                                         others_cpus=0, others_gpus=0, others_mem=0)
    # Not every node is in every partition, as on ROMEO (the others' jobs below stay in theirs)
    for i in range(41, 59):
        nodes["romeo-a%03d" % i]["partitions"] = "short,instant" if i < 57 else "instant"
    for i in range(26, 41):
        nodes["romeo-c%03d" % i]["partitions"] = "short,instant" if i < 40 else "instant"
    # Other users' jobs, so that the cluster looks used
    busy = {"romeo-a%03d" % i: (288, 4) for i in range(30, 44)}
    busy.update({"romeo-a%03d" % i: (128, 2) for i in range(44, 50)})
    busy.update({"romeo-c%03d" % i: (192, 0) for i in range(11, 26)})
    busy.update({"romeo-c%03d" % i: (120, 0) for i in range(26, 34)})
    busy["romeo-c010"] = (96, 0)
    # A cluster that moves: romeo-a[050-055] fill up and empty again every minute (the overview's curves)
    wave = (NOW // 5) % 12
    for k in range(wave if wave <= 6 else 12 - wave):
        busy["romeo-a%03d" % (50 + k)] = (288, 4)
    for name, (cpus, gpus) in busy.items():
        nodes[name].update(others_cpus=cpus, others_gpus=gpus, others_mem=cpus * 2048)
    for name in ("romeo-a057", "romeo-c040"):
        nodes[name]["state"] = "DOWN*"
    nodes["romeo-a058"]["state"] = "DRAINED"
    return nodes


def job(**kw):
    j = dict(id=None, job_id=None, array_job=None, array_task=None, name="job", state="PENDING", reason="Priority",
             partition="short", account="proj-cfd", qos="normal", alloc=[], num_nodes=1, num_cpus=1, gpus=0,
             mem_per_node=4096, limit=HOUR, elapsed=0, start=0, end=0, submit=NOW - 10 * MIN, workdir=HOME,
             stdout=None, stderr=None, dependency="(null)", exit="0:0", command=None)
    j.update(kw)
    j["job_id"] = j["job_id"] or j["id"]
    if j["state"] in ("RUNNING", "COMPLETING") and not j["start"]:
        j["start"] = ANCHOR - j["elapsed"]
    if j["start"] and j["state"] != "PENDING":
        j["submit"] = min(j["submit"], j["start"] - 5 * MIN)
    if j["state"] == "RUNNING":
        j["end"] = j["start"] + j["limit"]
    j["command"] = j["command"] or os.path.join(j["workdir"], "job.sh")
    out = j["stdout"] or os.path.join(j["workdir"], "slurm-%j.out")
    j["stdout"] = out
    j["stderr"] = j["stderr"] or out
    return j


def alloc(hosts, cpus, gpus="", mem=0):
    return dict(hosts=hosts, cpus=cpus, gpus=gpus, mem=mem)


def demo_jobs():
    runs = os.path.join(HOME, "runs")
    jobs = [
        job(id="812345", name="lbm-d3q19", state="RUNNING", reason="None", partition="short", account="proj-cfd",
            alloc=[alloc("romeo-a[012-013]", "0-15,72-87,144-159,216-231", "0-3", 262144)],
            num_nodes=2, num_cpus=128, gpus=8, mem_per_node=262144, limit=12 * HOUR, elapsed=3 * HOUR + 12 * MIN + 45,
            submit=NOW - 3 * HOUR - 20 * MIN, workdir=os.path.join(runs, "lbm"),
            stdout=os.path.join(runs, "lbm", "lbm-%j.out"), stderr=os.path.join(runs, "lbm", "lbm-%j.err")),
        job(id="812350", name="train resnet50 (fp16)", state="RUNNING", reason="None", partition="short",
            account="proj-ml", alloc=[alloc("romeo-a027", "0-31", "0-1", 122880)], num_cpus=32, gpus=2,
            mem_per_node=122880, limit=4 * HOUR, elapsed=45 * MIN + 10, workdir=os.path.join(runs, "resnet")),
        job(id="812377", name="motorBike-simpleFoam", state="RUNNING", reason="None", partition="long",
            alloc=[alloc("romeo-c[004-007]", "0-191", "", 1024000)], num_nodes=4, num_cpus=768,
            mem_per_node=1024000, limit=3 * DAY, elapsed=DAY + 5 * HOUR + 22 * MIN, workdir=os.path.join(runs, "motorbike")),
        job(id="812402_[159-200%8]", job_id="812402", array_job="812402", array_task="159-200%8", name="param-sweep",
            state="PENDING", reason="JobArrayTaskLimit", partition="instant", num_cpus=8, mem_per_node=16384,
            limit=HOUR, start=NOW + 25 * MIN, workdir=os.path.join(runs, "sweep"),
            stdout=os.path.join(runs, "sweep", "sweep-%A_%a.out")),
        job(id="812455", name="post-process", state="PENDING", reason="Dependency", partition="short",
            dependency="afterok:812345(unfulfilled)", num_cpus=16, mem_per_node=65536, limit=2 * HOUR,
            workdir=os.path.join(runs, "lbm")),
        job(id="812460", name="weak-scaling-1024-ranks-final-v2", state="PENDING", reason="Resources",
            partition="long", account="proj-cfd", num_nodes=16, num_cpus=4608, gpus=64, mem_per_node=768000,
            limit=DAY, start=NOW + DAY + 4 * HOUR, workdir=os.path.join(runs, "scaling")),
        job(id="812301", name="mesh-refine", state="COMPLETED", reason="None", partition="instant",
            alloc=[alloc("romeo-c003", "0-63", "", 131072)], num_cpus=64, mem_per_node=131072, limit=HOUR,
            start=NOW - 40 * MIN, end=NOW - 3 * MIN, elapsed=37 * MIN, workdir=os.path.join(runs, "mesh")),
    ]
    return jobs + array_tasks()


# The tasks of array 812402 squeue still has: 8 running, the last 2 ended (the others ended long ago: sacct only)
ARRAY_FAILED = {17: ("FAILED", "1:0"), 43: ("FAILED", "1:0"), 77: ("OUT_OF_MEMORY", "0:125"), 98: ("FAILED", "2:0"),
                121: ("TIMEOUT", "0:0")}


def array_tasks():
    runs = os.path.join(HOME, "runs")
    tasks = []
    for t in range(149, 159):
        running = t >= 151
        tasks.append(job(id="812402_%d" % t, job_id=str(812402 + t), array_job="812402", array_task=str(t),
                         name="param-sweep", state="RUNNING" if running else "COMPLETED", reason="None", partition="instant",
                         alloc=[alloc("romeo-c%03d" % (10 + (t - 149) // 4), "%d-%d" % (8 * (t % 4), 8 * (t % 4) + 7), "", 16384)],
                         num_cpus=8, mem_per_node=16384, limit=HOUR, elapsed=(t - 148) * 3 * MIN if running else 41 * MIN,
                         start=0 if running else NOW - 44 * MIN, end=0 if running else NOW - (153 - t) * MIN,
                         workdir=os.path.join(runs, "sweep"), stdout=os.path.join(runs, "sweep", "sweep-%A_%a.out")))
    return tasks


def colleague_jobs():
    """Jobs of other people in the user's accounts (proj-cfd, proj-ml), for the project view (squeue -A)"""
    jobs = []
    for i, (user, name, state, account, nodes, cpus, gpus) in enumerate([
            ("lea", "les-channel-re590", "RUNNING", "proj-cfd", "romeo-a[044-045]", 576, 8),
            ("tom", "pinn-burgers", "RUNNING", "proj-ml", "romeo-a046", 72, 1),
            ("tom", "pinn-burgers-sweep", "PENDING", "proj-ml", "", 72, 1),
            ("lea", "post-les", "PENDING", "proj-cfd", "", 32, 0)]):
        j = job(id=str(811900 + i), name=name, state=state, reason="None" if state == "RUNNING" else "Priority",
                partition="short", account=account, num_nodes=2 if "[" in nodes else 1, num_cpus=cpus, gpus=gpus,
                alloc=[alloc(nodes, "0-%d" % (cpus // max(1, 2 if "[" in nodes else 1) - 1), "0-%d" % (gpus - 1) if gpus else "",
                             262144)] if nodes else [],
                limit=DAY, elapsed=(i + 2) * HOUR if state == "RUNNING" else 0, start=0 if state == "RUNNING" else NOW + 3 * HOUR,
                workdir="/home/" + user)
        j["user"] = user
        jobs.append(j)
    return jobs


def many_jobs():
    jobs = []
    for i in range(300):
        state = "RUNNING" if i % 3 == 0 else "PENDING"
        node = "romeo-c%03d" % (1 + i % 40)
        jobs.append(job(id=str(900000 + i), name="case-%03d" % i, state=state,
                        reason="None" if state == "RUNNING" else ("Priority" if i % 2 else "Resources"),
                        partition=("short", "long", "instant")[i % 3],
                        alloc=[alloc(node, "%d-%d" % (8 * (i % 24), 8 * (i % 24) + 7), "", 8192)] if state == "RUNNING" else [],
                        num_cpus=8, mem_per_node=8192, limit=4 * HOUR, elapsed=(i * 97) % (3 * HOUR) if state == "RUNNING" else 0,
                        start=0 if state == "RUNNING" else NOW + (i % 50) * 7 * MIN))
    return jobs


def unicode_jobs():
    jobs = demo_jobs()
    jobs[0]["name"] = "数値流体-シミュレーション"
    jobs[1]["name"] = "🚀 training run ✨"
    jobs[2]["name"] = "étude-écoulement-très-long-nom-de-job"
    jobs[6]["name"] = "Ω-analysis ​zero-width"
    return jobs


SCENARIOS = {"demo": demo_jobs, "empty": lambda: [], "many": many_jobs, "unicode": unicode_jobs}


def other_jobs():
    """Other users' jobs, on the nodes make_nodes() marks as busy: squeue shows them without -u"""
    jobs = []

    def add(user, n, **kw):
        for i in range(n):
            j = job(id=str(810000 + len(jobs)), account="proj-" + user[:3], workdir="/home/" + user, **kw)
            j["user"] = user
            if callable(kw.get("alloc")):
                j["alloc"] = kw["alloc"](i)
            jobs.append(j)

    for i in range(7):   # romeo-a[030-043]: full, 2 nodes each
        add("alice", 1, name="md-run", state="RUNNING", reason="None", partition="long",
            alloc=[alloc("romeo-a[%03d-%03d]" % (30 + 2 * i, 31 + 2 * i), "0-287", "0-3", 600000)],
            num_nodes=2, num_cpus=576, gpus=8, limit=2 * DAY, elapsed=(i + 3) * HOUR)
    for i in range(6):   # romeo-a[044-049]: 128 cores and 2 GPUs each
        add("bmartin", 1, name="fine-tune", state="RUNNING", reason="None", partition="short",
            alloc=[alloc("romeo-a%03d" % (44 + i), "0-127", "0-1", 200000)], num_cpus=128, gpus=2, limit=8 * HOUR,
            elapsed=(i + 1) * 40 * MIN)
    for i in range(15):  # romeo-c[011-025]: full
        add("chen", 1, name="cfd-%02d" % i, state="RUNNING", reason="None", partition="long",
            alloc=[alloc("romeo-c%03d" % (11 + i), "0-191", "", 900000)], num_cpus=192, limit=3 * DAY, elapsed=(i + 2) * HOUR)
    for i in range(8):   # romeo-c[026-033]: 120 cores
        add("dupont", 1, name="qe-scf", state="RUNNING", reason="None", partition="short",
            alloc=[alloc("romeo-c%03d" % (26 + i), "0-119", "", 400000)], num_cpus=120, limit=HOUR * 20, elapsed=(i + 1) * 25 * MIN)
    add("erik", 1, name="postproc", state="RUNNING", reason="None", partition="instant",
        alloc=[alloc("romeo-c010", "16-111", "", 100000)], num_cpus=96, limit=HOUR, elapsed=20 * MIN)
    add("fatou", 4, name="gromacs", state="PENDING", reason="Priority", partition="long", num_nodes=4, num_cpus=1152,
        gpus=16, limit=DAY, start=NOW + 3 * HOUR)
    add("hugo", 2, name="big-scaling", state="PENDING", reason="Resources", partition="long", num_nodes=16, num_cpus=4608,
        gpus=64, limit=12 * HOUR, start=NOW + DAY)
    j = job(id="810100_[1-30]", job_id="810100", array_job="810100", array_task="1-30", name="sweep", state="PENDING",
            reason="QOSMaxJobsPerUserLimit", partition="instant", num_cpus=8, limit=HOUR, account="proj-giu", workdir="/home/giulia")
    j["user"] = "giulia"
    jobs.append(j)
    return jobs


def history():
    rows = []

    # ago_end None: still running (no end, no CPU time recorded yet); elapsed None: still pending
    def add(jid, name, state, ago_end, elapsed, limit, ncpus, nnodes, req_mem, maxrss, totalcpu, exitcode="0:0",
            partition="short", nodes="romeo-c001", gpus=0):
        end = None if ago_end is None else NOW - ago_end
        start = None if elapsed is None else (end or NOW) - elapsed
        rows.append(dict(JobID=jid, JobName=name, State=state, Partition=partition, Account="proj-cfd",
                         Submit=(start or NOW) - 120, Start=start, End=end, Elapsed=elapsed or 0, Timelimit=limit,
                         NNodes=nnodes, NCPUs=ncpus, ReqMem=req_mem, TotalCPU=totalcpu, MaxRSS=maxrss,
                         ExitCode=exitcode, NodeList=nodes, GPUs=gpus))

    add("812345", "lbm-d3q19", "RUNNING", None, 3 * HOUR + 12 * MIN + 45, 12 * HOUR, 128, 2, "256G", "", 0,
        nodes="romeo-a[012-013]", gpus=8)
    add("812460", "weak-scaling-1024-ranks-final-v2", "PENDING", None, None, DAY, 4608, 16, "750G", "", 0,
        partition="long", nodes="None assigned", gpus=64)

    add("812301", "mesh-refine", "COMPLETED", 3 * MIN, 37 * MIN, HOUR, 64, 1, "128G", "41G", 37 * MIN * 64 * 0.93,
        partition="instant", nodes="romeo-c003")
    add("812290", "lbm-d3q19", "TIMEOUT", 2 * HOUR, 12 * HOUR, 12 * HOUR, 128, 2, "256G", "198G", 12 * HOUR * 128 * 0.97,
        "0:15", nodes="romeo-a[020-021]", gpus=8)
    add("812276", "train resnet50 (fp16)", "COMPLETED", 5 * HOUR, 3 * HOUR + 40 * MIN, 4 * HOUR, 32, 1, "120G", "87G",
        (3 * HOUR + 40 * MIN) * 32 * 0.41, nodes="romeo-a027", gpus=2)
    add("812260", "post-process", "OUT_OF_MEMORY", 9 * HOUR, 14 * MIN, 2 * HOUR, 16, 1, "64G", "64G", 14 * MIN * 16 * 0.88,
        "0:125", nodes="romeo-c008")
    add("812244", "motorBike-simpleFoam", "FAILED", DAY + 2 * HOUR, 25 * MIN, 3 * DAY, 768, 4, "1000G", "302G",
        25 * MIN * 768 * 0.95, "1:0", partition="long", nodes="romeo-c[004-007]")
    add("812231", "param-sweep", "CANCELLED by 4242", DAY + 6 * HOUR, 2 * MIN, HOUR, 8, 1, "16G", "1.2G", 2 * MIN * 8 * 0.99,
        "0:0", partition="instant", nodes="romeo-c010")
    for k in range(6):
        add("812100_%d" % k, "param-sweep", "COMPLETED", 2 * DAY + k * 600, 18 * MIN + k * 30, HOUR, 8, 1, "16G",
            "%.1fG" % (5.5 + k * 0.3), (18 * MIN + k * 30) * 8 * (0.97 - k * 0.01), partition="instant",
            nodes="romeo-c%03d" % (10 + k))
    add("812050", "compile-deps", "COMPLETED", 4 * DAY, 9 * MIN, 30 * MIN, 32, 1, "8G", "2.1G", 9 * MIN * 32 * 0.32,
        partition="instant", nodes="romeo-c002")
    add("812001", "lbm-d3q19", "COMPLETED", 6 * DAY, 11 * HOUR + 5 * MIN, 12 * HOUR, 128, 2, "256G", "201G",
        (11 * HOUR + 5 * MIN) * 128 * 0.98, nodes="romeo-a[012-013]", gpus=8)
    return rows


# ---------------------------------------------------------------- state (cancelled jobs, log files)

def load_state():
    try:
        with open(os.path.join(STATE, "state.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"cancelled": {}}


def save_state(state):
    os.makedirs(STATE, exist_ok=True)
    with open(os.path.join(STATE, "state.json"), "w") as f:
        json.dump(state, f)


def expand_path(path, j):
    base = j["array_job"] or j["job_id"]
    task = j["array_task"] or "4294967294"
    out = path.replace("%A", base).replace("%a", task).replace("%j", j["job_id"]).replace("%x", j["name"])
    return out.replace("%u", USER)


def train_log(f):
    """A training run that ran out of GPU memory once, then went on with smaller batches"""
    for step in range(100, 2100, 100):
        f.write("epoch 1/90  step %5d/5004  loss=%.4f  lr=0.1000  img/s=%d\n"
                % (step, 6.9 - step * 2.1e-4, 2790 + (step * 13) % 40))
    f.write("Traceback (most recent call last):\n"
            "  File \"train.py\", line 212, in <module>\n"
            "    loss.backward()\n"
            "torch.OutOfMemoryError: CUDA out of memory. Tried to allocate 2.00 GiB. GPU 1 has 94.50 GiB in total\n"
            "retrying epoch 1 with batch size 128\n")
    for step in range(100, 4100, 100):
        f.write("epoch 1/90  step %5d/10008  loss=%.4f  lr=0.0500  img/s=%d\n"
                % (step, 6.9 - step * 1.1e-4, 2610 + (step * 7) % 30))


def make_logs(jobs):
    for j in jobs:
        if j["state"] not in ("RUNNING", "COMPLETED", "COMPLETING"):
            continue
        out = expand_path(j["stdout"], j)
        if not os.path.exists(out):
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "w") as f:
                f.write("Starting %s on %s\n" % (j["name"], j["alloc"][0]["hosts"] if j["alloc"] else "?"))
                if "resnet" in j["name"]:
                    train_log(f)
                    continue
                for step in range(0, 60):
                    f.write("step %6d  t=%.4e  residual=%.3e  MLUPS=%7.1f\n"
                            % (step * 200, step * 2.5e-5, 1.0 / (step + 1) ** 1.5, 2400 + (step * 37) % 90))
                    # Made-up errors for the log view: a task of the sweep diverges once
                    if j["array_task"] == "153" and step == 40:
                        f.write("case 153: residual is NaN at step 8000\n"
                                "error: solver diverged, restarting from checkpoint 7800 with dt/2\n")
                if j["state"] == "COMPLETED" and "mesh" in j["name"]:
                    f.write("refinement done: 2.4M cells, 0 errors, 3 warnings\nTotal errors: 0\n")
        err = expand_path(j["stderr"], j)
        if err != out and not os.path.exists(err):
            with open(err, "w") as f:
                f.write("warning: \x1b[33mGPU clock throttled\x1b[0m\n\tretrying with smaller tile\n")


def all_jobs():
    jobs = SCENARIOS.get(SCENARIO, demo_jobs)()
    state = load_state()
    for j in jobs:
        if j["state"] == "PENDING" and ({j["id"], re.sub(r"%\d+", "", j["id"]), j["array_job"]} & set(state.get("held", {}))):
            j["reason"] = "JobHeldUser"
        if j["id"] in state["cancelled"] or j["job_id"] in state["cancelled"] or (
                j["array_job"] in state["cancelled"] and j["state"] in ("RUNNING", "PENDING")):
            when = state["cancelled"].get(j["id"]) or state["cancelled"].get(j["job_id"]) or state["cancelled"].get(j["array_job"])
            j["state"] = "CANCELLED"
            j["reason"] = "None"
            j["end"] = when
            if not j["start"] or j["start"] > when:
                j["start"] = when
            j["elapsed"] = max(0, when - j["start"])
    # Slurm forgets finished jobs after MinJobAge (300 s)
    jobs = [j for j in jobs if j["state"] in ("RUNNING", "PENDING", "COMPLETING") or NOW - j["end"] < 300]
    try:
        make_logs(jobs)
    except OSError:
        pass
    return jobs


# ---------------------------------------------------------------- formatting

def duration(s):
    if s is None or s < 0:
        return "INVALID"
    s = int(s)
    d, s = divmod(s, DAY)
    h, s = divmod(s, HOUR)
    m, s = divmod(s, MIN)
    return ("%d-%02d:%02d:%02d" % (d, h, m, s)) if d else ("%d:%02d:%02d" % (h, m, s) if h else "%d:%02d" % (m, s))


def limit_str(s):
    d, s = divmod(int(s), DAY)
    h, s = divmod(s, HOUR)
    m, s = divmod(s, MIN)
    return ("%d-%02d:%02d:%02d" % (d, h, m, s)) if d else "%02d:%02d:%02d" % (h, m, s)


def stamp(t):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(t)) if t else "N/A"


def mem(mb):
    return "%dG" % (mb // 1024) if mb % 1024 == 0 else "%dM" % mb


def node_names(j):
    hosts = []
    for a in j["alloc"]:
        hosts += expand(a["hosts"])
    return hosts


def tres(j):
    out = "cpu=%d,mem=%s,node=%d,billing=%d" % (j["num_cpus"], mem(j["mem_per_node"] * j["num_nodes"]), j["num_nodes"], j["num_cpus"])
    if j["gpus"]:
        out += ",gres/gpu=%d" % j["gpus"]
    return out


def squeue_field(j, name):
    name = name.lower()
    running = j["state"] in ("RUNNING", "COMPLETING")
    used = j["elapsed"] if not running else NOW - j["start"]
    values = {
        "jobarrayid": j["id"], "jobid": j["job_id"], "arrayjobid": j["array_job"] or j["job_id"],
        "arraytaskid": j["array_task"] or "N/A", "name": j["name"], "state": j["state"],
        "statecompact": {"RUNNING": "R", "PENDING": "PD", "COMPLETING": "CG", "COMPLETED": "CD", "CANCELLED": "CA"}.get(j["state"], "?"),
        "partition": j["partition"], "account": j["account"], "qos": j["qos"], "username": j.get("user", USER),
        "user": j.get("user", USER),
        "numnodes": str(j["num_nodes"]), "numcpus": str(j["num_cpus"]), "tres-alloc": tres(j),
        "timeused": duration(used if j["state"] != "PENDING" else 0), "timelimit": limit_str(j["limit"]),
        "timeleft": duration(j["limit"] - used) if running else limit_str(j["limit"]),
        "starttime": stamp(j["start"]), "endtime": stamp(j["end"] if j["state"] != "PENDING" else (j["start"] + j["limit"] if j["start"] else 0)),
        "submittime": stamp(j["submit"]), "reason": j["reason"],
        "nodelist": compress(node_names(j)) if j["state"] != "PENDING" else "",
        "reasonlist": compress(node_names(j)) if j["state"] != "PENDING" else "(%s)" % j["reason"],
        "dependency": j["dependency"], "stdout": expand_path(j["stdout"], j), "stderr": expand_path(j["stderr"], j),
        "workdir": j["workdir"], "command": j["command"], "minmemory": mem(j["mem_per_node"]),
        "tres-per-node": "gres/gpu:%d" % (j["gpus"] // j["num_nodes"]) if j["gpus"] else "N/A",
    }
    return values.get(name, "N/A")


LETTERS = {"i": "jobarrayid", "A": "jobid", "j": "name", "T": "state", "t": "statecompact", "P": "partition",
           "a": "account", "q": "qos", "u": "user", "D": "numnodes", "C": "numcpus", "M": "timeused",
           "l": "timelimit", "L": "timeleft", "S": "starttime", "e": "endtime", "V": "submittime", "r": "reason",
           "R": "reasonlist", "N": "nodelist", "b": "tres-per-node", "Z": "workdir", "m": "minmemory", "E": "dependency"}


def format_o(fmt, value_of):
    def repl(m):
        right, width, letter = m.group(1), m.group(2), m.group(3)
        if letter == "%":
            return "%"
        v = value_of(LETTERS.get(letter, "?"))
        if width:
            w = int(width)
            v = v[:w]
            v = v.rjust(w) if right else v.ljust(w)
        return v
    return re.sub(r"%(\.)?(\d+)?([A-Za-z%])", repl, fmt)


def format_O(fmt, value_of):
    out = ""
    for spec in fmt.split(","):
        name, _, rest = spec.partition(":")
        m = re.match(r"^(\.)?(\d*)(.*)$", rest)
        right, width, suffix = m.groups()
        v = value_of(name)
        w = int(width) if width else (0 if suffix else 20)
        if w:
            v = v[:w]
            v = v.rjust(w) if right else v.ljust(w)
        out += v + suffix
    return out


def options(args, with_value):
    """Parses -x value, -xvalue, --long=value, --long value. Returns (dict, positionals)."""
    opts = {}
    rest = []
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith("--"):
            key, eq, val = a[2:].partition("=")
            if not eq and key in with_value:
                i += 1
                val = args[i] if i < len(args) else ""
            opts[key] = val if (eq or key in with_value) else True
        elif a.startswith("-") and len(a) > 1:
            key = a[1]
            if key in with_value:
                if len(a) > 2:
                    val = a[2:]
                else:
                    i += 1
                    val = args[i] if i < len(args) else ""
                opts[key] = val
            else:
                for k in a[1:]:
                    opts[k] = True
        else:
            rest.append(a)
        i += 1
    return opts, rest


# ---------------------------------------------------------------- commands

def squeue():
    opts, _ = options(ARGS, {"u", "user", "t", "states", "o", "format", "O", "Format", "j", "jobs", "p", "partition", "M", "S", "sort",
                             "A", "account", "w", "nodelist"})
    jobs = all_jobs()
    who = opts.get("u") or opts.get("user")
    accounts = opts.get("A") or opts.get("account")
    if accounts:
        wanted = set(accounts.split(","))
        jobs = [j for j in jobs + (colleague_jobs() if SCENARIO == "demo" else []) if j["account"] in wanted]
    elif not who and SCENARIO == "demo":
        jobs = jobs + other_jobs()
    elif who and who != USER:
        jobs = [j for j in other_jobs() if j["user"] == who]
    states = (opts.get("t") or opts.get("states") or "").upper()
    if states and states != "ALL":
        wanted = set()
        for s in states.split(","):
            wanted |= {"R": {"RUNNING"}, "RUNNING": {"RUNNING"}, "PD": {"PENDING"}, "PENDING": {"PENDING"},
                       "CG": {"COMPLETING"}, "COMPLETING": {"COMPLETING"}}.get(s, {s})
        jobs = [j for j in jobs if j["state"] in wanted]
    elif not states:
        jobs = [j for j in jobs if j["state"] in ("RUNNING", "PENDING", "COMPLETING")]
    on = opts.get("w") or opts.get("nodelist")
    if on:
        on = set(expand(on))
        jobs = [j for j in jobs if on & set(node_names(j))]
    ids = opts.get("j") or opts.get("jobs")
    if ids:
        ids = set(str(ids).split(","))
        jobs = [j for j in jobs if j["id"] in ids or j["job_id"] in ids]
        if not jobs:
            fail("slurm_load_jobs error: Invalid job id specified")
    lines = []
    if "O" in opts or "Format" in opts:
        fmt = opts.get("O") or opts.get("Format")
        for j in jobs:
            lines.append(format_O(fmt, lambda n, j=j: squeue_field(j, n)))
    else:
        fmt = opts.get("o") or opts.get("format") or "%.18i %.9P %.8j %.8u %.2t %.10M %.6D %R"
        if not (opts.get("h") or opts.get("noheader")):
            lines.append("JOBID PARTITION NAME USER ST TIME NODES NODELIST(REASON)")
        for j in jobs:
            lines.append(format_o(fmt, lambda n, j=j: squeue_field(j, n)))
    print("\n".join(lines))


def job_record(j, detail):
    running = j["state"] in ("RUNNING", "COMPLETING")
    used = NOW - j["start"] if running else j["elapsed"]
    hosts = node_names(j)
    lines = [
        "JobId=%s %sJobName=%s" % (j["job_id"], ("ArrayJobId=%s ArrayTaskId=%s " % (j["array_job"], j["array_task"])) if j["array_job"] else "", j["name"]),
        "   UserId=%s(4242) GroupId=%s(4242) MCS_label=N/A" % (j.get("user", USER), j.get("user", USER)),
        "   Priority=60779 Nice=0 Account=%s QOS=%s" % (j["account"], j["qos"]),
        "   JobState=%s Reason=%s Dependency=%s" % (j["state"], j["reason"], j["dependency"]),
        "   Requeue=1 Restarts=0 BatchFlag=1 Reboot=0 ExitCode=%s" % j["exit"],
        "   RunTime=%s TimeLimit=%s TimeMin=N/A" % (duration(used if j["state"] != "PENDING" else 0), limit_str(j["limit"])),
        "   SubmitTime=%s EligibleTime=%s" % (stamp(j["submit"]), stamp(j["submit"])),
        "   AccrueTime=%s" % stamp(j["submit"]),
        "   StartTime=%s EndTime=%s Deadline=N/A" % (stamp(j["start"]) if j["start"] else "Unknown",
                                                     stamp(j["end"]) if j["state"] != "PENDING" else "Unknown"),
        "   Partition=%s AllocNode:Sid=romeo1:139327" % j["partition"],
        "   ReqNodeList=(null) ExcNodeList=(null)",
        "   NodeList=%s" % (compress(hosts) if hosts else "(null)"),
    ]
    if hosts:
        lines.append("   BatchHost=%s" % hosts[0])
    lines += [
        "   NumNodes=%d NumCPUs=%d NumTasks=%d CPUs/Task=%d ReqB:S:C:T=0:0:*:*" % (j["num_nodes"], j["num_cpus"], j["num_nodes"], j["num_cpus"] // j["num_nodes"]),
        "   ReqTRES=%s" % tres(j),
        "   AllocTRES=%s" % (tres(j) if j["state"] != "PENDING" else "(null)"),
        "   Socks/Node=* NtasksPerN:B:S:C=1:0:*:* CoreSpec=*",
    ]
    if detail and j["state"] != "PENDING":
        if j["gpus"]:
            lines.append("   JOB_GRES=gpu:h100:%d" % j["gpus"])
        for a in j["alloc"]:
            gres = ""
            if a["gpus"]:
                n = len(a["gpus"].split("-")) and (int(a["gpus"].split("-")[-1]) - int(a["gpus"].split("-")[0]) + 1)
                gres = " GRES=gpu:h100:%d(IDX:%s)" % (n, a["gpus"])
            lines.append("     Nodes=%s CPU_IDs=%s Mem=%d%s" % (a["hosts"], a["cpus"], a["mem"], gres))
    lines += [
        "   MinCPUsNode=%d MinMemoryNode=%s MinTmpDiskNode=0" % (j["num_cpus"] // j["num_nodes"], mem(j["mem_per_node"])),
        "   Features=%s DelayBoot=00:00:00" % ("armgpu" if j["gpus"] or any(h.startswith("romeo-a") for h in hosts) else "x64cpu"),
        "   OverSubscribe=OK Contiguous=0 Licenses=(null) Network=(null)",
        "   Command=%s" % j["command"],
        "   WorkDir=%s" % j["workdir"],
        "   StdErr=%s" % expand_path(j["stderr"], j),
        "   StdIn=/dev/null",
        "   StdOut=%s" % expand_path(j["stdout"], j),
        "   Power=",
    ]
    if j["gpus"]:
        lines.append("   TresPerNode=gres/gpu:%d" % (j["gpus"] // j["num_nodes"]))
    return "\n".join(lines) + "\n   \n"


def node_line(name, n, jobs, detail=False):
    mine_cpus = 0
    mine_gpus = 0
    mine_mem = 0
    gpu_ids = set()
    for j in jobs:
        if j["state"] not in ("RUNNING", "COMPLETING"):
            continue
        for a in j["alloc"]:
            if name in expand(a["hosts"]):
                mine_cpus += len(expand_ids(a["cpus"]))
                mine_gpus += len(expand_ids(a["gpus"])) if a["gpus"] else 0
                gpu_ids |= set(expand_ids(a["gpus"])) if a["gpus"] else set()
                mine_mem += a["mem"]
    cpu_alloc = min(n["cpus"], mine_cpus + n["others_cpus"])
    gpu_alloc = min(n["gpus"], mine_gpus + n["others_gpus"])
    state = n["state"]
    if state == "IDLE" and cpu_alloc:
        state = "ALLOCATED" if cpu_alloc == n["cpus"] else "MIXED"
    load = cpu_alloc * 0.97
    alloc_tres = "cpu=%d,mem=%dM" % (cpu_alloc, mine_mem + n["others_mem"])
    if gpu_alloc:
        alloc_tres += ",gres/gpu=%d" % gpu_alloc
    gres = "gpu:%s:%d" % (n["gpu_type"], n["gpus"]) if n["gpus"] else "(null)"
    # With -d: the GPUs in use and their indices (the user's jobs' ones, then the first free ones for the others)
    gres_used = ""
    if detail and n["gpus"]:
        free = [i for i in range(n["gpus"]) if i not in gpu_ids]
        used = sorted(gpu_ids | set(free[:max(0, gpu_alloc - len(gpu_ids))]))
        gres_used = " GresDrain=N/A GresUsed=gpu:%s:%d(IDX:%s)" % (n["gpu_type"], len(used), cpu_ranges(used) if used else "N/A")
    # scontrol's states, and why a node is down or drained
    shown_state = {"DOWN*": "DOWN+NOT_RESPONDING", "DRAINED": "IDLE+DRAIN"}.get(state, state)
    reason = ""
    for why, user, since, nodes, _ in DOWN_REASONS:
        if name in expand(nodes):
            reason = " Reason=%s [%s@%s]" % (why, user, stamp(since))
    line = ("NodeName=%s Arch=%s CoresPerSocket=%d  CPUAlloc=%d CPUEfctv=%d CPUTot=%d CPULoad=%.2f "
            "AvailableFeatures=%s ActiveFeatures=%s Gres=%s%s NodeAddr=%s NodeHostName=%s Version=23.11.10 "
            "OS=Linux 5.14.0-570.el9_6.%s #1 SMP PREEMPT_DYNAMIC Thu Sep 10 02:14:15 UTC 2026  RealMemory=%d AllocMem=%d "
            "FreeMem=%d Sockets=%d Boards=1 MemSpecLimit=15360 State=%s ThreadsPerCore=1 TmpDisk=0 Weight=100 "
            "Owner=N/A MCS_label=N/A Partitions=%s  BootTime=2026-10-02T13:32:47 "
            "SlurmdStartTime=2026-10-21T03:29:09 CfgTRES=cpu=%d,mem=%dM,billing=%d%s AllocTRES=%s CapWatts=n/a "
            "CurrentWatts=0 AveWatts=0 ExtSensorsJoules=n/a ExtSensorsWatts=0 ExtSensorsTemp=n/a%s") % (
        name, n["arch"], n["cores"], cpu_alloc, n["cpus"], n["cpus"], load, n["features"], n["features"], gres, gres_used,
        name, name, n["arch"], n["mem"], mine_mem + n["others_mem"], n["mem"] // 3, n["sockets"], shown_state,
        n.get("partitions", "long,short,instant"), n["cpus"], n["mem"], n["cpus"],
        (",gres/gpu=%d" % n["gpus"]) if n["gpus"] else "", alloc_tres, reason)
    if state == "DOWN*":   # a node that doesn't answer: no Arch, no OS
        line = re.sub(r"OS=.*? 2026  ", "", re.sub(r"Arch=\S+ ", "", line))
    return line, state, cpu_alloc, gpu_alloc


def expand_ids(s):
    out = []
    for part in s.split(","):
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            out += list(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out


def scontrol():
    args = [a for a in ARGS if a not in ("-d", "-dd", "--details", "-o", "--oneliner")]
    detail = any(a in ("-d", "-dd", "--details") for a in ARGS)
    oneliner = any(a in ("-o", "--oneliner") for a in ARGS)
    if len(args) >= 2 and args[0] == "show" and args[1] in ("job", "jobid", "jobs"):
        jobs = all_jobs() + (colleague_jobs() if SCENARIO == "demo" else [])
        if len(args) > 2:
            wanted = args[2]
            jobs = [j for j in jobs if wanted in (j["id"], j["job_id"]) or (j["array_job"] == wanted)]
            if not jobs:
                fail("slurm_load_jobs error: Invalid job id specified")
        sys.stdout.write("".join(job_record(j, detail) + "\n" for j in jobs))
    elif len(args) >= 2 and args[0] == "show" and args[1] in ("node", "nodes"):
        nodes = make_nodes()
        names = expand(args[2]) if len(args) > 2 else list(nodes)
        jobs = all_jobs()
        out = []
        for name in names:
            if name not in nodes:
                fail("Node %s not found" % name)
            line = node_line(name, nodes[name], jobs, detail)[0]
            if not oneliner:
                line = line.replace("  ", " ").replace(" ", "\n   ", 0)
            out.append(line)
        print(("\n" if oneliner else "\n\n").join(out))
    elif len(args) >= 3 and args[0] == "show" and args[1] == "hostnames":
        print("\n".join(expand(args[2])))
    elif args[:1] == ["ping"]:
        print("Slurmctld(primary) at admin1 is UP")
    elif args[:1] in (["hold"], ["release"]) and len(args) > 1:
        state = load_state()
        held = state.setdefault("held", {})
        pending = {k for j in all_jobs() if j["state"] == "PENDING"
                   for k in (j["id"], re.sub(r"%\d+", "", j["id"]), j["job_id"], j["array_job"]) if k}
        for i in args[1].split(","):
            if i not in pending:
                fail("Job is no longer pending execution for job %s" % i)
            if args[0] == "hold":
                held[i] = NOW
            else:
                held.pop(i, None)
        save_state(state)
        with open(os.path.join(STATE, "scancel.log"), "a") as f:
            f.write("%d scontrol %s\n" % (NOW, " ".join(args)))
    elif args[:2] == ["write", "batch_script"] and len(args) > 2:
        match = [j for j in all_jobs() if args[2] in (j["id"], j["job_id"], j["array_job"]) and j["state"] in ("RUNNING", "PENDING")]
        if not match:
            fail("job script retrieval failed: Invalid job id specified")
        j = match[0]
        print("#!/bin/bash\n#SBATCH --job-name=%s\n#SBATCH --account=%s\n#SBATCH --partition=%s\n#SBATCH --constraint=%s\n"
              "#SBATCH --nodes=%d\n#SBATCH --ntasks-per-node=1\n#SBATCH --cpus-per-task=%d\n#SBATCH --mem=%s\n%s#SBATCH --time=%s\n"
              "#SBATCH --output=%s\n\n# Made-up script of the fake Slurm\nset -euo pipefail\nromeo_load_%s_env\n\n"
              "echo \"start $(date) on $SLURM_JOB_NODELIST\"\nsrun ./solver --config case.yaml --steps 12000\n"
              % (j["name"], j["account"], j["partition"], "armgpu" if j["gpus"] else "x64cpu", j["num_nodes"],
                 j["num_cpus"] // j["num_nodes"], mem(j["mem_per_node"]),
                 "#SBATCH --gpus-per-node=%d\n" % (j["gpus"] // j["num_nodes"]) if j["gpus"] else "", limit_str(j["limit"]),
                 j["stdout"], "armgpu" if j["gpus"] else "x64cpu"))
    elif args[:2] == ["show", "reservation"]:
        print("ReservationName=maintenance-gpfs StartTime=%s EndTime=%s Duration=08:00:00\n"
              "   Nodes=romeo-a[001-058],romeo-c[001-040,101-104] NodeCnt=102 CoreCnt=25152 Features=(null) PartitionName=(null) Flags=MAINT,IGNORE_JOBS\n"
              "   TRES=cpu=25152\n   Users=root Groups=(null) Accounts=(null) Licenses=(null) State=INACTIVE BurstBuffer=(null)\n\n"
              "ReservationName=training-gpu StartTime=%s EndTime=%s Duration=05:00:00\n"
              "   Nodes=romeo-a[019,021-022,025-027] NodeCnt=6 CoreCnt=1728 Features=armgpu PartitionName=instant Flags=FLEX\n"
              "   TRES=cpu=1728\n   Users=(null) Groups=(null) Accounts=proj-edu Licenses=(null) State=INACTIVE BurstBuffer=(null)\n" % (
                  stamp(NOW + 6 * DAY), stamp(NOW + 6 * DAY + 8 * HOUR), stamp(NOW + 12 * DAY), stamp(NOW + 12 * DAY + 5 * HOUR)))
    elif args[:2] == ["show", "partition"]:
        for name, limit, tier, nodes in (("long", "30-00:00:00", 1, "romeo-a[001-040],romeo-c[001-020,101-104]"),
                                          ("short", "1-00:00:00", 50, "romeo-a[001-056],romeo-c[001-039,101-104]"),
                                          ("instant", "01:00:00", 100, "romeo-a[001-058],romeo-c[001-040,101-104]")):
            print("PartitionName=%s AllowGroups=ALL AllowAccounts=ALL AllowQos=ALL Default=%s QoS=%s MaxNodes=UNLIMITED "
                  "MaxTime=%s PriorityJobFactor=%d PriorityTier=%d Nodes=%s State=UP DefMemPerNode=UNLIMITED MaxMemPerNode=UNLIMITED" % (
                      name, "YES" if name == "instant" else "NO", name.capitalize(), limit, tier, tier, nodes))
    else:
        fail("scontrol: fake: unsupported command: %s" % " ".join(ARGS))


# Nodes reserved by a job that does almost nothing (for the "reserved but idle" marks)
IDLE_NODES = {"romeo-c026", "romeo-c027"}

DOWN_REASONS = [("Not responding", "slurm", NOW - 3 * DAY, "romeo-a057", "down*"),
                ("SlurmdSpoolDir is full", "root", NOW - 5 * HOUR, "romeo-a058", "drained"),
                ("disk replacement", "root", NOW - 2 * HOUR, "romeo-c040", "down*")]


def sinfo():
    opts, _ = options(ARGS, {"o", "format", "O", "Format", "p", "partition", "t", "states"})
    if "R" in opts or "list-reasons" in opts:
        fmt = opts.get("o") or opts.get("format") or "%E|%u|%H|%N|%T"
        letters = {"E": 0, "u": 1, "H": 2, "N": 3, "T": 4}
        for r in DOWN_REASONS:
            values = [r[0], r[1], stamp(r[2]), r[3], r[4]]
            print(re.sub(r"%(\.)?(\d+)?([A-Za-z])", lambda m: values[letters.get(m.group(3), 0)], fmt))
        return
    nodes = make_nodes()
    jobs = all_jobs()
    rows = []
    for partition, limit in (("instant*", "1:00:00"), ("short", "1-00:00:00"), ("long", "30-00:00:00")):
        for name, n in nodes.items():
            if partition.rstrip("*") not in n.get("partitions", "long,short,instant").split(","):
                continue
            _, state, cpu_alloc, gpu_alloc = node_line(name, n, jobs)
            compact = {"IDLE": "idle", "MIXED": "mix", "ALLOCATED": "alloc", "DOWN*": "down*", "DRAINED": "drain"}[state]
            long_state = {"IDLE": "idle", "MIXED": "mixed", "ALLOCATED": "allocated", "DOWN*": "down*", "DRAINED": "drained"}[state]
            other = n["cpus"] if state in ("DOWN*", "DRAINED") else 0
            idle = 0 if other else n["cpus"] - cpu_alloc
            mem_alloc = sum(a["mem"] for j in jobs if j["state"] in ("RUNNING", "COMPLETING") for a in j["alloc"]
                            if name in expand(a["hosts"])) + n["others_mem"]
            load = 0.0 if state in ("DOWN*", "DRAINED") else cpu_alloc * (0.08 if name in IDLE_NODES else 0.96)
            rows.append(dict(partition=partition, avail="up", available="up", time=limit, nodelist=name, statecompact=compact,
                             features=n["features"], cpusload="%.2f" % load, allocmem=str(mem_alloc),
                             freemem=str(max(0, n["mem"] - mem_alloc - 20000)), memory=str(n["mem"]),
                             statelong=long_state, cpusstate="%d/%d/%d/%d" % (0 if other else cpu_alloc, idle, other, n["cpus"]),
                             gres="gpu:%s:%d" % (n["gpu_type"], n["gpus"]) if n["gpus"] else "(null)",
                             gresused=("gpu:%s:%d(IDX:%s)" % (n["gpu_type"], gpu_alloc, ("0-%d" % (gpu_alloc - 1)) if gpu_alloc else "N/A")) if n["gpus"] else "gpu:0"))
    per_node = opts.get("N") or opts.get("Node")
    if not per_node:
        grouped = {}
        for r in rows:
            key = (r["partition"], r["statecompact"], r["gres"])
            g = grouped.setdefault(key, dict(r, nodes=0, hosts=[], cpus=[0, 0, 0, 0]))
            g["nodes"] += 1
            g["hosts"].append(r["nodelist"])
            g["cpus"] = [a + int(b) for a, b in zip(g["cpus"], r["cpusstate"].split("/"))]
        rows = []
        for g in grouped.values():
            g["nodelist"] = compress(g["hosts"])
            g["cpusstate"] = "/".join(map(str, g["cpus"]))
            rows.append(g)
    lines = []
    if "O" in opts or "Format" in opts:
        fmt = opts.get("O") or opts.get("Format")
        for r in rows:
            lines.append(format_O(fmt, lambda name, r=r: str(r.get(name.lower(), r.get("nodes", 1) if name.lower() == "nodes" else "N/A"))))
    else:
        fmt = opts.get("o") or opts.get("format") or "%P %a %l %D %T %N"
        letters = {"P": "partition", "a": "avail", "l": "time", "D": "nodes", "T": "statelong", "t": "statecompact",
                   "C": "cpusstate", "G": "gres", "N": "nodelist", "m": "memory", "f": "features"}
        for r in rows:
            lines.append(re.sub(r"%(\.)?(\d+)?([A-Za-z])", lambda m, r=r: str(r.get(letters.get(m.group(3), "?"), 1 if m.group(3) == "D" else "N/A")), fmt))
    print("\n".join(lines))


SACCT_UNITS = {"K": 1, "M": 1024, "G": 1024 * 1024, "T": 1024 ** 3}


def array_history(fields, sep):
    """sacct -j 812402: every task of the demo array, ended ones too"""
    lines = []
    for t in range(1, 159):
        state, code = ARRAY_FAILED.get(t, ("COMPLETED", "0:0"))
        if t >= 151:
            state, code = "RUNNING", "0:0"
        values = {"JobID": "812402_%d" % t, "State": state, "ExitCode": code}
        lines.append(sep.join(values.get(f, "") for f in fields))
    lines.append(sep.join({"JobID": "812402_[159-200%8]", "State": "PENDING", "ExitCode": "0:0"}.get(f, "") for f in fields))
    return lines


def sacct():
    opts, _ = options(ARGS, {"u", "user", "S", "starttime", "E", "endtime", "o", "format", "s", "state", "j", "jobs", "delimiter"})
    if opts.get("j") or opts.get("jobs"):
        fields = [f.split("%")[0] for f in (opts.get("o") or opts.get("format") or "JobID,State,ExitCode").split(",")]
        ids = (opts.get("j") or opts.get("jobs")).split(",")
        print("\n".join(array_history(fields, opts.get("delimiter") or "|") if "812402" in ids else []))
        return
    fields = [f.split("%")[0] for f in (opts.get("o") or opts.get("format") or "JobID,JobName,State,Elapsed").split(",")]
    sep = opts.get("delimiter") or "|"
    allocations_only = opts.get("X") or opts.get("allocations")
    states = opts.get("s") or opts.get("state")
    lines = []
    for r in history():
        if states:
            short = {"COMPLETED": "CD", "FAILED": "F", "TIMEOUT": "TO", "OUT_OF_MEMORY": "OOM", "CANCELLED": "CA"}.get(r["State"].split()[0], "")
            if short.lower() not in states.lower().split(",") and r["State"].split()[0].lower() not in states.lower().split(","):
                continue
        steps = [("", None)] if allocations_only or r["Start"] is None else [("", None), (".batch", "batch"), (".extern", "extern")]
        # As on ROMEO: CPU time and gres/gpu in AllocTRES; with -X the job's line has no CPU time (only its steps do)
        gres = ",gres/gpu=%d" % r["GPUs"] if r["GPUs"] else ""
        tres = "billing=%d,cpu=%d%s,mem=%dG,node=%d" % (r["NCPUs"], r["NCPUs"], gres, int(r["ReqMem"][:-1]) * r["NNodes"], r["NNodes"])
        for suffix, step in steps:
            cpu_time = r["Elapsed"] * r["NCPUs"]
            total_cpu = 0 if step == "extern" or (allocations_only and not step) else r["TotalCPU"]
            values = {
                "JobID": r["JobID"] + suffix, "JobName": step or r["JobName"],
                "State": r["State"] if not step else ("CANCELLED" if r["State"].startswith("CANCELLED") else r["State"]),
                "Partition": "" if step else r["Partition"], "Account": r["Account"], "User": "" if step else USER,
                "Submit": stamp(r["Submit"]), "Start": stamp(r["Start"]) if r["Start"] else "Unknown",
                "End": stamp(r["End"]) if r["End"] else "Unknown",
                "Elapsed": duration(r["Elapsed"]), "ElapsedRaw": str(r["Elapsed"]),
                "Timelimit": "" if step else limit_str(r["Timelimit"]), "TimelimitRaw": "" if step else str(r["Timelimit"] // 60),
                "NNodes": str(r["NNodes"]), "NCPUs": str(r["NCPUs"]), "ReqMem": "" if step else r["ReqMem"],
                "AllocTRES": tres if not step or r["Start"] else "",
                "TotalCPU": duration(total_cpu) + ".123" if total_cpu else "00:00:00",
                "CPUTime": duration(cpu_time), "CPUTimeRAW": str(cpu_time),
                "MaxRSS": "" if not step else (r["MaxRSS"] if step == "batch" else "0"),
                "ExitCode": r["ExitCode"], "NodeList": r["NodeList"], "WorkDir": HOME,
            }
            lines.append(sep.join(values.get(f, "") for f in fields))
    if not (opts.get("n") or opts.get("noheader")):
        lines.insert(0, sep.join(fields))
    print("\n".join(lines))


def sacctmgr():
    text = " ".join(ARGS)
    m = re.search(r"format=(\S+)", text, re.I)
    fields = (m.group(1) if m else "User,Account,GrpTRES").split(",")
    rows = [
        dict(User=USER, Account="default", Partition="", QOS="normal", GrpTRES="cpu=1536,gres/gpu=60", MaxTRES="",
             GrpJobs="", MaxJobs="0", MaxSubmit="0", MaxWall=""),
        dict(User=USER, Account="proj-cfd", Partition="", QOS="normal", GrpTRES="cpu=1536,gres/gpu=60", MaxTRES="",
             GrpJobs="", MaxJobs="", MaxSubmit="100", MaxWall=""),
        dict(User=USER, Account="proj-ml", Partition="", QOS="normal", GrpTRES="cpu=1536,gres/gpu=60", MaxTRES="",
             GrpJobs="", MaxJobs="", MaxSubmit="100", MaxWall=""),
    ]
    lines = ["|".join(r.get(f.split("%")[0], "") for f in fields) for r in rows]
    print("\n".join(lines))


def scancel():
    ids = [a for a in ARGS if not a.startswith("-")]
    pending_only = any(a in ("--state=PENDING", "--state=PD", "-tPENDING", "-tPD") for a in ARGS)
    state = load_state()
    jobs = all_jobs()
    os.makedirs(STATE, exist_ok=True)
    with open(os.path.join(STATE, "scancel.log"), "a") as f:
        f.write("%d scancel %s\n" % (NOW, " ".join(ARGS)))
    for i in ids:
        match = [j for j in jobs if i in (j["id"], j["job_id"], j["array_job"]) and j["state"] in ("RUNNING", "PENDING", "COMPLETING")]
        if pending_only:
            match = [j for j in match if j["state"] == "PENDING"]
            for j in match:
                state["cancelled"][j["id"]] = NOW
            continue
        if not match:
            fail("scancel: error: Kill job error on job id %s: Invalid job id specified" % i)
        state["cancelled"][i if any(j["array_job"] == i for j in match) else match[0]["id"]] = NOW
    save_state(state)


def sstat():
    opts, _ = options(ARGS, {"j", "jobs", "o", "format"})
    job_id = (opts.get("j") or opts.get("jobs") or "").split(".")[0]
    match = [j for j in all_jobs() if j["job_id"] == job_id and j["state"] == "RUNNING"]
    if not match:
        fail("sstat: error: no steps running for job %s" % job_id)
    j = match[0]
    used = NOW - j["start"]
    # A busy share that moves (two waves), so the live view (m) draws curves: its CPU time is the integral
    train = j["name"].startswith("train")
    base, waves = (0.42, ((0.25, 90), (0.05, 23))) if train else (0.91, ((0.05, 120), (0.03, 17)))
    cpu = used * base
    for amp, period in waves:
        w = 2 * math.pi / period
        cpu += amp / w * (math.cos(w * j["start"]) - math.cos(w * NOW))
    cpu *= j["num_cpus"]
    mem = j["mem_per_node"] * j["num_nodes"] * (0.45 + 0.25 * ((NOW % 300) / 300.0) if train else 0.6 + 0.04 * math.sin(NOW / 40.0))
    fmt = opts.get("format") or ""
    if "MaxRSSNode" not in fmt:
        print("%s.batch|1|%dM|cpu=%s,energy=0,fs/disk=123456,mem=%dM,pages=0,vmem=%dM" % (
            job_id, mem / 1024, duration(cpu * 0.1), mem * 0.1, mem * 0.2))
        print("%s.0|%d|%dM|cpu=%s,energy=0,fs/disk=654321,mem=%dM,pages=0,vmem=%dM" % (
            job_id, j["num_nodes"], mem / j["num_nodes"] / 1.5, duration(cpu * 0.9), mem * 0.9, mem * 1.2))
        return
    # The live view (m) asks for the node of the largest task and the bytes read and written: reads stream in
    # (about 400 MB/s, in waves), a checkpoint writes 30 GB every 90 s
    read = used * 4e8 + 1.5e8 * 30 / (2 * math.pi) * (math.cos(2 * math.pi * j["start"] / 30) - math.cos(2 * math.pi * NOW / 30))
    write = (used // 90) * 3e10 + min(used % 90, 15) / 15.0 * 3e10 + used * 2e6
    hosts = node_names(j)
    node = hosts[-1] if hosts else "romeo-a012"
    steps = [("batch", 1, 0.02, 0.05, 0.01, 0.01), ("0", j["num_nodes"], 0.88, 0.9, 0.97, 0.98)]
    if train:
        steps.append(("1", 1, 0.10, 0.05, 0.02, 0.01))   # a data loader on the side
    else:
        steps.append(("extern", j["num_nodes"], 0.0, 0.0, 0.0, 0.0))
    for name, tasks, c, m, r, w in steps:
        print("%s.%s|%d|%dM|%s|cpu=%s,energy=0,fs/disk=%d,mem=%dM,pages=0,vmem=%dM|energy=0,fs/disk=%d" % (
            job_id, name, tasks, mem * m / max(1, tasks) * 1.1, node, duration(cpu * c), read * r, mem * m, mem * m * 1.3, write * w))


def sprio():
    rows = []
    for j in all_jobs() + other_jobs():
        if j["state"] != "PENDING":
            continue
        fs = 30000 if j.get("user", USER) == USER else 52000 + (hash(j["job_id"]) % 20000)
        rows.append((j["job_id"], j.get("user", USER), j["partition"], fs + 1, 0, fs, 0, 0, 0, 1))
    for r in rows:
        print("%s|%s|%s|%d|%d|%d|%d|%d|%d|%d" % r)


def sshare():
    print("default|%s|0.001000|0|0.000000|1.000000" % USER)
    print("proj-cfd|%s|0.040000|41234567|0.612000|0.081234" % USER)
    print("proj-ml|%s|0.120000|2876543|0.210000|0.412345" % USER)


def mmlsquota():
    header = ("mmlsquota:%s:HEADER:version:reserved:reserved:filesystemName:quotaType:id:name:blockUsage:blockQuota:"
              "blockLimit:blockInDoubt:blockGrace:filesUsage:filesQuota:filesLimit:filesInDoubt:filesGrace:remarks:fid:filesetname:")
    if "-g" in ARGS:
        # A made-up group name, whatever the real one asked for (RSV asks for the user's real project groups)
        real = ARGS[ARGS.index("-g") + 1]
        group = ("proj-cfd", "proj-ml")[sum(map(ord, real)) % 2]
        print(header % "group")
        print("mmlsquota:group:0:1:::gpfs:GRP:300001:%s:432013312:1073741824:1127428915:0:none:51230:10000000:15000000:0:none:x:3:projet:" % group)
        return
    print(header % "user")
    print("mmlsquota:user:0:1:::gpfs:USR:4242:%s:3250000:104857600:110100480:0:none:98213:1000000:1500000:0:none:x:1:home:" % USER)
    print("mmlsquota:user:0:1:::gpfs:USR:4242:%s:23068672:104857600:110100480:0:none:1204551:1000000:1500000:0:6 days:x:2:scratch:" % USER)


def srun():
    # srun --jobid=ID --overlap ... -l nvidia-smi ...: one line per GPU, labelled with the task (node) number
    jid = [a.split("=", 1)[1] for a in ARGS if a.startswith("--jobid=")]
    match = [j for j in all_jobs() if jid and j["job_id"] == jid[0] and j["state"] == "RUNNING"]
    if not match or "nvidia-smi" not in ARGS:
        fail("srun: error: Unable to create step for job %s: Invalid job id specified" % (jid[0] if jid else "?"))
    j = match[0]
    per_node = max(1, j["gpus"] // j["num_nodes"])
    full = "temperature.gpu" in " ".join(ARGS)   # the live view (m) asks for temperature and power too
    loop = [int(a.split("=", 1)[1]) for a in ARGS if a.startswith("--loop-ms=")]

    def once(now):
        for task in range(j["num_nodes"]):
            for g in range(per_node):
                if j["name"].startswith("train") and g == 1:
                    util = 3
                else:   # each GPU its own wave, the job's steps make them dip together now and then
                    util = 88 - g * 3 + 8 * math.sin(now / 9.0 + g + task) - (35 if int(now / 20) % 4 == 0 else 0)
                util = max(0, min(100, util))
                line = "%d: %d, NVIDIA GH200 120GB, %d, %d, 97871" % (task, g, util, 70123 - g * 5000 + 3000 * math.sin(now / 30.0))
                if full:
                    line += ", %d, %.2f, 900.00" % (45 + util * 0.3, 90 + util * 5.5)
                print(line)
        sys.stdout.flush()

    if not loop:
        once(NOW)
        return
    while True:   # nvidia-smi --loop-ms: until it is killed (the view closes)
        once(time.time())
        time.sleep(loop[0] / 1000.0)


def sbatch():
    if "--test-only" not in ARGS:
        fail("sbatch: fake: only --test-only is supported (nothing is ever submitted)")
    opts = dict(a[2:].split("=", 1) for a in ARGS if a.startswith("--") and "=" in a)
    if "mem" not in opts:
        fail("sbatch: error: Memory resource is missing: use option --mem=<memory> or another memory control option\nallocation failure: Unspecified error")
    if "constraint" not in opts:
        fail("sbatch: error: Constraint is missing: use option --constraint=<feature>\nallocation failure: Unspecified error")
    nodes = int(opts.get("nodes", "1"))
    kind = opts["constraint"]
    if (kind == "x64cpu" and nodes > 40) or (kind == "armgpu" and nodes > 56) or int(opts.get("gpus-per-node", "0")) > 4:
        fail("allocation failure: Requested node configuration is not available")
    wait = 0 if nodes <= 4 else 3 * HOUR + nodes * 600
    first = 51 if kind == "armgpu" else 34
    hosts = compress(["romeo-%s%03d" % ("a" if kind == "armgpu" else "c", first + i) for i in range(nodes)])
    sys.stderr.write("sbatch: Job 999999 to start at %s using %d processors on nodes %s in partition %s\n" % (
        stamp(NOW + wait), nodes * int(opts.get("ntasks-per-node", "1")), hosts, opts.get("partition", "short")))


def sdiag():
    print("""*******************************************************
sdiag output at %s (%d)
Data since      %s (%d)
*******************************************************
Server thread count:  2

Jobs submitted: 1408
Jobs started:   1394
Jobs completed: 1207
Jobs canceled:  154
Jobs failed:    0

Main schedule statistics (microseconds):
	Last cycle:   61
	Max cycle:    242873
	Total cycles: 3245
	Mean cycle:   1993
	Mean depth cycle:  1
	Cycles per minute: 2
	Last queue length: 0

Main scheduler exit:
	End of job queue:3245

Backfilling stats
	Total backfilled jobs (since last slurm start): 242
	Total cycles: 612
	Last cycle: 344
	Max cycle:  105366
	Mean cycle: 1152
""" % (stamp(NOW), NOW, stamp(NOW - 22 * HOUR), NOW - 22 * HOUR))


COMMANDS = {"squeue": squeue, "scontrol": scontrol, "sinfo": sinfo, "sacct": sacct, "sacctmgr": sacctmgr,
            "scancel": scancel, "sstat": sstat, "sprio": sprio, "sshare": sshare, "mmlsquota": mmlsquota,
            "srun": srun, "sbatch": sbatch, "sdiag": sdiag}

if __name__ == "__main__":
    if CMD not in COMMANDS:
        fail("fakeslurm: unknown command %r" % CMD, 2)
    COMMANDS[CMD]()
