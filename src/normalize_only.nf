nextflow.enable.dsl=1

params.input = 'data/normalize_dwi/mgh1'
params.output = 'results/normalize_dwi'
params.max_dti_shell_value = 1200
params.dwi_shell_tolerance = 20
params.fa_mask_threshold = 0.4

Channel
    .fromPath("${params.input}/*", type: 'dir', checkIfExists: true)
    .map { subject ->
        def sid = subject.baseName

        def dwi = file("${subject}/dwi.nii.gz")
        def mask = file("${subject}/mask.nii.gz")
        def bval = file("${subject}/bval")
        def bvec = file("${subject}/bvec")

        if (!dwi.exists() || !mask.exists() || !bval.exists() || !bvec.exists()) {
            error "Missing input for ${sid}: expected dwi.nii.gz, mask.nii.gz, bval, bvec"
        }

        [sid, dwi, mask, bval, bvec]
    }
    .set { normalize_inputs }

process Normalize_DWI_ONLY {
    tag "$sid"
    label 'big_mem'
    cpus 1

    publishDir "${params.output}/${sid}", mode: 'copy'

    input:
    set sid, file(dwi), file(mask), file(bval), file(bvec) from normalize_inputs

    output:
    file 'dwi_normalized.nii.gz'
    file 'fa_wm_mask.nii.gz'

    script:
    """
    export ITK_GLOBAL_DEFAULT_NUMBER_OF_THREADS=1
    export OMP_NUM_THREADS=1
    export OPENBLAS_NUM_THREADS=1

    shells=\$(cut -d ' ' --output-delimiter=\$'\\n' -f 1- $bval |
        awk -F' ' '{v=int(\$1)}{if(v <= $params.max_dti_shell_value) print v}' |
        uniq)

    scil_extract_dwi_shell.py $dwi $bval $bvec \
        \$shells dwi_dti.nii.gz bval_dti bvec_dti \
        -t $params.dwi_shell_tolerance

    scil_compute_dti_metrics.py dwi_dti.nii.gz bval_dti bvec_dti \
        --mask $mask \
        --not_all \
        --fa fa.nii.gz \
        --force_b0_threshold

    mrthreshold fa.nii.gz fa_wm_mask.nii.gz \
        -abs $params.fa_mask_threshold \
        -nthreads 1

    dwinormalise individual $dwi \
        fa_wm_mask.nii.gz \
        dwi_normalized.nii.gz \
        -fslgrad $bvec $bval \
        -nthreads 1
    """
}