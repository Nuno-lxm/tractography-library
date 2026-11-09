#!/bin/bash
#SBATCH --account=I20240002G
#SBATCH --nodes=1
#SBATCH --gpus=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --time=8:00:00
#SBATCH --partition=normal-a100-40
#SBATCH --job-name=tractoflow_gpu
#SBATCH --no-requeue

# TractoFlow on an A100 node: run_tractoflow.sh with the use_gpu profile (eddy
# on CUDA). minimal and full also turn on GPU local tracking (npv seeding, 15
# seeds, batch size 40000); default keeps TractoFlow's own tracking (PFT on
# CPU, no local tracking). Takes the same options as run_tractoflow.sh.
#
# Usage:
#   sbatch run_tractoflow_gpu.sh --dataset mgh1 --pipeline full
#   sbatch run_tractoflow_gpu.sh --dataset mgh1 --pipeline full --lib buffer
#   sbatch run_tractoflow_gpu.sh --dataset Penthera_3T --pipeline default --lib logger

set -euo pipefail
GPU=true
source /projects/I20240002/nunolxm/tractography-library/scripts/lib/common.sh
source /projects/I20240002/nunolxm/tractography-library/scripts/lib/tractoflow.sh
