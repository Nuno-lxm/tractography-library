#!/bin/bash

export PYENV_ROOT="$HOME/.pyenv"
export PATH="$PYENV_ROOT/bin:$PATH"
eval "$(pyenv init -)"

pyenv activate scilpy

input_dir="/home/gsd/project/data/input1"
output_dir="/home/gsd/project/results/output1/"
work_dir="/home/gsd/project/nxf_work/"

lib_path="/home/gsd/project/tractography-library/bin/buffer_lib.so"

report_file="/home/gsd/project/results/output1/report.html"
timeline_file="/home/gsd/project/results/output1/timeline.html"
trace_file="/home/gsd/project/results/output1/trace.txt"

export LD_PRELOAD=$lib_path

nextflow run tractoflow/main.nf \
    --processes 6 \
    -with-report "$report_file" \
    -with-timeline "$timeline_file" \
    -with-trace "$trace_file" \
    -w "$work_dir" \
    --input "$input_dir" \
    --output_dir "$output_dir"
