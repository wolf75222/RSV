# Usage

```bash
rsv                  # your jobs
rsv --refresh 30     # read the queue every 30 s (default 15, at least 5)
rsv --color 256      # truecolor, 256, 16 or auto, see terminals.md
rsv --no-mouse       # leave the mouse to the terminal, to select text
```

The same as environment variables: `RSV_REFRESH=30`, `RSV_COLOR=256`, `RSV_MOUSE=0` (in `~/.bashrc` to keep them).

## The job list

Your jobs, running first, then pending, then the ones that just ended (Slurm forgets them after 5 minutes on ROMEO). `LEFT` is the time left of a running job, or `~` the time until a pending job should start, when Slurm has an estimate.

| Key | |
|---|---|
| `↑` `↓` `j` `k` | move |
| `PgUp` `PgDn`, `Ctrl-U` `Ctrl-D` | a page |
| `g` `G`, `Home` `End` | first, last |
| `/` | filter: type, `Enter` keeps it, `Esc` clears it. Matches the id, name, state, partition, account and pending reason |
| `s` | sort: state, newest first, name, time left |
| `J` `K` | scroll the details (or the mouse wheel over them) |
| `r` | refresh now |
| `q`, `Esc`, `Ctrl-C` | quit |

The selection follows the job through refreshes and sorts. An array's pending tasks are one line (`812402_[3-40]`), each running task has its own.

## The details

State, time used and left, partition, QOS and account, what the job uses (or asks for), its times, directory and output files. For a pending job: the reason in plain words, what to do about it, and the expected start.

Then its nodes, grouped by kind (`APU ARM (romeo-a)`, `APU Compute (romeo-c)`), one cell per node: `■` the job's cores and `.` the others, 20 per line, `●` the job's GPUs and `○` the others. The cores are those Slurm gave on that node (`scontrol show job -dd`), so their placement shows: 16 cores on each of the 4 sockets of a Grace node look like 4 runs of 16.

Under it, when there is room, the last lines of the job's output.

## Views

| Key | View | |
|---|---|---|
| `l`, `Enter` | Log | The output file, followed as it grows. `j` `k` `g` `G` scroll, `f` follow or pause, `←` `→` sideways, `Tab` output or errors, `y` copy the path. Only the last 2 MB of a big file are read. |
| `d` | scontrol | Everything Slurm says about the job, the important fields highlighted. |
| `c` | Cancel | Asks first (`y` to cancel, anything else keeps the job). For `812402_[3-40]`, only these pending tasks are cancelled. |
| `a` | History | Your jobs of the last 7 days (`sacct`). `←` `→` filter by state, `r` read again. |
| `p` | Cluster | All of ROMEO, read again every 15 s while open, in three tabs (`Tab`, or `1` `2` `3`). See below. |
| `t` | Start time | When would a job start? Account, partition, constraint, nodes, cores, GPUs, memory and time: `Enter` asks Slurm (`sbatch --test-only`, nothing is submitted) and shows when and where, or why it can't. |
| `n` | GPUs | For a running job with GPUs: each GPU's use and memory, from `nvidia-smi` run in the job itself (`srun --overlap`, only when you press `n`). |
| `u` | Limits | For each of your accounts: its CPU, GPU and job limits and what your jobs use now (`sacctmgr`, `squeue`), its fairshare and the CPU-hours used (`sshare`), then your disk quotas and your projects' (`mmlsquota`), with a warning when one is over. |
| `?` | Help | |

`y` copies the job id, `Y` its output path (see [terminals](terminals.md) for how copying works over SSH).

![The log of a running job, followed](../img/log.png)

## Cluster tabs

- **Overview**: jobs and users, CPUs and GPUs in use and asked for by pending jobs, the load (reserved cores really busy, nodes reserved but idle), memory, storage (`/gpfs`), nodes, Slurm's controller and scheduler cycles (`sdiag`); reservations and maintenances to come (a job still running when one starts waits); partitions with their time limit, priority tier and rules; kinds of nodes (armgpu, x64cpu).
- **Queue**: everyone's jobs by partition, why the pending ones wait with what they ask for (`Resources`: the cluster is full for them, `Priority`: others go first), the users with the most CPUs (`a`: by account).
- **Nodes**: the nodes down or drained, since when, by whom and why (`sinfo -R`), then every node: a map (`■` busy, `■` yellow reserved but idle, `.` free, `●` GPU in use, `x` down, your jobs' nodes inverted), or a table with its load, memory and note (`t`).

![The cluster overview](../img/cluster.png)

![Every node: why some are down, what the others run](../img/nodes.png)

## In the details

- A running job: `Now` is the CPU time of its running steps over the time reserved so far, and their memory (`sstat`).
- A pending job: its priority, its rank among the jobs waiting in its partition, and what the priority is made of (`sprio`).

## History columns

- `CPU`: CPU time used / CPU time allocated (`TotalCPU / CPUTimeRAW`). 50% on 64 cores: half of them waited.
- `MEM`: the largest memory used by a task (`MaxRSS`, over all steps) / the memory per node. A rough measure for multi-task jobs: Slurm only keeps the largest task.
- `TIME`: run time / time limit. Asking for 12 h and using 1 h makes the job wait longer: backfill can only start a short job in a gap.

The line under the table says what the selected job used and, when it applies, what to change.

![History with what each job used](../img/history.png)

## Where the data comes from

| | Command | When |
|---|---|---|
| Jobs | `squeue -u $USER -t all -O ...` | every 15 s |
| Details, nodes | `scontrol show job -dd`, `scontrol -o show node` | for the selected job |
| Live use, priority | `sstat -a -j <job>`, `sprio` | for the selected job |
| History | `sacct -u $USER -S now-7days -P` | when opened, at most once a minute |
| Cluster | `sinfo -N -O ...`, `squeue -t R,PD,CG -O ...` (everyone's jobs), `sinfo -R`, `scontrol show reservation`, `scontrol ping`, `statvfs(/gpfs)` | every 15 s while open |
| Cluster, at opening | `sdiag`, `scontrol show partition` | when opened |
| Start time | `sbatch --test-only` | on `Enter` |
| GPUs | `srun --jobid=<job> --overlap nvidia-smi` | on `n` |
| Limits | `sacctmgr show assoc`, `sshare -U`, `mmlsquota -Y` | when opened |
| Cancel | `scancel <id>` | when confirmed |

Every command runs in the background with a time limit (15 s, 30 s for `sacct`): when Slurm is slow, RSV keeps the last answer on screen and says what failed in its top line.
