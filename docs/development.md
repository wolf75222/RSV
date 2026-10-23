# Development

```bash
./dev.sh          # build (cmake from Spack on ROMEO)
./dev.sh test     # build and run the parser tests
./dev.sh demo     # run with a fake Slurm and made-up jobs, on any machine
./dev.sh run      # run on the real Slurm
```

## Layout

- `src/main.cpp`: options, colors, the `Viewer` (UI state, keys, layout), the workers, the hangup watchdog.
- `src/api/run.hpp`: runs a command without a shell, with a time limit; `src/api/slurmjobs.hpp`: what RSV asks Slurm and the parsing of its answers.
- `src/components/`: the views (list, details, node cells, log, history, partitions, limits, help).
- `src/utils/format.hpp`: widths, cutting, durations, sizes.

Threads: the UI state belongs to FTXUI's thread. Slurm commands run on one worker, file reads on another; they hand their results back with `screen.Post()` (then an event to redraw: in FTXUI 7 a posted task alone doesn't). A timer thread asks for refreshes and watches stdin for the hangup.

## Tests

- `tests/parse_test.cpp`: the parsers on Slurm output recorded on ROMEO (made-up names): `cmake -DRSV_TESTS=ON`, then `ctest` or `./build/parse_test`.
- `tests/fake-slurm/`: `squeue`, `scontrol`, `sinfo`, `sacct`, `sacctmgr` and `scancel` with made-up jobs. Put `tests/fake-slurm/bin` first in `PATH`. `FAKE_SLURM_SCENARIO=empty|many|unicode`, `FAKE_SLURM_DELAY=20` (a hung slurmctld), `FAKE_SLURM_FAIL=squeue`. Its `scancel` cancels nothing real.
- `tests/tui.py`: runs a program in a pseudo-terminal, answers the terminal queries, sends keys, keeps the screen with `pyte` and draws it with Pillow (`pip install pyte pillow`).
- `tests/screenshots.py`: the GIF and screenshots of the README, from the fake Slurm. `--ssh romeo --dir '~/RSV'` runs RSV on ROMEO and draws it here.
