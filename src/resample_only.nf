nextflow.enable.dsl=1

params.input = 'data/resample_dwi/mgh1'
params.output = 'results/resample_dwi'
params.ext = 'nii.gz'
params.dwi_resolution = 1
params.dwi_interpolation = 'lin'
params.publish = false

// FSL picks its output format from FSLOUTPUTTYPE, MRtrix and nibabel from the extension
fsl_output_type = params.ext == 'nii' ? 'NIFTI' : 'NIFTI_GZ'

Channel
    .fromPath("${params.input}/*", type: 'dir', checkIfExists: true)
    .map { subject ->
        def sid = subject.baseName

        def dwi = file("${subject}/dwi.${params.ext}")
        def mask = file("${subject}/mask.${params.ext}")

        if (!dwi.exists() || !mask.exists()) {
            error "Missing input for ${sid}: expected dwi.${params.ext}, mask.${params.ext}"
        }

        [sid, dwi, mask]
    }
    .set { resample_inputs }

process Resample_DWI_ONLY {
    tag "$sid"
    cpus 1

    publishDir "${params.output}/${sid}", mode: 'copy', enabled: params.publish

    input:
    set sid, file(dwi), file(mask) from resample_inputs

    output:
    file "${sid}__dwi_resampled.${params.ext}"

    script:
    """
    export ITK_GLOBAL_DEFAULT_NUMBER_OF_THREADS=1
    export OMP_NUM_THREADS=1
    export OPENBLAS_NUM_THREADS=1
    export FSLOUTPUTTYPE=$fsl_output_type

    scil_resample_volume.py $dwi \
        dwi_resample.${params.ext} \
        --voxel_size $params.dwi_resolution \
        --interp $params.dwi_interpolation
    fslmaths dwi_resample.${params.ext} -thr 0 dwi_resample_clipped.${params.ext}
    scil_resample_volume.py $mask \
        mask_resample.${params.ext} \
        --ref dwi_resample.${params.ext} \
        --enforce_dimensions \
        --interp nn
    mrcalc dwi_resample_clipped.${params.ext} mask_resample.${params.ext} \
        -mult ${sid}__dwi_resampled.${params.ext} -quiet -nthreads 1
    """
}
