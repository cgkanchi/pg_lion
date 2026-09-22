# Quick index progress benchmark

Commit `390653883ea1d8ca0a3e057e0f9d8f9c56ec2041`. Profile `quick-v2`. Completed in **227.0 seconds**, with **160 exact-result checks** and **320 timings**. All checks pass.

Only B-tree, GIN, roaring, and roaring_bitmap are timed. Documents use GIN and roaring; B-tree has no matching array/full-text index here. Sequential scans supply untimed correctness references. 2 timing rounds, 1 warmup(s), one index build; warm cache, single-client queries, parallel query/JIT off. Small-sample medians/min/max are progress signals, not confidence intervals or production capacity estimates. Build and maintenance times are single observations.

Read timings are EXPLAIN execution time; planning is separate. Positive change means slower than the baseline. Inspect plan changes and repeat suspicious differences, especially sub-millisecond results. Different commits/extension binaries are expected; incompatible workloads/server settings are rejected.

Baseline: `/home/cgkanchi/code/pg_lion/bench/results/quick/2026-09-21-481f876`, commit `481f8769caf4b28ba228c865921fad0ca7cc80d9`.

## Build and size

| Suite | Rows | Family | Build ms | Indexes MiB |
| --- | --- | --- | --- | --- |
| scalar | 1000000 | gin | 771.496 | 18.945 |
| scalar | 1000000 | roaring | 2744.282 | 22.305 |
| scalar | 1000000 | btree | 1095.581 | 33.516 |
| scalar | 5000000 | roaring | 16107.592 | 94.008 |
| scalar | 5000000 | btree | 5991.335 | 166.812 |
| scalar | 5000000 | gin | 3281.183 | 62.703 |
| documents | 200000 | roaring | 1782.067 | 7.672 |
| documents | 200000 | gin | 334.752 | 6.375 |

## Maintenance

| Rows | Family | Operation | Elapsed ms | WAL MiB |
| --- | --- | --- | --- | --- |
| 1000000 | gin | insert | 57.446 | 9.234 |
| 1000000 | gin | indexed_update | 106.711 | 12.547 |
| 1000000 | gin | vacuum | 162.419 | 14.838 |
| 1000000 | roaring | insert | 339.324 | 39.166 |
| 1000000 | roaring | indexed_update | 462.889 | 52.866 |
| 1000000 | roaring | vacuum | 119.910 | 4.204 |
| 1000000 | btree | insert | 92.127 | 14.041 |
| 1000000 | btree | indexed_update | 203.060 | 18.106 |
| 1000000 | btree | vacuum | 118.968 | 4.223 |
| 5000000 | roaring | insert | 1704.076 | 191.936 |
| 5000000 | roaring | indexed_update | 2231.910 | 257.974 |
| 5000000 | roaring | vacuum | 437.463 | 20.729 |
| 5000000 | btree | insert | 548.301 | 59.688 |
| 5000000 | btree | indexed_update | 1205.313 | 77.010 |
| 5000000 | btree | vacuum | 435.219 | 20.739 |
| 5000000 | gin | insert | 285.447 | 46.051 |
| 5000000 | gin | indexed_update | 792.083 | 62.568 |
| 5000000 | gin | vacuum | 584.741 | 56.255 |

## Query timings and baseline comparison

| Suite | Rows | Variant | State | Planner | Memory | Query | n | Median ms | Min ms | Max ms | Planning ms | Plan | Baseline ms | Change % | Baseline plan | Plan changed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| documents | 200000 | gin | clean | default | 64MB | array_and | 2 | 0.869 | 0.860 | 0.878 | 0.058 | Bitmap Heap Scan, Bitmap Index Scan | 0.956 | -9.148 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | gin | clean | default | 64MB | array_common | 2 | 3.994 | 3.901 | 4.087 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan | 4.909 | -18.631 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | gin | clean | default | 64MB | array_or | 2 | 6.617 | 6.608 | 6.625 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan | 8.040 | -17.710 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | gin | clean | default | 64MB | ts_and | 2 | 1.030 | 1.001 | 1.058 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan | 1.069 | -3.695 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | gin | clean | default | 64MB | ts_common | 2 | 29.555 | 29.021 | 30.089 | 0.045 | Seq Scan | 35.243 | -16.141 | Seq Scan | False |
| documents | 200000 | gin | clean | default | 64MB | ts_fetch | 2 | 4.457 | 4.336 | 4.577 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan | 4.903 | -9.107 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | gin | clean | default | 64MB | ts_phrase | 2 | 7.088 | 6.804 | 7.373 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan | 7.893 | -10.198 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | gin | clean | default | 64MB | ts_prefix | 2 | 1.673 | 1.664 | 1.682 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan | 1.720 | -2.761 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | roaring | clean | default | 64MB | array_and | 2 | 0.149 | 0.128 | 0.170 | 0.078 | LionCount | 0.115 | 29.565 | LionCount | False |
| documents | 200000 | roaring | clean | default | 64MB | array_common | 2 | 0.025 | 0.025 | 0.025 | 0.080 | LionCount | 0.025 | 2.041 | LionCount | False |
| documents | 200000 | roaring | clean | default | 64MB | array_or | 2 | 0.244 | 0.242 | 0.247 | 0.089 | LionCount | 0.277 | -11.573 | LionCount | False |
| documents | 200000 | roaring | clean | default | 64MB | ts_and | 2 | 0.135 | 0.131 | 0.138 | 0.082 | LionCount | 0.116 | 15.948 | LionCount | False |
| documents | 200000 | roaring | clean | default | 64MB | ts_common | 2 | 0.032 | 0.031 | 0.032 | 0.067 | LionCount | 0.030 | 5.000 | LionCount | False |
| documents | 200000 | roaring | clean | default | 64MB | ts_fetch | 2 | 4.068 | 3.853 | 4.283 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan | 5.700 | -28.625 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | roaring | clean | default | 64MB | ts_phrase | 2 | 35.391 | 33.276 | 37.506 | 0.057 | Seq Scan | 46.614 | -24.076 | Seq Scan | False |
| documents | 200000 | roaring | clean | default | 64MB | ts_prefix | 2 | 27.441 | 25.925 | 28.957 | 0.050 | Seq Scan | 29.463 | -6.864 | Seq Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_and | 2 | 0.208 | 0.204 | 0.212 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan | 0.189 | 10.345 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_common | 2 | 3.474 | 3.376 | 3.571 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan | 3.478 | -0.144 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_or | 2 | 5.524 | 5.187 | 5.860 | 0.063 | Bitmap Heap Scan, Bitmap Index Scan | 5.773 | -4.322 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_and | 2 | 0.215 | 0.201 | 0.229 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan | 0.181 | 18.457 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_common | 2 | 28.910 | 28.384 | 29.437 | 0.045 | Seq Scan | 31.383 | -7.877 | Seq Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_fetch | 2 | 3.763 | 3.755 | 3.771 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan | 4.203 | -10.469 | Bitmap Heap Scan, Bitmap Index Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_phrase | 2 | 32.694 | 31.490 | 33.897 | 0.045 | Seq Scan | 36.467 | -10.348 | Seq Scan | False |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_prefix | 2 | 25.601 | 24.798 | 26.404 | 0.059 | Seq Scan | 26.913 | -4.877 | Seq Scan | False |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.345 | 0.340 | 0.350 | 0.042 | Index Only Scan | 0.355 | -2.817 | Index Only Scan | False |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 88.275 | 82.022 | 94.527 | 0.035 | Index Only Scan | 86.829 | 1.664 | Index Only Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | and2 | 2 | 2.639 | 2.614 | 2.664 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan | 2.764 | -4.505 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | and3 | 2 | 2.604 | 2.590 | 2.619 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan | 2.566 | 1.520 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 0.346 | 0.325 | 0.366 | 0.033 | Index Only Scan | 0.318 | 8.648 | Index Only Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.022 | 0.022 | 0.022 | 0.033 | Index Only Scan | 0.021 | 2.326 | Index Only Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 33.817 | 30.817 | 36.816 | 0.033 | Index Only Scan | 32.336 | 4.578 | Index Only Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | fetch_medium | 2 | 3.611 | 3.469 | 3.753 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan | 3.578 | 0.936 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | group_c200 | 2 | 80.819 | 79.496 | 82.141 | 0.035 | Index Only Scan | 85.142 | -5.078 | Index Only Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 0.323 | 0.318 | 0.328 | 0.087 | Index Only Scan | 0.325 | -0.462 | Index Only Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | is_null | 2 | 6.743 | 6.726 | 6.760 | 0.028 | Index Only Scan | 7.758 | -13.078 | Index Only Scan | False |
| scalar | 1000000 | btree | clean | default | 64MB | range_random | 2 | 0.313 | 0.310 | 0.316 | 0.051 | Index Only Scan | 0.298 | 4.858 | Index Only Scan | False |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 3.149 | 3.026 | 3.273 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan | 3.024 | 4.133 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.340 | 0.334 | 0.346 | 0.048 | Index Only Scan | 0.348 | -2.299 | Index Only Scan | False |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 68.400 | 68.161 | 68.638 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan | 72.782 | -6.022 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 143.024 | 142.198 | 143.849 | 0.044 | Seq Scan | 142.221 | 0.564 | Seq Scan | False |
| scalar | 1000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 80.267 | 79.864 | 80.670 | 0.036 | Index Only Scan | 78.750 | 1.926 | Index Only Scan | False |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 3.232 | 3.220 | 3.243 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan | 3.128 | 3.309 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 155.277 | 152.492 | 158.062 | 0.035 | Seq Scan | 150.645 | 3.074 | Seq Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | and2 | 2 | 32.556 | 32.286 | 32.826 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan | 33.469 | -2.729 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | and3 | 2 | 5.067 | 5.031 | 5.103 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan | 5.069 | -0.039 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 3.200 | 3.117 | 3.284 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan | 2.827 | 13.212 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 0.050 | 0.047 | 0.052 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan | 0.051 | -1.980 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 93.520 | 92.072 | 94.968 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan | 93.011 | 0.548 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | fetch_medium | 2 | 3.132 | 3.067 | 3.197 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan | 3.228 | -2.989 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | group_c200 | 2 | 140.827 | 140.186 | 141.468 | 0.041 | Seq Scan | 142.057 | -0.866 | Seq Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 3.503 | 3.421 | 3.585 | 0.105 | Bitmap Heap Scan, Bitmap Index Scan | 3.417 | 2.517 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | is_null | 2 | 61.439 | 58.462 | 64.416 | 0.026 | Seq Scan | 56.189 | 9.342 | Seq Scan | False |
| scalar | 1000000 | gin | clean | default | 64MB | range_random | 2 | 41.853 | 39.011 | 44.695 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan | 38.532 | 8.617 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 33.800 | 33.194 | 34.405 | 0.070 | Bitmap Heap Scan, Bitmap Index Scan | 34.035 | -0.693 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 3.264 | 3.150 | 3.378 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan | 3.010 | 8.439 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 90.312 | 89.843 | 90.782 | 0.058 | Bitmap Heap Scan, Bitmap Index Scan | 88.222 | 2.369 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 144.748 | 138.648 | 150.848 | 0.034 | Seq Scan | 146.727 | -1.349 | Seq Scan | False |
| scalar | 1000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 148.520 | 145.883 | 151.157 | 0.051 | Seq Scan | 141.812 | 4.730 | Seq Scan | False |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.038 | 0.038 | 0.039 | 0.036 | LionCount | 0.049 | -21.429 | LionCount | False |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 4.550 | 4.546 | 4.554 | 0.033 | LionCount | 4.463 | 1.938 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | and2 | 2 | 0.372 | 0.363 | 0.381 | 0.043 | LionCount | 0.357 | 4.202 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | and3 | 2 | 0.317 | 0.310 | 0.324 | 0.063 | LionCount | 0.295 | 7.458 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.027 | 0.026 | 0.028 | 0.036 | LionCount | 0.028 | -5.263 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.014 | 0.013 | 0.016 | 0.038 | LionCount | 0.017 | -12.121 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 0.356 | 0.356 | 0.357 | 0.040 | LionCount | 0.408 | -12.623 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 3.196 | 3.158 | 3.233 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan | 3.029 | 5.514 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring | clean | default | 64MB | group_c200 | 2 | 3.753 | 3.724 | 3.783 | 0.033 | LionCount | 3.874 | -3.110 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 0.160 | 0.159 | 0.161 | 0.132 | LionCount | 0.577 | -72.246 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | is_null | 2 | 0.092 | 0.092 | 0.093 | 0.038 | LionCount | 0.114 | -18.860 | LionCount | False |
| scalar | 1000000 | roaring | clean | default | 64MB | range_random | 2 | 111.055 | 110.400 | 111.710 | 0.035 | Seq Scan | 119.384 | -6.977 | Seq Scan | False |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 0.433 | 0.377 | 0.489 | 0.045 | LionCount | 0.352 | 22.837 | LionCount | False |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.037 | 0.029 | 0.046 | 0.039 | LionCount | 0.030 | 22.951 | LionCount | False |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 0.364 | 0.363 | 0.365 | 0.037 | LionCount | 0.398 | -8.428 | LionCount | False |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 3.646 | 3.634 | 3.658 | 0.036 | LionCount | 3.699 | -1.419 | LionCount | False |
| scalar | 1000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 3.641 | 3.611 | 3.670 | 0.033 | LionCount | 3.890 | -6.414 | LionCount | False |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 2.921 | 2.851 | 2.990 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan | 3.051 | -4.293 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 148.002 | 145.740 | 150.263 | 0.032 | Seq Scan | 145.240 | 1.901 | Seq Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 7.008 | 6.913 | 7.103 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan | 6.678 | 4.942 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 2.413 | 2.387 | 2.439 | 0.064 | Bitmap Heap Scan, Bitmap Index Scan | 2.340 | 3.120 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 3.054 | 2.766 | 3.341 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan | 2.635 | 15.860 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 0.056 | 0.046 | 0.065 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan | 0.048 | 16.842 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 74.308 | 69.123 | 79.492 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan | 62.758 | 18.403 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 3.053 | 3.018 | 3.087 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan | 2.806 | 8.765 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 145.173 | 141.774 | 148.573 | 0.033 | Seq Scan | 142.688 | 1.742 | Seq Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 3.154 | 2.706 | 3.602 | 0.090 | Bitmap Heap Scan, Bitmap Index Scan | 3.386 | -6.865 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 26.940 | 25.663 | 28.216 | 0.028 | Bitmap Heap Scan, Bitmap Index Scan | 26.041 | 3.448 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 53.296 | 52.638 | 53.955 | 0.037 | Seq Scan | 55.235 | -3.510 | Seq Scan | False |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 6.784 | 6.710 | 6.857 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan | 3.155 | 115.042 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 2.753 | 2.730 | 2.776 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan | 2.782 | -1.060 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 66.173 | 62.568 | 69.779 | 0.032 | Bitmap Heap Scan, Bitmap Index Scan | 66.356 | -0.276 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 140.151 | 138.020 | 142.283 | 0.036 | Seq Scan | 142.659 | -1.758 | Seq Scan | False |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 145.092 | 144.763 | 145.421 | 0.052 | Seq Scan | 139.688 | 3.869 | Seq Scan | False |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 1.661 | 1.642 | 1.681 | 0.056 | Index Only Scan | 1.773 | -6.289 | Index Only Scan | False |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 432.493 | 426.493 | 438.492 | 0.037 | Index Only Scan | 464.946 | -6.980 | Index Only Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | and2 | 2 | 15.588 | 13.660 | 17.517 | 0.071 | Bitmap Heap Scan, Bitmap Index Scan | 16.193 | -3.733 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | and3 | 2 | 13.884 | 13.326 | 14.441 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan | 13.888 | -0.032 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 1.522 | 1.520 | 1.524 | 0.034 | Index Only Scan | 1.530 | -0.490 | Index Only Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.040 | 0.032 | 0.048 | 0.033 | Index Only Scan | 0.033 | 23.077 | Index Only Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 166.330 | 165.396 | 167.264 | 0.034 | Index Only Scan | 167.163 | -0.499 | Index Only Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | fetch_medium | 2 | 15.440 | 14.276 | 16.604 | 0.040 | Bitmap Heap Scan, Bitmap Index Scan | 18.396 | -16.071 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | group_c200 | 2 | 414.257 | 409.613 | 418.901 | 0.047 | Index Only Scan | 417.244 | -0.716 | Index Only Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 1.607 | 1.583 | 1.631 | 0.092 | Index Only Scan | 1.635 | -1.743 | Index Only Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | is_null | 2 | 33.987 | 33.720 | 34.255 | 0.028 | Index Only Scan | 36.306 | -6.387 | Index Only Scan | False |
| scalar | 5000000 | btree | clean | default | 64MB | range_random | 2 | 1.962 | 1.682 | 2.243 | 0.067 | Index Only Scan | 1.542 | 27.311 | Index Only Scan | False |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 220.593 | 219.247 | 221.939 | 0.085 | Bitmap Heap Scan, Bitmap Index Scan | 224.435 | -1.712 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 1.867 | 1.580 | 2.154 | 0.064 | Index Only Scan | 1.945 | -4.010 | Index Only Scan | False |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 638.375 | 631.447 | 645.304 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan | 651.881 | -2.072 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 947.060 | 942.825 | 951.294 | 0.048 | Seq Scan | 956.909 | -1.029 | Seq Scan | False |
| scalar | 5000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 415.103 | 406.451 | 423.754 | 0.061 | Index Only Scan | 415.941 | -0.202 | Index Only Scan | False |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 17.548 | 15.296 | 19.800 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan | 18.405 | -4.659 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 1019.433 | 1005.215 | 1033.651 | 0.033 | Seq Scan | 1022.826 | -0.332 | Seq Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | and2 | 2 | 183.444 | 174.739 | 192.148 | 0.062 | Bitmap Heap Scan, Bitmap Index Scan | 178.657 | 2.679 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | and3 | 2 | 26.452 | 26.124 | 26.780 | 0.086 | Bitmap Heap Scan, Bitmap Index Scan | 28.892 | -8.447 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 108.290 | 66.852 | 149.728 | 0.063 | Bitmap Heap Scan, Bitmap Index Scan | 110.119 | -1.661 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 0.978 | 0.232 | 1.724 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan | 1.146 | -14.697 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 923.293 | 898.428 | 948.157 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan | 958.400 | -3.663 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | fetch_medium | 2 | 75.135 | 18.035 | 132.234 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan | 79.973 | -6.051 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | group_c200 | 2 | 1231.842 | 1145.311 | 1318.374 | 0.033 | Seq Scan | 1252.029 | -1.612 | Seq Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 247.934 | 222.330 | 273.538 | 0.143 | Bitmap Heap Scan, Bitmap Index Scan | 255.224 | -2.856 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | is_null | 2 | 734.407 | 681.023 | 787.791 | 0.050 | Seq Scan | 746.335 | -1.598 | Seq Scan | False |
| scalar | 5000000 | gin | clean | default | 64MB | range_random | 2 | 379.028 | 272.869 | 485.186 | 0.062 | Bitmap Heap Scan, Bitmap Index Scan | 381.769 | -0.718 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 178.312 | 173.085 | 183.538 | 0.076 | Bitmap Heap Scan, Bitmap Index Scan | 177.716 | 0.335 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 96.131 | 62.319 | 129.944 | 0.072 | Bitmap Heap Scan, Bitmap Index Scan | 97.884 | -1.790 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 809.815 | 785.128 | 834.502 | 0.076 | Bitmap Heap Scan, Bitmap Index Scan | 802.671 | 0.890 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 942.106 | 872.903 | 1011.310 | 0.072 | Seq Scan | 934.408 | 0.824 | Seq Scan | False |
| scalar | 5000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 993.291 | 989.114 | 997.467 | 0.057 | Seq Scan | 1016.574 | -2.290 | Seq Scan | False |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.160 | 0.158 | 0.161 | 0.036 | LionCount | 0.152 | 4.934 | LionCount | False |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 21.547 | 21.127 | 21.966 | 0.034 | LionCount | 22.009 | -2.101 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | and2 | 2 | 1.792 | 1.771 | 1.812 | 0.048 | LionCount | 1.643 | 9.005 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | and3 | 2 | 1.294 | 1.266 | 1.322 | 0.059 | LionCount | 1.135 | 14.059 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.119 | 0.113 | 0.126 | 0.055 | LionCount | 0.103 | 16.585 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.025 | 0.019 | 0.031 | 0.081 | LionCount | 0.025 | 0.000 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 1.811 | 1.780 | 1.842 | 0.037 | LionCount | 2.034 | -10.942 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 15.664 | 14.038 | 17.289 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan | 15.239 | 2.789 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring | clean | default | 64MB | group_c200 | 2 | 19.688 | 17.559 | 21.817 | 0.036 | LionCount | 18.645 | 5.594 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 0.869 | 0.637 | 1.101 | 0.153 | LionCount | 2.454 | -64.588 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | is_null | 2 | 0.418 | 0.417 | 0.420 | 0.032 | LionCount | 0.452 | -7.309 | LionCount | False |
| scalar | 5000000 | roaring | clean | default | 64MB | range_random | 2 | 705.309 | 685.367 | 725.251 | 0.050 | Seq Scan | 713.062 | -1.087 | Seq Scan | False |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 1.896 | 1.776 | 2.015 | 0.045 | LionCount | 1.861 | 1.826 | LionCount | False |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.098 | 0.098 | 0.098 | 0.040 | LionCount | 0.199 | -50.877 | LionCount | False |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 1.802 | 1.781 | 1.823 | 0.037 | LionCount | 1.981 | -9.013 | LionCount | False |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 18.128 | 17.299 | 18.958 | 0.032 | LionCount | 18.465 | -1.825 | LionCount | False |
| scalar | 5000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 17.846 | 17.320 | 18.373 | 0.033 | LionCount | 20.389 | -12.470 | LionCount | False |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 19.255 | 15.007 | 23.503 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan | 17.528 | 9.853 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 1030.082 | 1023.865 | 1036.299 | 0.045 | Seq Scan | 1068.235 | -3.572 | Seq Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 39.047 | 38.829 | 39.265 | 0.067 | Bitmap Heap Scan, Bitmap Index Scan | 17.598 | 121.889 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 17.564 | 13.453 | 21.675 | 0.074 | Bitmap Heap Scan, Bitmap Index Scan | 19.634 | -10.541 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 114.026 | 19.196 | 208.856 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan | 114.758 | -0.638 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 2.297 | 1.837 | 2.757 | 0.073 | Bitmap Heap Scan, Bitmap Index Scan | 2.387 | -3.791 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 624.255 | 484.206 | 764.303 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan | 617.308 | 1.125 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 129.808 | 25.562 | 234.053 | 0.075 | Bitmap Heap Scan, Bitmap Index Scan | 127.818 | 1.557 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 971.446 | 949.590 | 993.302 | 0.047 | Seq Scan | 976.453 | -0.513 | Seq Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 217.441 | 214.510 | 220.373 | 0.098 | Bitmap Heap Scan, Bitmap Index Scan | 224.831 | -3.286 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 511.104 | 448.179 | 574.030 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan | 546.985 | -6.560 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 508.948 | 493.485 | 524.411 | 0.041 | Seq Scan | 511.040 | -0.409 | Seq Scan | False |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 38.430 | 37.491 | 39.368 | 0.068 | Bitmap Heap Scan, Bitmap Index Scan | 39.472 | -2.642 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 139.907 | 64.461 | 215.354 | 0.067 | Bitmap Heap Scan, Bitmap Index Scan | 138.925 | 0.707 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 676.956 | 629.600 | 724.312 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan | 683.163 | -0.909 | Bitmap Heap Scan, Bitmap Index Scan | False |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 859.762 | 788.038 | 931.485 | 0.049 | Seq Scan | 927.783 | -7.332 | Seq Scan | False |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 938.481 | 929.948 | 947.014 | 0.059 | Seq Scan | 960.958 | -2.339 | Seq Scan | False |

