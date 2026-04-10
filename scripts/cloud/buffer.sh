#!/bin/bash

input_dir="/home/gsd/project/data/input1"
output_dir="/home/gsd/project/results/buffer/"
work_dir="/home/gsd/project/nxf_work/buffer/"
tmp_dir="/tmp/tractoflow-buffer-${USER}"

singularity="/home/gsd/project/tractoflow/scilus_1.6.0.sif"

config_file="/home/gsd/project/tractography-library/config-files/nextflow-buffer.config"

report_file="/home/gsd/project/output/buffer/report.html"
timeline_file="/home/gsd/project/output/buffer/timeline.html"
trace_file="/home/gsd/project/output/buffer/trace.txt"

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
