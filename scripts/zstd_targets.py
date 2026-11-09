#!/usr/bin/env python3
"""Print ZSTD_FILE_TARGETS globs from a build_io_map.py map.

A name seen with a "<sid>__" prefix becomes "*__<name>", one seen without it
stays exact. Every glob is checked against all names in the map, and the
script fails if one would also match a file that isn't eligible. With --final
it prints FINAL_FILE_TARGETS globs instead: the published files, checked
against the ones that aren't.

Usage:
  scripts/zstd_targets.py ../output/tests/minimal/mgh1_logger_x86_2083793/io_map_2083793.tsv
  scripts/zstd_targets.py --loose MAP     # Python accesses allowed (needs a Python-side hook)
  scripts/zstd_targets.py --final MAP     # final outputs, for FINAL_FILE_TARGETS
"""
import argparse
import csv
import re
import sys
from fnmatch import fnmatchcase

SID_RE = re.compile(r"^.+?__")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map", help="TSV written by build_io_map.py --out")
    ap.add_argument("--loose", action="store_true", help="use the eligible_loose column")
    ap.add_argument("--final", action="store_true", help="globs for the published files instead")
    ap.add_argument("--sep", default=",", help="separator between globs (default: ,)")
    args = ap.parse_args()

    if args.loose and args.final:
        sys.exit("--loose and --final don't go together")
    col = "published" if args.final else "eligible_loose" if args.loose else "eligible"
    with open(args.map) as f:
        rows = list(csv.DictReader(f, delimiter="\t"))

    globs, others = set(), set()
    for r in rows:
        names = r["names"].split(",")
        if r[col] == "True":
            for n in names:
                globs.add("*__" + SID_RE.sub("", n) if SID_RE.match(n) else n)
        else:
            others.update(names)

    clashes = [(g, n) for g in globs for n in others if fnmatchcase(n, g)]
    if clashes:
        for g, n in sorted(clashes):
            print(f"glob {g} also matches {'unpublished' if args.final else 'ineligible'} {n}", file=sys.stderr)
        sys.exit(1)
    print(args.sep.join(sorted(globs)))


if __name__ == "__main__":
    main()
