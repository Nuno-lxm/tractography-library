#!/bin/bash
# Submit resample_dwi.sh for gz, deflate-py (compression_lib) and nii, several
# times, one job after another. The order rotates every repeat so no
# compression always runs first. Results go under tests/isolated/resample_compression;
# only the first repeat publishes its output (for the correctness check).
# With LEVEL, deflate-py compresses every image at that libdeflate level.
#
# Usage:
#   submit_resample_compression.sh mgh1
#   submit_resample_compression.sh mgh1 1

set -euo pipefail

DATASET="${1:?Usage: $0 DATASET [LEVEL]}"
LEVEL="${2:-}"
REPEATS=1
COMPRESSIONS=(gz deflate-py nii)

script="/projects/I20240002/nunolxm/tractography-library/scripts/isolated/resample_dwi.sh"

prev=""
for repeat in $(seq 1 "$REPEATS"); do
    publish=""
    [[ "$repeat" -eq 1 ]] && publish="--publish"
    for i in "${!COMPRESSIONS[@]}"; do
        compression="${COMPRESSIONS[$(( (i + repeat - 1) % ${#COMPRESSIONS[@]} ))]}"
        # Wait for the previous job so runs don't compete for the filesystem
        dep=${prev:+--dependency=afterany:$prev}
        level=""
        [[ -n "$LEVEL" && "$compression" == deflate* ]] && level="--level $LEVEL"
        prev=$(sbatch --parsable $dep "$script" --dataset "$DATASET" --compression "$compression" \
            --mode none --test-dir resample_compression --clean-work $publish $level)
        echo "$prev $compression $repeat"
    done
done
