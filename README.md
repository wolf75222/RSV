# Romeo Slurm Viewer (RSV)

Your Slurm jobs on the [ROMEO](https://romeo.univ-reims.fr/) cluster, from the terminal: where they run, how long they have left, why they wait, their logs, and what they really used.

![RSV with made-up jobs](img/rsv.gif)

## Install

On a ROMEO login node:

```bash
git clone https://github.com/wolf75222/RSV.git ~/RSV && ~/RSV/install.sh
```

Then run `rsv`. To update: `cd ~/RSV && git pull && ./install.sh`.

## Keys

| Key | |
|---|---|
| `↑` `↓` or `j` `k` | move (`g` `G`: first, last) |
| `l` or `Enter` | the job's log, followed as it grows (`Tab`: errors) |
| `d` | everything `scontrol` says about the job |
| `c` | cancel the job (asks first) |
| `y` | copy the job id |
| `/` | filter |
| `s` | sort |
| `a` | history of 7 days, with what each job used |
| `p` | partitions |
| `u` | your accounts and their limits |
| `?` | help |
| `q` | quit |

## What it does

- Lists your jobs with the time they have left, or when a pending one should start, and why it waits, in plain words.
- Shows where a job runs: its nodes, with their cores (`■`) and GPUs (`●`) in use.
- Follows the job's output live, and shows its last lines under the details.
- History with CPU, memory and time efficiency, and a hint when a job asked for much more than it used.
- Partitions (free nodes, CPUs and GPUs) and your accounts' limits.
- Never freezes: Slurm is asked in the background, with timeouts. Quits when the SSH connection drops.
- Works in any terminal, over SSH too: 24-bit colors, aligned columns whatever the names, small windows.

## Docs

[Usage](docs/usage.md) · [Terminals](docs/terminals.md) · [Changes](docs/changes.md) · [Development](docs/development.md)
