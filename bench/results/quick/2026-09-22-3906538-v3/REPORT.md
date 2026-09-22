# Quick index progress benchmark

Commit `bfae6e16d922d5b36555a7641fb3418a8f0017e3`. Profile `quick-v3`. Completed in **262.1 seconds**, with **168 exact-result checks** and **336 timings**. All checks pass.

Only B-tree, GIN, roaring, and roaring_bitmap are timed. Documents use GIN and roaring; B-tree has no matching array/full-text index here. Sequential scans supply untimed correctness references. 2 timing rounds, 1 warmup(s), one index build; warm cache, single-client queries, parallel query/JIT off. Small-sample medians/min/max are progress signals, not confidence intervals or production capacity estimates. Build and maintenance times are single observations.

Read timings are EXPLAIN execution time; planning is separate. Positive change means slower than the baseline. Inspect plan changes and repeat suspicious differences, especially sub-millisecond results. Different commits/extension binaries are expected; incompatible workloads/server settings are rejected.

## Build and size

| Suite | Rows | Family | Build ms | Indexes MiB |
| --- | --- | --- | --- | --- |
| scalar | 1000000 | gin | 733.950 | 18.945 |
| scalar | 1000000 | roaring | 2805.092 | 22.305 |
| scalar | 1000000 | btree | 1098.546 | 33.516 |
| scalar | 5000000 | gin | 3304.938 | 62.703 |
| scalar | 5000000 | btree | 5800.677 | 166.812 |
| scalar | 5000000 | roaring | 15776.295 | 94.008 |
| documents | 200000 | gin | 323.097 | 6.375 |
| documents | 200000 | roaring | 1785.319 | 7.672 |

## Maintenance

| Rows | Family | Operation | Elapsed ms | WAL MiB |
| --- | --- | --- | --- | --- |
| 1000000 | gin | insert | 68.435 | 9.234 |
| 1000000 | gin | indexed_update | 108.996 | 12.547 |
| 1000000 | gin | vacuum | 150.770 | 14.830 |
| 1000000 | gin | delete | 135.859 | 71.172 |
| 1000000 | gin | vacuum_after_delete | 297.938 | 82.543 |
| 1000000 | gin | mid_insert | 60.005 | 17.349 |
| 1000000 | roaring | insert | 353.883 | 39.167 |
| 1000000 | roaring | indexed_update | 444.432 | 52.866 |
| 1000000 | roaring | vacuum | 109.953 | 4.204 |
| 1000000 | roaring | delete | 131.676 | 71.172 |
| 1000000 | roaring | vacuum_after_delete | 377.255 | 89.547 |
| 1000000 | roaring | mid_insert | 435.295 | 66.321 |
| 100000 | roaring | churn_update | 507.824 | 36.341 |
| 100000 | roaring | churn_vacuum | 54.686 | 6.399 |
| 100000 | roaring | churn_update | 501.037 | 36.321 |
| 100000 | roaring | churn_vacuum | 61.950 | 6.369 |
| 1000000 | btree | insert | 89.038 | 14.041 |
| 1000000 | btree | indexed_update | 203.121 | 18.106 |
| 1000000 | btree | vacuum | 114.412 | 4.222 |
| 1000000 | btree | delete | 134.950 | 71.172 |
| 1000000 | btree | vacuum_after_delete | 316.092 | 103.394 |
| 1000000 | btree | mid_insert | 120.171 | 24.814 |
| 100000 | btree | churn_update | 229.912 | 25.137 |
| 100000 | btree | churn_vacuum | 28.159 | 2.655 |
| 100000 | btree | churn_update | 207.713 | 25.108 |
| 100000 | btree | churn_vacuum | 33.708 | 2.680 |
| 5000000 | gin | insert | 297.085 | 46.051 |
| 5000000 | gin | indexed_update | 749.743 | 62.569 |
| 5000000 | gin | vacuum | 546.879 | 56.252 |
| 5000000 | gin | delete | 1247.140 | 355.837 |
| 5000000 | gin | vacuum_after_delete | 2692.457 | 410.241 |
| 5000000 | gin | mid_insert | 306.858 | 86.409 |
| 5000000 | btree | insert | 530.137 | 59.688 |
| 5000000 | btree | indexed_update | 1338.280 | 77.017 |
| 5000000 | btree | vacuum | 438.142 | 20.735 |
| 5000000 | btree | delete | 1170.791 | 355.837 |
| 5000000 | btree | vacuum_after_delete | 2862.530 | 515.698 |
| 5000000 | btree | mid_insert | 559.607 | 110.190 |
| 5000000 | roaring | insert | 1764.995 | 191.936 |
| 5000000 | roaring | indexed_update | 2379.362 | 257.978 |
| 5000000 | roaring | vacuum | 431.873 | 20.751 |
| 5000000 | roaring | delete | 1219.859 | 355.837 |
| 5000000 | roaring | vacuum_after_delete | 3258.788 | 443.047 |
| 5000000 | roaring | mid_insert | 2099.847 | 357.275 |

## Query timings and baseline comparison

| Suite | Rows | Variant | State | Planner | Memory | Query | n | Median ms | Min ms | Max ms | Planning ms | Plan | Baseline ms | Change % | Baseline plan | Plan changed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| documents | 200000 | gin | clean | default | 64MB | array_and | 2 | 0.875 | 0.874 | 0.876 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | array_common | 2 | 3.872 | 3.869 | 3.875 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | array_or | 2 | 6.581 | 6.546 | 6.615 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_and | 2 | 1.016 | 1.009 | 1.023 | 0.046 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_common | 2 | 30.075 | 29.684 | 30.466 | 0.044 | Seq Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_fetch | 2 | 4.247 | 4.209 | 4.285 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_phrase | 2 | 6.874 | 6.828 | 6.920 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_prefix | 2 | 1.591 | 1.573 | 1.610 | 0.068 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_and | 2 | 0.141 | 0.126 | 0.156 | 0.074 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_common | 2 | 0.025 | 0.023 | 0.026 | 0.094 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_or | 2 | 0.260 | 0.240 | 0.279 | 0.091 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_and | 2 | 0.147 | 0.139 | 0.154 | 0.063 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_common | 2 | 0.030 | 0.030 | 0.030 | 0.066 | LionCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_fetch | 2 | 3.984 | 3.863 | 4.105 | 0.055 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_phrase | 2 | 35.049 | 34.276 | 35.822 | 0.050 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_prefix | 2 | 24.926 | 24.690 | 25.162 | 0.068 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_and | 2 | 0.227 | 0.227 | 0.227 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_common | 2 | 3.703 | 3.555 | 3.851 | 0.058 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_or | 2 | 5.665 | 5.244 | 6.086 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_and | 2 | 0.216 | 0.202 | 0.229 | 0.045 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_common | 2 | 31.783 | 30.792 | 32.774 | 0.046 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_fetch | 2 | 4.521 | 4.438 | 4.605 | 0.055 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_phrase | 2 | 33.225 | 31.144 | 35.307 | 0.061 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_prefix | 2 | 30.087 | 27.953 | 32.221 | 0.055 | Seq Scan |  |  |  |  |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.417 | 0.409 | 0.425 | 0.036 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 97.972 | 97.235 | 98.710 | 0.037 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | and2 | 2 | 2.649 | 2.599 | 2.699 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | and3 | 2 | 2.579 | 2.563 | 2.595 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | and3_selective | 2 | 0.327 | 0.318 | 0.335 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 0.325 | 0.319 | 0.331 | 0.033 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.020 | 0.019 | 0.021 | 0.034 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 31.120 | 30.395 | 31.845 | 0.048 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | fetch_medium | 2 | 2.808 | 2.705 | 2.910 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | group_c200 | 2 | 80.019 | 79.631 | 80.407 | 0.033 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 0.320 | 0.314 | 0.325 | 0.084 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | is_null | 2 | 6.670 | 6.550 | 6.790 | 0.029 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | range_random | 2 | 0.312 | 0.302 | 0.322 | 0.065 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 3.314 | 3.250 | 3.378 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.348 | 0.324 | 0.371 | 0.036 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 67.075 | 66.667 | 67.482 | 0.035 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 144.411 | 140.295 | 148.526 | 0.036 | Seq Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 85.495 | 82.396 | 88.593 | 0.035 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 3.545 | 3.458 | 3.632 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 149.303 | 146.304 | 152.302 | 0.032 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | and2 | 2 | 34.171 | 33.935 | 34.407 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | and3 | 2 | 5.409 | 5.266 | 5.553 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | and3_selective | 2 | 0.575 | 0.571 | 0.580 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 3.365 | 3.047 | 3.682 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 0.058 | 0.056 | 0.061 | 0.091 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 105.759 | 104.194 | 107.324 | 0.034 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | fetch_medium | 2 | 3.429 | 3.307 | 3.552 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | group_c200 | 2 | 143.755 | 140.164 | 147.345 | 0.031 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 3.373 | 2.931 | 3.814 | 0.107 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | is_null | 2 | 56.645 | 55.138 | 58.153 | 0.025 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | range_random | 2 | 38.892 | 37.644 | 40.140 | 0.045 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 33.766 | 33.323 | 34.208 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 2.981 | 2.807 | 3.154 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 101.964 | 99.652 | 104.275 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 143.406 | 140.584 | 146.227 | 0.053 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 145.434 | 141.467 | 149.401 | 0.038 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.115 | 0.105 | 0.126 | 0.038 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 11.654 | 10.265 | 13.042 | 0.038 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | and2 | 2 | 0.384 | 0.361 | 0.406 | 0.043 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | and3 | 2 | 0.289 | 0.287 | 0.292 | 0.051 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | and3_selective | 2 | 0.340 | 0.337 | 0.342 | 0.061 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.029 | 0.028 | 0.029 | 0.038 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.015 | 0.012 | 0.017 | 0.048 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 0.362 | 0.353 | 0.371 | 0.036 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 3.117 | 2.948 | 3.287 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | group_c200 | 2 | 3.732 | 3.727 | 3.738 | 0.031 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 0.158 | 0.157 | 0.159 | 0.117 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | is_null | 2 | 0.090 | 0.090 | 0.091 | 0.032 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | range_random | 2 | 110.094 | 110.069 | 110.119 | 0.036 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 0.364 | 0.356 | 0.373 | 0.044 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.029 | 0.028 | 0.029 | 0.037 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 0.361 | 0.361 | 0.362 | 0.035 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 3.657 | 3.605 | 3.709 | 0.030 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 3.720 | 3.691 | 3.748 | 0.032 | LionCount |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 2.788 | 2.693 | 2.884 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 146.770 | 146.000 | 147.540 | 0.037 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 6.631 | 6.589 | 6.673 | 0.040 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 2.370 | 2.333 | 2.406 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and3_selective | 2 | 0.328 | 0.319 | 0.337 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 2.512 | 2.511 | 2.513 | 0.032 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 0.049 | 0.049 | 0.049 | 0.032 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 61.703 | 60.763 | 62.644 | 0.039 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 3.010 | 2.822 | 3.198 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 148.411 | 146.301 | 150.520 | 0.030 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 2.950 | 2.944 | 2.955 | 0.074 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 23.091 | 22.936 | 23.247 | 0.026 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 53.938 | 53.480 | 54.397 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 6.628 | 6.597 | 6.659 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 2.767 | 2.691 | 2.843 | 0.033 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 64.792 | 62.318 | 67.267 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 143.805 | 140.763 | 146.846 | 0.030 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 140.080 | 139.937 | 140.223 | 0.043 | Seq Scan |  |  |  |  |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 2.131 | 2.099 | 2.163 | 0.060 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 547.844 | 541.991 | 553.698 | 0.050 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | and2 | 2 | 16.184 | 15.961 | 16.408 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | and3 | 2 | 13.125 | 12.926 | 13.324 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | and3_selective | 2 | 1.570 | 1.546 | 1.594 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 1.551 | 1.522 | 1.580 | 0.045 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.033 | 0.032 | 0.033 | 0.033 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 160.649 | 156.072 | 165.226 | 0.031 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | fetch_medium | 2 | 16.142 | 14.081 | 18.202 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | group_c200 | 2 | 407.125 | 401.942 | 412.307 | 0.038 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 1.602 | 1.601 | 1.603 | 0.076 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | is_null | 2 | 36.355 | 33.864 | 38.847 | 0.028 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | range_random | 2 | 1.570 | 1.567 | 1.573 | 0.054 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 218.627 | 217.098 | 220.157 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 1.898 | 1.575 | 2.221 | 0.062 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 594.919 | 517.421 | 672.417 | 0.068 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 827.091 | 800.846 | 853.337 | 0.051 | Seq Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 410.975 | 405.706 | 416.244 | 0.053 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 21.910 | 21.793 | 22.027 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 1572.657 | 1562.331 | 1582.984 | 0.045 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | and2 | 2 | 189.146 | 179.904 | 198.388 | 0.074 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | and3 | 2 | 30.459 | 26.896 | 34.021 | 0.066 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | and3_selective | 2 | 2.800 | 2.795 | 2.804 | 0.081 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 17.144 | 16.810 | 17.478 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 0.273 | 0.211 | 0.334 | 0.060 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 958.696 | 906.424 | 1010.968 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | fetch_medium | 2 | 25.991 | 20.734 | 31.248 | 0.061 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | group_c200 | 2 | 1261.985 | 1237.284 | 1286.685 | 0.045 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 167.879 | 129.428 | 206.330 | 0.094 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | is_null | 2 | 629.389 | 561.196 | 697.582 | 0.032 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | range_random | 2 | 403.992 | 262.415 | 545.569 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 194.300 | 180.978 | 207.623 | 0.074 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 185.436 | 125.005 | 245.867 | 0.067 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 757.998 | 693.277 | 822.719 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 911.890 | 859.176 | 964.605 | 0.050 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 982.651 | 975.970 | 989.333 | 0.056 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.613 | 0.613 | 0.614 | 0.045 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 54.291 | 53.678 | 54.905 | 0.033 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | and2 | 2 | 1.804 | 1.791 | 1.816 | 0.041 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | and3 | 2 | 1.327 | 1.282 | 1.371 | 0.067 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | and3_selective | 2 | 1.650 | 1.611 | 1.689 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.097 | 0.097 | 0.097 | 0.037 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.022 | 0.018 | 0.027 | 0.057 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 1.792 | 1.763 | 1.820 | 0.037 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 16.044 | 14.414 | 17.675 | 0.046 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | group_c200 | 2 | 17.668 | 17.569 | 17.767 | 0.035 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 0.705 | 0.687 | 0.723 | 0.122 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | is_null | 2 | 0.459 | 0.407 | 0.510 | 0.036 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | range_random | 2 | 699.534 | 690.683 | 708.385 | 0.048 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 1.766 | 1.764 | 1.769 | 0.045 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.106 | 0.094 | 0.118 | 0.041 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 1.776 | 1.727 | 1.826 | 0.036 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 17.578 | 17.359 | 17.798 | 0.043 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 18.410 | 18.145 | 18.676 | 0.033 | LionCount |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 20.106 | 18.807 | 21.405 | 0.059 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 1586.481 | 1575.035 | 1597.927 | 0.044 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 92.716 | 36.761 | 148.671 | 0.066 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 13.277 | 11.797 | 14.756 | 0.061 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and3_selective | 2 | 1.648 | 1.578 | 1.718 | 0.060 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 115.392 | 19.264 | 211.519 | 0.060 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 2.346 | 2.110 | 2.582 | 0.059 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 709.736 | 699.573 | 719.900 | 0.030 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 18.558 | 18.037 | 19.079 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 965.923 | 819.246 | 1112.600 | 0.033 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 221.441 | 210.850 | 232.033 | 0.108 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 514.613 | 335.184 | 694.041 | 0.040 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 514.486 | 428.831 | 600.142 | 0.046 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 98.525 | 36.217 | 160.834 | 0.066 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 74.677 | 17.706 | 131.649 | 0.045 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 680.890 | 596.733 | 765.048 | 0.046 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 979.396 | 961.546 | 997.245 | 0.047 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 996.392 | 995.425 | 997.360 | 0.054 | Seq Scan |  |  |  |  |

