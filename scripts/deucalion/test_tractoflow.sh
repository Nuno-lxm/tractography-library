#!/bin/bash
#SBATCH --account=I20240002G
#SBATCH --nodes=1
#SBATCH --gpus=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --time=4:00:00
#SBATCH --partition normal-a100-40
#SBATCH --job-name=tractoflow-test
#SBATCH --output=out-tractoflow-test_%j.out
#SBATCH --error=err-tractoflow-test_%j.err

module load Java/11.0.20

NXF_VER=21.04.0 nextflow run /projects/I20240002/nunolxm/tractoflow_files/tractoflow/main.nf \
    -with-singularity /projects/I20240002/nunolxm/tractoflow_files/scilus_1.6.0.sif \
    --help