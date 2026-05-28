#!/bin/bash
#SBATCH --account=I20240002G
#SBATCH --nodes=1
#SBATCH --gpus=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --time=8:00:00
#SBATCH --partition=normal-a100-40
#SBATCH --job-name=test_buf_mgh1
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/buffer/mgh1_%j/out_%j.out
#SBATCH --error=/projects/I20240002/nunolxm/output/tests/buffer/mgh1_%j/err_%j.err

module load Java/11.0.20

project_dir="/projects/I20240002/nunolxm"
output_dir="$project_dir/output/tests/buffer/mgh1_${SLURM_JOB_ID}"

input_dir="$project_dir/data/mgh1"
results_dir="$project_dir/results/tests/buffer/mgh1_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/tests/buffer/work_${SLURM_JOB_ID}"

report_file="$output_dir/report_test_buffer_mgh1_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_test_buffer_mgh1_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_test_buffer_mgh1_${SLURM_JOB_ID}.txt"

config_file="$project_dir/tractography-library/config-files/deucalion/buffer-deucalion.config"

mkdir -p "$output_dir"

cleanup() {
    if [[ -n "${VMSTAT_PID:-}" ]]; then
        kill "$VMSTAT_PID" 2>/dev/null || true
    fi
    if [[ -n "${GPU_PID:-}" ]]; then
        kill "$GPU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

(
    vmstat -S M 5 | while IFS= read -r line; do
        printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$line"
    done > "$output_dir/vmstat_test_buffer_mgh1_${SLURM_JOB_ID}.log"
) &
VMSTAT_PID=$!

GPU_LOG="$output_dir/gpu_test_buffer_mgh1_${SLURM_JOB_ID}.log"
if command -v nvidia-smi >/dev/null 2>&1; then
    nvidia-smi --query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.total,memory.used,temperature.gpu,power.draw \
        --format=csv -l 5 > "$GPU_LOG" &
    GPU_PID=$!
else
    echo "nvidia-smi not found" > "$GPU_LOG"
fi

echo "SLURM_JOB_ID=$SLURM_JOB_ID"
echo "SLURM_JOB_GPUS=${SLURM_JOB_GPUS:-}"
echo "CUDA_VISIBLE_DEVICES=${CUDA_VISIBLE_DEVICES:-}"
nvidia-smi -L || true

NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    -profile use_gpu \
    --run_pft_tracking false \
    --run_local_tracking true \
    --local_seeding npv \
    --local_nbr_seeds 15 \
    --processes 32 \
    --local_batch_size_gpu 40000 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input="$input_dir" \
    --output_dir="$results_dir" \
    -c "$config_file"
