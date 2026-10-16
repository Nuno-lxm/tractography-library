#!/bin/bash
# Build the isolated Resample_DWI input (data/resample_dwi/NAME/<sid>/dwi.nii.gz
# and mask.nii.gz) from a finished TractoFlow run: the Normalize_DWI output and
# the Crop_DWI b0 mask, read through the results dir. The results are symlinks
# into the work dir, so run this before that work dir is deleted.
#
# Usage:
#   resample_dwi_dataset.sh [--nii] RESULTS_DIR NAME
#
# --nii   write uncompressed dwi.nii and mask.nii to data/resample_dwi_nii/NAME
#         instead (for run_isolated.sh --nii)
#
# Example:
#   scripts/data/resample_dwi_dataset.sh \
#       /projects/I20240002/nunolxm/results/tests/minimal/Penthera_3T_none_x86_2061445 Penthera_3T

set -euo pipefail

NII=false
if [[ "${1:-}" == --nii ]]; then
    NII=true
    shift
fi
if [[ $# -ne 2 ]]; then
    sed -n '2,/^$/{ s/^# \?//; p }' "$0" >&2
    exit 1
fi

results_dir="$1"
name="$2"
data_dir="${DATA_DIR:-/projects/I20240002/nunolxm/data}"
if [[ "$NII" == true ]]; then
    dst_root="$data_dir/resample_dwi_nii/$name"
else
    dst_root="$data_dir/resample_dwi/$name"
fi

if [[ -e "$dst_root" ]]; then
    echo "Error: $dst_root already exists" >&2
    exit 1
fi

copied=0
for sub_dir in "$results_dir"/*/; do
    sid=$(basename "$sub_dir")
    dwi="$sub_dir/Normalize_DWI/${sid}__dwi_normalized.nii.gz"
    mask="$sub_dir/Crop_DWI/${sid}__b0_mask_cropped.nii.gz"
    [[ -d "$sub_dir/Normalize_DWI" ]] || continue
    if [[ ! -f "$dwi" || ! -f "$mask" ]]; then
        echo "Error: $sid is missing $dwi or $mask (dangling link?)" >&2
        exit 1
    fi

    mkdir -p "$dst_root/$sid"
    if [[ "$NII" == true ]]; then
        gzip -dc "$dwi" > "$dst_root/$sid/dwi.nii"
        gzip -dc "$mask" > "$dst_root/$sid/mask.nii"
    else
        cp -L "$dwi" "$dst_root/$sid/dwi.nii.gz"
        cp -L "$mask" "$dst_root/$sid/mask.nii.gz"
    fi
    copied=$((copied + 1))
done

if [[ "$copied" -eq 0 ]]; then
    echo "Error: no subject with a Normalize_DWI output in $results_dir" >&2
    exit 1
fi
echo "Copied $copied subjects to $dst_root ($(du -sh --apparent-size "$dst_root" | cut -f1))"
