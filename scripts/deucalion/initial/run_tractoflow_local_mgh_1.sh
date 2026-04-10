#!/bin/bash
#SBATCH --account=I20240002G
#SBATCH --nodes=1
#SBATCH --gpus=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --time=24:00:00
#SBATCH --partition normal-a100-40
#SBATCH --job-name=local_mgh_1
#SBATCH --output=/projects/I20240002/nunolxm/output/local_mgh_1/out_local_mgh_1_%j.out
#SBATCH --error=/projects/I20240002/nunolxm/output/local_mgh_1/err_local_mgh_1_%j.err

module load Java/11.0.20

input_dir="/projects/I20240002/nunolxm/data/mgh1"
output_dir="/projects/I20240002/nunolxm/results/results_local_mgh_1_${SLURM_JOB_ID}"
work_dir="/projects/I20240002/nunolxm/tractoflow_files/work_${SLURM_JOB_ID}"

config_file="/projects/I20240002/nunolxm/tractoflow_files/custom.config"
report_file="/projects/I20240002/nunolxm/output/local_mgh_1/report_local_mgh_1_${SLURM_JOB_ID}.html"
timeline_file="/projects/I20240002/nunolxm/output/local_mgh_1/timeline_local_mgh_1_${SLURM_JOB_ID}.html"
trace_file="/projects/I20240002/nunolxm/output/local_mgh_1/trace_local_mgh_1_${SLURM_JOB_ID}.txt"

vmstat 5 > /projects/I20240002/nunolxm/output/local_mgh_1/vmstat_local_mgh_1_${SLURM_JOB_ID}.log &
VMSTAT_PID=$!

NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    -profile use_gpu \
    --run_pft_tracking false \
    --run_local_tracking true \
    --local_seeding npv \
    --local_nbr_seeds 15\
    --processes 32 \
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
    --output_dir="$output_dir" \

kill $VMSTAT_PID