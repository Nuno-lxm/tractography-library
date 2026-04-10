#!/bin/bash

singularity="/home/gsd/project/tractoflow/scilus_1.6.0.sif"

config_file="/home/gsd/project/tractography-library/custom_nextflow.config"

nextflow run tractoflow/main.nf \
    -with-singularity $singularity \
    -c $config_file \
    --help
#    --singularity-bind "$lib_path_host:$lib_path_container" \    
