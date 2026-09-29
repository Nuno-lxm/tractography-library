# Tractography-Library: LD_PRELOAD Buffering & Prefetching Library

## Overview
This project provides an LD_PRELOAD-based shared library for transparent I/O buffering and prefetching, designed to accelerate file operations for specific neuroimaging tools (e.g., `dwidenoise`, `fslmaths`).

**It is currently designed and tested for use with TractoFlow running under Singularity containers, ensuring compatibility with HPC (High-Performance Computing) environments.**

The library is intended for high-throughput pipelines such as TractoFlow (Nextflow-based), where I/O performance can be a bottleneck.

## Features
- **Write Buffering:** Aggregates small write operations into larger chunks to reduce system call overhead and improve disk throughput.
- **Read Prefetching:** Reads larger blocks of data in advance to minimize read latency and improve sequential read performance.
- **Targeted Interception:** Only activates for specified target commands (e.g., `dwidenoise`, `fslmaths`).
- **File Filtering:** File glob patterns (`BUFFER_FILE_TARGETS`) restrict which files are intercepted per process.
- **Logging:** All I/O statistics and debug messages are written to a configurable log file.

## Components

| Component | Source | Build Target | Description |
|---|---|---|---|
| Buffer library | `src/buffer_lib.c` | `bin/buffer_lib.so` | Write buffering + read prefetching via LD_PRELOAD |
| Logger | `src/logger.c` | `bin/logger.so` | Pass-through I/O call logger (no buffering) |
| Unit test | `src/test.c` | `bin/test` | 12-scenario test for buffer library |
| Log parser | `scripts/parse_buffer_log.py` | — | Parses `buffer.log` and summarizes stats |

## Build

```sh
make
```

Produces `bin/buffer_lib.so`, `bin/logger.so`, `bin/test`.

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `BUFFER_WRITE_TARGETS` | none | Comma-separated process names to enable write buffering for |
| `BUFFER_PREFETCH_TARGETS` | none | Comma-separated process names to enable read prefetching for |
| `BUFFER_FILE_TARGETS` | none | Comma-separated glob patterns (e.g. `*.nii.gz`) to filter which files are intercepted |
| `BUFFER_LOG_FILE` | `/mnt/buffer.log` | Path to the log file |

## Config Files

Nextflow config files are in `config-files/`:

| Config | Environment | Purpose |
|---|---|---|
| `deucalion/base.config` | Deucalion | Loaded by every run: repo bind as `/mnt`, queue size, `maxForks`, `use_gpu` profile |
| `deucalion/buffer.config` | Deucalion | Buffer library (`LD_PRELOAD`, targets, buffer sizes), layered after `base.config` |
| `cloud/buffer-cloud.config` | Cloud | Buffer library for cloud environment |
| `cloud/logger-cloud.config` | Cloud | Logger library for cloud environment |

The run scripts generate a small `run.config` per run for everything else
(compression library, logger, prefetch-only, compression level and debug).

## Run Scripts

Slurm batch scripts for Deucalion. All take `--lib none|buffer|prefetch|deflate|deflate-py|logger`
(see `scripts/lib/common.sh`, which they share):

| Script | What it runs |
|---|---|
| `scripts/tests/run_tractoflow.sh` | TractoFlow on x86, `--pipeline minimal` (default) or `full`, optional `--ssd` |
| `scripts/tests/run_tractoflow_gpu.sh` | Same, on an A100 node with the `use_gpu` profile and GPU local tracking |
| `scripts/isolated/run_isolated.sh` | One step on its own: `--step normalize\|extract_b0\|resample` |
| `scripts/submit_series.sh` | Submits several variants of a run script, repeated and chained one after another |

## Usage

### 1. Manual

```sh
export LD_PRELOAD=/path/to/bin/buffer_lib.so
export BUFFER_WRITE_TARGETS="dwidenoise,fslmaths,mrconvert"
export BUFFER_FILE_TARGETS="*.nii,*.nii.gz"
<your_command>
```

### 2. TractoFlow (HPC/Singularity)

```sh
nextflow run main.nf \
    -with-singularity scilus_1.6.0.sif \
    -c tractography-library/config-files/deucalion/base.config \
    -c tractography-library/config-files/deucalion/buffer.config \
    --input /path/to/data \
    --output_dir /path/to/results
```

The config files set `LD_PRELOAD` and Singularity bind mounts automatically.

## Internal Architecture

The buffer library maintains a single `fd_entry_t` table indexed by file descriptor, with each entry containing:
- `fd_state_t`: logical/kernel offset tracking, seekable flag, per-fd buffer/prefetch enable flags
- `wr_buf`: lazily allocated 64KB write buffer (only if write buffering is active for this fd)
- `pf_buf`: lazily allocated 64KB prefetch buffer (only if read prefetching is active)

Buffers are allocated on demand and the table grows dynamically via `realloc`. On `fork()`, all entries are cleared in the child process to avoid state corruption.

## Notes
- Only the specified target commands and file patterns will have their I/O intercepted.
- The library is safe to use in multi-process environments and handles forked processes correctly.
- For troubleshooting, consult the buffer log file (default `/mnt/buffer.log`).
- Use `scripts/parse_buffer_log.py` to summarize logged statistics.
