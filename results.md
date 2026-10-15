| mgh1 resample | resample_dwi | fslmaths | mrcalc | step | vs gz | final output | intermediate | peak memory |
|---|---|---|---|---|---|---|---|---|
| gz (system zlib) | 258 s | 192 s | 204 s | 655 s | 1.00× | 2.71 GB | 2.86 GB | 13.6 GB |
| zstd | 260 s | 78 s | 192 s | 530 s | 1.24× | 2.71 GB | 2.85 GB (zstd) | 20.1 GB |
| zlib-ng (level 6) | 166 s | 138 s | 133 s | 438 s | 1.50× | 2.71 GB | 2.85 GB | 13.6 GB |
| deflate-py | 172 s | 111 s | 114 s | 398 s | 1.65× | 2.72 GB | 2.86 GB | 20.1 GB |
| deflate-py level 1 | 172 s | 91 s | 95 s | 359 s | 1.82× | 2.71 GB | 2.85 GB | 20.1 GB |
| zstd + deflate-py | 171 s | 76 s | 102 s | 350 s | 1.87× | 2.72 GB | 2.85 GB (zstd) | 20.1 GB |
| zstd + deflate-py level 1 | 171 s | 76 s | 83 s | 331 s | 1.98× | 2.71 GB | 2.85 GB (zstd) | 20.1 GB |
| zlib-ng level 1 | 166 s | 83 s | 78 s | 328 s | 2.00× | 3.20 GB | 3.34 GB | 13.6 GB |
| nii | 136 s | 43 s | 40 s | 220 s | 2.98× | 6.25 GB | 6.25 GB | 12.3–13.6 GB |

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
