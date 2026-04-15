#!/bin/bash
#SBATCH --account=I20240002G
#SBATCH --nodes=1
#SBATCH --gpus=2
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --time=8:00:00
#SBATCH --partition normal-a100-40
#SBATCH --job-name=local_mgh4_2g
#SBATCH --output=/projects/I20240002/nunolxm/output/baseline/local/mgh4_2g_%j/out_%j.out
#SBATCH --error=/projects/I20240002/nunolxm/output/baseline/local/mgh4_2g_%j/err_%j.err

module load Java/11.0.20

project_dir="/projects/I20240002/nunolxm"
output_dir="$project_dir/output/baseline/local/mgh4_2g_${SLURM_JOB_ID}"

input_dir="$project_dir/data/mgh4"
results_dir="$project_dir/results/baseline/local/mgh4_2g_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/baseline/work_${SLURM_JOB_ID}"

report_file="$output_dir/report_local_mgh4_2g_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_local_mgh4_2g_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_local_mgh4_2g_${SLURM_JOB_ID}.txt"

mkdir -p "$output_dir"

vmstat 5 > "$output_dir"/vmstat_local_mgh4_2g_${SLURM_JOB_ID}.log &
VMSTAT_PID=$!

NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    -profile use_gpu \
    --run_pft_tracking false \
    --run_local_tracking true \
    --local_seeding npv \
    --local_nbr_seeds 15 \
    --processes 64 \
    --processes_brain_extraction_t1 16 \
    --processes_denoise_dwi 16 \
    --processes_denoise_t1 16 \
    --processes_eddy 16 \
    --processes_fodf 16 \
    --processes_registration 16 \
    --processes_local_tracking 16 \
    --local_batch_size_gpu 40000 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input="$input_dir" \
    --output_dir="$results_dir"

kill $VMSTAT_PID

exit 0
