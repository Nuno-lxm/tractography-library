#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --time=00:10:00
#SBATCH --partition=normal-x86
#SBATCH --job-name=build_compression
#SBATCH --no-requeue
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/isolated/build/build_compression_%j.out

# Build bin/compression_lib.so inside the TractoFlow container, which has the
# libdeflate headers and library the login node lacks.
#
# Usage:
#   sbatch build_compression_lib.sh

set -euo pipefail

project_dir="/projects/I20240002/nunolxm"
lib_dir="$project_dir/tractography-library"
container="$project_dir/tractoflow_files/scilus_1.6.0.sif"

echo "Node: $(hostname)"

singularity exec --no-home --bind "$lib_dir:/mnt" "$container" bash -s <<'EOF'
set -e
command -v gcc >/dev/null || { echo "Error: no gcc in the container"; exit 1; }
header=$(echo '#include <libdeflate.h>' | gcc -E -x c - 2>/dev/null | grep -m1 -o '/[^"]*libdeflate.h' || true)
[[ -n "$header" ]] || { echo "Error: libdeflate.h not found in the container"; exit 1; }
echo "gcc: $(gcc --version | head -1)"
echo "libdeflate.h: $header"

make -C /mnt compression_lib.so
echo "ldd /mnt/bin/compression_lib.so:"
ldd /mnt/bin/compression_lib.so | grep -E 'deflate|libz|libc\.so'
EOF
