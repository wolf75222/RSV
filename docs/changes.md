# Changes in RSV 2

**Fixes**

- Node view: the CPUs of a job were split between its nodes. `scontrol` gives the CPU ids used on *each* node of a group (`Nodes=romeo-a[031-032] CPU_IDs=0-15,72-87,144-159,216-231`): a 2-node job with 64 cores per node showed 32 on each. GPUs were the job's total divided by the nodes, and the first ones were colored instead of the ones used (`IDX:1,3`). Only the first group of nodes was read: a job with different allocations per node showed the first one everywhere.
- The UI froze while Slurm answered: every move in the list ran `scontrol` for the job, then once per node, on the UI thread, and the refresh every 30 s too. With Slurm answering in 2 s, a key took 6 s to show. Commands now run in the background, with a time limit, and the UI answers at once.
- Quitting took up to 30 s (the refresh thread slept 30 s before it could stop). It is now immediate, and a command still running is stopped, also on Ctrl-C, `kill` or a hangup.
- The partitions view ran `sinfo` at every frame (each key, each mouse move).
- No job: RSV printed "No jobs found" and quit, so the history, partitions and limits couldn't be seen. Jobs that ended while RSV was open left stale details.
- Job names with spaces were cut at the first one (`train resnet50 (fp16)` showed `train`).
- A number Slurm printed differently (`std::stoi` throws) ended the program.
- History: the memory column was always empty (MaxRSS is only known for the steps, which were skipped), and changing the filter ran `sacct` again on the UI thread.
- Logs: a big file was read whole, lines were cut in the middle of UTF-8 characters, tabs, colors and `\r` progress bars broke the screen, and scrolling went by 5% of the file (25 lines at a time in a 500-line log).
- Views didn't keep the keys: `PgDn`, `Home` or the mouse moved the selection behind them, and ran `scontrol`.
- Copy used `xclip` on the login node, which has no display: nothing was copied, but "Copied" was shown.
- Limits: only the last account was kept, the GPU and submitted jobs limits were ignored.
- Output paths: `%A`, `%a`, `%u`, `%N` and zero padding (`%4a`) weren't expanded, relative paths weren't in the job's directory.
- The last line of a node's cores was padded with dots: cores that don't exist looked free.
- The first key after the start was ignored.

**New**

- Time left of running jobs, expected start of pending ones, live clocks between refreshes.
- Pending reasons in plain words with what to do, for the ROMEO limits too (`QOSMaxJobsPerUserLimit`, `AssocGrpCpuLimit`...).
- Log viewer that follows the file like `tail -f`, the last lines of the output under the details.
- History with CPU, memory and time efficiency, and a hint when a job asked for much more than it used.
- Filter (`/`), sorts by state, newest, name or time left, vim keys, the mouse wheel moves through the list.
- Partitions with CPUs and GPUs in use, limits for every account (on ROMEO "default" allows no job: it says so).
- Copy through the terminal (OSC 52), works over SSH.
- `install.sh`: builds with cmake from Spack and installs to `~/.local/bin`. `deploy.sh` (build elsewhere and copy) is gone.

**Terminals** (see [terminals.md](terminals.md))

- 24-bit colors over SSH, applied again after FTXUI's own detection. `RSV_COLOR` or `--color` force a mode.
- Columns measured by display width: CJK, emoji and accents keep them aligned, invisible characters are removed.
- Layouts for narrow and short terminals, and a message below 40×10.
- Quits when the SSH connection drops, even if the hangup signal doesn't reach it.
- Mouse moves aren't reported: they redrew the whole screen over SSH.

**Build**

- FTXUI 7.0.3 (was 5.0.0) from its release archive, checked by its hash: a git clone could hang on the login nodes. CMake 3.16 or newer, Release by default, warnings on (`-Wall -Wextra -Wpedantic -Wshadow`), none left.
- Parser tests on recorded Slurm output (`-DRSV_TESTS=ON`), a fake Slurm and a terminal harness for the rest, see [development.md](development.md).

**Measured on ROMEO** (login node, made-up jobs from the fake Slurm, over SSH)

| | RSV 1 | RSV 2 |
|---|---|---|
| Quit | 28 s | 0.04 s (0.12 s while a command hangs) |
| A key, Slurm answering in 2 s | 6.1 s | 0.05 s |
| First screen | 0.8 s | 0.6 s |
| CPU when idle | 0.23% | 0.13% |
| Memory | 5.1 MB | 5.6 MB |
| SSH drop | stays | quits |
| Install, FTXUI download included | | 17 s |
