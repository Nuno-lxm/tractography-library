#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=8:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=x86_test

# Unified x86 full/buffer test script.
# Replaces: x86_full.sh, x86_buf.sh, x86_mgh_8.sh, x86_mgh_buf_8.sh,
#           x86_ssd_full.sh, x86_ssd_buf.sh
#
# Usage:
#   sbatch run_x86_test.sh --dataset Penthera_3T
#   sbatch run_x86_test.sh --dataset Penthera_3T --buffer
#   sbatch run_x86_test.sh --dataset mgh8 --buffer
#   sbatch run_x86_test.sh --dataset Penthera_3T --ssd
#   sbatch run_x86_test.sh --dataset Penthera_3T --buffer --ssd
#
# Options:
#   --dataset NAME   Dataset name under $project_dir/data/ (required)
#   --buffer         Enable buffer library (uses x86-buffer.config)
#   --ssd            Copy data to local SSD (/tmp) before running

set -euo pipefail

# ── Argument parsing ────────────────────────────────────────────────────────

DATASET=""
BUFFER=false
SSD=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dataset) DATASET="$2"; shift 2 ;;
        --buffer)  BUFFER=true; shift ;;
        --ssd)     SSD=true; shift ;;
        --help|-h)
            sed -n '2,/^$/{ s/^# \?//; p }' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$DATASET" ]]; then
    echo "Error: --dataset is required" >&2
    echo "Usage: sbatch $0 --dataset NAME [--buffer] [--ssd]" >&2
    exit 1
fi

# ── Config & paths ──────────────────────────────────────────────────────────

project_dir="/projects/I20240002/nunolxm"
input_dir="$project_dir/data/$DATASET"

if [[ "$BUFFER" == true ]]; then
    config_file="$project_dir/tractography-library/config-files/deucalion/x86-buffer.config"
    test_category="buffer"
else
    config_file="$project_dir/tractography-library/config-files/deucalion/x86-base.config"
    test_category="baseline"
fi

if [[ "$SSD" == true ]]; then
    test_name="${DATASET}_ssd_${test_category}"
else
    test_name="${DATASET}_${test_category}"
fi

output_dir="$project_dir/output/tests/${test_name}_${SLURM_JOB_ID}"
results_dir="$project_dir/results/tests/${test_name}_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/tests/${test_name}_work_${SLURM_JOB_ID}"

report_file="$output_dir/report_${test_name}_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_${test_name}_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_${test_name}_${SLURM_JOB_ID}.txt"

# ── SSD setup ───────────────────────────────────────────────────────────────

if [[ "$SSD" == true ]]; then
    local_base="/tmp/ssd_${DATASET}_${SLURM_JOB_ID}"
    local_input="$local_base/input/$DATASET"
    local_work="$local_base/work"
    local_results="$local_base/results"
    local_tractoflow="$local_base/tractoflow_files"
    run_input="$local_input"
    run_work="$local_work"
    run_results="$local_results"
    run_tractoflow="$local_tractoflow"
else
    run_input="$input_dir"
    run_work="$work_dir"
    run_results="$results_dir"
fi

# ── Setup ───────────────────────────────────────────────────────────────────

module load Java/11.0.20

mkdir -p "$output_dir"

if [[ "$SSD" == true ]]; then
    mkdir -p "$local_base"

    echo "[$(date)] Copying input data..."
    cp -r "$input_dir" "$local_base/input/"

    echo "[$(date)] Copying Singularity image..."
    cp "$project_dir/tractoflow_files/scilus_1.6.0.sif" "$local_tractoflow/"

    echo "[$(date)] Copying TractoFlow main.nf..."
    cp -r "$project_dir/tractoflow_files/tractoflow" "$local_tractoflow/"

    echo "[$(date)] Copy complete."
fi

exec > "$output_dir/out_${SLURM_JOB_ID}.out" 2> "$output_dir/err_${SLURM_JOB_ID}.err"

start_ts=$(date +%s)

cleanup() {
    if [[ -n "${VMSTAT_PID:-}" ]]; then
        kill "$VMSTAT_PID" 2>/dev/null || true
    fi
    if [[ "$SSD" == true ]]; then
        echo "[$(date)] Copying results back to Lustre..."
        mkdir -p "$results_dir"
        cp -r "$local_results"/* "$results_dir/" 2>/dev/null || true
        cp "$local_work"/.nextflow.log* "$output_dir/" 2>/dev/null || true
        echo "[$(date)] Cleaning up local SSD..."
        rm -rf "$local_base"
        echo "[$(date)] Done."
    fi
}
trap cleanup EXIT

(
    vmstat -S M 5 | while IFS= read -r line; do
        printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$line"
    done > "$output_dir/vmstat_${test_name}_${SLURM_JOB_ID}.log"
) &
VMSTAT_PID=$!

echo "SLURM_JOB_ID=$SLURM_JOB_ID"
echo "Dataset: $DATASET ($input_dir)"
echo "Buffer: $BUFFER"
echo "SSD: $SSD"
echo "Config: $config_file"

# ── Run ─────────────────────────────────────────────────────────────────────

if [[ "$SSD" == true ]]; then
    NXF_VER=21.04.0 nextflow run "$run_tractoflow/tractoflow/main.nf" \
        -with-singularity "$run_tractoflow/scilus_1.6.0.sif" \
        --run_pft_tracking false \
        --run_eddy false \
        --processes 128 \
        -with-report "$report_file" \
        -with-timeline "$timeline_file" \
        -with-trace "$trace_file" \
        -w "$run_work" \
        --input="$run_input" \
        --output_dir="$run_results" \
        -c "$config_file"
else
    NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
        -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
        --run_pft_tracking false \
        --run_eddy false \
        --processes 128 \
        -with-report "$report_file" \
        -with-timeline "$timeline_file" \
        -with-trace "$trace_file" \
        -w "$run_work" \
        --input="$run_input" \
        --output_dir="$run_results" \
        -c "$config_file"
fi

end_ts=$(date +%s)
elapsed=$((end_ts - start_ts))
echo "Total wall time: ${elapsed}s ($((elapsed / 60))m $((elapsed % 60))s)" | tee "$output_dir/wall_time_${SLURM_JOB_ID}.txt"
