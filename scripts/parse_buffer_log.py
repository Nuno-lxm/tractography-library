#!/usr/bin/env python3
import argparse
import re


WRITE_INT_RE = re.compile(r"write_stats intercepted calls=(\d+) bytes=(\d+)")
WRITE_EXEC_RE = re.compile(r"write_stats executed calls=(\d+) bytes=(\d+)")
READ_INT_RE = re.compile(r"read_stats intercepted calls=(\d+) bytes=(\d+)")
READ_EXEC_RE = re.compile(r"read_stats executed calls=(\d+) bytes=(\d+)")


def parse_log(path):
    totals = {
        "write_intercept_calls": 0,
        "write_intercept_bytes": 0,
        "write_exec_calls": 0,
        "write_exec_bytes": 0,
        "read_intercept_calls": 0,
        "read_intercept_bytes": 0,
        "read_exec_calls": 0,
        "read_exec_bytes": 0,
    }

    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            m = WRITE_INT_RE.search(line)
            if m:
                totals["write_intercept_calls"] += int(m.group(1))
                totals["write_intercept_bytes"] += int(m.group(2))
                continue

            m = READ_INT_RE.search(line)
            if m:
                totals["read_intercept_calls"] += int(m.group(1))
                totals["read_intercept_bytes"] += int(m.group(2))
                continue

            m = WRITE_EXEC_RE.search(line)
            if m:
                totals["write_exec_calls"] += int(m.group(1))
                totals["write_exec_bytes"] += int(m.group(2))
                continue

            m = READ_EXEC_RE.search(line)
            if m:
                totals["read_exec_calls"] += int(m.group(1))
                totals["read_exec_bytes"] += int(m.group(2))
                continue

    return totals


def safe_avg(bytes_total, calls_total):
    if calls_total <= 0:
        return 0.0
    return bytes_total / calls_total


def fmt_bytes(value):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024.0:
            return f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{value:.1f} PB"


def main():
    parser = argparse.ArgumentParser(description="Summarize buffer log totals.")
    parser.add_argument("logfile", help="Path to a buffer log file")
    args = parser.parse_args()

    totals = parse_log(args.logfile)

    print("write_intercepted_calls:", totals["write_intercept_calls"])
    print("write_intercepted_bytes:", fmt_bytes(totals["write_intercept_bytes"]))
    print(
        "write_intercepted_avg_bytes:",
        f"{safe_avg(totals['write_intercept_bytes'], totals['write_intercept_calls']):.1f}",
    )
    print("write_executed_calls:", totals["write_exec_calls"])
    print("write_executed_bytes:", fmt_bytes(totals["write_exec_bytes"]))
    print(
        "write_executed_avg_bytes:",
        f"{safe_avg(totals['write_exec_bytes'], totals['write_exec_calls']):.1f}",
    )

    print("read_intercepted_calls:", totals["read_intercept_calls"])
    print("read_intercepted_bytes:", fmt_bytes(totals["read_intercept_bytes"]))
    print(
        "read_intercepted_avg_bytes:",
        f"{safe_avg(totals['read_intercept_bytes'], totals['read_intercept_calls']):.1f}",
    )
    print("read_executed_calls:", totals["read_exec_calls"])
    print("read_executed_bytes:", fmt_bytes(totals["read_exec_bytes"]))
    print(
        "read_executed_avg_bytes:",
        f"{safe_avg(totals['read_exec_bytes'], totals['read_exec_calls']):.1f}",
    )


if __name__ == "__main__":
    main()
