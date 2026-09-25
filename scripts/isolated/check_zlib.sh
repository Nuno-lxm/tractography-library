#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --time=00:10:00
#SBATCH --partition=normal-x86
#SBATCH --job-name=check_zlib
#SBATCH --no-requeue
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/isolated/zlib_check/check_zlib_%j.out

# Check whether the tools in the TractoFlow container load zlib as a shared
# library (interceptable with LD_PRELOAD) or have it compiled in.
#
# Usage:
#   sbatch check_zlib.sh

set -euo pipefail

project_dir="/projects/I20240002/nunolxm"
container="$project_dir/tractoflow_files/scilus_1.6.0.sif"

echo "Node: $(hostname)"
echo "Container: $container"

singularity exec --no-home "$container" bash -s <<'EOF'
echo "== zlib-family libraries in the container"
ldconfig -p | grep -E 'libz\.so|libzstd|libdeflate|libz-ng' || echo "  (none found by ldconfig)"

# Non-system libraries a binary loads (skip libc and friends)
extra_libs() {
    ldd "$1" 2>/dev/null | awk '/=>/ && $3 ~ /^\// {print $3}' |
        grep -vE '/(libc|libm|libpthread|libdl|librt|libstdc\+\+|libgcc_s|ld-linux)[.-]'
}

zlib_imports() { nm -D --undefined-only "$1" 2>/dev/null | grep -oE '\b(gz[a-z0-9_]+|deflate[A-Za-z0-9_]*|inflate[A-Za-z0-9_]*)\b' | sort -u | tr '\n' ' '; }
zlib_defines() { nm -D --defined-only "$1" 2>/dev/null | grep -oE '\b(gzopen|gzwrite|gzread|deflate|inflate)\b' | sort -u | tr '\n' ' '; }

check() {
    local name=$1 bin
    bin=$(command -v "$name" 2>/dev/null || true)
    if [[ -z "$bin" ]]; then
        echo "== $name: not found"
        return
    fi
    bin=$(readlink -f "$bin")

    # FSL and MRtrix entries on PATH can be shell wrappers
    if ! file -b "$bin" | grep -q ELF; then
        echo "== $name: $bin is not ELF ($(file -b "$bin" | cut -c1-60))"
        echo "  binaries referenced: $(grep -aoE '/[^ "]*bin/[A-Za-z0-9_.+-]+' "$bin" | sort -u | head -5 | tr '\n' ' ')"
        return
    fi

    echo "== $name: $bin"
    echo "  libz via ldd:            $(ldd "$bin" 2>/dev/null | grep -E 'libz\.so' | xargs || true)"
    echo "  zlib symbols imported:   $(zlib_imports "$bin")"
    echo "  zlib symbols defined:    $(zlib_defines "$bin")"
    echo "  embedded zlib versions:  $(strings "$bin" | grep -oE '(deflate|inflate) [0-9]+\.[0-9.]+ Copyright' | sort -u | tr '\n' ';')"
    for lib in $(extra_libs "$bin"); do
        imp=$(zlib_imports "$lib")
        def=$(zlib_defines "$lib")
        if [[ -n "$imp$def" ]]; then
            echo "    lib $lib: imports [$imp] defines [$def]"
        fi
    done
}

for t in fslmaths /fsl/bin/fslmaths \
         mrcalc /mrtrix3_install/bin/mrcalc \
         mrconvert /mrtrix3_install/bin/mrconvert \
         dwidenoise /mrtrix3_install/bin/dwidenoise \
         eddy_cuda10.2 /fsl/bin/eddy_cuda10.2 bet \
         N4BiasFieldCorrection antsRegistration; do
    check "$t"
done

echo "== python (nibabel/scilpy use Python's zlib module)"
python3 - <<'PY'
import sys, zlib
print("  python", sys.version.split()[0])
print("  zlib module file:", getattr(zlib, "__file__", "built into the interpreter"))
print("  ZLIB_VERSION (compiled):", zlib.ZLIB_VERSION, " ZLIB_RUNTIME_VERSION:", zlib.ZLIB_RUNTIME_VERSION)
PY
py=$(readlink -f "$(command -v python3)")
echo "  python3 binary $py, libz via ldd: $(ldd "$py" | grep -E 'libz\.so' | xargs || true)"
zmod=$(python3 -c 'import zlib; print(getattr(zlib, "__file__", ""))')
if [[ -n "$zmod" ]]; then
    echo "  zlib module libz via ldd: $(ldd "$zmod" | grep -E 'libz\.so' | xargs || true)"
fi
EOF
