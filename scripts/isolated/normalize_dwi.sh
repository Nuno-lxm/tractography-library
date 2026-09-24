#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=00:30:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=normalize_dwi
#SBATCH --no-requeue

# Unified isolated Normalize_DWI test.
#
# Usage:
#   sbatch normalize_dwi.sh --dataset mgh1 --mode none
#   sbatch normalize_dwi.sh --dataset mgh1 --mode prefetch
#   sbatch normalize_dwi.sh --dataset mgh1 --mode full
#
# Modes (all load x86-buffer.config, so only the library settings differ):
#   none      library not loaded
#   prefetch  read prefetching only
#   full      read prefetching and write buffering (--buffer is an alias)
#
# --test-dir NAME puts output, results and work under tests/isolated/NAME
# (default: normalize).
#
# Expected input layout:
#   data/normalize_dwi/<dataset>/S1/
#       dwi.nii.gz
#       mask.nii.gz
#       bval
#       bvec

set -euo pipefail

# ── Argument parsing ────────────────────────────────────────────────────────

DATASET=""
MODE="none"
TEST_DIR="normalize"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dataset)
            DATASET="$2"
            shift 2
            ;;
        --test-dir)
            TEST_DIR="$2"
            shift 2
            ;;
        --mode)
            MODE="$2"
            shift 2
            ;;
        --buffer)
            MODE="full"
            shift
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
    echo "Usage: sbatch $0 --dataset NAME [--mode none|prefetch|full]" >&2
    exit 1
fi

case "$MODE" in
    none|prefetch|full) ;;
    *)
        echo "Error: --mode must be none, prefetch or full (got: $MODE)" >&2
        exit 1
        ;;
esac


# ── Config & paths ──────────────────────────────────────────────────────────

project_dir="/projects/I20240002/nunolxm"
pipeline_file="$project_dir/tractography-library/src/normalize_only.nf"
input_dir="$project_dir/data/normalize_dwi/$DATASET"

baseline_config="$project_dir/tractoflow_files/tractoflow/nextflow.config"
config_dir="$project_dir/tractography-library/config-files/deucalion"
buffer_lib="$project_dir/tractography-library/bin/buffer_lib.so"

config_args=(-c "$baseline_config" -c "$config_dir/x86-buffer.config")

case "$MODE" in
    none)     config_args+=(-c "$config_dir/x86-buffer-none.config") ;;
    prefetch) config_args+=(-c "$config_dir/x86-buffer-prefetch.config") ;;
    full)     ;;
esac

test_name="${DATASET}_normalize_${MODE}_x86"
output_dir="$project_dir/output/tests/isolated/${TEST_DIR}/${test_name}_${SLURM_JOB_ID}"
results_dir="$project_dir/results/tests/isolated/${TEST_DIR}/${test_name}_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/tests/isolated/${TEST_DIR}/${test_name}_work_${SLURM_JOB_ID}"

report_file="$output_dir/report_${test_name}_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_${test_name}_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_${test_name}_${SLURM_JOB_ID}.txt"

# ── Setup ───────────────────────────────────────────────────────────────────

module load Java/11.0.20

mkdir -p "$output_dir"

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
echo "Mode: $MODE"
echo "Node: $(hostname)"
echo "Buffer lib: $(strings "$buffer_lib" | awk '/^[0-9a-f]{7}(-dirty)?$/ { v = $0 } END { print v ? v : "unknown" }') (md5 $(md5sum "$buffer_lib" | cut -d' ' -f1))"
echo "Config: ${config_args[*]}"
echo "Workflow: $pipeline_file"

# ── Run isolated Normalize_DWI ──────────────────────────────────────────────

NXF_VER=21.04.0 nextflow run "$pipeline_file" \
    -with-singularity "$project_dir/tractoflow_files/scilus_1.6.0.sif" \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input="$input_dir" \
    --output="$results_dir" \
    "${config_args[@]}"

# Show the library settings a task actually ran with
task_run=$(find "$work_dir" -name .command.run -print -quit)
if [[ -n "$task_run" ]]; then
    echo "Task env ($task_run):"
    grep -E '^export (LD_PRELOAD|BUFFER_)' "$task_run" || echo "  (no buffer settings)"
fi
