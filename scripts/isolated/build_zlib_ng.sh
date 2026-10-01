#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --time=00:15:00
#SBATCH --partition=normal-x86
#SBATCH --job-name=build_zlib_ng
#SBATCH --no-requeue
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/isolated/build/build_zlib_ng_%j.out

# Build zlib-ng in zlib-compat mode inside the TractoFlow container, as a
# drop-in libz.so.1 for LD_PRELOAD (--lib zlib-ng), at bin/zlib-ng/libz.so.1.
# The source is fetched on the login node, which has network access:
#   curl -fL https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz |
#       tar xz -C /projects/I20240002/nunolxm/tractoflow_files
#
# Usage:
#   sbatch build_zlib_ng.sh

set -euo pipefail

project_dir="/projects/I20240002/nunolxm"
lib_dir="$project_dir/tractography-library"
container="$project_dir/tractoflow_files/scilus_1.6.0.sif"
src="$project_dir/tractoflow_files/zlib-ng-2.3.3"

if [[ ! -f "$src/configure" ]]; then
    echo "Error: $src missing; see the fetch command in $0" >&2
    exit 1
fi

echo "Node: $(hostname)"

singularity exec --no-home --bind "$lib_dir:/mnt" --bind "$src:/src:ro" "$container" bash -s <<'EOF'
set -e
echo "gcc: $(gcc --version | head -1)"

# configure writes into the source dir, so build from a copy
build=$(mktemp -d)
cp -r /src/. "$build"
cd "$build"
./configure --zlib-compat
make -j"${SLURM_CPUS_PER_TASK:-4}" >/dev/null

so=$(ls libz.so.1.* | head -1)
mkdir -p /mnt/bin/zlib-ng
cp "$so" /mnt/bin/zlib-ng/libz.so.1
rm -rf "$build"

echo "Built /mnt/bin/zlib-ng/libz.so.1 from $so"
echo "Symbol versions: $(objdump -T /mnt/bin/zlib-ng/libz.so.1 | grep -oE 'ZLIB_[0-9.]+' | sort -uV | tr '\n' ' ')"

# Python's zlib, gzip round trip and the version it reports under the preload
LD_PRELOAD=/mnt/bin/zlib-ng/libz.so.1 python3 - <<'PY'
import gzip, os, zlib
data = os.urandom(1 << 20) + bytes(8 << 20)
assert gzip.decompress(gzip.compress(data)) == data
assert zlib.decompress(zlib.compress(data, 1)) == data
print("python zlib runtime under preload:", zlib.ZLIB_RUNTIME_VERSION)
PY
EOF
