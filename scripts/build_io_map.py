#!/usr/bin/env python3
"""Build a writer/reader map of the .nii.gz files in a TractoFlow logger run.

One row per file name (basename with the "<sid>__" prefix stripped). Accesses
are attributed per process: a process that logged a gzopen of a path went
through the gz API for every open of that path. Nextflow's staged copies
(stageInMode 'copy', done outside the container, so not in the log) are mapped
back to their source with the staged_inputs table. A file renamed into
another name (the task scripts' mv lines, from the renames table, plus the
rename() calls in the log; mv itself calls renameat, which the logger doesn't
hook) is only eligible when every name it is renamed into is: fast's
t1_pve_2.nii.gz becomes the published map_wm.nii.gz.

Usage:
  scripts/build_io_map.py --log logs/logger_2083793.log \
      --run-dir ../output/tests/minimal/mgh1_logger_x86_2083793 \
      --out ../output/tests/minimal/mgh1_logger_x86_2083793/io_map_2083793.tsv
"""
import argparse
import csv
import glob
import os
import re
import sys
from collections import defaultdict

LINE_RE = re.compile(r"^\[[^\]]*\] \[pid=(\d+) tid=\d+\] (.*)$")
INIT_RE = re.compile(r"^Initializing logger library - exe='(.*?)' cwd='(.*?)' ppid=(-?\d+) ")
FORK_RE = re.compile(r"^fork\(\) = (\d+)$")
OPEN_RE = re.compile(r"^(open|open64|__open64_2)\('(.*)', flags=0x([0-9a-f]+)\) = (-?\d+)$")
OPENAT_RE = re.compile(r"^(openat|openat64)\(dirfd=(-?\d+), path='(.*)', flags=0x([0-9a-f]+)\) = (-?\d+)$")
FOPEN_RE = re.compile(r"^(fopen|fopen64|gzopen|gzopen64)\('(.*)', mode='(.*)'\) = (.*)$")
RENAME_RE = re.compile(r"^rename\('(.*)', '(.*)'\) = (-?\d+)$")
TASK_DIR_RE = re.compile(r"/([0-9a-f]{2})/([0-9a-f]{30})(?:/|$)")
SID_RE = re.compile(r"^.+?__")

AT_FDCWD = -100
O_ACCMODE, O_CREAT = 0x3, 0x40
SUFFIX = ".nii.gz"


def family(exe):
    if "python" in os.path.basename(exe):
        return "Python"
    if exe.startswith("/fsl/"):
        return "FSL"
    if exe.startswith("/mrtrix3_install/"):
        return "MRtrix"
    if exe.startswith("/ants/"):
        return "ANTs"
    return "other"


def read_tasks(trace_path):
    tasks = {}
    with open(trace_path) as f:
        for row in csv.DictReader(f, delimiter="\t"):
            tasks[row["hash"]] = row["name"]
    return tasks


def task_of(path, tasks):
    m = TASK_DIR_RE.search(path or "")
    if not m:
        return None
    return tasks.get(f"{m.group(1)}/{m.group(2)[:6]}")


def read_staged(path, project_dir):
    """Map each staged copy (absolute path) to its source file."""
    staged = {}
    with open(path) as f:
        for line in f:
            task, copy, source = line.rstrip("\n").split("\t")
            staged[os.path.join(project_dir, copy)] = source
    return staged


def read_renames(path):
    """(old, new) of each mv line in the task scripts."""
    with open(path) as f:
        return [tuple(line.rstrip("\n").split("\t")[1:3]) for line in f if line.strip()]


def read_published(path):
    with open(path) as f:
        return {line.rstrip("\n").split("\t")[1] for line in f if "\t" in line}


def canonical(path, staged):
    seen = set()
    while path in staged and path not in seen:
        seen.add(path)
        path = staged[path]
    return path


def parse_log(log_path):
    """Returns {(pid, abspath): access} and per-pid process info."""
    procs = {}          # pid -> (exe, cwd), the latest exec
    parent = {}         # child pid -> parent pid, from fork() lines without exec
    access = {}
    renames = []
    unresolved_dirfd = 0

    def proc(pid):
        p = pid
        for _ in range(64):
            if p in procs:
                return procs[p]
            if p not in parent:
                break
            p = parent[p]
        return ("?", None)

    def record(pid, path, write, gz, call):
        exe, cwd = proc(pid)
        if not os.path.isabs(path):
            path = os.path.join(cwd or "/", path)
        path = os.path.normpath(path)
        a = access.get((pid, path))
        if a is None:
            a = access[(pid, path)] = {"exe": exe, "cwd": cwd, "write": False, "read": False,
                                       "gz": False, "calls": set()}
        a["write" if write else "read"] = True
        a["gz"] = a["gz"] or gz
        a["calls"].add(call)

    with open(log_path, "rb") as f:
        for raw in f:
            if b".nii.gz" not in raw and b"Initializing" not in raw and b"fork()" not in raw:
                continue
            m = LINE_RE.match(raw.decode("utf-8", "replace").rstrip("\n"))
            if not m:
                continue
            pid, msg = int(m.group(1)), m.group(2)

            m = INIT_RE.match(msg)
            if m:
                procs[pid] = (m.group(1), m.group(2))
                continue
            m = FORK_RE.match(msg)
            if m:
                child = int(m.group(1))
                if child > 0:
                    parent[child] = pid
                continue
            if SUFFIX not in msg:
                continue

            m = OPEN_RE.match(msg)
            if m:
                call, path, flags, ret = m.group(1), m.group(2), int(m.group(3), 16), int(m.group(4))
                if ret >= 0 and path.endswith(SUFFIX):
                    record(pid, path, bool(flags & O_ACCMODE or flags & O_CREAT), False, call)
                continue
            m = OPENAT_RE.match(msg)
            if m:
                call, dirfd, path = m.group(1), int(m.group(2)), m.group(3)
                flags, ret = int(m.group(4), 16), int(m.group(5))
                if ret >= 0 and path.endswith(SUFFIX):
                    if not os.path.isabs(path) and dirfd != AT_FDCWD:
                        unresolved_dirfd += 1
                    record(pid, path, bool(flags & O_ACCMODE or flags & O_CREAT), False, call)
                continue
            m = FOPEN_RE.match(msg)
            if m:
                call, path, mode, ret = m.groups()
                if ret not in ("(nil)", "0x0") and path.endswith(SUFFIX):
                    gz = call.startswith("gz")
                    write = any(c in mode for c in ("w", "a")) or (not gz and "+" in mode)
                    record(pid, path, write, gz, call)
                continue
            m = RENAME_RE.match(msg)
            if m and int(m.group(3)) == 0:
                renames.append((pid, m.group(1), m.group(2)))

    return access, procs, renames, unresolved_dirfd


def name_key(path):
    return SID_RE.sub("", os.path.basename(path))


def build_map(access, renames, tasks, staged, published, project_dir):
    """renames: (old, new) paths or names."""
    published = {canonical(p, staged) for p in published}
    published_keys = {name_key(p) for p in published}
    files = defaultdict(list)       # canonical path -> accesses
    for (pid, path), a in access.items():
        a = dict(a, pid=pid, task=task_of(a["cwd"], tasks) or "?", family=family(a["exe"]))
        files[canonical(path, staged)].append(a)

    rows = defaultdict(lambda: {"paths": set(), "accesses": []})
    for path, accs in files.items():
        key = name_key(path)
        rows[key]["paths"].add(path)
        rows[key]["accesses"].extend(accs)

    renamed_into = {name_key(new) for _, new in renames}
    dests = defaultdict(set)        # name -> names it is renamed into
    for old, new in renames:
        if name_key(old) != name_key(new):
            dests[name_key(old)].add(name_key(new))
    out = []
    for key in sorted(rows):
        paths, accs = rows[key]["paths"], rows[key]["accesses"]
        writers = [a for a in accs if a["write"]]
        readers = [a for a in accs if not a["write"]]
        sizes = [os.stat(p).st_size for p in paths if os.path.exists(p)]
        is_published = any(p in published for p in paths)
        inputs = [p for p in paths if not p.startswith(os.path.join(project_dir, "tractoflow_files/work/"))]
        fams = {a["family"] for a in accs}

        def why_not(allow_python):
            reasons = []
            if not writers:
                reasons.append("pipeline input" if inputs else "no writer in log")
            elif len(writers) > 1:
                reasons.append(f"written {len(writers)} times")
            if "ANTs" in fams:
                reasons.append("ANTs access")
            if not allow_python and "Python" in fams:
                reasons.append("Python access")
            non_gz = {a["family"] for a in accs
                      if not a["gz"] and not (allow_python and a["family"] == "Python")}
            if non_gz:
                reasons.append("non-gz access by " + "/".join(sorted(non_gz)))
            if is_published:
                reasons.append("published")
            return reasons

        strict, loose = why_not(False), why_not(True)
        fmt = lambda a: f"{a['task']}:{os.path.basename(a['exe'])}{'[gz]' if a['gz'] else ''}"
        out.append({
            "file": key,
            "names": ",".join(sorted({os.path.basename(p) for p in paths})),
            "n_paths": len(paths),
            "size_bytes": sum(sizes) if sizes else "",
            "published": is_published,
            "writers": ", ".join(sorted({fmt(a) for a in writers})) or "-",
            "readers": ", ".join(sorted({fmt(a) for a in readers})) or "-",
            "families": "/".join(sorted(fams)),
            "renamed_into": key in renamed_into,
            "renamed_to": ",".join(sorted(dests[key])),
            "eligible": not strict,
            "reason": "; ".join(strict),
            "eligible_loose": not loose,
            "reason_loose": "; ".join(loose),
        })

    # The data written under a name ends up under every name it's renamed into,
    # so a name is only eligible when those are. Their own missing writer is the
    # rename itself, so it doesn't count
    by_key = {r["file"]: r for r in out}

    def blocked(name, col, seen):
        for d in dests.get(name, ()):
            if d in seen:
                continue
            seen.add(d)
            r = by_key.get(d)
            own = (r["reason" if col == "eligible" else "reason_loose"].split("; ") if r
                   else ["published"] if d in published_keys else [])
            if [x for x in own if x and x != "no writer in log"] or blocked(d, col, seen):
                return d
        return None

    for r in out:
        for col, reason in (("eligible", "reason"), ("eligible_loose", "reason_loose")):
            d = blocked(r["file"], col, {r["file"]})
            if d:
                r[col] = False
                r[reason] = "; ".join(filter(None, [r[reason], f"renamed into {d}"]))
    return out


def staged_bytes(staged):
    total = nii = 0
    for copy, source in staged.items():
        p = copy if os.path.exists(copy) else source
        if os.path.exists(p):
            size = os.stat(p).st_size
            total += size
            nii += size if p.endswith(SUFFIX) else 0
    return total, nii


def trace_write_bytes(trace_path):
    units = {"B": 1, "KB": 1 << 10, "MB": 1 << 20, "GB": 1 << 30, "TB": 1 << 40}
    total = 0
    with open(trace_path) as f:
        for row in csv.DictReader(f, delimiter="\t"):
            m = re.match(r"([\d.]+) (\w+)", row.get("write_bytes", ""))
            if m:
                total += float(m.group(1)) * units.get(m.group(2), 1)
    return int(total)


def summarize(rows, title, eligible_col):
    def group(sel):
        rs = [r for r in rows if sel(r)]
        return len(rs), sum(r["n_paths"] for r in rs), sum(r["size_bytes"] or 0 for r in rs)

    print(f"\n{title}")
    print(f"  {'group':<24} {'names':>6} {'files':>6} {'GB':>9}")
    for name, sel in [("eligible", lambda r: r[eligible_col]),
                      ("published", lambda r: r["published"]),
                      ("touched by Python", lambda r: "Python" in r["families"]),
                      ("touched by ANTs", lambda r: "ANTs" in r["families"]),
                      ("all", lambda r: True)]:
        n, files, size = group(sel)
        print(f"  {name:<24} {n:>6} {files:>6} {size / 1e9:>9.3f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", required=True, help="full logger.so log of the run")
    ap.add_argument("--run-dir", required=True,
                    help="run output dir holding trace_*.txt, staged_inputs_*.tsv, results_listing_*.txt and renames_*.tsv")
    ap.add_argument("--project-dir", default="/projects/I20240002/nunolxm",
                    help="root the staged_inputs copy paths are relative to")
    ap.add_argument("--out", help="write the map as TSV here (default: stdout only)")
    args = ap.parse_args()

    def one(pattern):
        found = glob.glob(os.path.join(args.run_dir, pattern))
        if len(found) != 1:
            sys.exit(f"expected one {pattern} in {args.run_dir}, found {len(found)}")
        return found[0]

    trace = one("trace_*.txt")
    tasks = read_tasks(trace)
    staged = read_staged(one("staged_inputs_*.tsv"), args.project_dir)
    published = read_published(one("results_listing_*.txt"))
    mv_renames = read_renames(one("renames_*.tsv"))

    access, procs, log_renames, unresolved = parse_log(args.log)
    renames = [(old, new) for _, old, new in log_renames] + mv_renames
    rows = build_map(access, renames, tasks, staged, published, args.project_dir)

    cols = ["file", "names", "n_paths", "size_bytes", "published", "writers", "readers",
            "families", "renamed_into", "renamed_to", "eligible", "reason", "eligible_loose", "reason_loose"]
    if args.out:
        with open(args.out, "w") as f:
            w = csv.DictWriter(f, fieldnames=cols, delimiter="\t", extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        print(f"map: {args.out} ({len(rows)} names)")

    print(f"processes: {len(procs)}, .nii.gz accesses (pid, path): {len(access)}, "
          f"renames: {len(log_renames)} in the log, {len(mv_renames)} mv lines, "
          f"relative openat with a dirfd: {unresolved}")
    no_task = {a["exe"] for a in access.values() if not task_of(a["cwd"], tasks)}
    if no_task:
        print(f"accesses outside a task dir, by: {', '.join(sorted(no_task))}")
    summarize(rows, "Strict rule (gz API only, no ANTs/Python, not published):", "eligible")
    summarize(rows, "Loose rule (Python allowed; no ANTs, not published):", "eligible_loose")

    total, nii = staged_bytes(staged)
    written = sum(r["size_bytes"] or 0 for r in rows if r["writers"] != "-")
    print(f"\nNextflow staging copies: {len(staged)} files, {total / 1e9:.3f} GB "
          f"({nii / 1e9:.3f} GB .nii.gz)")
    print(f"tool-written .nii.gz still on disk: {written / 1e9:.3f} GB; "
          f"trace write_bytes (all tasks): {trace_write_bytes(trace) / 1e9:.3f} GB")

    missing = [r["file"] for r in rows if r["reason"].startswith("no writer in log") and not r["renamed_into"]]
    print(f"\nno writer in log (and not renamed into): {', '.join(missing) or 'none'}")
    moved = [f"{r['file']} -> {r['renamed_to']}" for r in rows if r["renamed_to"]]
    print(f"renamed: {', '.join(moved) or 'none'}")
    bad = [r["file"] for r in rows if r["published"] and r["eligible"]]
    if bad:
        print(f"ERROR: published files marked eligible: {', '.join(bad)}")


if __name__ == "__main__":
    main()
