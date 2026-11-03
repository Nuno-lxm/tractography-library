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
# With a LEVEL argument the default compression level (6) becomes LEVEL, at
# bin/zlib-ng-lLEVEL/libz.so.1 (--lib zlib-ng --level LEVEL). FSL and MRtrix
# open gz files without a level, so they get LEVEL; callers that ask for a
# level (nibabel asks for 1) keep theirs. LEVEL+ (e.g. 2+, at bin/zlib-ng-l2+)
# also raises the levels 1 to LEVEL-1 that callers ask for to LEVEL, so
# nibabel's level 1 becomes 2; level 0 (stored) stays.
# The source is fetched on the login node, which has network access:
#   curl -fL https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz |
#       tar xz -C /projects/I20240002/nunolxm/tractoflow_files
#
# Usage:
#   sbatch build_zlib_ng.sh [LEVEL[+]]

set -euo pipefail

project_dir="/projects/I20240002/nunolxm"
lib_dir="$project_dir/tractography-library"
container="$project_dir/tractoflow_files/scilus_1.6.0.sif"
src="$project_dir/tractoflow_files/zlib-ng-2.3.3"

level_arg="${1:-}"
if [[ -n "$level_arg" ]] && ! [[ "$level_arg" =~ ^[1-9]\+?$ ]]; then
    echo "Error: LEVEL must be 1-9, optionally followed by + (got: $level_arg)" >&2
    exit 1
fi
export DEFAULT_LEVEL="${level_arg%+}"
export RAISE_LEVELS=$([[ "$level_arg" == *+ ]] && echo 1 || echo 0)
export OUT_DIR="/mnt/bin/zlib-ng${level_arg:+-l$level_arg}"

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
if [[ -n "$DEFAULT_LEVEL" ]]; then
    # deflateInit2 and deflateParams both turn Z_DEFAULT_COMPRESSION into 6
    [[ $(grep -c '^ *level = 6;$' deflate.c) == 2 ]] || { echo "Error: default level lines not found in deflate.c"; exit 1; }
    sed -i "s/^\( *level = \)6;$/\1$DEFAULT_LEVEL;/" deflate.c
    echo "Default level: $DEFAULT_LEVEL"
fi
if [[ "$RAISE_LEVELS" == 1 ]]; then
    # Right after each default, so deflateInit2 and deflateParams both raise
    sed -i "s/^\( *level = $DEFAULT_LEVEL;\)$/\1\n    if (level >= 1 \&\& level < $DEFAULT_LEVEL) level = $DEFAULT_LEVEL;/" deflate.c
    [[ $(grep -c "^    if (level >= 1 && level < $DEFAULT_LEVEL) level = $DEFAULT_LEVEL;$" deflate.c) == 2 ]] || { echo "Error: level raise not added to deflate.c"; exit 1; }
    echo "Levels 1-$((DEFAULT_LEVEL - 1)) raised to $DEFAULT_LEVEL"
fi
./configure --zlib-compat
make -j"${SLURM_CPUS_PER_TASK:-4}" >/dev/null

so=$(ls libz.so.1.* | head -1)
mkdir -p "$OUT_DIR"
cp "$so" "$OUT_DIR/libz.so.1"
rm -rf "$build"

echo "Built $OUT_DIR/libz.so.1 from $so"
echo "Symbol versions: $(objdump -T "$OUT_DIR/libz.so.1" | grep -oE 'ZLIB_[0-9.]+' | sort -uV | tr '\n' ' ')"

# Python's zlib, gzip round trip and the version it reports under the preload
LD_PRELOAD="$OUT_DIR/libz.so.1" python3 - <<'PY'
import gzip, os, zlib
data = os.urandom(1 << 20) + bytes(8 << 20)
assert gzip.decompress(gzip.compress(data)) == data
assert zlib.decompress(zlib.compress(data, 1)) == data
print("python zlib runtime under preload:", zlib.ZLIB_RUNTIME_VERSION)
level = int(os.environ["DEFAULT_LEVEL"] or 6)
assert zlib.compress(data) == zlib.compress(data, level)
print("default level:", level)
if os.environ["RAISE_LEVELS"] == "1":
    assert all(zlib.compress(data, l) == zlib.compress(data, level) for l in range(1, level))
    assert zlib.compress(data, 0) != zlib.compress(data, level)
    print(f"levels 1-{level - 1} give level {level}, level 0 stays stored")
PY
EOF
