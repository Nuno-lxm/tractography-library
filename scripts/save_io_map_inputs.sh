#!/bin/bash
# Save the two run-dir inputs of build_io_map.py that the logger can't see:
#   staged_inputs_<run>.tsv    task, staged copy (relative to the project dir),
#                              source; from the nxf_stage function of each
#                              task's .command.run (stageInMode 'copy': the
#                              copies run outside the container)
#   results_listing_<run>.txt  published path, link target; every symlink
#                              under the results dir (the final-output set)
# <run> is the basename of OUTPUT_DIR (<test>_<jobid>). run_tractoflow*.sh
# calls this after --lib logger runs; run it by hand for older runs.
#
# Usage:
#   scripts/save_io_map_inputs.sh OUTPUT_DIR WORK_DIR RESULTS_DIR

set -euo pipefail

if [[ $# -ne 3 ]]; then
    sed -n '2,/^$/{ s/^# \?//; p }' "$0" >&2
    exit 1
fi
output_dir="$1"
work_dir="$2"
results_dir="$3"
project_dir="/projects/I20240002/nunolxm"
run=$(basename "$output_dir")

for d in "$output_dir" "$work_dir" "$results_dir"; do
    [[ -d "$d" ]] || { echo "Error: $d is not a directory" >&2; exit 1; }
done

staged="$output_dir/staged_inputs_$run.tsv"
find "$work_dir" -name .command.run -exec awk -v root="$project_dir" '
    FNR == 1 {
        dir = FILENAME; sub(/\/[^\/]*$/, "", dir)
        if (index(dir, root "/") == 1) dir = substr(dir, length(root) + 2)
        task = ""; in_stage = 0
    }
    /^# NEXTFLOW TASK: / { task = $4 }
    /^nxf_stage\(\) \{/ { in_stage = 1; next }
    in_stage && /^\}/ { in_stage = 0 }
    in_stage && $1 == "cp" && $2 == "-fRL" { print task "\t" dir "/" $4 "\t" $3 }
' {} + > "$staged"

listing="$output_dir/results_listing_$run.txt"
find "$results_dir" -type l -printf '%p\t%l\n' > "$listing"

echo "Staged inputs: $(wc -l < "$staged") copies -> $staged"
echo "Results listing: $(wc -l < "$listing") links -> $listing"
