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

# The container ships libzstd.so.1 (1.4.8) but not its header or libzstd.so,
# so build against the matching release's zstd.h and link the library by name
zstd_include="$project_dir/tractoflow_files/zstd-1.4.8/include"
if [[ ! -f "$zstd_include/zstd.h" ]]; then
    echo "Error: $zstd_include/zstd.h missing; fetch it with:" >&2
    echo "  mkdir -p $zstd_include && curl -fL -o $zstd_include/zstd.h https://raw.githubusercontent.com/facebook/zstd/v1.4.8/lib/zstd.h" >&2
    exit 1
fi

echo "Node: $(hostname)"
echo "Arguments: $*"

# Bind the project so the input paths work unchanged inside the container
singularity exec --no-home --bind "$lib_dir:/mnt" --bind "$project_dir:$project_dir" \
    --bind "$zstd_include:/zstd" "$container" bash -s -- "$@" <<'EOF'
set -e
for h in zlib.h libdeflate.h zstd.h; do
    header=$(echo "#include <$h>" | gcc -I/zstd -E -x c - 2>/dev/null | grep -m1 -o "/[^\"]*$h" || true)
    [[ -n "$header" ]] || { echo "Error: $h not found in the container"; exit 1; }
    echo "$h: $header"
done

make -B -C /mnt bench_compression ZSTD_CFLAGS=-I/zstd ZSTD_LIBS=-l:libzstd.so.1
/mnt/bin/bench_compression "$@"
EOF
