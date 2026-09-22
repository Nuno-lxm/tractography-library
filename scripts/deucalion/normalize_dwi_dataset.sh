SRC=/projects/I20240002/nunolxm/tractoflow_files/work/tests/minimal/work_1941004/72/4256b21a836d199230fc08be84ef32
DST=/projects/I20240002/nunolxm/data/normalize_dwi/mgh1/S1

mkdir -p "$DST"

cp "$SRC/S1__dwi_cropped.nii.gz" "$DST/dwi.nii.gz"
cp "$SRC/S1__b0_mask_cropped.nii.gz" "$DST/mask.nii.gz"
cp "$SRC/bval" "$DST/bval"
cp "$SRC/bvec" "$DST/bvec"