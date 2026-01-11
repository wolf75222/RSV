# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Romeo Slurm Viewer (RSV) is a C++17 Terminal User Interface (TUI) application for viewing SLURM job allocations on HPC clusters. It displays CPU/GPU usage per node using ASCII graphics.

## Build Commands

```bash
# Build the project
mkdir -p build && cd build && cmake .. && make

# Deploy to remote cluster
./deploy.sh <username> <host>
```

The executable is statically linked and can be transferred to any Linux cluster with SLURM.

## Architecture

The codebase follows a simple layered architecture:

- **src/main.cpp** - Application entry point, manages FTXUI screen and component layout. Handles mouse wheel scrolling for node display.

- **src/api/slurmjobs.hpp** - SLURM API layer. Executes shell commands (`squeue`, `scontrol`) and parses output using regex to extract job/node information. Contains `Job`, `DetailedJob`, and `NodeAllocation` structs.

- **src/components/** - UI components using FTXUI:
  - `jobdetails.hpp` - Renders job metadata (ID, status, partition, etc.)
  - `nodedetails.hpp` - Renders CPU/GPU allocation grids for each node
  - `title.hpp` - Application title bar

## Key Dependencies

- **FTXUI** - Terminal UI library (linked via CMake `find_package(ftxui REQUIRED)`)
- **SLURM commands** - Application requires `squeue` and `scontrol` to be available at runtime

## Code Conventions

- All components are header-only files
- UI rendering uses functional FTXUI patterns with `Renderer` lambdas
- SLURM command output is parsed with regex patterns defined inline
