#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=8:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=buf_ssd_pent3t
#SBATCH --output=/projects/I20240002/nunolxm/output/buffer/ssd/pent3t_%j/out_%j.out
#SBATCH --error=/projects/I20240002/nunolxm/output/buffer/ssd/pent3t_%j/err_%j.err

module load Java/11.0.20

project_dir="/projects/I20240002/nunolxm"
output_dir="$project_dir/output/buffer/ssd/pent3t_${SLURM_JOB_ID}"
results_dir="$project_dir/results/buffer/ssd/pent3t_${SLURM_JOB_ID}"
config_file="$project_dir/tractography-library/config-files/deucalion/x86-buffer.config"

local_base="/tmp/ssd_pent3t_${SLURM_JOB_ID}"
local_input="$local_base/input/Penthera_3T"
local_work="$local_base/work"
local_results="$local_base/results"
local_tractoflow="$local_base/tractoflow_files"

report_file="$output_dir/report_ssd_pent3t_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_ssd_pent3t_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_ssd_pent3t_${SLURM_JOB_ID}.txt"

mkdir -p "$output_dir" "$local_base"

echo "[$(date)] Copying input data..."
cp -r "$project_dir/data/Penthera_3T" "$local_base/input/"

echo "[$(date)] Copying Singularity image..."
cp "$project_dir/tractoflow_files/scilus_1.6.0.sif" "$local_tractoflow/"

echo "[$(date)] Copying TractoFlow main.nf..."
cp -r "$project_dir/tractoflow_files/tractoflow" "$local_tractoflow/"

echo "[$(date)] Copy complete."

start_ts=$(date +%s)

cleanup() {
    [[ -n "${VMSTAT_PID:-}" ]] && kill "$VMSTAT_PID" 2>/dev/null || true
    echo "[$(date)] Copying results back to Lustre..."
    mkdir -p "$results_dir"
    cp -r "$local_results"/* "$results_dir/" 2>/dev/null || true
    cp "$local_work"/.nextflow.log* "$output_dir/" 2>/dev/null || true
    echo "[$(date)] Cleaning up local SSD..."
    rm -rf "$local_base"
    echo "[$(date)] Done."
}
trap cleanup EXIT

(
    vmstat -S M 5 | while IFS= read -r line; do
        printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$line"
    done > "$output_dir/vmstat_ssd_pent3t_${SLURM_JOB_ID}.log"
) &
VMSTAT_PID=$!

echo "SLURM_JOB_ID=$SLURM_JOB_ID"
echo "Local base: $local_base"

NXF_VER=21.04.0 nextflow run "$local_tractoflow/tractoflow/main.nf" \
    -with-singularity "$local_tractoflow/scilus_1.6.0.sif" \
    --run_pft_tracking false \
    --run_eddy false \
    --processes 128 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$local_work" \
    --input="$local_input" \
    --output_dir="$local_results" \
    -c "$config_file"

end_ts=$(date +%s)
elapsed=$((end_ts - start_ts))
echo "Total wall time: ${elapsed}s ($((elapsed / 60))m $((elapsed % 60))s)" | tee "$output_dir/wall_time_${SLURM_JOB_ID}.txt"
