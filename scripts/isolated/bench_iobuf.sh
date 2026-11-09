#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=06:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=bench_iobuf
#SBATCH --no-requeue
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/isolated/iobuf_bench/bench_%j.out

# Does the I/O buffer size matter? Times streaming writes and reads of real
# images through zlib's gz API at different gzbuffer sizes (system zlib and the
# zlib-ng drop-in) and through zstd's streaming API at different read/write
# buffer sizes, on Lustre and on node-local /tmp, with 1 or more processes at
# once. Runs inside the TractoFlow container.
#
# Usage:
#   sbatch bench_iobuf.sh [-r REPEATS] [-b SIZES] [-p "PROCS..."] [-l "LOCATIONS..."]
#                         [-v "VARIANTS..."] [-c CHUNK] FILE.nii...
#
#   -r  repetitions per size (default 3)
#   -b  buffer sizes, comma separated, K/M allowed (default 8K,32K,64K,128K,256K,1M,4M)
#   -p  process counts to run at once (default "1 16")
#   -l  where the files go: lustre and/or local (default "lustre local")
#   -v  variants (default "zlib-L6 zlib-L1 zlibng-L1 zstd-L3 zstd-L1"):
#         zlib-LN    system zlib, gz API, level N
#         zlibng-LN  zlib-ng drop-in from bin/zlib-ng (build_zlib_ng.sh), level N;
#                    the level goes in gzopen's mode, so one build serves every level
#         zstd-LN    zstd streaming, level N
#   -c  bytes per gzwrite/gzread call (default 0 = whole image; 8192 mimics small calls)
#
# Input: an uncompressed image, e.g. data/resample_dwi_nii/mgh1/S1/dwi.nii.
# Time: each variant x location x process count runs (sizes x repeats) writes
# and reads of the whole file. Level 6 gz writes run at tens of MB/s, so for a
# file of several GB pick a smaller input or fewer sizes, or raise --time.
#
# Results: output/tests/isolated/iobuf_bench/<jobid>/ with one CSV per process,
# all.csv with everything, and summary.md from summarize_iobuf.py.

set -euo pipefail

repeats=3
sizes="8K,32K,64K,128K,256K,1M,4M"
procs_list="1 16"
locations="lustre local"
variants="zlib-L6 zlib-L1 zlibng-L1 zstd-L3 zstd-L1"
chunk=0
while getopts "r:b:p:l:v:c:" opt; do
    case $opt in
        r) repeats=$OPTARG ;;
        b) sizes=$OPTARG ;;
        p) procs_list=$OPTARG ;;
        l) locations=$OPTARG ;;
        v) variants=$OPTARG ;;
        c) chunk=$OPTARG ;;
        *) echo "Usage: sbatch $0 [-r N] [-b SIZES] [-p \"PROCS\"] [-l \"LOCATIONS\"] [-v \"VARIANTS\"] [-c CHUNK] FILE.nii..." >&2; exit 1 ;;
    esac
done
shift $((OPTIND - 1))
if [[ $# -eq 0 ]]; then
    echo "Error: give at least one uncompressed input image" >&2
    exit 1
fi

project_dir="/projects/I20240002/nunolxm"
lib_dir="$project_dir/tractography-library"
container="$project_dir/tractoflow_files/scilus_1.6.0.sif"
job_id="${SLURM_JOB_ID:-manual_$$}"
out_dir="$project_dir/output/tests/isolated/iobuf_bench/$job_id"
local_dir="/tmp/iobuf_$job_id"

# The container ships libzstd.so.1 (1.4.8) but not its header, as for bench_compression.sh
zstd_include="$project_dir/tractoflow_files/zstd-1.4.8/include"
if [[ ! -f "$zstd_include/zstd.h" ]]; then
    echo "Error: $zstd_include/zstd.h missing; fetch it with:" >&2
    echo "  mkdir -p $zstd_include && curl -fL -o $zstd_include/zstd.h https://raw.githubusercontent.com/facebook/zstd/v1.4.8/lib/zstd.h" >&2
    exit 1
fi

mkdir -p "$out_dir" "$local_dir"
echo "Node: $(hostname)"
echo "Job: $job_id"
echo "Inputs: $*"
echo "Variants: $variants | locations: $locations | processes: $procs_list | sizes: $sizes | repeats: $repeats | chunk: $chunk"

# Seen inside the container (singularity passes the environment through)
export BENCH_REPEATS="$repeats" BENCH_SIZES="$sizes" BENCH_PROCS="$procs_list" BENCH_LOCATIONS="$locations"
export BENCH_VARIANTS="$variants" BENCH_CHUNK="$chunk" BENCH_OUT="$out_dir" BENCH_LOCAL="$local_dir"

singularity exec --no-home --bind "$lib_dir:/mnt" --bind "$project_dir:$project_dir" \
    --bind "$zstd_include:/zstd" --bind /tmp:/tmp "$container" bash -s -- "$@" <<'EOF'
set -euo pipefail

gcc -O2 -Wall -I/zstd -o /mnt/bin/bench_iobuf /mnt/tests/bench_iobuf.c -lz -l:libzstd.so.1
echo "Built /mnt/bin/bench_iobuf"

for variant in $BENCH_VARIANTS; do
    preload=""
    case "$variant" in
        zlib-L*)   mode=gz;   level=${variant#zlib-L} ;;
        zlibng-L*) mode=gz;   level=${variant#zlibng-L}; preload=/mnt/bin/zlib-ng/libz.so.1 ;;
        zstd-L*)   mode=zstd; level=${variant#zstd-L} ;;
        *) echo "Unknown variant $variant, skipped"; continue ;;
    esac
    if [[ -n "$preload" && ! -f "$preload" ]]; then
        echo "Missing $preload (build it with scripts/isolated/build_zlib_ng.sh), $variant skipped"
        continue
    fi

    for where in $BENCH_LOCATIONS; do
        case "$where" in
            lustre) base="$BENCH_OUT/files" ;;
            local)  base="$BENCH_LOCAL" ;;
            *) echo "Unknown location $where, skipped"; continue ;;
        esac

        for procs in $BENCH_PROCS; do
            echo "== $variant on $where with $procs process(es)"
            pids=()
            for k in $(seq 1 "$procs"); do
                dir="$base/${variant}_p${procs}_k${k}"
                mkdir -p "$dir"
                csv="$BENCH_OUT/${variant}_${where}_p${procs}_k${k}.csv"
                LD_PRELOAD="$preload" /mnt/bin/bench_iobuf -m "$mode" -l "$level" -d "$dir" \
                    -r "$BENCH_REPEATS" -b "$BENCH_SIZES" -c "$BENCH_CHUNK" \
                    -t "$variant" -w "$where" -p "$procs" -s -x "$@" \
                    > "$csv" 2> "$csv.err" &
                pids+=($!)
            done
            for pid in "${pids[@]}"; do wait "$pid"; done
            # The stderr line names the library that actually loaded
            head -1 "$BENCH_OUT/${variant}_${where}_p${procs}_k1.csv.err"
            if [[ -n "$preload" ]] && ! grep -q zlib-ng "$BENCH_OUT/${variant}_${where}_p${procs}_k1.csv.err"; then
                echo "Warning: $variant did not report zlib-ng; check that the preload took effect"
            fi
        done
    done
done

# One CSV with a single header, then the summary
first=1
for csv in "$BENCH_OUT"/*_k*.csv; do
    if (( first )); then cat "$csv"; first=0; else tail -n +2 "$csv"; fi
done > "$BENCH_OUT/all.csv"
python3 /mnt/scripts/isolated/summarize_iobuf.py "$BENCH_OUT/all.csv" > "$BENCH_OUT/summary.md"
cat "$BENCH_OUT/summary.md"
EOF

rm -rf "$local_dir"
echo "Results in $out_dir"
