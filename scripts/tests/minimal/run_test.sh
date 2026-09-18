#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=8:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86

# Unified minimal test script for tractoflow pipeline.
# Replaces: mgh.sh, mgh_buffer.sh, mgh8_buffer.sh, minimal_base.sh,
#           pent.sh, pent_buffer.sh, minimal_pent1.sh
#
# Usage:
#   sbatch run_test.sh --dataset pent1
#   sbatch run_test.sh --dataset pent1 --buffer
#   sbatch run_test.sh --dataset mgh8
#   sbatch run_test.sh --dataset mgh8 --buffer
#   sbatch run_test.sh --dataset Penthera_3T
#
# Options:
#   --dataset NAME   Dataset name under $project_dir/data/ (required)
#   --buffer         Enable buffer library (uses minimal-buffer.config)

set -euo pipefail

# ── Argument parsing ────────────────────────────────────────────────────────

DATASET=""
BUFFER=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dataset) DATASET="$2"; shift 2 ;;
        --buffer)  BUFFER=true; shift ;;
        --help|-h)
            sed -n '2,/^$/{ s/^# \?//; p }' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$DATASET" ]]; then
    echo "Error: --dataset is required" >&2
    echo "Usage: sbatch $0 --dataset NAME [--buffer]" >&2
    exit 1
fi

# ── Config & paths ──────────────────────────────────────────────────────────

project_dir="/projects/I20240002/nunolxm"
input_dir="$project_dir/data/$DATASET"

if [[ "$BUFFER" == true ]]; then
    config_file="$project_dir/tractography-library/config-files/deucalion/minimal-buffer.config"
    test_name="${DATASET}_buffer_x86"
else
    config_file="$project_dir/tractography-library/config-files/deucalion/x86-base.config"
    test_name="${DATASET}_x86"
fi

output_dir="$project_dir/output/tests/minimal/${test_name}_${SLURM_JOB_ID}"
results_dir="$project_dir/results/tests/minimal/${test_name}_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/tests/minimal/work_${SLURM_JOB_ID}"

report_file="$output_dir/report_test_minimal_${test_name}_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_test_minimal_${test_name}_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_test_minimal_${test_name}_${SLURM_JOB_ID}.txt"

# ── Setup ───────────────────────────────────────────────────────────────────

module load Java/11.0.20

mkdir -p "$output_dir"

cleanup() {
    if [[ -n "${VMSTAT_PID:-}" ]]; then
        kill "$VMSTAT_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

(
    vmstat -S M 5 | while IFS= read -r line; do
        printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$line"
    done > "$output_dir/vmstat_test_minimal_${test_name}_${SLURM_JOB_ID}.log"
) &
VMSTAT_PID=$!

echo "SLURM_JOB_ID=$SLURM_JOB_ID"
echo "Dataset: $DATASET ($input_dir)"
echo "Buffer: $BUFFER"
echo "Config: $config_file"

# ── Run ─────────────────────────────────────────────────────────────────────

NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    --run_dwi_denoising false \
    --run_topup false \
    --run_eddy false \
    --run_resample_dwi false \
    --run_t1_denoising false \
    --run_resample_t1 false \
    --run_pft_tracking false \
    --run_local_tracking false \
    --processes 128 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input="$input_dir" \
    --output_dir="$results_dir" \
    -c "$config_file"
