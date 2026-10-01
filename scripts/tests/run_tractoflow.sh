#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=8:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=tractoflow
#SBATCH --no-requeue

# TractoFlow on a dataset, with or without one of the libraries.
#
# Usage:
#   sbatch run_tractoflow.sh --dataset mgh1
#   sbatch run_tractoflow.sh --dataset mgh1 --lib buffer
#   sbatch run_tractoflow.sh --dataset mgh8 --lib deflate-py --level 1 --pipeline full
#   sbatch run_tractoflow.sh --dataset Penthera_3T --pipeline full --ssd
#
# --dataset NAME      data/NAME (required)
# --pipeline minimal|full
#                     minimal (default): preprocessing without denoising, topup,
#                     eddy and tracking; full: everything but eddy
#                     and PFT tracking
# --lib, --level, --debug, --zstd-targets
#                     library options, see scripts/lib/common.sh
# --ssd               copy the input, container and TractoFlow to the node's
#                     /tmp and run from there
#
# run_tractoflow_gpu.sh takes the same options and runs on an A100 node with
# the use_gpu profile and GPU local tracking.

set -euo pipefail
GPU=false
source /projects/I20240002/nunolxm/tractography-library/scripts/lib/common.sh
source /projects/I20240002/nunolxm/tractography-library/scripts/lib/tractoflow.sh
