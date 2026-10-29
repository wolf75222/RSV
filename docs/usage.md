# Usage

```bash
rsv                  # your jobs
rsv --refresh 30     # read the queue every 30 s (default 15, at least 5)
rsv --color 256      # truecolor, 256, 16 or auto, see terminals.md
rsv --no-mouse       # leave the mouse to the terminal, to select text
```

The same as environment variables: `RSV_REFRESH=30`, `RSV_COLOR=256`, `RSV_MOUSE=0` (in `~/.bashrc` to keep them). `RSV_VSCODE_HOST` names ROMEO in your SSH config for the links to VS Code (`romeo` by default), `RSV_LINKS=none` takes the links away (see [terminals](terminals.md)).

When `squeue` is slow, RSV reads the queue less often by itself: ten times the time `squeue` took (2 s: every 20 s), twice longer after each failure, 2 minutes at most, and back to `--refresh` when Slurm is. The top line says so (`Slurm slow: every 20 s`).

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
| `Space` | mark the job (or array) and go to the next one: `c` then cancels all the marked ones, `Esc` unmarks them |
| `Enter`, `→` `←` | on an array: show its tasks under it, fold them |
| `o` | the project view: everyone's jobs in your accounts, read-only (`o` again: yours) |
| `r` | refresh now |
| `q`, `Esc`, `Ctrl-C` | quit (`Esc` clears the marks or the filter first) |

The selection follows the job through refreshes and sorts.

**Arrays.** All the tasks of an array are one row, `812402_*`, with a bar of its tasks (done, failed, cancelled, running, waiting) and how many have ended out of how many. Its details say how many tasks are done, running, waiting, failed (with their exit codes), on which nodes the running ones are, why the others wait and when they should start. `Enter` shows its tasks under it, each with its own details, log and script. Slurm forgets a task 5 minutes after it ended: the older ones come from `sacct`, read once a minute.

**Hold and release on ROMEO.** ROMEO's submit plugin refuses every change of a job that doesn't give a time limit. `scontrol hold`, `uhold` and `release` give none: they are all refused with "Requested time limit is invalid", and `H` says so. A hold does go through with one (`scontrol update jobid=<id> TimeLimit=<its limit> Priority=0`), but then nothing lets a user release the job: `scontrol release` is refused the same way, and a user can't raise a priority. RSV doesn't hold jobs that way. A job submitted with `--hold` can only be released by the support team: cancel it and submit it again. (Tried on two test jobs, October 2026.)

**Project.** `o` shows the jobs of everyone in your accounts (those that can run jobs, from `sacctmgr`), with a `USER` column: ROMEO lets every user see the queue (`PrivateData = none`). Read-only: nothing can be cancelled or held there. The details and the logs work when the files can be read.

## The details

State, time used and left, partition, QOS and account, what the job uses (or asks for), its times, directory and output files. For a pending job: the reason in plain words, what to do about it, and the expected start.

Then its nodes, grouped by kind (`APU ARM (romeo-a)`, `APU Compute (romeo-c)`), one cell per node: `■` the job's cores and `.` the others, 20 per line, `●` the job's GPUs and `○` the others. The cores are those Slurm gave on that node (`scontrol show job -dd`), so their placement shows: 16 cores on each of the 4 sockets of a Grace node look like 4 runs of 16.

Under it, when there is room, the last lines of the job's output.

## Views

| Key | View | |
|---|---|---|
| `l`, `Enter` | Log | The output file, followed as it grows. `j` `k` `g` `G` scroll, `f` follow or pause, `←` `→` sideways, `Tab` output or errors, `y` copy the path. `/` search (`n` `N` next and previous), error lines in red (`e` `E` next and previous error). Only the last 2 MB of a big file are read. |
| `b` | Script | The script the job was submitted with (`scontrol write batch_script`), `#SBATCH` lines highlighted, `y` copies it. Slurm keeps it while the job waits or runs: ROMEO doesn't store the scripts of ended jobs (`AccountingStoreFlags` has no `job_script`). |
| `d` | scontrol | Everything Slurm says about the job, the important fields highlighted. |
| `c` | Cancel | The marked jobs, or the selected one, listed first (`y` to cancel, anything else keeps them). On an array's row, all its tasks; on `812402_[159-200]`, only the tasks still waiting when you confirm (`scancel --state=PENDING`). |
| `H` | Hold | Holds the waiting jobs (they keep waiting but don't start), or releases them when they are all held already (`scontrol hold`, `release`). For an array, its tasks still waiting when you confirm. A running job can't be paused by its user. **ROMEO refuses both** (see below). |
| `a` | History | Your jobs of the last 7 days (`sacct`), what they used, see below. `←` `→` filter by state, `s` sort, `r` read again. |
| `p` | Cluster | All of ROMEO, read again every 15 s while open, in four tabs: `←` `→` (or `Tab`, `1` to `4`) change tabs everywhere in it. See below. |
| `t` | Start time | When would a job start? Account, partition, constraint, nodes, cores, GPUs, memory and time: `Enter` asks Slurm (`sbatch --test-only`, nothing is submitted) and shows when and where, or why it can't. |
| `n` | GPUs | For a running job with GPUs: each GPU's use and memory, from `nvidia-smi` run in the job itself (`srun --overlap`, only when you press `n`). |
| `m` | Live | A running job live, in two views. `1` overview: the CPUs' curve and under it the GPUs' (or the memory's), each with its scale; beside them a gauge for the CPUs, memory, GPUs and their memory, and each GPU with its little curve, use and temperature (each step when there is no GPU); under them `memory` (asked, in use with its curve, free, the largest task and its node, GPU memory), `disk` (the bytes its steps read and write per second, reads above and writes under them, with their totals) and `steps` (each step with the cores it keeps busy and their curve, its memory and largest task). `2` performance, as a task manager's: CPU, memory, disk and each GPU on the left with their little curves, the one picked (`↑` `↓`) on the right with its chart, scale, time and figures. `+` `-` zoom in time (1 to 30 s per column, up to the last hour), `g` draws the graphs in braille (the default), bars or blocks, `r` reads again. `sstat` every 2 s; the GPUs from one `srun --overlap` step that runs `nvidia-smi --loop-ms=2000` while the view is open and ends with it (one step, not one every few seconds). |
| `u` | Limits | For each of your accounts: its CPU, GPU and job limits and what your jobs use now (`sacctmgr`, `squeue`), its fairshare and the CPU-hours used (`sshare`), then your disk quotas and your projects' (`mmlsquota`), with a warning when one is over. |
| `?` | Help | |

`y` copies the job id, `Y` its output path (see [terminals](terminals.md) for how copying works over SSH).

![The log of a running job, followed](../img/log.png)

![A running job live (m): the overview](../img/live.png)

![The same, as a task manager's Performance tab](../img/live-performance.png)

## Cluster tabs

- **Overview**: jobs and users, CPUs and GPUs in use and asked for by pending jobs, with their curve since the view was opened (one point per refresh, its lowest and highest value), the load (reserved cores really busy, nodes reserved but idle), memory, storage (`/gpfs`), nodes, Slurm's controller and scheduler cycles (`sdiag`); reservations and maintenances to come (a job still running when one starts waits); partitions with their time limit, priority tier and rules; kinds of nodes (armgpu, x64cpu).
- **Queue**: everyone's jobs by partition, why the pending ones wait with what they ask for (`Resources`: the cluster is full for them, `Priority`: others go first), the users with the most CPUs (`a`: by account).
- **Nodes**: the nodes down or drained, since when, by whom and why (`sinfo -R`), then every node: a map (`■` busy, `■` yellow reserved but idle, `.` free, `●` GPU in use, `x` down, your jobs' nodes inverted), or a table with its load, memory and note (`t`). `↑` `↓` move a cursor from node to node (`PgUp` `PgDn` a row of the map, `g` `G` the first and last), `Enter` zooms on the node: its state and why it is down (by whom, since when), CPUs reserved and busy, memory reserved and free, GPUs in use, and its jobs (`squeue -w`: user, CPUs and GPUs, nodes, since when, time left). In the zoom `n` `N` go to the next and previous node, `j` `k` scroll its jobs, `Esc` back to the map.
- **Hardware**: each kind of node: how many and which, CPU model with sockets × cores, arch, memory per node and for jobs, GPUs with their model and memory, local disk, partitions. Slurm doesn't know the CPU and GPU models: they come from a small table of ROMEO's hardware (AMD EPYC 9654, NVIDIA GH200).

![The cluster overview](../img/cluster.png)

![Every node: why some are down, what the others run](../img/nodes.png)

![A zoom on a node: its jobs, load and memory](../img/node.png)

![The hardware of each kind of node](../img/hardware.png)

## In the details

- A running job: `Now` is how busy its CPUs were between the last two readings (`sstat`, CPU time over time × CPUs: the first reading counts since the job started), and the memory of its running steps.
- A pending job: its priority, its rank among the jobs waiting in its partition, and what the priority is made of (`sprio`).

## History columns

- `CPU`: CPU time used / CPU time allocated (`TotalCPU / CPUTimeRAW`). 50% on 64 cores: half of them waited.
- `MEM`: the largest memory used by a task (`MaxRSS`, over all steps) / the memory per node. A rough measure for multi-task jobs: Slurm only keeps the largest task.
- `TIME`: run time / time limit. Asking for 12 h and using 1 h makes the job wait longer: backfill can only start a short job in a gap.

The line under the table says what the selected job used and, when it applies, what to change.

Above the table, what the jobs of the last 7 days used: CPU hours reserved, used and idle (the idle share, and how much of it in GPU jobs), GPU hours, and the time asked with `--time` that completed jobs didn't use. `CPU-H` and `GPU-H` columns when the window is wide enough, `s` sorts by the most idle CPU hours. A running job's CPU time is only known once its steps end: running jobs are left out of the totals. Slurm doesn't record how busy GPUs were on ROMEO: only the GPU hours reserved.

![History with what each job used](../img/history.png)

## Where the data comes from

| | Command | When |
|---|---|---|
| Jobs | `squeue -u $USER -t all -O ...` (`-A <accounts>` in the project view) | every 15 s, less often when Slurm is slow |
| Arrays | `sacct -X -j <arrays> -o JobID,State,ExitCode` | once a minute, and when a new array shows up |
| Script | `scontrol write batch_script <job> -` | on `b` |
| Details, nodes | `scontrol show job -dd`, `scontrol -o show node` | for the selected job |
| Live use, priority | `sstat -a -j <job>`, `sprio` | for the selected job |
| History | `sacct -u $USER -S now-7days -P` | when opened, at most once a minute |
| Cluster | `sinfo -N -O ...`, `squeue -t R,PD,CG -O ...` (everyone's jobs), `sinfo -R`, `scontrol show reservation`, `scontrol ping`, `statvfs(/gpfs)` | every 15 s while open |
| Cluster, at opening | `sdiag`, `scontrol show partition` | when opened |
| Hardware | `scontrol -o show node` | when its tab is first shown |
| Node zoom | `scontrol -d -o show node <node>`, `squeue -w <node>` | on `Enter`, then every refresh |
| Start time | `sbatch --test-only` | on `Enter` |
| GPUs | `srun --jobid=<job> --overlap nvidia-smi` | on `n` |
| Live | `sstat -a -j <job>` (each step: CPU time, memory, bytes read and written, largest task and its node), `srun --jobid=<job> --overlap nvidia-smi --loop-ms=2000` (one step) | every 2 s while open |
| Limits | `sacctmgr show assoc`, `sshare -U`, `mmlsquota -Y` | when opened |
| Cancel, hold | `scancel <ids>`, `scontrol hold <ids>`, `scontrol release <ids>` | when confirmed |

Every command runs in the background with a time limit (15 s, 30 s for `sacct`): when Slurm is slow, RSV keeps the last answer on screen and says what failed in its top line.
