| mgh1 resample | resample_dwi | fslmaths | mrcalc | step | vs gz | final output | intermediate | peak memory |
|---|---|---|---|---|---|---|---|---|
| gz (system zlib) | 258 s | 192 s | 204 s | 655 s | 1.00× | 2.71 GB | 2.86 GB | 13.6 GB |
| zstd (one-shot) | 260 s | 78 s | 192 s | 530 s | 1.24× | 2.71 GB | 2.85 GB (zstd) | 20.1 GB |
| zstd (streaming) | 259 s | 71 s | 185 s | 516 s | 1.27× | 2.71 GB | 2.85 GB (zstd) | 13.6 GB |
| zlib-ng (level 6) | 166 s | 138 s | 133 s | 438 s | 1.50× | 2.71 GB | 2.85 GB | 13.6 GB |
| deflate-py | 172 s | 111 s | 114 s | 398 s | 1.65× | 2.72 GB | 2.86 GB | 20.1 GB |
| zlib-ng level 2 | 166 s | 104 s | 100 s | 372 s | 1.76× | 2.71 GB | 2.85 GB | 13.6 GB |
| deflate-py level 1 | 172 s | 91 s | 95 s | 359 s | 1.82× | 2.71 GB | 2.85 GB | 20.1 GB |
| zstd + zlib-ng level 1, final level 6 | 167 s | 63 s | 126 s | 357 s | 1.84× | 2.71 GB | 2.85 GB (zstd) | 13.6 GB |
| zstd + deflate-py | 171 s | 76 s | 102 s | 350 s | 1.87× | 2.72 GB | 2.85 GB (zstd) | 20.1 GB |
| zstd + deflate-py level 1 | 171 s | 76 s | 83 s | 331 s | 1.98× | 2.71 GB | 2.85 GB (zstd) | 20.1 GB |
| zlib-ng level 1 | 166 s | 83 s | 78 s | 328 s | 2.00× | 3.20 GB | 3.34 GB | 13.6 GB |
| nii | 136 s | 43 s | 40 s | 220 s | 2.98× | 6.25 GB | 6.25 GB | 12.3–13.6 GB |

zstd (streaming): job 2181343 on cnx176, Nov 3, 2026, `zstd_lib.so` from `0be4b80`, target `dwi_resample_clipped.nii.gz` at zstd level 1. It compressed 6.249 GB to 2.848 GB in 15.4 s (the gz intermediate is 2.855 GB), and mrcalc read it back in 5.7 s with 0 rewinds, after a 128 KB header read on a separate handle. A gz run on the same day (2181342, cnx176) took 657 s with the same per-command times as the gz row and the same 13.6 GB peak, and its final output is byte-identical (2,713,540,498 bytes, md5 ebced6c6…). Read and write calls: 382k and 354k, against 709k and 680k for gz.

zstd + zlib-ng level 1, final level 6: job 2182019, Nov 3, 2026, `--lib zstd-zlib-ng --final-targets '*__dwi_resampled.nii.gz'` (`zstd_lib.so` with the level hook from 2181985, `bin/zlib-ng-l1` from Oct 6). mrcalc's final output went to zlib-ng at level 6 (2,707,486,489 bytes, 0.2% under gz). zlib-ng level 2: job 2182020, `bin/zlib-ng-l2` (2181986, default level 2): mrcalc wrote the final output at level 2 in 100 s at the same size as level 6 (2,709,855,688 bytes), and fslmaths's intermediate is 2,849,260,064 bytes (gz: 2,855,406,789). In both, nibabel's `dwi_resample.nii.gz` stays at the level 1 it asks for and grows to 3,344,582,203 bytes (gz: 2,889,980,208, +16%). With zlib-ng loaded, read and write calls drop to 55k and 43k (zlib-ng's larger gz buffers).

| mgh1 minimal pipeline | runs | wall | vs none | DTI_Metrics end | FODF_Metrics end | DWI behind T1 | task time | CPU-h | final outputs | work-dir images |
|---|---|---|---|---|---|---|---|---|---|---|
| none (system zlib) | 2 | 2272 s | 1.00× | 1796 s | 2169 s | 597 s | 1.12 h | 2.14 | 10.15 GB | 35.13 GB |
| zlib-ng (level 6) | 1 | 1839 s | 1.24× | 1377 s | 1726 s | 162 s | 0.97 h | 2.00 | 10.97 GB | 37.24 GB |
| deflate-py | 2 | 1839 s | 1.24× | 1374 s | 1736 s | 168 s | 0.98 h | 2.00 | 10.03 GB | 34.85 GB |
| deflate-py level 1 | 1 | 1807 s | 1.26× | 1332 s | 1694 s | 126 s | 0.96 h | 1.98 | 10.04 GB | 34.86 GB |
| deflate-all level 1 | 1 | 1762 s | 1.29× | 1298 s | 1659 s | 80 s | 0.96 h | 1.98 | 10.04 GB | 34.86 GB |
| zlib-ng level 1 | 1 | 1692 s | 1.34× | 1222 s | 1569 s | 6 s | 0.92 h | 1.95 | 11.80 GB | 40.39 GB |

| mgh1 pipeline step realtime | Resample_DWI | N4_DWI | Extract_DTI_Shell | Extract_FODF_Shell | DTI_Metrics | FODF_Metrics | sum |
|---|---|---|---|---|---|---|---|
| none (system zlib) | 652 s | 594 s | 61 s | 187 s | 145 s | 369 s | 2008 s |
| zlib-ng (level 6) | 434 s | 548 s | 30 s | 88 s | 135 s | 345 s | 1580 s |
| deflate-py | 394 s | 557 s | 39 s | 97 s | 138 s | 359 s | 1584 s |
| deflate-py level 1 | 357 s | 567 s | 38 s | 99 s | 143 s | 358 s | 1562 s |
| deflate-all level 1 | 333 s | 565 s | 41 s | 99 s | 143 s | 358 s | 1539 s |
| zlib-ng level 1 | 323 s | 545 s | 30 s | 84 s | 136 s | 342 s | 1460 s |

Pipeline tables: means where runs = 2 (wall spread 6 s for none, 12 s for deflate-py). Ends are seconds after the first task's submit. DWI behind T1 = DTI_Metrics end − Crop_T1 end: Register_T1 waits for both, so this is the most a faster DWI branch can still take off the wall time. Task time = sum of realtime, CPU-h = sum of realtime × %cpu. Final outputs = .nii.gz in the results dir (symlinks followed). Jobs: none 1960388, 1960391; zlib-ng 1961975; deflate-py 1960389, 1960390; deflate-py L1 1961906; deflate-all L1 1961725; zlib-ng L1 1993337. compression_lib md5 e93aa83d (deflate-py), c281c132 (deflate-py L1, deflate-all L1); zlib-ng aea3dd68 (L6), 0e27ccca (L1).

| full pipeline (start date) | job | dataset | subjects | library | partition, node | DWI steps | tasks | wall | task time | CPU-h | max peak_rss | note |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-02-10 | 953625 | penthera1 | 1 | none | a100 gnx516 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.57 h | 0.8 h | 2.0 | 34.8 GB |  |
| 2026-02-10 | 953626 | penthera8 | 8 | none | a100 gnx516 (32 cpus) | Denoise+Topup+Local tracking | 193 | 2.17 h | 6.3 h | 15.7 | 41.4 GB |  |
| 2026-02-10 | 953627 | penthera1 | 1 | none | a100 gnx516 (32 cpus) | Denoise+Topup+PFT tracking | 25 | 4.42 h | 4.6 h | 5.8 | 34.7 GB |  |
| 2026-02-10 | 953628 | penthera8 | 8 | none | a100 gnx522 (32 cpus) | Denoise+Topup+PFT tracking | 193 | 6.03 h | 35.7 h | 45.1 | 41.2 GB |  |
| 2026-02-11 | 957729 | mgh1 | 1 | none | a100 gnx510 (32 cpus) | Denoise+Eddy+Local tracking | 24 | 2.14 h | 2.5 h | 12.1 | 155.9 GB |  |
| 2026-02-12 | 958924 | mgh1 | 1 | none | a100 gnx506 (32 cpus) | Denoise+Eddy+Local tracking | 24 | 2.15 h | 2.5 h | 12.1 | 155.9 GB |  |
| 2026-04-09 | 1132836 | pent1 | 1 | none | a100 gnx516 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.61 h | 0.8 h | 2.0 | 34.7 GB |  |
| 2026-04-09 | 1132837 | pent1 | 1 | logger | a100 gnx522 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.61 h | 0.8 h | 2.0 | 34.6 GB |  |
| 2026-04-09 | 1133187 | pent1 | 1 | logger | a100 gnx516 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.62 h | 0.8 h | 2.0 | 34.8 GB |  |
| 2026-04-10 | 1133790 | pent1 | 1 | buffer | a100 gnx516 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.60 h | 0.8 h | 2.0 | 34.7 GB |  |
| 2026-04-13 | 1141434 | mgh4 | 4 | none | a100 gnx503 (32 cpus) | Denoise+Eddy+Local tracking | 93 (+1 failed) | 4.61 h | 11.2 h | 52.8 | 192.3 GB | job OOM-killed |
| 2026-04-15 | 1151003 | mgh4 | 4 | none | a100 gnx512 (64 cpus) | Denoise+Eddy+Local tracking | 93 (+1 failed) | 3.63 h | 12.1 h | 55.8 | 192.3 GB | 1 retried |
| 2026-04-24 | 1167478 | mgh8 | 8 | none | a100 gnx502 (32 cpus) | Denoise+Eddy+Local tracking | 95 (+24 failed) | 5.13 h | 30.7 h | 93.1 | 60.2 GB | job OOM-killed |
| 2026-04-24 | 1167644 | pent8 | 8 | none | a100 gnx504 (32 cpus) | Denoise+Topup+Local tracking | 193 | 1.66 h | 11.2 h | 18.1 | 16.3 GB |  |
| 2026-04-24 | 1167668 | pent32 | 32 | none | a100 gnx502 (32 cpus) | Denoise+Topup+Local tracking | 378 (+115 failed) | 3.61 h | 21.0 h | 40.9 | 16.5 GB | 23 tasks never completed |
| 2026-04-24 | 1167986 | pent32 | 32 | none | a100 gnx507 (32 cpus) | Denoise+Topup+Local tracking | 769 (+1 failed) | 5.45 h | 44.3 h | 72.1 | 16.6 GB | 1 retried |
| 2026-04-24 | 1168155 | pent1 | 1 | buffer | a100 gnx504 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.62 h | 0.8 h | 2.0 | 34.9 GB |  |
| 2026-04-28 | 1187674 | mgh1 | 1 | none | a100 gnx502 (32 cpus) | Denoise+Eddy+Local tracking | 24 | 2.39 h | 2.8 h | 12.0 | 155.9 GB |  |
| 2026-05-06 | 1198428 | mgh6 | 6 | none | a100 gnx502 (32 cpus) | Denoise+Eddy+Local tracking | 67 (+19 failed) | 6.50 h | 36.4 h | 107.2 | 64.1 GB | job OOM-killed |
| 2026-05-20 | 1290972 | pent1 | 1 | none | a100 gnx530 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.68 h | 1.0 h | 1.8 | 13.8 GB |  |
| 2026-05-20 | 1290795 | pent1 | 1 | buffer | a100 gnx527 (32 cpus) | Denoise+Topup+Local tracking | 25 | 0.69 h | 1.0 h | 1.9 | 14.0 GB |  |
| 2026-05-27 | 1370534 | pent32 | 32 | none | a100 gnx504 (32 cpus) | Denoise+Topup+Local tracking | 769 | 5.62 h | 47.5 h | 76.6 | 16.7 GB |  |
| 2026-05-27 | 1370212 | pent32 | 32 | buffer | a100 gnx504 (32 cpus) | Denoise+Topup+Local tracking | 769 | 5.74 h | 48.8 h | 77.1 | 16.6 GB |  |
| 2026-05-28 | 1373650 | mgh1 | 1 | none | a100 gnx510 (32 cpus) | Denoise+Eddy+Local tracking | 24 | 3.88 h | 4.4 h | 11.5 | 51.9 GB |  |
| 2026-05-28 | 1373649 | mgh1 | 1 | buffer | a100 gnx510 (32 cpus) | Denoise+Eddy+Local tracking | 24 | 3.88 h | 4.4 h | 11.6 | 51.9 GB |  |
| 2026-05-29 | 1376351 | mgh4 | 4 | buffer | a100 gnx506 (32 cpus) | Denoise+Eddy+Local tracking | 57 (+14 failed) | 5.25 h | 23.7 h | 61.2 | 64.1 GB | job OOM-killed |
| 2026-06-11 | 1436676 | mgh2 | 2 | buffer | a100 gnx513 (32 cpus) | Denoise+Eddy+Local tracking | 47 | 3.96 h | 8.9 h | 23.6 | 54.6 GB |  |
| 2026-06-26 | 1706332 | Penthera_3T | 74 | none | x86 cnx027 (128 cpus) | Denoise | 1333 | 1.44 h | 52.1 h | 122.3 | 16.8 GB |  |
| 2026-06-26 | 1706366 | Penthera_3T | 74 | buffer | x86 cnx201 (128 cpus) | Denoise | 1333 | 1.46 h | 52.8 h | 124.3 | 16.8 GB |  |
| 2026-06-29 | 1711577 | mgh8 | 8 | none | x86 cnx031 (128 cpus) | Denoise | 145 (+2 failed) | 3.08 h | 27.7 h | 86.0 | 68.6 GB | 2 retried |
| 2026-06-29 | 1711578 | mgh8 | 8 | buffer | x86 cnx257 (128 cpus) | Denoise | 145 | 3.08 h | 27.7 h | 86.0 | 68.6 GB |  |

| full pipeline, per-subject mean realtime (min) | Denoise_DWI | Topup | Eddy | N4_DWI | Resample_DWI | FODF_Metrics | Register_T1 | Local_Tracking | PFT_Tracking |
|---|---|---|---|---|---|---|---|---|---|
| 953625 penthera1 none a100 | 0.9 | 5.8 | – | 1.3 | 2.3 | 5.2 | 2.0 | 8.7 | – |
| 953626 penthera8 none a100 | 0.9 | 5.8 | – | 1.3 | 2.4 | 5.1 | 2.0 | 10.2 | – |
| 953627 penthera1 none a100 | 0.9 | 5.8 | – | 1.3 | 2.3 | 5.3 | 2.0 | – | 240.2 |
| 953628 penthera8 none a100 | 0.8 | 5.8 | – | 1.3 | 2.4 | 5.2 | 2.1 | – | 230.9 |
| 957729 mgh1 none a100 | 40.0 | – | 47.9 | 10.4 | 10.8 | 4.7 | 1.9 | 5.0 | – |
| 958924 mgh1 none a100 | 40.0 | – | 47.9 | 10.4 | 10.8 | 4.7 | 1.9 | 5.0 | – |
| 1132836 pent1 none a100 | 0.8 | 5.8 | – | 1.3 | 2.3 | 5.2 | 1.9 | 10.4 | – |
| 1132837 pent1 logger a100 | 0.9 | 5.7 | – | 1.2 | 2.3 | 5.2 | 1.9 | 10.6 | – |
| 1133187 pent1 logger a100 | 0.9 | 5.8 | – | 1.3 | 2.4 | 5.2 | 1.9 | 10.7 | – |
| 1133790 pent1 buffer a100 | 0.9 | 5.7 | – | 1.2 | 2.3 | 5.1 | 1.9 | 10.4 | – |
| 1151003 mgh4 none a100 | 45.1 | – | 67.3 | 11.5 | 12.0 | 5.3 | 2.1 | 8.0 | – |
| 1167644 pent8 none a100 | 2.6 | 5.9 | – | 1.3 | 2.4 | 6.6 | 6.8 | 13.7 | – |
| 1167986 pent32 none a100 | 2.5 | 5.9 | – | 1.3 | 2.5 | 6.7 | 6.8 | 18.1 | – |
| 1168155 pent1 buffer a100 | 0.9 | 5.8 | – | 1.3 | 2.3 | 5.4 | 1.9 | 11.0 | – |
| 1187674 mgh1 none a100 | 42.3 | – | 55.1 | 11.0 | 12.7 | 5.2 | 2.0 | 6.3 | – |
| 1290972 pent1 none a100 | 2.4 | 5.8 | – | 1.3 | 2.3 | 6.5 | 6.2 | 10.4 | – |
| 1290795 pent1 buffer a100 | 2.5 | 5.8 | – | 1.3 | 2.3 | 6.5 | 6.3 | 11.0 | – |
| 1370534 pent32 none a100 | 2.8 | 6.1 | – | 1.4 | 2.5 | 7.0 | 7.1 | 18.5 | – |
| 1370212 pent32 buffer a100 | 2.6 | 6.0 | – | 1.3 | 2.5 | 6.8 | 7.0 | 21.1 | – |
| 1373650 mgh1 none a100 | 133.3 | – | 54.8 | 10.3 | 10.8 | 6.0 | 6.0 | 6.6 | – |
| 1373649 mgh1 buffer a100 | 133.4 | – | 54.1 | 10.3 | 10.7 | 6.0 | 7.5 | 6.2 | – |
| 1436676 mgh2 buffer a100 | 136.0 | – | 52.6 | 10.7 | 11.0 | 6.8 | 6.3 | 6.4 | – |
| 1706332 Penthera_3T none x86 | 3.0 | – | – | 1.5 | 2.6 | 6.9 | 7.6 | – | – |
| 1706366 Penthera_3T buffer x86 | 3.0 | – | – | 1.5 | 2.6 | 7.0 | 7.7 | – | – |
| 1711577 mgh8 none x86 | 133.9 | – | – | 11.9 | 12.9 | 7.7 | 7.2 | – | – |
| 1711578 mgh8 buffer x86 | 134.0 | – | – | 11.9 | 12.9 | 7.6 | 7.2 | – | – |

Full pipeline tables: from the traces in output/ (baseline/, buffer/, logger/, tests/baseline/, tests/buffer/). The output images of these runs are gone: they were only in the work dirs, deleted in the Oct 2026 cleanup. Wall = Slurm elapsed (sacct). Task time and CPU-h count completed tasks only; max peak_rss is the largest single task. a100 = normal-a100 partition (A100 40 GB, Eddy and tracking on GPU); x86 = normal-x86 exclusive node, pipeline without Eddy, Topup and tracking. The per-subject table only lists runs where every task completed, as minutes per subject averaged over subjects. The multi-threaded steps (Denoise_DWI, Register_T1) did not get the same cores in every run: about 12 (1090–1370 %cpu) in every run up to Apr 15 and in 1168155 and 1187674, but about 4 (355–385 %cpu) in 1167644 and 1167986 (Apr 24) and in every run from May 20 on. That is why Denoise_DWI on mgh1 goes from 40–42 to 133 min and the mgh1 wall from 2.1–2.4 to 3.9 h; only compare runs from the same group. Left out: 1115208, 1174146, 1174273, 1174829 (short or aborted test runs, traces with unstarted tasks).
