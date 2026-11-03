#!/usr/bin/env python3
"""Summarise bench_iobuf CSVs: median time per buffer size, and the time saved
against each library's default buffer (8 KB for system zlib, 128 KB for zlib-ng and
zstd), for every variant, location, process count and direction.

Usage: summarize_iobuf.py all.csv [more.csv ...] > summary.md
"""
import csv
import statistics
import sys
from collections import defaultdict


def default_size(variant, mode):
    if mode == "gz" and not variant.startswith("zlibng"):
        return 8192          # GZBUFSIZE in zlib 1.2.11
    return 131072            # zlib-ng's GZBUFSIZE, about ZSTD_CStreamOutSize()


def human(size):
    if size % (1 << 20) == 0:
        return f"{size >> 20} MB"
    if size % (1 << 10) == 0:
        return f"{size >> 10} KB"
    return f"{size} B"


def main(paths):
    groups = defaultdict(list)
    for path in paths:
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                if row.get("variant") == "variant":
                    continue    # a header repeated inside a concatenated file
                key = (row["variant"], row["where"], int(row["procs"]), row["mode"], row["op"])
                groups[key].append((int(row["bufsize"]), float(row["seconds"]), float(row["MBps"]),
                                    int(row["calls"])))

    print("# I/O buffer size benchmark\n")
    print("Median of all runs (all processes and repetitions) per buffer size. "
          "Time saved is against the library's default buffer: positive is faster, negative slower. "
          "Spread is min-max of the times. MB/s is per process, on uncompressed bytes.\n")
    for key in sorted(groups):
        variant, where, procs, mode, op = key
        by_size = defaultdict(list)
        for size, seconds, mbps, calls in groups[key]:
            by_size[size].append((seconds, mbps, calls))
        base = default_size(variant, mode)
        base_median = statistics.median(s for s, _, _ in by_size[base]) if base in by_size else None

        print(f"## {variant} · {where} · {procs} process{'es' if procs > 1 else ''} · {op}\n")
        print("| Buffer | Median time (s) | Spread (s) | MB/s | Time saved vs default | Syscalls |")
        print("| --- | --- | --- | --- | --- | --- |")
        for size in sorted(by_size):
            times = [s for s, _, _ in by_size[size]]
            median = statistics.median(times)
            mbps = statistics.median(m for _, m, _ in by_size[size])
            calls = by_size[size][0][2]
            saved = f"{100 * (1 - median / base_median):+.1f}%" if base_median else "n/a"
            mark = " (default)" if size == base else ""
            print(f"| {human(size)}{mark} | {median:.3f} | {min(times):.3f}-{max(times):.3f} | "
                  f"{mbps:.1f} | {saved} | {calls if calls >= 0 else '-'} |")
        print()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
