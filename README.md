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
| `n` | the job's GPUs now: use and memory |
| `c` | cancel the job (asks first) |
| `y` | copy the job id |
| `/` | filter |
| `s` | sort |
| `a` | history of 7 days, with what each job used |
| `p` | the whole cluster, live: overview, queue, nodes |
| `t` | when would a job start? |
| `u` | your accounts: limits, fairshare, disk quotas |
| `?` | help |
| `q` | quit |

## What it does

- Lists your jobs with the time they have left, or when a pending one should start, and why it waits, in plain words.
- Shows where a job runs: its nodes, with their cores (`■`) and GPUs (`●`) in use.
- Follows the job's output live, and shows its last lines under the details.
- History with CPU, memory and time efficiency, and a hint when a job asked for much more than it used.
- Running jobs: CPU and memory used now, GPUs on demand. Pending ones: their priority and rank in the queue.
- The whole cluster, live: CPUs, GPUs, load, memory and storage in use, reservations and maintenances, partitions and their rules, everyone's queue and why it waits, the nodes down and why, and a map of every node (yours marked).
- When would a job start? Slurm's own estimate for the job you describe, without submitting it.
- Your accounts: their limits, fairshare and disk quotas.
- Never freezes: Slurm is asked in the background, with timeouts. Quits when the SSH connection drops.
- Works in any terminal, over SSH too: 24-bit colors, aligned columns whatever the names, small windows.

## Docs

[Usage](docs/usage.md) · [Terminals](docs/terminals.md) · [Changes](docs/changes.md) · [Development](docs/development.md)
