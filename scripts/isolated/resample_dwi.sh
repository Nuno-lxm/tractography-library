#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=01:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=resample_dwi
#SBATCH --no-requeue

# Unified isolated Resample_DWI test.
#
# Usage:
#   sbatch resample_dwi.sh --dataset mgh1 --compression gz --mode none
#   sbatch resample_dwi.sh --dataset mgh1 --compression nii --mode full
#   sbatch resample_dwi.sh --dataset mgh1 --compression deflate
#   sbatch resample_dwi.sh --dataset mgh1 --compression deflate-py
#
# Modes (all load x86-buffer.config, so only the library settings differ):
#   none      library not loaded
#   prefetch  read prefetching only
#   full      read prefetching and write buffering
#
# --compression gz|nii|deflate|deflate-py
#                       gz: .nii.gz as in TractoFlow; nii: uncompressed .nii;
#                       deflate: .nii.gz written through compression_lib.so
#                       (libdeflate), buffer library off (--mode must be none);
#                       deflate-py: deflate plus Python's zlib streams
#                       (nibabel) and crc32 through libdeflate
# --debug               with deflate/deflate-py: log every gzopen/gzwrite/gzclose
#                       and deflateInit2_/deflate/deflateEnd
# --level N             with deflate/deflate-py: compress every image at libdeflate
#                       level N (0-12) instead of the level each tool asks for
# --test-dir NAME       put output, results and work under tests/isolated/NAME
#                       (default: resample)
# --publish             copy the resampled DWI to results (off by default: ~6 GB each)
# --clean-work          delete the work dir at the end
#
# Expected input layout:
#   data/resample_dwi/<dataset>/S1/      dwi.nii.gz  mask.nii.gz
#   data/resample_dwi_nii/<dataset>/S1/  dwi.nii     mask.nii

set -euo pipefail

# ── Argument parsing ────────────────────────────────────────────────────────

DATASET=""
MODE="none"
COMPRESSION="gz"
TEST_DIR="resample"
PUBLISH=false
CLEAN_WORK=false
DEBUG=false
LEVEL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dataset)
            DATASET="$2"
            shift 2
            ;;
        --mode)
            MODE="$2"
            shift 2
            ;;
        --compression)
            COMPRESSION="$2"
            shift 2
            ;;
        --test-dir)
            TEST_DIR="$2"
            shift 2
            ;;
        --publish)
            PUBLISH=true
            shift
            ;;
        --clean-work)
            CLEAN_WORK=true
            shift
            ;;
        --debug)
            DEBUG=true
            shift
            ;;
        --level)
            LEVEL="$2"
            shift 2
            ;;
        --help|-h)
            sed -n '2,/^$/{ s/^# \?//; p }' "$0"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$DATASET" ]]; then
    echo "Error: --dataset is required" >&2
    echo "Usage: sbatch $0 --dataset NAME [--compression gz|nii|deflate|deflate-py] [--mode none|prefetch|full] [--level N]" >&2
    exit 1
fi

case "$MODE" in
    none|prefetch|full) ;;
    *)
        echo "Error: --mode must be none, prefetch or full (got: $MODE)" >&2
        exit 1
        ;;
esac

case "$COMPRESSION" in
    gz|deflate|deflate-py) ext="nii.gz"; data_name="resample_dwi" ;;
    nii)                   ext="nii";    data_name="resample_dwi_nii" ;;
    *)
        echo "Error: --compression must be gz, nii, deflate or deflate-py (got: $COMPRESSION)" >&2
        exit 1
        ;;
esac

if [[ "$COMPRESSION" == deflate* && "$MODE" != none ]]; then
    echo "Error: --compression $COMPRESSION only works with --mode none" >&2
    exit 1
fi

if [[ -n "$LEVEL" ]]; then
    if [[ "$COMPRESSION" != deflate* ]]; then
        echo "Error: --level only works with --compression deflate or deflate-py" >&2
        exit 1
    fi
    if ! [[ "$LEVEL" =~ ^[0-9]+$ ]] || (( LEVEL > 12 )); then
        echo "Error: --level must be 0-12 (got: $LEVEL)" >&2
        exit 1
    fi
fi


# ── Config & paths ──────────────────────────────────────────────────────────

project_dir="/projects/I20240002/nunolxm"
pipeline_file="$project_dir/tractography-library/src/resample_only.nf"
input_dir="$project_dir/data/$data_name/$DATASET"

baseline_config="$project_dir/tractoflow_files/tractoflow/nextflow.config"
config_dir="$project_dir/tractography-library/config-files/deucalion"
buffer_lib="$project_dir/tractography-library/bin/buffer_lib.so"

config_args=(-c "$baseline_config" -c "$config_dir/x86-buffer.config")

case "$MODE" in
    none)     config_args+=(-c "$config_dir/x86-buffer-none.config") ;;
    prefetch) config_args+=(-c "$config_dir/x86-buffer-prefetch.config") ;;
    full)     ;;
esac

# compression_lib replaces the (disabled) buffer library in LD_PRELOAD
if [[ "$COMPRESSION" == deflate* ]]; then
    config_args+=(-c "$config_dir/x86-compression.config")
    if [[ "$COMPRESSION" == deflate-py ]]; then
        config_args+=(-c "$config_dir/x86-compression-streams.config")
    fi
    if [[ "$DEBUG" == true ]]; then
        config_args+=(-c "$config_dir/x86-compression-debug.config")
    fi
fi

test_name="${DATASET}_resample_${COMPRESSION}${LEVEL:+_l$LEVEL}_${MODE}_x86"
output_dir="$project_dir/output/tests/isolated/${TEST_DIR}/${test_name}_${SLURM_JOB_ID}"
results_dir="$project_dir/results/tests/isolated/${TEST_DIR}/${test_name}_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/tests/isolated/${TEST_DIR}/${test_name}_work_${SLURM_JOB_ID}"

report_file="$output_dir/report_${test_name}_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_${test_name}_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_${test_name}_${SLURM_JOB_ID}.txt"

# ── Setup ───────────────────────────────────────────────────────────────────

module load Java/11.0.20

mkdir -p "$output_dir"

# COMPRESSION_LEVEL overrides the level every tool asks compression_lib for
if [[ -n "$LEVEL" ]]; then
    level_config="$output_dir/compression_level.config"
    printf 'env {\n    COMPRESSION_LEVEL = %s\n}\n' "$LEVEL" > "$level_config"
    config_args+=(-c "$level_config")
fi

exec > "$output_dir/out_${SLURM_JOB_ID}.out" \
     2> "$output_dir/err_${SLURM_JOB_ID}.err"

cleanup() {
    if [[ -n "${VMSTAT_PID:-}" ]]; then
        kill "$VMSTAT_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

(
    vmstat -S M 5 | while IFS= read -r line; do
        printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$line"
    done > "$output_dir/vmstat_${test_name}_${SLURM_JOB_ID}.log"
) &
VMSTAT_PID=$!

if [[ ! -f "$pipeline_file" ]]; then
    echo "Missing workflow: $pipeline_file" >&2
    exit 1
fi

if [[ ! -d "$input_dir" ]]; then
    echo "Missing input directory: $input_dir" >&2
    exit 1
fi

subject_count=$(find "$input_dir" -mindepth 1 -maxdepth 1 -type d | wc -l)

if [[ "$subject_count" -eq 0 ]]; then
    echo "No subject directories found in: $input_dir" >&2
    exit 1
fi

echo "SLURM_JOB_ID=$SLURM_JOB_ID"
echo "Dataset: $DATASET ($input_dir)"
echo "Subjects: $subject_count"
echo "Compression: $COMPRESSION${LEVEL:+ (level $LEVEL)}"
echo "Mode: $MODE"
echo "Node: $(hostname)"
echo "Buffer lib: $(strings "$buffer_lib" | awk '/^[0-9a-f]{7}(-dirty)?$/ { v = $0 } END { print v ? v : "unknown" }') (md5 $(md5sum "$buffer_lib" | cut -d' ' -f1))"
echo "Config: ${config_args[*]}"
echo "Workflow: $pipeline_file"

# ── Run isolated Resample_DWI ───────────────────────────────────────────────

NXF_VER=21.04.0 nextflow run "$pipeline_file" \
    -with-singularity "$project_dir/tractoflow_files/scilus_1.6.0.sif" \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input="$input_dir" \
    --output="$results_dir" \
    --ext="$ext" \
    --publish="$PUBLISH" \
    "${config_args[@]}"

# Show the library settings a task actually ran with
task_run=$(find "$work_dir" -name .command.run -print -quit)
if [[ -n "$task_run" ]]; then
    echo "Task env ($task_run):"
    grep -E '^export (LD_PRELOAD|BUFFER_|COMPRESSION_)' "$task_run" || echo "  (no buffer settings)"
fi

# Per-command times and compression_lib summaries from each task's stderr
for err in $(find "$work_dir" -name .command.err); do
    echo "Task $(basename "$(dirname "$err")"):"
    grep -E '^TIME |^compression_lib:' "$err" | sed 's/^/  /' || true
done

# Size of every image the tasks wrote (intermediates and output)
echo "Image files in work dir:"
find "$work_dir" -type f \( -name '*.nii' -o -name '*.nii.gz' \) -printf '  %s %f\n' |
    awk '{ printf "  %8.3f GB  %s\n", $1 / 1e9, $2; total += $1 } END { printf "  %8.3f GB  total\n", total / 1e9 }'

# Kernel I/O counters summed over all tasks (independent of what the library sees)
find "$work_dir" -name .command.trace -exec cat {} + | awk -F= '
    $1 == "syscr" { syscr += $2 }  $1 == "syscw" { syscw += $2 }
    $1 == "rchar" { rchar += $2 }  $1 == "wchar" { wchar += $2 }
    $1 == "peak_rss" { tasks++; if ($2 > rss) rss = $2 }
    END {
        printf "I/O totals over %d tasks:\n", tasks
        printf "  read calls  %12d  %8.2f GB  avg %10.0f B\n", syscr, rchar / 1e9, syscr ? rchar / syscr : 0
        printf "  write calls %12d  %8.2f GB  avg %10.0f B\n", syscw, wchar / 1e9, syscw ? wchar / syscw : 0
        printf "  max peak_rss %.1f GB\n", rss / 1024 / 1024
    }'

if [[ "$CLEAN_WORK" == true ]]; then
    rm -rf "$work_dir"
    echo "Removed work dir: $work_dir"
fi
