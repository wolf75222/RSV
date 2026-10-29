# Development

```bash
./dev.sh          # build (cmake from Spack on ROMEO)
./dev.sh test     # build and run the parser tests
./dev.sh demo     # run with a fake Slurm and made-up jobs, on any machine
./dev.sh run      # run on the real Slurm
```

## Layout

- `src/main.cpp`: options, colors, the `Viewer` (UI state, keys, layout), the workers, the hangup watchdog.
- `src/api/run.hpp`: runs a command without a shell, with a time limit; `src/api/slurmjobs.hpp`: what RSV asks Slurm about jobs and the parsing of its answers; `arrays.hpp`: arrays and their progress; `cluster.hpp`: the cluster view's data; `usage.hpp`: live use, priority, shares, quotas, the start-time estimate, the live view's GPU steps; `inference.hpp`: finding a job's server of models and reading its Prometheus metrics, one adapter per engine.
- `src/components/`: the views (list, details, node cells, log, history, partitions, limits, help). The live view: `monitor_view.hpp` (its tabs), `inference_view.hpp` (the model server's), `live.hpp` (readings in time, boxes, gauges), `charts.hpp` (areas, braille and block graphs, sparklines, meters, charts with lines).
- `src/utils/format.hpp`: widths, cutting, durations, sizes.

Threads: the UI state belongs to FTXUI's thread. Slurm commands run on one worker, file reads on another; they hand their results back with `screen.Post()` (then an event to redraw: in FTXUI 7 a posted task alone doesn't). A timer thread asks for refreshes and watches stdin for the hangup.

## Tests

- `tests/parse_test.cpp`: the parsers on Slurm output recorded on ROMEO (made-up names): `cmake -DRSV_TESTS=ON`, then `ctest` or `./build/parse_test`.
- `tests/fake-slurm/`: `squeue`, `scontrol`, `sinfo`, `sacct`, `sacctmgr`, `scancel`, `sstat`, `sprio`, `sshare`, `mmlsquota`, `srun` (nvidia-smi, dmon, listpids), `sbatch`, `sdiag` and `curl` (a vLLM's metrics) with made-up jobs: an array of 200 tasks, colleagues in the same accounts for the project view, logs with errors. Put `tests/fake-slurm/bin` first in `PATH`. `FAKE_SLURM_SCENARIO=empty|many|unicode`, `FAKE_SLURM_DELAY=20` (a hung slurmctld; a `delay` file in the state folder slows it while a test runs), `FAKE_SLURM_FAIL=squeue`. Its `scancel` and `scontrol hold` change nothing real: they mark the jobs in its state folder.
- `tests/tui.py`: runs a program in a pseudo-terminal, answers the terminal queries, sends keys, keeps the screen with `pyte` and draws it with Pillow (`pip install pyte pillow`).
- `tests/screenshots.py`: the GIFs and screenshots of the README and the usage, from the fake Slurm (`--only live` for the live view's). `--ssh romeo --dir '~/RSV'` runs RSV on ROMEO and draws it here.
