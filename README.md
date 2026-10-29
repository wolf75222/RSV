# Romeo Slurm Viewer (RSV)

Your Slurm jobs on the [ROMEO](https://romeo.univ-reims.fr/) cluster, from the terminal: where they run, how long they have left, why they wait, their logs, and what they really used.

![RSV with made-up jobs](img/rsv.gif)

## Install

On a ROMEO login node:

```bash
git clone https://github.com/wolf75222/RSV.git ~/RSV && ~/RSV/install.sh
```

Then run `rsv`. To update: `cd ~/RSV && git pull && ./install.sh`.

## What it does

- Your jobs, their time left or expected start, and why they wait, in plain words. An array is one row.
- Their nodes, cores and GPUs, their log followed live, their script and `scontrol` details.
- A running job live: CPUs, memory, disk, GPUs in depth, and the model server it runs (vLLM, SGLang, Ollama…).
- History and efficiency over 7 days, your project's jobs, the whole cluster live, your limits and quotas.
- Never freezes, works over SSH in any terminal.

[See more](docs/features.md)

![A job live: overview, performance, GPUs, its model server](img/live.gif)

## Keys

| Key | |
|---|---|
| `↑` `↓` or `j` `k` | move |
| `l` or `Enter` | the job's log, followed live |
| `m` / `n` / `i` | the job live / its GPUs / its model server |
| `c` | cancel (asks first, `Space` marks several) |
| `a` / `p` / `u` | history / cluster / your accounts |
| `?` / `q` | help / quit |

[All the keys](docs/usage.md)

## Docs

[Features](docs/features.md) · [Usage](docs/usage.md) · [Terminals](docs/terminals.md) · [Changes](docs/changes.md) · [Development](docs/development.md)
