#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --time=01:00:00
#SBATCH --partition=normal-x86
#SBATCH --job-name=map_logger
#SBATCH --no-requeue
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/map_logger_%j.out

# Everything to read out of a --lib logger run, as a job instead of on the
# login node (the log is several GB): the writer/reader map, the strict and
# loose zstd targets, the difference from a reference map's targets, and the
# processes that open gz files from more than one thread. Results go into
# RUN_DIR as io_map_<id>.tsv, zstd_targets_<id>.txt and
# zstd_targets_loose_<id>.txt (<id> is RUN_DIR's job id); the report is this
# job's output file.
#
# Usage (from the repo dir):
#   sbatch scripts/map_logger_run.sh LOG RUN_DIR [REFERENCE_MAP]
#   sbatch scripts/map_logger_run.sh logs/logger_2181339.log \
#       ../output/tests/default/mgh1_logger_gpu_2181339 \
#       ../output/tests/minimal/mgh1_logger_x86_2083793/io_map_2083793.tsv

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    sed -n '/^# Everything/,/^$/{ s/^# \?//; p }' "$0" >&2
    exit 1
fi
log="$1"
run_dir="$2"
reference="${3:-}"
for f in "$log" "$run_dir" ${reference:+"$reference"}; do
    [[ -e "$f" ]] || { echo "Error: $f not found" >&2; exit 1; }
done
id="${run_dir%/}"
id="${id##*_}"
map="$run_dir/io_map_$id.tsv"
# sbatch runs a copy of this script, so under Slurm the repo is found by path
scripts="/projects/I20240002/nunolxm/tractography-library/scripts"
[[ -z "${SLURM_JOB_ID:-}" ]] && scripts=$(dirname "$0")

echo "Log: $log ($(du -h "$log" | cut -f1)), run dir: $run_dir"
echo

echo "== Map"
python3 "$scripts"/build_io_map.py --log "$log" --run-dir "$run_dir" --out "$map"
echo

# zstd_targets.py fails when a glob would also match an ineligible file; the
# rest of the report is still worth having then
echo "== Strict targets (written to $run_dir/zstd_targets_$id.txt)"
python3 "$scripts"/zstd_targets.py --sep $'\n' "$map" | tee "$run_dir/zstd_targets_$id.txt" || echo "(failed)"
echo
echo "== Loose targets, Python allowed (written to $run_dir/zstd_targets_loose_$id.txt)"
python3 "$scripts"/zstd_targets.py --loose --sep $'\n' "$map" | tee "$run_dir/zstd_targets_loose_$id.txt" || echo "(failed)"
echo

if [[ -n "$reference" ]]; then
    echo "== Strict targets: < only in $reference, > only in this run"
    diff <(python3 "$scripts"/zstd_targets.py --sep $'\n' "$reference") "$run_dir/zstd_targets_$id.txt" && echo "(same)" || true
    echo
fi

echo "== Processes that called gzopen and logged more than one thread"
awk 'match($0, /pid=([0-9]+) tid=([0-9]+)\] (.*)/, m) {
        if (m[3] ~ /^gzopen/) gz[m[1]] = 1
        seen[m[1] " " m[2]] = 1
    }
    END {
        for (k in seen) { split(k, a, " "); if (a[1] in gz) n[a[1]]++ }
        for (p in n) if (n[p] > 1) { print "  pid " p ": " n[p] " threads"; found = 1 }
        if (!found) print "  none"
    }' "$log"
