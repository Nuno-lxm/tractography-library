#!/bin/bash

for i in {1..8}; do
    src="data/MGH_HCP/mgh_100${i}"
    tgt="data/mgh8/S${i}"

    mkdir -p "$tgt"

    cp "$src/anat/T1/T1.nii.gz" "$tgt/t1.nii.gz"
    cp "$src/diff/raw/mri/diff.nii.gz" "$tgt/dwi.nii.gz"
    cp "$src/diff/raw/bvals.txt" "$tgt/bval"
    cp "$src/diff/raw/bvecs_fsl.txt" "$tgt/bvec"

    echo "Copied subject $i"

done