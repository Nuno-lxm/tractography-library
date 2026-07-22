#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=8:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=test_buf_mgh8_x86
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/buffer/mgh8_x86_%j/out_%j.out
#SBATCH --error=/projects/I20240002/nunolxm/output/tests/buffer/mgh8_x86_%j/err_%j.err

module load Java/11.0.20

project_dir="/projects/I20240002/nunolxm"
output_dir="$project_dir/output/tests/buffer/mgh8_x86_${SLURM_JOB_ID}"

input_dir="$project_dir/data/mgh8"
results_dir="$project_dir/results/tests/buffer/mgh8_x86_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/tests/buffer/work_${SLURM_JOB_ID}"

report_file="$output_dir/report_test_buffer_mgh8_x86_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_test_buffer_mgh8_x86_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_test_buffer_mgh8_x86_${SLURM_JOB_ID}.txt"

config_file="$project_dir/tractography-library/config-files/deucalion/x86-buffer.config"

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
    done > "$output_dir/vmstat_test_buffer_mgh8_x86_${SLURM_JOB_ID}.log"
) &
VMSTAT_PID=$!

echo "SLURM_JOB_ID=$SLURM_JOB_ID"

NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    --run_pft_tracking false \
    --run_eddy false \
    --processes 128 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input="$input_dir" \
    --output_dir="$results_dir" \
    -c "$config_file"
