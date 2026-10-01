#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=01:00:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=isolated
#SBATCH --no-requeue

# One TractoFlow step on its own (src/<step>_only.nf), with or without one
# of the libraries.
#
# Usage:
#   sbatch run_isolated.sh --step normalize --dataset mgh1 --lib prefetch
#   sbatch run_isolated.sh --step resample --dataset mgh1 --lib deflate-py --level 1
#   sbatch run_isolated.sh --step resample --dataset mgh1 --nii
#   sbatch run_isolated.sh --step resample --dataset mgh1 --lib zstd --debug
#
# --step normalize|extract_b0|resample   (required)
# --dataset NAME      (required) input under data/<step dir>/NAME:
#                       normalize   data/normalize_dwi/  (dwi, mask, bval, bvec)
#                       extract_b0  data/extract_b0/     (dwi, bval, bvec)
#                       resample    data/resample_dwi/   (dwi, mask), or
#                                   data/resample_dwi_nii/ with --nii
# --lib, --level, --debug, --zstd-targets
#                     library options, see scripts/lib/common.sh. With --lib
#                     zstd and --step resample, --zstd-targets defaults to the
#                     fslmaths output that mrcalc reads back
# --nii               resample: uncompressed .nii in and out
# --publish           resample: copy the resampled DWI to results (~6 GB each)
# --test-dir NAME     put output, results and work under tests/isolated/NAME
#                     (default: the step name)
# --clean-work        delete the work dir at the end

set -euo pipefail
source /projects/I20240002/nunolxm/tractography-library/scripts/lib/common.sh

STEP=""
DATASET=""
NII=false
PUBLISH=false
TEST_DIR=""
CLEAN_WORK=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --step)       STEP="$2"; shift 2 ;;
        --dataset)    DATASET="$2"; shift 2 ;;
        --lib)        LIB="$2"; shift 2 ;;
        --level)      LEVEL="$2"; shift 2 ;;
        --debug)      DEBUG=true; shift ;;
        --zstd-targets) ZSTD_TARGETS="$2"; shift 2 ;;
        --nii)        NII=true; shift ;;
        --publish)    PUBLISH=true; shift ;;
        --test-dir)   TEST_DIR="$2"; shift 2 ;;
        --clean-work) CLEAN_WORK=true; shift ;;
        --help|-h)    sed -n '/^#[S!]/d; /^#/,/^$/{ s/^# \?//; p }' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$STEP" || -z "$DATASET" ]]; then
    echo "Usage: sbatch $0 --step normalize|extract_b0|resample --dataset NAME [--lib MODE] [--level N] [--debug] [--zstd-targets GLOBS] [--nii] [--publish]" >&2
    exit 1
fi
# Intermediate written by fslmaths (gzopen) and only read by mrcalc (gzopen);
# the final output stays gzip
if [[ "$LIB" == zstd && "$STEP" == resample && -z "$ZSTD_TARGETS" ]]; then
    ZSTD_TARGETS="dwi_resample_clipped.nii.gz"
fi
check_lib_options

step_args=()
case "$STEP" in
    normalize)  data_name="normalize_dwi" ;;
    extract_b0) data_name="extract_b0" ;;
    resample)
        data_name="resample_dwi"
        ext="nii.gz"
        if [[ "$NII" == true ]]; then
            data_name="resample_dwi_nii"
            ext="nii"
        fi
        step_args=(--ext "$ext" --publish "$PUBLISH") ;;
    *)
        echo "Error: --step must be normalize, extract_b0 or resample (got: $STEP)" >&2
        exit 1 ;;
esac

if [[ "$STEP" != resample ]] && [[ "$NII" == true || "$PUBLISH" == true ]]; then
    echo "Error: --nii and --publish only work with --step resample" >&2
    exit 1
fi

# ── Paths ───────────────────────────────────────────────────────────────────

pipeline_file="$lib_dir/src/${STEP}_only.nf"
input_dir="$project_dir/data/$data_name/$DATASET"

test_name="${DATASET}_${STEP}_${LIB}${LEVEL:+_l$LEVEL}"
[[ "$NII" == true ]] && test_name+="_nii"
test_name+="_x86"
set_run_dirs "isolated/${TEST_DIR:-$STEP}"

module load Java/11.0.20
mkdir -p "$output_dir"

# The workflow isn't next to TractoFlow's nextflow.config, so load it first
config_args=(-c "$project_dir/tractoflow_files/tractoflow/nextflow.config")
add_lib_configs

trap stop_monitoring EXIT
start_monitoring

subject_count=$(find "$input_dir" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
if [[ "$subject_count" -eq 0 ]]; then
    echo "No subject directories found in: $input_dir" >&2
    exit 1
fi

echo "SLURM_JOB_ID=$SLURM_JOB_ID on $(hostname)"
echo "Test: $test_name ($subject_count subjects in $input_dir)"
print_lib_version
echo "Config: ${config_args[*]}"
[[ -f "$output_dir/run.config" ]] && cat "$output_dir/run.config"

# ── Run ─────────────────────────────────────────────────────────────────────

NXF_VER=21.04.0 nextflow run "$pipeline_file" \
    -with-singularity "$sif" \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input "$input_dir" \
    --output "$results_dir" \
    "${step_args[@]}" \
    "${config_args[@]}"

# Per-command times from each task's stderr (resample_only.nf times each command)
for err in $(find "$work_dir" -name .command.err); do
    grep '^TIME ' "$err" | sed "s|^|  $(basename "$(dirname "$err")") |" || true
done

print_summary

if [[ "$CLEAN_WORK" == true ]]; then
    rm -rf "$work_dir"
    echo "Removed work dir: $work_dir"
fi
