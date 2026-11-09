# TractoFlow run shared by scripts/tests/run_tractoflow.sh (x86) and
# run_tractoflow_gpu.sh (A100). The caller sets GPU=true|false and sources
# common.sh first; the options are documented in run_tractoflow.sh.

DATASET=""
PIPELINE="minimal"
SSD=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dataset)  DATASET="$2"; shift 2 ;;
        --pipeline) PIPELINE="$2"; shift 2 ;;
        --lib)      LIB="$2"; shift 2 ;;
        --level)    LEVEL="$2"; shift 2 ;;
        --debug)    DEBUG=true; shift ;;
        --zstd-targets) ZSTD_TARGETS="$2"; shift 2 ;;
        --final-targets) FINAL_TARGETS="$2"; shift 2 ;;
        --ssd)      SSD=true; shift ;;
        --help|-h)  sed -n '/^#[S!]/d; /^#/,/^$/{ s/^# \?//; p }' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$DATASET" ]]; then
    echo "Usage: sbatch $0 --dataset NAME [--pipeline minimal|full|default] [--lib MODE] [--level N] [--debug] [--zstd-targets GLOBS] [--final-targets GLOBS] [--ssd]" >&2
    exit 1
fi
check_lib_options

case "$PIPELINE" in
    minimal)
        pipeline_args=(--run_dwi_denoising false --run_topup false --run_eddy false
                       --run_t1_denoising false
                       --run_pft_tracking false --run_local_tracking false) ;;
    full)
        pipeline_args=(--run_pft_tracking false --run_eddy false) ;;
    default)
        # TractoFlow's own defaults: DWI denoising, topup (for subjects with a
        # rev_b0), eddy and PFT tracking on; T1 denoising and local tracking off
        pipeline_args=() ;;
    *)
        echo "Error: --pipeline must be minimal, full or default (got: $PIPELINE)" >&2
        exit 1 ;;
esac

# default only adds the profile (eddy on CUDA), keeping TractoFlow's tracking
if [[ "$GPU" == true ]]; then
    pipeline_args+=(-profile use_gpu)
    if [[ "$PIPELINE" != default ]]; then
        pipeline_args+=(--run_local_tracking true --local_seeding npv
                        --local_nbr_seeds 15 --local_batch_size_gpu 40000)
    fi
fi

# ── Paths ───────────────────────────────────────────────────────────────────

input_dir="$project_dir/data/$DATASET"
tractoflow_dir="$project_dir/tractoflow_files/tractoflow"

test_name="${DATASET}_${LIB}${LEVEL:+_l$LEVEL}"
[[ "$SSD" == true ]] && test_name+="_ssd"
[[ "$GPU" == true ]] && test_name+="_gpu" || test_name+="_x86"
set_run_dirs "$PIPELINE"

module load Java/11.0.20
mkdir -p "$output_dir"

config_args=()
add_lib_configs

# ── Local SSD ───────────────────────────────────────────────────────────────

if [[ "$SSD" == true ]]; then
    local_base="/tmp/ssd_${DATASET}_${SLURM_JOB_ID}"
    mkdir -p "$local_base/input" "$local_base/tractoflow_files"
    echo "[$(date)] Copying input, container and TractoFlow to $local_base..."
    cp -r "$input_dir" "$local_base/input/"
    cp "$sif" "$local_base/tractoflow_files/"
    cp -r "$tractoflow_dir" "$local_base/tractoflow_files/"

    input_dir="$local_base/input/$DATASET"
    sif="$local_base/tractoflow_files/$(basename "$sif")"
    tractoflow_dir="$local_base/tractoflow_files/tractoflow"
    final_results_dir="$results_dir"
    results_dir="$local_base/results"
    work_dir="$local_base/work"
fi

cleanup() {
    stop_monitoring
    if [[ "$SSD" == true ]]; then
        echo "[$(date)] Copying results back and cleaning up $local_base..."
        mkdir -p "$final_results_dir"
        cp -r "$results_dir"/* "$final_results_dir/" 2>/dev/null || true
        rm -rf "$local_base"
    fi
}
trap cleanup EXIT

start_monitoring

echo "SLURM_JOB_ID=$SLURM_JOB_ID on $(hostname)"
echo "Test: $test_name ($PIPELINE pipeline, $input_dir)"
print_lib_version
echo "Config: ${config_args[*]}"
[[ -f "$output_dir/run.config" ]] && cat "$output_dir/run.config"

# ── Run ─────────────────────────────────────────────────────────────────────

start_ts=$(date +%s)

NXF_VER=21.04.0 nextflow run "$tractoflow_dir/main.nf" \
    -with-singularity "$sif" \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input "$input_dir" \
    --output_dir "$results_dir" \
    --processes "$SLURM_CPUS_PER_TASK" \
    "${pipeline_args[@]}" \
    "${config_args[@]}"

elapsed=$(( $(date +%s) - start_ts ))
echo "Total wall time: ${elapsed}s ($((elapsed / 60))m $((elapsed % 60))s)"

# build_io_map.py's other inputs; with --ssd the work dir is gone at exit
if [[ "$LIB" == logger && "$SSD" == false ]]; then
    "$lib_dir/scripts/save_io_map_inputs.sh" "$output_dir" "$work_dir" "$results_dir" ||
        echo "Warning: save_io_map_inputs.sh failed" >&2
fi

print_summary
