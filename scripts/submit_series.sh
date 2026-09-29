#!/bin/bash
# Submit several variants of a run script, REPEATS times, one job after
# another so runs don't compete for the filesystem. Each variant is one
# quoted argument string; the order rotates every repeat so no variant
# always runs first. -f adds arguments to the first repeat only.
#
# Usage:
#   submit_series.sh [-n REPEATS] [-f FIRST_ARGS] SCRIPT "COMMON_ARGS" "VARIANT"...
#
# Examples (from the repo root on Deucalion):
#   # Normalize_DWI matrix: every buffer mode, 3 times
#   scripts/submit_series.sh -n 3 scripts/isolated/run_isolated.sh \
#       "--step normalize --dataset mgh64 --test-dir normalize_matrix" \
#       "--lib none" "--lib prefetch" "--lib buffer"
#
#   # Resample compression: gz, deflate-py and nii; publish the first repeat
#   scripts/submit_series.sh -f --publish scripts/isolated/run_isolated.sh \
#       "--step resample --dataset mgh1 --test-dir resample_compression --clean-work" \
#       "--lib none" "--lib deflate-py" "--lib none --nii"
#
#   # TractoFlow baseline vs buffer
#   scripts/submit_series.sh scripts/tests/run_tractoflow.sh "--dataset mgh8" "--lib none" "--lib buffer"

set -euo pipefail

REPEATS=1
FIRST_ARGS=""

while getopts "n:f:" opt; do
    case "$opt" in
        n) REPEATS="$OPTARG" ;;
        f) FIRST_ARGS="$OPTARG" ;;
        *) exit 1 ;;
    esac
done
shift $((OPTIND - 1))

if [[ $# -lt 3 ]]; then
    sed -n '2,/^$/{ s/^# \?//; p }' "$0" >&2
    exit 1
fi

script="$(realpath "$1")"
common="$2"
shift 2
variants=("$@")

prev=""
for repeat in $(seq 1 "$REPEATS"); do
    extra=""
    [[ "$repeat" -eq 1 ]] && extra="$FIRST_ARGS"
    for i in "${!variants[@]}"; do
        variant="${variants[$(( (i + repeat - 1) % ${#variants[@]} ))]}"
        dep=${prev:+--dependency=afterany:$prev}
        # Word splitting of the argument strings is intended
        # shellcheck disable=SC2086
        prev=$(sbatch --parsable $dep "$script" $common $variant $extra)
        echo "$prev repeat $repeat: $variant"
    done
done
