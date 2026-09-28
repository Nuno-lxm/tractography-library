#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=01:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=bench_compression
#SBATCH --no-requeue
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/isolated/compression_bench/bench_%j.out

# Compare zlib, libdeflate and zstd (ratio, compression and decompression
# speed) on uncompressed images, inside the TractoFlow container.
#
# Usage:
#   sbatch bench_compression.sh [-r REPEATS] [-t THREADS] FILE.nii...
#
# Suggested inputs:
#   data/resample_dwi_nii/mgh1/S1/dwi.nii
#   results/tests/isolated/resample_compression/mgh1_resample_nii_none_x86_*/S1/S1__dwi_resampled.nii

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "Usage: sbatch $0 [-r REPEATS] [-t THREADS] FILE.nii..." >&2
    exit 1
fi

project_dir="/projects/I20240002/nunolxm"
lib_dir="$project_dir/tractography-library"
container="$project_dir/tractoflow_files/scilus_1.6.0.sif"

echo "Node: $(hostname)"
echo "Arguments: $*"

# Bind the project so the input paths work unchanged inside the container
singularity exec --no-home --bind "$lib_dir:/mnt" --bind "$project_dir:$project_dir" "$container" \
    bash -s -- "$@" <<'EOF'
set -e
for h in zlib.h libdeflate.h zstd.h; do
    header=$(echo "#include <$h>" | gcc -E -x c - 2>/dev/null | grep -m1 -o "/[^\"]*$h" || true)
    [[ -n "$header" ]] || { echo "Error: $h not found in the container"; exit 1; }
    echo "$h: $header"
done

make -C /mnt bench_compression
/mnt/bin/bench_compression "$@"
EOF
