#!/bin/bash
# Submit normalize_dwi.sh for every mode, several times, one job after another.
# Results go under tests/isolated/normalize_matrix.
#
# Usage:
#   submit_normalize_matrix.sh mgh64

set -euo pipefail

DATASET="${1:?Usage: $0 DATASET}"
REPEATS=3

script="/projects/I20240002/nunolxm/tractography-library/scripts/isolated/normalize_dwi.sh"

prev=""
for repeat in $(seq 1 "$REPEATS"); do
    for mode in none prefetch full; do
        # Wait for the previous job so runs don't compete for the filesystem
        dep=${prev:+--dependency=afterany:$prev}
        prev=$(sbatch --parsable $dep "$script" --dataset "$DATASET" --mode "$mode" \
            --test-dir normalize_matrix)
        echo "$prev $mode $repeat"
    done
done
