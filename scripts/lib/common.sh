# Shared setup for the Slurm run scripts. Source it by absolute path: sbatch
# runs a copy of the script from its spool dir, not from the repo.
#
# Library options (parsed by each script, checked by check_lib_options):
#   --lib none|buffer|prefetch|deflate|deflate-py|deflate-all|zlib-ng|zstd|zstd-deflate-py|logger
#       none        no library (baseline)
#       buffer      buffer_lib, read prefetching and write buffering
#       prefetch    buffer_lib, read prefetching only
#       deflate     compression_lib, gzopen/gzwrite/gzclose through libdeflate
#       deflate-py  deflate plus Python's raw deflate streams and crc32
#       deflate-all deflate-py plus gz reads (FSL, MRtrix) decompressed with libdeflate
#       zlib-ng     zlib-ng built as a drop-in libz.so.1 (scripts/isolated/build_zlib_ng.sh)
#       zstd        zstd_lib, gz writes to the --zstd-targets files become zstd
#                   (still named .gz); any zstd file read through gzopen is
#                   decompressed. Only list files read by gzopen tools (FSL,
#                   MRtrix): Python and ANTs can't read them
#       zstd-deflate-py zstd for the --zstd-targets files, deflate-py for every
#                   other gz write and Python's streams
#       logger      logger.so, logs every IO call to /mnt/logger.log
#   --level N   deflate*, zstd-deflate-py: compress every image at libdeflate
#               level N (0-12); the zstd targets stay at zstd's default
#               zstd: zstd level N (1-19, default 3)
#               zlib-ng: default level N (1-9), from the build at
#               bin/zlib-ng-lN (scripts/isolated/build_zlib_ng.sh N)
#   --debug     deflate*, zstd*: log every wrapped call
#   --zstd-targets GLOBS
#               zstd*: files to write as zstd, as ZSTD_FILE_TARGETS (globs
#               separated by commas; a pattern with a / matches the full path)

project_dir="/projects/I20240002/nunolxm"
lib_dir="$project_dir/tractography-library"
config_dir="$lib_dir/config-files/deucalion"
sif="$project_dir/tractoflow_files/scilus_1.6.0.sif"

LIB="none"
LEVEL=""
DEBUG=false
ZSTD_TARGETS=""

check_lib_options() {
    case "$LIB" in
        none|buffer|prefetch|deflate|deflate-py|deflate-all|zlib-ng|zstd|zstd-deflate-py|logger) ;;
        *) echo "Error: --lib must be none, buffer, prefetch, deflate, deflate-py, deflate-all, zlib-ng, zstd, zstd-deflate-py or logger (got: $LIB)" >&2; exit 1 ;;
    esac
    if [[ "$LIB" != deflate* && "$LIB" != zstd* && "$DEBUG" == true ]]; then
        echo "Error: --debug only works with the deflate and zstd modes" >&2
        exit 1
    fi
    if [[ "$LIB" != deflate* && "$LIB" != zstd* && "$LIB" != zlib-ng && -n "$LEVEL" ]]; then
        echo "Error: --level only works with the deflate, zstd and zlib-ng modes" >&2
        exit 1
    fi
    if [[ "$LIB" == deflate* || "$LIB" == zstd-deflate-py ]] && [[ -n "$LEVEL" ]] && { ! [[ "$LEVEL" =~ ^[0-9]+$ ]] || (( LEVEL > 12 )); }; then
        echo "Error: --level must be 0-12 for libdeflate (got: $LEVEL)" >&2
        exit 1
    fi
    if [[ "$LIB" == zstd && -n "$LEVEL" ]] && { ! [[ "$LEVEL" =~ ^[0-9]+$ ]] || (( LEVEL < 1 || LEVEL > 19 )); }; then
        echo "Error: --level must be 1-19 for zstd (got: $LEVEL)" >&2
        exit 1
    fi
    if [[ "$LIB" == zlib-ng && -n "$LEVEL" ]] && ! [[ "$LEVEL" =~ ^[1-9]$ ]]; then
        echo "Error: --level must be 1-9 for zlib-ng (got: $LEVEL)" >&2
        exit 1
    fi
    if [[ "$LIB" == zstd* && -z "$ZSTD_TARGETS" ]]; then
        echo "Error: --lib $LIB needs --zstd-targets (no step default)" >&2
        exit 1
    fi
    if [[ "$LIB" != zstd* && -n "$ZSTD_TARGETS" ]]; then
        echo "Error: --zstd-targets only works with the zstd modes" >&2
        exit 1
    fi
}

# Sets output_dir, results_dir, work_dir and the Nextflow report paths for
# test_name under tests/CATEGORY
set_run_dirs() {
    local category="$1"
    output_dir="$project_dir/output/tests/$category/${test_name}_${SLURM_JOB_ID}"
    results_dir="$project_dir/results/tests/$category/${test_name}_${SLURM_JOB_ID}"
    work_dir="$project_dir/tractoflow_files/work/tests/$category/${test_name}_work_${SLURM_JOB_ID}"
    report_file="$output_dir/report_${test_name}_${SLURM_JOB_ID}.html"
    timeline_file="$output_dir/timeline_${test_name}_${SLURM_JOB_ID}.html"
    trace_file="$output_dir/trace_${test_name}_${SLURM_JOB_ID}.txt"
}

# Appends base.config, buffer.config for the buffer modes, and a generated
# run.config with this run's env settings to config_args
add_lib_configs() {
    config_args+=(-c "$config_dir/base.config")
    local env=()
    case "$LIB" in
        buffer)     config_args+=(-c "$config_dir/buffer.config") ;;
        prefetch)   config_args+=(-c "$config_dir/buffer.config"); env+=("BUFFER_WRITE_ENABLED = 0") ;;
        deflate*)   env+=("LD_PRELOAD = '/mnt/bin/compression_lib.so'") ;;
        zlib-ng)    env+=("LD_PRELOAD = '/mnt/bin/zlib-ng${LEVEL:+-l$LEVEL}/libz.so.1'") ;;
        zstd)       env+=("LD_PRELOAD = '/mnt/bin/zstd_lib.so'") ;;
        # zstd_lib first: it hands every gz call it doesn't take to compression_lib.
        # Colon, not space: Nextflow re-exports the env unquoted in the container
        zstd-deflate-py) env+=("LD_PRELOAD = '/mnt/bin/zstd_lib.so:/mnt/bin/compression_lib.so'") ;;
        logger)     env+=("LD_PRELOAD = '/mnt/bin/logger.so'") ;;
    esac
    [[ "$LIB" == zstd* ]] && env+=("ZSTD_FILE_TARGETS = '$ZSTD_TARGETS'")
    [[ "$LIB" == deflate-py || "$LIB" == deflate-all || "$LIB" == zstd-deflate-py ]] && env+=("COMPRESSION_STREAMS = 1")
    [[ "$LIB" == deflate-all ]] && env+=("COMPRESSION_READS = 1")
    if [[ "$DEBUG" == true ]]; then
        [[ "$LIB" == zstd* ]] && env+=("ZSTD_DEBUG = 1")
        [[ "$LIB" == deflate* || "$LIB" == zstd-deflate-py ]] && env+=("COMPRESSION_DEBUG = 1")
    fi
    if [[ -n "$LEVEL" ]]; then
        [[ "$LIB" == zstd ]]     && env+=("ZSTD_LEVEL = $LEVEL")
        [[ "$LIB" == deflate* || "$LIB" == zstd-deflate-py ]] && env+=("COMPRESSION_LEVEL = $LEVEL")
    fi
    if (( ${#env[@]} > 0 )); then
        { echo "env {"; printf '    %s\n' "${env[@]}"; echo "}"; } > "$output_dir/run.config"
        config_args+=(-c "$output_dir/run.config")
    fi
}

# Redirects the script's output into output_dir and starts vmstat (and
# nvidia-smi on GPU nodes) in the background until the script exits
start_monitoring() {
    exec > "$output_dir/out_${SLURM_JOB_ID}.out" \
         2> "$output_dir/err_${SLURM_JOB_ID}.err"

    (
        vmstat -S M 5 | while IFS= read -r line; do
            printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$line"
        done > "$output_dir/vmstat_${test_name}_${SLURM_JOB_ID}.log"
    ) &
    monitor_pids=($!)

    if command -v nvidia-smi >/dev/null 2>&1; then
        nvidia-smi -L || true
        nvidia-smi --query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.total,memory.used,temperature.gpu,power.draw \
            --format=csv -l 5 > "$output_dir/gpu_${test_name}_${SLURM_JOB_ID}.log" &
        monitor_pids+=($!)
    fi
}

stop_monitoring() {
    kill "${monitor_pids[@]}" 2>/dev/null || true
}

# Prints which build of the loaded library this run uses
print_lib_version() {
    local sos=() so
    case "$LIB" in
        buffer|prefetch) sos=("$lib_dir/bin/buffer_lib.so") ;;
        deflate*)        sos=("$lib_dir/bin/compression_lib.so") ;;
        zlib-ng)         sos=("$lib_dir/bin/zlib-ng${LEVEL:+-l$LEVEL}/libz.so.1") ;;
        zstd)            sos=("$lib_dir/bin/zstd_lib.so") ;;
        zstd-deflate-py) sos=("$lib_dir/bin/zstd_lib.so" "$lib_dir/bin/compression_lib.so") ;;
        logger)          sos=("$lib_dir/bin/logger.so") ;;
        *)               return ;;
    esac
    for so in "${sos[@]}"; do
        if [[ ! -f "$so" ]]; then
            echo "Missing $so" >&2
            exit 1
        fi
        echo "Library: $so (md5 $(md5sum "$so" | cut -d' ' -f1))"
    done
}

# Library settings the tasks ran with, compression_lib totals per process,
# image sizes and kernel IO totals, all read from work_dir
print_summary() {
    local task_run
    task_run=$(find "$work_dir" -name .command.run -print -quit)
    if [[ -n "$task_run" ]]; then
        echo "Task env ($task_run):"
        grep -E '^export (LD_PRELOAD|BUFFER_|COMPRESSION_|ZSTD_)' "$task_run" || echo "  (no library settings)"
    fi

    if [[ "$LIB" == deflate* || "$LIB" == zstd-deflate-py ]]; then
        echo "compression_lib per process (files, GB in -> out, seconds):"
        local err name
        for err in $(find "$work_dir" -name .command.err); do
            name=$(sed -n 's/^# NEXTFLOW TASK: \([^ ]*\).*/\1/p' "$(dirname "$err")/.command.run")
            grep '^compression_lib:' "$err" | sed "s/^/$name /" || true
        done | awk '{
                p = $3 == "read" ? $1 " (reads)" : $1
                for (i = 1; i <= NF; i++) if ($i == "->") { bin[p] += $(i - 1); bout[p] += $(i + 1) }
                files[p]++; secs[p] += $(NF - 1)
            }
            END { for (p in files) printf "  %-38s %5d  %8.3f -> %8.3f  %8.2f\n", p, files[p], bin[p] / 1e9, bout[p] / 1e9, secs[p] }'
    fi

    if [[ "$LIB" == zstd* ]]; then
        echo "zstd_lib per process (files, GB in -> out, seconds):"
        local err name
        for err in $(find "$work_dir" -name .command.err); do
            name=$(sed -n 's/^# NEXTFLOW TASK: \([^ ]*\).*/\1/p' "$(dirname "$err")/.command.run")
            grep '^zstd_lib: \(level\|read,\)' "$err" | sed "s/^/$name /" || true
        done | awk '{
                p = $3 == "read," ? $1 " (reads)" : $1
                for (i = 1; i <= NF; i++) if ($i == "->") { bin[p] += $(i - 1); bout[p] += $(i + 1) }
                files[p]++; secs[p] += $(NF - 1)
            }
            END { for (p in files) printf "  %-38s %5d  %8.3f -> %8.3f  %8.2f\n", p, files[p], bin[p] / 1e9, bout[p] / 1e9, secs[p] }'
        # Failed zstd reads/writes; the task itself fails too
        find "$work_dir" -name .command.err -exec grep -H "^zstd_lib: can't\|^zstd_lib: compress failed" {} + || true
        echo "Files written as zstd in work dir:"
        find "$work_dir" -type f -name '*.gz' -exec sh -c \
            'head -c 4 "$1" | od -An -tx1 | grep -q "28 b5 2f fd" && printf "  %s\n" "$1"' _ {} \; || true
    fi

    find "$work_dir" -type f \( -name '*.nii' -o -name '*.nii.gz' \) -printf '%s\n' |
        awk '{ total += $1; n++ } END { printf "Images in work dir: %d files, %.3f GB\n", n, total / 1e9 }'

    find "$work_dir" -name .command.trace -exec cat {} + | awk -F= '
        $1 == "syscr" { syscr += $2 }  $1 == "syscw" { syscw += $2 }
        $1 == "rchar" { rchar += $2 }  $1 == "wchar" { wchar += $2 }
        $1 == "peak_rss" { tasks++; if ($2 > rss) rss = $2 }
        END {
            printf "I/O totals over %d tasks:\n", tasks
            printf "  read calls  %12d  %8.2f GB  avg %10.0f B\n", syscr, rchar / 1e9, syscr ? rchar / syscr : 0
            printf "  write calls %12d  %8.2f GB  avg %10.0f B\n", syscw, wchar / 1e9, syscw ? wchar / syscw : 0
            printf "  max peak_rss %.1f GB\n", rss / 1024 / 1024
        }'
}
