nextflow.enable.dsl=1

params.input = 'data/extract_b0/mgh1'
params.output = 'results/extract_b0'
params.b0_thr_extract_b0 = 10

Channel
    .fromPath("${params.input}/*", type: 'dir', checkIfExists: true)
    .map { subject ->
        def sid = subject.baseName

        def dwi = file("${subject}/dwi.nii.gz")
        def bval = file("${subject}/bval")
        def bvec = file("${subject}/bvec")

        if (!dwi.exists() || !bval.exists() || !bvec.exists()) {
            error "Missing input for ${sid}: expected dwi.nii.gz, bval, bvec"
        }

        [sid, dwi, bval, bvec]
    }
    .set { extract_b0_inputs }

process Extract_B0_ONLY {
    tag "$sid"
    label 'big_mem'
    cpus 1

    publishDir "${params.output}/${sid}", mode: 'copy'

    input:
    set sid, file(dwi), file(bval), file(bvec) from extract_b0_inputs

    output:
    file "${sid}__b0_resampled.nii.gz"
    file "${sid}__b0_mask_resampled.nii.gz"

    script:
    """
    export ITK_GLOBAL_DEFAULT_NUMBER_OF_THREADS=1
    export OMP_NUM_THREADS=1
    export OPENBLAS_NUM_THREADS=1

    scil_extract_b0.py $dwi $bval $bvec ${sid}__b0_resampled.nii.gz --mean \
        --b0_thr $params.b0_thr_extract_b0 --force_b0_threshold

    mrthreshold ${sid}__b0_resampled.nii.gz ${sid}__b0_mask_resampled.nii.gz \
        --abs 0.00001 -nthreads 1
    """
}
