# Quick index progress benchmark

Commit `481f8769caf4b28ba228c865921fad0ca7cc80d9`. Profile `quick-v2`. Completed in **231.4 seconds**, with **160 exact-result checks** and **320 timings**. All checks pass.

Only B-tree, GIN, roaring, and roaring_bitmap are timed. Documents use GIN and roaring; B-tree has no matching array/full-text index here. Sequential scans supply untimed correctness references. 2 timing rounds, 1 warmup(s), one index build; warm cache, single-client queries, parallel query/JIT off. Small-sample medians/min/max are progress signals, not confidence intervals or production capacity estimates. Build and maintenance times are single observations.

Read timings are EXPLAIN execution time; planning is separate. Positive change means slower than the baseline. Inspect plan changes and repeat suspicious differences, especially sub-millisecond results. Different commits/extension binaries are expected; incompatible workloads/server settings are rejected.

## Build and size

| Suite | Rows | Family | Build ms | Indexes MiB |
| --- | --- | --- | --- | --- |
| scalar | 1000000 | gin | 741.683 | 18.945 |
| scalar | 1000000 | roaring | 3277.060 | 22.617 |
| scalar | 1000000 | btree | 1062.214 | 33.516 |
| scalar | 5000000 | roaring | 19013.154 | 91.406 |
| scalar | 5000000 | btree | 5838.712 | 166.812 |
| scalar | 5000000 | gin | 3400.628 | 62.703 |
| documents | 200000 | roaring | 1606.244 | 8.055 |
| documents | 200000 | gin | 346.948 | 6.375 |

## Maintenance

| Rows | Family | Operation | Elapsed ms | WAL MiB |
| --- | --- | --- | --- | --- |
| 1000000 | gin | insert | 60.477 | 9.234 |
| 1000000 | gin | indexed_update | 113.493 | 12.547 |
| 1000000 | gin | vacuum | 149.666 | 14.830 |
| 1000000 | roaring | insert | 301.255 | 39.023 |
| 1000000 | roaring | indexed_update | 359.709 | 52.405 |
| 1000000 | roaring | vacuum | 111.803 | 4.219 |
| 1000000 | btree | insert | 87.018 | 14.041 |
| 1000000 | btree | indexed_update | 194.860 | 18.106 |
| 1000000 | btree | vacuum | 120.507 | 4.217 |
| 5000000 | roaring | insert | 1600.515 | 208.808 |
| 5000000 | roaring | indexed_update | 2020.713 | 271.021 |
| 5000000 | roaring | vacuum | 429.944 | 20.729 |
| 5000000 | btree | insert | 473.005 | 59.688 |
| 5000000 | btree | indexed_update | 1111.647 | 77.010 |
| 5000000 | btree | vacuum | 421.067 | 20.733 |
| 5000000 | gin | insert | 320.169 | 46.051 |
| 5000000 | gin | indexed_update | 777.381 | 62.567 |
| 5000000 | gin | vacuum | 564.815 | 56.255 |

## Query timings and baseline comparison

| Suite | Rows | Variant | State | Planner | Memory | Query | n | Median ms | Min ms | Max ms | Planning ms | Plan | Baseline ms | Change % | Baseline plan | Plan changed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| documents | 200000 | gin | clean | default | 64MB | array_and | 2 | 0.956 | 0.941 | 0.972 | 0.067 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | array_common | 2 | 4.909 | 3.852 | 5.965 | 0.075 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | array_or | 2 | 8.040 | 7.550 | 8.531 | 0.071 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_and | 2 | 1.069 | 1.019 | 1.119 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_common | 2 | 35.243 | 34.011 | 36.476 | 0.050 | Seq Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_fetch | 2 | 4.903 | 4.436 | 5.370 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_phrase | 2 | 7.893 | 7.414 | 8.373 | 0.062 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_prefix | 2 | 1.720 | 1.681 | 1.760 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_and | 2 | 0.115 | 0.112 | 0.118 | 0.090 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_common | 2 | 0.025 | 0.024 | 0.025 | 0.081 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_or | 2 | 0.277 | 0.232 | 0.321 | 0.124 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_and | 2 | 0.116 | 0.112 | 0.120 | 0.081 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_common | 2 | 0.030 | 0.030 | 0.030 | 0.066 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_fetch | 2 | 5.700 | 5.452 | 5.947 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_phrase | 2 | 46.614 | 45.359 | 47.869 | 0.059 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_prefix | 2 | 29.463 | 28.111 | 30.816 | 0.061 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_and | 2 | 0.189 | 0.184 | 0.193 | 0.064 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_common | 2 | 3.478 | 3.335 | 3.622 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_or | 2 | 5.773 | 5.563 | 5.983 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_and | 2 | 0.181 | 0.181 | 0.182 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_common | 2 | 31.383 | 31.375 | 31.390 | 0.045 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_fetch | 2 | 4.203 | 4.085 | 4.321 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_phrase | 2 | 36.467 | 35.115 | 37.819 | 0.044 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_prefix | 2 | 26.913 | 26.624 | 27.203 | 0.049 | Seq Scan |  |  |  |  |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.355 | 0.336 | 0.374 | 0.041 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 86.829 | 85.525 | 88.134 | 0.035 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | and2 | 2 | 2.764 | 2.742 | 2.785 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | and3 | 2 | 2.566 | 2.493 | 2.638 | 0.066 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 0.318 | 0.314 | 0.322 | 0.033 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.021 | 0.020 | 0.023 | 0.036 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 32.336 | 32.016 | 32.656 | 0.033 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | fetch_medium | 2 | 3.578 | 2.872 | 4.283 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | group_c200 | 2 | 85.142 | 84.837 | 85.447 | 0.034 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 0.325 | 0.315 | 0.334 | 0.096 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | is_null | 2 | 7.758 | 7.494 | 8.021 | 0.029 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | range_random | 2 | 0.298 | 0.298 | 0.299 | 0.051 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 3.024 | 2.892 | 3.157 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.348 | 0.348 | 0.348 | 0.061 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 72.782 | 72.753 | 72.812 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 142.221 | 139.901 | 144.542 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 78.750 | 77.654 | 79.846 | 0.056 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 3.128 | 2.800 | 3.456 | 0.080 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 150.645 | 148.155 | 153.136 | 0.043 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | and2 | 2 | 33.469 | 33.315 | 33.624 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | and3 | 2 | 5.069 | 5.026 | 5.112 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 2.827 | 2.715 | 2.939 | 0.035 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 0.051 | 0.046 | 0.055 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 93.011 | 87.008 | 99.013 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | fetch_medium | 2 | 3.228 | 2.972 | 3.485 | 0.040 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | group_c200 | 2 | 142.057 | 142.045 | 142.069 | 0.041 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 3.417 | 3.272 | 3.562 | 0.108 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | is_null | 2 | 56.189 | 54.379 | 58.000 | 0.025 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | range_random | 2 | 38.532 | 38.199 | 38.866 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 34.035 | 33.884 | 34.187 | 0.058 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 3.010 | 2.980 | 3.040 | 0.045 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 88.222 | 86.763 | 89.682 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 146.727 | 143.998 | 149.456 | 0.031 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 141.812 | 140.332 | 143.293 | 0.045 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.049 | 0.041 | 0.057 | 0.054 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 4.463 | 4.358 | 4.569 | 0.032 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | and2 | 2 | 0.357 | 0.339 | 0.375 | 0.042 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | and3 | 2 | 0.295 | 0.275 | 0.315 | 0.065 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.028 | 0.027 | 0.030 | 0.115 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.017 | 0.013 | 0.020 | 0.037 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 0.408 | 0.400 | 0.416 | 0.035 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 3.029 | 2.693 | 3.364 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | group_c200 | 2 | 3.874 | 3.712 | 4.036 | 0.033 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 0.577 | 0.557 | 0.596 | 0.111 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | is_null | 2 | 0.114 | 0.099 | 0.129 | 0.124 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | range_random | 2 | 119.384 | 114.710 | 124.058 | 0.033 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 0.352 | 0.350 | 0.355 | 0.038 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.030 | 0.027 | 0.034 | 0.034 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 0.398 | 0.391 | 0.404 | 0.034 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 3.699 | 3.698 | 3.699 | 0.032 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 3.890 | 3.873 | 3.907 | 0.033 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 3.051 | 2.983 | 3.120 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 145.240 | 141.333 | 149.148 | 0.031 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 6.678 | 6.487 | 6.869 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 2.340 | 2.295 | 2.385 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 2.635 | 2.514 | 2.757 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 0.048 | 0.045 | 0.050 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 62.758 | 61.945 | 63.571 | 0.059 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 2.806 | 2.776 | 2.837 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 142.688 | 142.582 | 142.794 | 0.032 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 3.386 | 3.343 | 3.430 | 0.083 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 26.041 | 24.074 | 28.009 | 0.027 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 55.235 | 54.818 | 55.652 | 0.036 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 3.155 | 3.101 | 3.208 | 0.064 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 2.782 | 2.471 | 3.094 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 66.356 | 62.249 | 70.464 | 0.034 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 142.659 | 142.251 | 143.067 | 0.034 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 139.688 | 137.891 | 141.485 | 0.057 | Seq Scan |  |  |  |  |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 1.773 | 1.666 | 1.880 | 0.048 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 464.946 | 457.902 | 471.990 | 0.036 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | and2 | 2 | 16.193 | 14.912 | 17.474 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | and3 | 2 | 13.888 | 13.317 | 14.459 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 1.530 | 1.519 | 1.540 | 0.034 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.033 | 0.032 | 0.033 | 0.033 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 167.163 | 162.028 | 172.299 | 0.034 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | fetch_medium | 2 | 18.396 | 17.581 | 19.212 | 0.046 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | group_c200 | 2 | 417.244 | 410.947 | 423.541 | 0.048 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 1.635 | 1.574 | 1.697 | 0.089 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | is_null | 2 | 36.306 | 33.195 | 39.418 | 0.030 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | range_random | 2 | 1.542 | 1.531 | 1.552 | 0.068 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 224.435 | 223.365 | 225.504 | 0.116 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 1.945 | 1.624 | 2.266 | 0.058 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 651.881 | 648.248 | 655.515 | 0.045 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 956.909 | 956.832 | 956.985 | 0.052 | Seq Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 415.941 | 410.562 | 421.320 | 0.061 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 18.405 | 17.285 | 19.526 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 1022.826 | 1012.990 | 1032.662 | 0.036 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | and2 | 2 | 178.657 | 173.471 | 183.844 | 0.060 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | and3 | 2 | 28.892 | 27.402 | 30.383 | 0.087 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 110.119 | 66.898 | 153.340 | 0.064 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 1.146 | 0.235 | 2.058 | 0.059 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 958.400 | 938.151 | 978.649 | 0.063 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | fetch_medium | 2 | 79.973 | 18.301 | 141.646 | 0.046 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | group_c200 | 2 | 1252.029 | 1156.258 | 1347.800 | 0.040 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 255.224 | 220.734 | 289.714 | 0.131 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | is_null | 2 | 746.335 | 677.900 | 814.770 | 0.066 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | range_random | 2 | 381.769 | 263.161 | 500.377 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 177.716 | 171.714 | 183.717 | 0.073 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 97.884 | 67.586 | 128.182 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 802.671 | 775.020 | 830.322 | 0.058 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 934.408 | 870.921 | 997.895 | 0.061 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 1016.574 | 1010.480 | 1022.668 | 0.054 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.152 | 0.147 | 0.157 | 0.035 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 22.009 | 21.934 | 22.084 | 0.031 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | and2 | 2 | 1.643 | 1.628 | 1.659 | 0.042 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | and3 | 2 | 1.135 | 1.102 | 1.167 | 0.046 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.103 | 0.098 | 0.107 | 0.050 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.025 | 0.022 | 0.028 | 0.050 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 2.034 | 1.980 | 2.087 | 0.035 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 15.239 | 13.209 | 17.268 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | group_c200 | 2 | 18.645 | 17.843 | 19.447 | 0.032 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 2.454 | 2.437 | 2.471 | 0.115 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | is_null | 2 | 0.452 | 0.443 | 0.460 | 0.034 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | range_random | 2 | 713.062 | 708.336 | 717.789 | 0.036 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 1.861 | 1.759 | 1.964 | 0.042 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.199 | 0.112 | 0.287 | 0.036 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 1.981 | 1.974 | 1.987 | 0.035 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 18.465 | 18.332 | 18.599 | 0.033 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 20.389 | 18.076 | 22.702 | 0.054 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 17.528 | 15.703 | 19.353 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 1068.235 | 1037.264 | 1099.206 | 0.044 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 17.598 | 15.888 | 19.307 | 0.068 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 19.634 | 12.089 | 27.178 | 0.070 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 114.758 | 19.176 | 210.341 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 2.387 | 1.757 | 3.018 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 617.308 | 490.353 | 744.262 | 0.046 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 127.818 | 18.667 | 236.969 | 0.061 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 976.453 | 972.048 | 980.858 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 224.831 | 221.285 | 228.376 | 0.090 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 546.985 | 449.546 | 644.424 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 511.040 | 483.098 | 538.983 | 0.038 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 39.472 | 37.443 | 41.502 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 138.925 | 69.504 | 208.347 | 0.058 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 683.163 | 648.265 | 718.062 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 927.783 | 841.509 | 1014.056 | 0.049 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 960.958 | 955.907 | 966.010 | 0.074 | Seq Scan |  |  |  |  |

