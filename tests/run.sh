#!/bin/bash
# Benchmark runner — compares baseline vs buffer library
# Usage: ./tests/run.sh [test_number]

export LC_ALL=C
set -e

BENCH="bin/benchmark"
TEST_DIR="tests/output"
BUF_LOG="/tmp/buffer_bench.log"
BUF_LIB="bin/buffer_lib.so"
TMP_BASE=$(mktemp)
TMP_BUF=$(mktemp)
TMP_STATS=$(mktemp)

cleanup() {
    rm -rf "$TMP_BASE" "$TMP_BUF" "$TMP_STATS" "$TEST_DIR"
}
trap cleanup EXIT

echo "==> Building..."
make -s benchmark buffer_lib.so 2>/dev/null
mkdir -p "$TEST_DIR"

TESTS=()
if [ $# -ge 1 ] && [ "$1" -ge 1 ] 2>/dev/null && [ "$1" -le 6 ]; then
    TESTS=("$1")
else
    TESTS=(1 2 3 4 5 6)
fi

run_baseline() {
    > "$TMP_BASE"
    for t in "${TESTS[@]}"; do
        "$BENCH" "$t" >> "$TMP_BASE"
    done
}

run_buffered() {
    > "$TMP_BUF"
    > "$TMP_STATS"
    export LD_PRELOAD="$PWD/$BUF_LIB"
    export BUFFER_WRITE_TARGETS="benchmark"
    export BUFFER_PREFETCH_TARGETS="benchmark"
    export BUFFER_FILE_TARGETS="*.nii.gz"
    export BUFFER_LOG_FILE="$BUF_LOG"
    for t in "${TESTS[@]}"; do
        if [ "$t" -eq 6 ]; then
            for sz in 1 4 8 64; do
                > "$BUF_LOG"
                "$BENCH" 6 --sweep-size "$sz" >> "$TMP_BUF"
                if [ -f "$BUF_LOG" ]; then
                    printf "[sweep_%sKB]\n" "$sz" >> "$TMP_STATS"
                    cat "$BUF_LOG" >> "$TMP_STATS"
                fi
            done
        else
            > "$BUF_LOG"
            "$BENCH" "$t" >> "$TMP_BUF"
            if [ -f "$BUF_LOG" ]; then
                printf "[test %d]\n" "$t" >> "$TMP_STATS"
                cat "$BUF_LOG" >> "$TMP_STATS"
            fi
        fi
    done
}

calc() { awk "BEGIN { printf \"$1\", $2 }" 2>/dev/null || echo "N/A"; }

get_ratio() {
    local header=$1
    local block
    block=$(awk -v h="$header" '
        index($0, h) == 1 { found=1; next }
        /^\[test/ && found { exit }
        /^\[sweep/ && found { exit }
        found
    ' "$TMP_STATS" 2>/dev/null)
    [ -z "$block" ] && { echo "N/A"; return; }
    local ic ec
    ic=$(echo "$block" | grep "write_stats intercepted" | grep -o 'calls=[0-9]*' | cut -d= -f2 | awk '{s+=$1} END{print s}')
    ec=$(echo "$block" | grep "write_stats executed"   | grep -o 'calls=[0-9]*' | cut -d= -f2 | awk '{s+=$1} END{print s}')
    [ -z "$ic" ] && ic=0; [ -z "$ec" ] && ec=0
    [ "$ec" -eq 0 ] 2>/dev/null && { echo "N/A"; return; }
    calc "%.1f" "$ic / $ec"
}

NAMES=(dummy write_heavy read_heavy pipeline multi_file non_matching sweep)
SUFFIXES=("sweep_1KB" "sweep_4KB" "sweep_8KB" "sweep_64KB")

printf "\n--- Baseline ---\n"
printf "# %-20s %10s %9s %9s %12s %12s\n" "test_name" "time_s" "data_mb" "mbps" "write_calls" "read_calls"
run_baseline
grep "RESULT" "$TMP_BASE"

printf "\n--- Buffered ---\n"
printf "# %-20s %10s %9s %9s %12s %12s\n" "test_name" "time_s" "data_mb" "mbps" "write_calls" "read_calls"
run_buffered
grep "RESULT" "$TMP_BUF"

printf "\n%s\n" "================================================================================="
printf "%-20s %11s %11s %9s %11s\n" "Test" "Baseline(s)" "Buffered(s)" "Speedup" "SyscallRed"
printf "%s\n" "---------------------------------------------------------------------------------"

for t in "${TESTS[@]}"; do
    name=${NAMES[$t]}

    if [ "$t" -eq 6 ]; then
        for sfx in "${SUFFIXES[@]}"; do
            b=$(grep "RESULT" "$TMP_BASE" | grep "$sfx" | head -1)
            u=$(grep "RESULT" "$TMP_BUF" | grep "$sfx" | head -1)
            [ -z "$b" ] && continue
            bt=$(echo "$b" | awk '{print $3}')
            ut=$(echo "$u" | awk '{print $3}')
            sp=$(calc "%.2f" "$bt / $ut")
            rr=$(get_ratio "[$sfx]")
            printf "%-20s %11.4f %11.4f %9s %11s\n" "$sfx" "$bt" "$ut" "$sp" "$rr"
        done
    else
        b=$(grep "RESULT" "$TMP_BASE" | grep "$name" | head -1)
        u=$(grep "RESULT" "$TMP_BUF" | grep "$name" | head -1)
        [ -z "$b" ] && continue
        bt=$(echo "$b" | awk '{print $3}')
        ut=$(echo "$u" | awk '{print $3}')
        sp=$(calc "%.2f" "$bt / $ut")
        rr=$(get_ratio "[test $t]")
        printf "%-20s %11.4f %11.4f %9s %11s\n" "$name" "$bt" "$ut" "$sp" "$rr"
    fi
done
printf "%s\n" "================================================================================="

printf "\n--- Buffer library stats ---\n"
if [ -s "$TMP_STATS" ]; then
    while IFS= read -r line; do
        case "$line" in
            \[test*) printf "\n%s\n" "$line" ;;
            \[sweep*) printf "\n%s\n" "$line" ;;
            *intercepted*|*executed*|*flush_on_reads*) printf "  %s\n" "$line" ;;
        esac
    done < "$TMP_STATS"
else
    printf "(no interception)\n"
fi
