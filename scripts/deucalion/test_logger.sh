#!/bin/bash
#SBATCH --account=I20240002G
#SBATCH --nodes=1
#SBATCH --gpus=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --time=4:00:00
#SBATCH --partition normal-a100-40
#SBATCH --job-name=test_logger
#SBATCH --output=/projects/I20240002/nunolxm/output/test_logger/out_test_logger_%j.out
#SBATCH --error=/projects/I20240002/nunolxm/output/test_logger/err_test_logger_%j.err

module load Java/11.0.20

input_dir="/projects/I20240002/nunolxm/data/input1"
output_dir="/projects/I20240002/nunolxm/results/results_test_logger_${SLURM_JOB_ID}"
work_dir="/projects/I20240002/nunolxm/tractoflow_files/work_${SLURM_JOB_ID}"

config_file="/projects/I20240002/nunolxm/tractography-library/config-files/nextflow-logger.config"

report_file="/projects/I20240002/nunolxm/output/test_logger/report_test_logger_${SLURM_JOB_ID}.html"
timeline_file="/projects/I20240002/nunolxm/output/test_logger/timeline_test_logger_${SLURM_JOB_ID}.html"
trace_file="/projects/I20240002/nunolxm/output/test_logger/trace_test_logger_${SLURM_JOB_ID}.txt"


NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -c "$config_file" \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    -profile use_gpu \
    --run_pft_tracking false \
    --run_local_tracking true \
    --processes 32 \
    --processes_brain_extraction_t1 16 \
    --processes_denoise_dwi 16 \
    --processes_denoise_t1 16 \
    --processes_eddy 16 \
    --processes_fodf 16 \
    --processes_registration 16 \
    --processes_local_tracking 16 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input "$input_dir" \
    --output_dir "$output_dir" \