#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=8:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=compression
#SBATCH --no-requeue

# TractoFlow (minimal by default) with compression_lib.so.
#
# Usage:
#   sbatch run_compression_test.sh --dataset mgh1 --compression gz
#   sbatch run_compression_test.sh --dataset mgh1 --compression deflate-py --level 1 --full
#
# --compression gz|deflate|deflate-py
#                   gz: no library (baseline); deflate: gzopen/gzwrite/gzclose
#                   through libdeflate; deflate-py: also Python's raw deflate
#                   streams and crc32
# --level N         libdeflate level N (0-12) for every image
# --debug           log every wrapped call
# --full            run all of TractoFlow instead of the minimal pipeline

set -euo pipefail

DATASET=""
COMPRESSION="gz"
LEVEL=""
DEBUG=false
FULL=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dataset)     DATASET="$2"; shift 2 ;;
        --compression) COMPRESSION="$2"; shift 2 ;;
        --level)       LEVEL="$2"; shift 2 ;;
        --debug)       DEBUG=true; shift ;;
        --full)        FULL=true; shift ;;
        --help|-h)     sed -n '2,/^$/{ s/^# \?//; p }' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$DATASET" || ! "$COMPRESSION" =~ ^(gz|deflate|deflate-py)$ ]]; then
    echo "Usage: sbatch $0 --dataset NAME [--compression gz|deflate|deflate-py] [--level N] [--debug] [--full]" >&2
    exit 1
fi

# ── Paths ───────────────────────────────────────────────────────────────────

project_dir="/projects/I20240002/nunolxm"
pipeline=$([[ "$FULL" == true ]] && echo full || echo minimal)
test_name="${DATASET}_${pipeline}_${COMPRESSION}${LEVEL:+_l$LEVEL}_x86_${SLURM_JOB_ID}"

output_dir="$project_dir/output/tests/compression/$pipeline/$test_name"
results_dir="$project_dir/results/tests/compression/$pipeline/$test_name"
work_dir="$project_dir/tractoflow_files/work/tests/compression/$pipeline/$test_name"

mkdir -p "$output_dir"
exec > "$output_dir/out.out" 2> "$output_dir/err.err"

# ── Config (one per run, generated here) ────────────────────────────────────

config="$output_dir/run.config"
{
    echo "singularity.runOptions = '--no-home --bind $project_dir/tractography-library/:/mnt:rw'"
    echo "executor.queueSize = 1000"
    echo "process { withLabel: 'big_mem' { maxForks = 999 } }"
    if [[ "$COMPRESSION" != gz ]]; then
        echo "env {"
        echo "    LD_PRELOAD = '/mnt/bin/compression_lib.so'"
        [[ "$COMPRESSION" == deflate-py ]] && echo "    COMPRESSION_STREAMS = 1"
        [[ "$DEBUG" == true ]]             && echo "    COMPRESSION_DEBUG = 1"
        [[ -n "$LEVEL" ]]                  && echo "    COMPRESSION_LEVEL = $LEVEL"
        echo "}"
    fi
} > "$config"

minimal_args=()
if [[ "$FULL" != true ]]; then
    minimal_args=(--run_dwi_denoising false --run_topup false --run_eddy false
                  --run_resample_dwi false --run_t1_denoising false --run_resample_t1 false
                  --run_pft_tracking false --run_local_tracking false)
fi

echo "Job $SLURM_JOB_ID on $(hostname): $test_name"
echo "compression_lib.so md5: $(md5sum "$project_dir/tractography-library/bin/compression_lib.so" | cut -d' ' -f1)"
cat "$config"

# ── Run ─────────────────────────────────────────────────────────────────────

module load Java/11.0.20

vmstat -S M -t 5 > "$output_dir/vmstat.log" &
trap 'kill $! 2>/dev/null' EXIT

NXF_VER=21.04.0 nextflow run "$project_dir/tractoflow_files/tractoflow/main.nf" \
    -with-singularity "$project_dir/tractoflow_files/scilus_1.6.0.sif" \
    -with-report "$output_dir/report.html" \
    -with-timeline "$output_dir/timeline.html" \
    -with-trace "$output_dir/trace.txt" \
    -w "$work_dir" \
    -c "$config" \
    --input "$project_dir/data/$DATASET" \
    --output_dir "$results_dir" \
    --processes 128 \
    "${minimal_args[@]}"

# ── Summary: compression_lib totals per process ─────────────────────────────

echo "compression_lib per process (files, GB in -> out, seconds):"
for err in $(find "$work_dir" -name .command.err); do
    name=$(sed -n 's/^# NEXTFLOW TASK: \([^ ]*\).*/\1/p' "$(dirname "$err")/.command.run")
    grep '^compression_lib:' "$err" | sed "s/^/$name /" || true
done | awk '{
        for (i = 1; i <= NF; i++) if ($i == "->") { bin[$1] += $(i - 1); bout[$1] += $(i + 1) }
        files[$1]++; secs[$1] += $(NF - 1)
    }
    END { for (p in files) printf "  %-30s %5d  %8.3f -> %8.3f  %8.2f\n", p, files[p], bin[p] / 1e9, bout[p] / 1e9, secs[p] }'
