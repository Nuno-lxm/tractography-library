#!/bin/bash
# Submit resample_dwi.sh for gz, deflate (compression_lib) and nii, several
# times, one job after another. The order rotates every repeat so no
# compression always runs first. Results go under tests/isolated/resample_compression;
# only the first repeat publishes its output (for the correctness check).
#
# Usage:
#   submit_resample_compression.sh mgh1

set -euo pipefail

DATASET="${1:?Usage: $0 DATASET}"
REPEATS=1
COMPRESSIONS=(gz deflate nii)

script="/projects/I20240002/nunolxm/tractography-library/scripts/isolated/resample_dwi.sh"

prev=""
for repeat in $(seq 1 "$REPEATS"); do
    publish=""
    [[ "$repeat" -eq 1 ]] && publish="--publish"
    for i in "${!COMPRESSIONS[@]}"; do
        compression="${COMPRESSIONS[$(( (i + repeat - 1) % ${#COMPRESSIONS[@]} ))]}"
        # Wait for the previous job so runs don't compete for the filesystem
        dep=${prev:+--dependency=afterany:$prev}
        prev=$(sbatch --parsable $dep "$script" --dataset "$DATASET" --compression "$compression" \
            --mode none --test-dir resample_compression --clean-work $publish)
        echo "$prev $compression $repeat"
    done
done
