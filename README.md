# SLURM Job Monitoring Script with Graphical Visualization

This small project provides a **Terminal User Interface (TUI)** of your SLURM jobs, showing CPU and GPU allocations per node using characters. It is intended for use on clusters like [ROMEO](https://romeo.univ-reims.fr/).

---

## Features

- Lists all SLURM jobs for the current user.
- Interactive scrolling with mouse wheel.
- UI with a sidebar menu for job selection.
- Shows detailed job information:
  - Job ID
  - Name
  - Submission time
  - Number of nodes
  - Max duration
  - Partition
  - Status (color-coded)
  - Constraints
- Visualizes node allocations for running jobs:
  - CPU usage (`■` = allocated, `.` = free)
  - GPU usage (`●` = allocated, `○` = free)
  - Dynamic expansion of compressed node lists (e.g., `romeo-a[045-046]`)
  - ASCII graphical representation of cores and GPUs per node.

## Planned Features
- Color-coded status:
  - `RUNNING` → Green
  - `PENDING` → Yellow
  - `COMPLETED` → Blue
  - `FAILED` → Red
  - `CANCELLED` → Purple

---

## Prerequisites

- Bash shell (`#!/bin/bash`)
- SLURM commands available (`squeue`, `scontrol`)
- SSH access to cluster nodes (if remote queries needed)
- `grep`, `cut`, `printf` commands (standard on Linux)
- **C++17 compiler** (GCC or Clang recommended)
- **CMake ≥ 3.14**
- **FTXUI library** installed
- Terminal supporting ANSI colors

---

## Building

1. Clone or copy the source code to your workstation or cluster.
2. Create a build directory and compile:

```bash
mkdir build
cd build
cmake ..
make
```

3. This will produce the executable rsv.
4. Libraries are statically linked, so the executable is transferable to your cluster

---

## Usage

1. Run the program:
```bash
./rsv
```

2. The program displays:
- A sidebar menu listing all your jobs.
- Details of the selected job in the main panel.
- Node allocations with CPU/GPU usage visualized in a grid.
- Navigate the UI:
  - Arrow keys → Move selection in sidebar.
  - Mouse wheel → Scroll node allocations vertically.

---

## Symbols (subject to change)

| Symbol | Meaning         |
|--------|----------------|
| `.`    | Free CPU core  |
| `■`    | Allocated CPU  |
| `○`    | Free GPU       |
| `●`    | Allocated GPU  |