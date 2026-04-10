#!/bin/bash
#SBATCH --account=I20240002G
#SBATCH --nodes=1
#SBATCH --gpus=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --time=8:00:00
#SBATCH --partition normal-a100-40
#SBATCH --job-name=local_pent1
#SBATCH --output=/projects/I20240002/nunolxm/output/baseline/local/pent1_%j/out_%j.out
#SBATCH --error=/projects/I20240002/nunolxm/output/baseline/local/pent1_%j/err_%j.err

module load Java/11.0.20

project_dir="/projects/I20240002/nunolxm"
output_dir="$project_dir/output/baseline/local/pent1_${SLURM_JOB_ID}"

input_dir="$project_dir/data/penthera1"
results_dir="$project_dir/results/baseline/local/pent1_${SLURM_JOB_ID}"
work_dir="$project_dir/tractoflow_files/work/baseline/work_${SLURM_JOB_ID}"

report_file="$output_dir/report_local_pent1_${SLURM_JOB_ID}.html"
timeline_file="$output_dir/timeline_local_pent1_${SLURM_JOB_ID}.html"
trace_file="$output_dir/trace_local_pent1_${SLURM_JOB_ID}.txt"

vmstat 5 > "$output_dir"/vmstat_local_pent1_${SLURM_JOB_ID}.log &
VMSTAT_PID=$!

NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    -profile use_gpu \
    --run_pft_tracking false \
    --run_local_tracking true \
    --local_seeding npv \
    --local_nbr_seeds 15 \
    --processes 32 \
    --processes_brain_extraction_t1 16 \
    --processes_denoise_dwi 16 \
    --processes_denoise_t1 16 \
    --processes_eddy 16 \
    --processes_fodf 16 \
    --processes_registration 16 \
    --processes_local_tracking 8 \
    --local_batch_size_gpu 40000 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input="$input_dir" \
    --output_dir="$results_dir"

kill $VMSTAT_PID