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

# Build a compression library inside the TractoFlow container, which has the
# libraries the login node lacks.
#   compression (default)  bin/compression_lib.so, against libdeflate
#   zstd                   bin/zstd_lib.so, against the container's
#                          libzstd.so.1 (1.4.8) and that release's zstd.h
#
# Usage:
#   sbatch build_compression_lib.sh [compression|zstd]

set -euo pipefail

project_dir="/projects/I20240002/nunolxm"
lib_dir="$project_dir/tractography-library"
container="$project_dir/tractoflow_files/scilus_1.6.0.sif"
lib="${1:-compression}"

echo "Node: $(hostname)"

case "$lib" in
    compression)
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
        ;;
    zstd)
        # The container ships libzstd.so.1 but not its header or libzstd.so
        zstd_include="$project_dir/tractoflow_files/zstd-1.4.8/include"
        if [[ ! -f "$zstd_include/zstd.h" ]]; then
            echo "Error: $zstd_include/zstd.h missing; fetch it with:" >&2
            echo "  mkdir -p $zstd_include && curl -fL -o $zstd_include/zstd.h https://raw.githubusercontent.com/facebook/zstd/v1.4.8/lib/zstd.h" >&2
            exit 1
        fi
        singularity exec --no-home --bind "$lib_dir:/mnt" --bind "$zstd_include:/zstd" "$container" bash -s <<'EOF'
set -e
command -v gcc >/dev/null || { echo "Error: no gcc in the container"; exit 1; }
echo "gcc: $(gcc --version | head -1)"

make -B -C /mnt zstd_lib.so ZSTD_CFLAGS=-I/zstd ZSTD_LIBS=-l:libzstd.so.1
echo "ldd /mnt/bin/zstd_lib.so:"
ldd /mnt/bin/zstd_lib.so | grep -E 'zstd|libc\.so'
EOF
        ;;
    *)
        echo "Error: library must be compression or zstd (got: $lib)" >&2
        exit 1 ;;
esac
