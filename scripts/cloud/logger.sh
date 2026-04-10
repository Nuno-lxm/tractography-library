#!/bin/bash

input_dir="/home/gsd/project/data/input1"
output_dir="/home/gsd/project/results/logger/"
work_dir="/home/gsd/project/nxf_work/logger/"
tmp_dir="/tmp/tractoflow-logger-${USER}"

singularity="/home/gsd/project/tractoflow/scilus_1.6.0.sif"

config_file="/home/gsd/project/tractography-library/config-files/nextflow-logger.config"

report_file="/home/gsd/project/output/logger/report.html"
timeline_file="/home/gsd/project/output/logger/timeline.html"
trace_file="/home/gsd/project/output/logger/trace.txt"

mkdir -p "$tmp_dir"
export TMPDIR="$tmp_dir"
export NXF_TEMP="$tmp_dir"
export APPTAINERENV_TMPDIR="$tmp_dir"
export APPTAINERENV_MPLCONFIGDIR="$tmp_dir/matplotlib"
mkdir -p "$APPTAINERENV_MPLCONFIGDIR"


nextflow run tractoflow/main.nf \
    --processes 6 \
    -with-singularity $singularity \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input $input_dir \
    --output_dir $output_dir \
    -c $config_file \
