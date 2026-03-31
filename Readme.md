# Tractography-Library: LD_PRELOAD Buffering & Prefetching Library

## Overview
This project provides an LD_PRELOAD-based shared library for transparent I/O buffering and prefetching, designed to accelerate file operations for specific neuroimaging tools (e.g., `dwidenoise`, `fslmaths`).

**It is currently designed and tested for use with TractoFlow running under Singularity containers, ensuring compatibility with HPC (High-Performance Computing) environments.**

The library is intended for high-throughput pipelines such as TractoFlow (Nextflow-based), where I/O performance can be a bottleneck.

## Features
- **Write Buffering:** Aggregates small write operations into larger chunks to reduce system call overhead and improve disk throughput.
- **Read Prefetching:** Reads larger blocks of data in advance to minimize read latency and improve sequential read performance.
- **Targeted Interception:** Only activates for specified target commands (e.g., `dwidenoise`, `fslmaths`).
- **Logging:** All I/O statistics and debug messages are written to `/mnt/buffer.log` for later analysis.

## Logger
The logger records:
- Initialization and shutdown events
- I/O statistics (number of intercepted and executed reads/writes, bytes transferred, average sizes)
- Buffer flushes and errors

Log file location: `/mnt/buffer.log`


## Usage

### 1. Manual Usage

- **Build the library:**
   ```sh
   make
   ```
- **Set the environment variable** before running your target tool or pipeline:
   ```sh
   export LD_PRELOAD=/path/to/libbuffer.so
   <your_command>
   ```

### 2. TractoFlow Integration (Recommended for HPC/Singularity)

- Add the config files from `tractography-library/config-files/` to your TractoFlow command:
   ```sh
   nextflow run main.nf -c tractography-library/config-files/nextflow-buffer.config -c tractography-library/config-files/nextflow-logger.config
   ```
- Or include them in your `nextflow.config`.

The config files will automatically set `LD_PRELOAD` for the relevant processes inside the Singularity container.

## Nextflow/TractoFlow Integration
- The `tractography-library/config-files/` directory contains example Nextflow configuration files for integrating the library with TractoFlow.
- These configs set the `LD_PRELOAD` environment variable for relevant processes, ensuring the library is loaded only where beneficial.
- Place the config files in your Nextflow pipeline directory and include them in your `nextflow.config` or via the `-c` option.

## Notes
- Only the specified target commands will have their I/O intercepted.
- The library is safe to use in multi-process environments and handles forked processes correctly.
- For troubleshooting, consult `/mnt/buffer.log`.

---
For more details, see the source code in `src/buffer_lib.c` and the example configs in `config-files/`.
