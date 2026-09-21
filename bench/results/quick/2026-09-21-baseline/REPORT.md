# Quick index progress benchmark

Commit `0e21a9754f4a63f1c0dd257d938a4d46d49a4990`. Profile `quick-v1`. Completed in **28.8 seconds**, with **120 exact-result checks** and **360 timings**. All checks pass.

Only B-tree, GIN, roaring, and roaring_bitmap are timed. Documents use GIN and roaring; B-tree has no matching array/full-text index here. Sequential scans supply untimed correctness references. Three rounds by default, one warmup, one index build; warm cache, single-client queries, parallel query/JIT off. Small-sample medians/min/max are progress signals, not confidence intervals or production capacity estimates. Build and maintenance times are single observations.

Read timings are EXPLAIN execution time; planning is separate. Positive change means slower than the baseline. Inspect plan changes and repeat suspicious differences, especially sub-millisecond results. Different commits/extension binaries are expected; incompatible workloads/server settings are rejected.

## Build and size

| Suite | Rows | Family | Build ms | Indexes MiB |
| --- | --- | --- | --- | --- |
| scalar | 500000 | gin | 1234.720 | 32.516 |
| scalar | 500000 | roaring | 3351.929 | 44.242 |
| scalar | 500000 | btree | 895.021 | 33.992 |
| documents | 50000 | roaring | 427.600 | 3.711 |
| documents | 50000 | gin | 136.488 | 2.656 |

## Maintenance

| Family | Operation | Elapsed ms | WAL MiB |
| --- | --- | --- | --- |
| gin | insert | 46.455 | 6.926 |
| gin | indexed_update | 67.545 | 8.592 |
| gin | vacuum | 145.218 | 19.829 |
| roaring | insert | 272.023 | 45.465 |
| roaring | indexed_update | 321.848 | 57.581 |
| roaring | vacuum | 94.864 | 2.135 |
| btree | insert | 110.173 | 18.928 |
| btree | indexed_update | 130.227 | 21.257 |
| btree | vacuum | 97.527 | 2.144 |

## Query timings and baseline comparison

| Suite | Rows | Variant | State | Planner | Memory | Query | n | Median ms | Min ms | Max ms | Planning ms | Plan | Baseline ms | Change % | Baseline plan | Plan changed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| documents | 50000 | gin | clean | default | 64MB | array_and | 3 | 0.233 | 0.232 | 0.258 | 0.068 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | gin | clean | default | 64MB | array_common | 3 | 0.932 | 0.926 | 0.944 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | gin | clean | default | 64MB | array_or | 3 | 1.839 | 1.709 | 1.924 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | gin | clean | default | 64MB | ts_and | 3 | 0.269 | 0.268 | 0.286 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | gin | clean | default | 64MB | ts_common | 3 | 7.601 | 7.109 | 7.896 | 0.047 | Seq Scan |  |  |  |  |
| documents | 50000 | gin | clean | default | 64MB | ts_fetch | 3 | 1.095 | 1.043 | 1.109 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | gin | clean | default | 64MB | ts_phrase | 3 | 1.803 | 1.754 | 1.809 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | gin | clean | default | 64MB | ts_prefix | 3 | 0.486 | 0.367 | 0.649 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | array_and | 3 | 0.037 | 0.037 | 0.062 | 0.080 | RoaringCount |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | array_common | 3 | 0.014 | 0.013 | 0.016 | 0.079 | RoaringCount |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | array_or | 3 | 0.067 | 0.067 | 0.069 | 0.076 | RoaringCount |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | ts_and | 3 | 0.036 | 0.035 | 0.038 | 0.064 | RoaringCount |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | ts_common | 3 | 0.016 | 0.014 | 0.017 | 0.066 | RoaringCount |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | ts_fetch | 3 | 0.961 | 0.921 | 1.087 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | ts_phrase | 3 | 18.684 | 18.388 | 20.031 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring | clean | default | 64MB | ts_prefix | 3 | 16.295 | 15.499 | 16.585 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | array_and | 3 | 0.083 | 0.064 | 0.099 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | array_common | 3 | 0.794 | 0.793 | 0.853 | 0.069 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | array_or | 3 | 1.289 | 1.272 | 1.298 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | ts_and | 3 | 0.063 | 0.062 | 0.063 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | ts_common | 3 | 6.967 | 6.964 | 7.121 | 0.045 | Seq Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | ts_fetch | 3 | 0.938 | 0.898 | 0.977 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | ts_phrase | 3 | 17.428 | 17.286 | 18.206 | 0.045 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 50000 | roaring_bitmap | clean | default | 64MB | ts_prefix | 3 | 15.540 | 14.863 | 15.719 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | after_maintenance | default | 64MB | and2 | 3 | 1.496 | 1.297 | 1.801 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 3 | 0.192 | 0.174 | 0.211 | 0.036 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | after_maintenance | default | 64MB | group_c200 | 3 | 43.399 | 41.767 | 43.826 | 0.038 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | after_maintenance | default | 64MB | is_null | 3 | 3.421 | 3.406 | 4.258 | 0.031 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | and2 | 3 | 1.457 | 1.249 | 1.688 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | and3 | 3 | 1.414 | 1.395 | 1.423 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | eq_c200_17 | 3 | 0.168 | 0.167 | 0.168 | 0.036 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | eq_c20k_123 | 3 | 0.019 | 0.019 | 0.020 | 0.035 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | eq_c2_0 | 3 | 15.558 | 15.259 | 15.620 | 0.035 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | fetch_medium | 3 | 1.519 | 1.348 | 1.708 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | group_c200 | 3 | 41.846 | 40.030 | 42.233 | 0.039 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | group_filtered | 3 | 1.626 | 1.476 | 1.766 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | in_c20k_100 | 3 | 0.175 | 0.170 | 0.179 | 0.082 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | is_null | 3 | 3.637 | 3.342 | 3.710 | 0.030 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | ordered_limit | 3 | 0.018 | 0.018 | 0.019 | 0.054 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | clean | default | 64MB | range_random | 3 | 0.166 | 0.161 | 0.176 | 0.053 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | dirty_scattered | default | 64MB | and2 | 3 | 1.648 | 1.299 | 1.696 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 3 | 0.180 | 0.169 | 0.221 | 0.064 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 3 | 35.424 | 33.756 | 36.317 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | dirty_scattered | default | 64MB | fetch_medium | 3 | 1.570 | 1.333 | 2.505 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | btree | dirty_scattered | default | 64MB | group_c200 | 3 | 75.339 | 71.138 | 77.320 | 0.050 | Seq Scan |  |  |  |  |
| scalar | 500000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 3 | 42.721 | 42.342 | 42.929 | 0.048 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | low_work_mem | prefer_index | 64kB | eq_c2_0 | 3 | 16.983 | 16.647 | 17.667 | 0.038 | Index Only Scan |  |  |  |  |
| scalar | 500000 | btree | low_work_mem | prefer_index | 64kB | group_c200 | 3 | 42.962 | 40.022 | 43.259 | 0.039 | Index Only Scan |  |  |  |  |
| scalar | 500000 | gin | after_maintenance | default | 64MB | and2 | 3 | 17.331 | 17.255 | 17.350 | 0.062 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 3 | 1.483 | 1.437 | 1.542 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | after_maintenance | default | 64MB | group_c200 | 3 | 75.677 | 74.427 | 79.037 | 0.044 | Seq Scan |  |  |  |  |
| scalar | 500000 | gin | after_maintenance | default | 64MB | is_null | 3 | 29.341 | 29.085 | 29.697 | 0.029 | Seq Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | and2 | 3 | 17.974 | 17.290 | 19.453 | 0.055 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | and3 | 3 | 2.582 | 2.572 | 2.613 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | eq_c200_17 | 3 | 1.489 | 1.421 | 1.494 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | eq_c20k_123 | 3 | 0.045 | 0.039 | 0.045 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | eq_c2_0 | 3 | 46.777 | 46.157 | 49.221 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | fetch_medium | 3 | 1.650 | 1.542 | 1.657 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | group_c200 | 3 | 74.382 | 73.931 | 77.062 | 0.032 | Seq Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | group_filtered | 3 | 1.862 | 1.646 | 1.887 | 0.046 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | in_c20k_100 | 3 | 1.858 | 1.714 | 2.408 | 0.128 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | is_null | 3 | 28.823 | 27.966 | 29.904 | 0.029 | Seq Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | ordered_limit | 3 | 45.990 | 44.320 | 46.668 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 500000 | gin | clean | default | 64MB | range_random | 3 | 20.310 | 19.941 | 22.504 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | dirty_scattered | default | 64MB | and2 | 3 | 17.114 | 16.167 | 17.260 | 0.061 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 3 | 1.411 | 1.397 | 1.622 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 3 | 46.303 | 45.638 | 49.394 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | dirty_scattered | default | 64MB | fetch_medium | 3 | 1.534 | 1.532 | 1.627 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | dirty_scattered | default | 64MB | group_c200 | 3 | 75.659 | 73.931 | 79.852 | 0.033 | Seq Scan |  |  |  |  |
| scalar | 500000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 3 | 72.338 | 71.423 | 72.419 | 0.046 | Seq Scan |  |  |  |  |
| scalar | 500000 | gin | low_work_mem | prefer_index | 64kB | eq_c2_0 | 3 | 63.208 | 62.110 | 63.531 | 0.061 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | gin | low_work_mem | prefer_index | 64kB | group_c200 | 3 | 71.774 | 70.972 | 78.803 | 0.039 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring | after_maintenance | default | 64MB | and2 | 3 | 0.195 | 0.185 | 0.216 | 0.056 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 3 | 0.027 | 0.026 | 0.029 | 0.037 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | after_maintenance | default | 64MB | group_c200 | 3 | 76.259 | 74.356 | 80.212 | 0.037 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring | after_maintenance | default | 64MB | is_null | 3 | 0.121 | 0.097 | 0.220 | 0.061 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | and2 | 3 | 0.184 | 0.181 | 0.194 | 0.044 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | and3 | 3 | 0.135 | 0.129 | 0.179 | 0.053 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | eq_c200_17 | 3 | 0.020 | 0.019 | 0.021 | 0.038 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | eq_c20k_123 | 3 | 0.013 | 0.011 | 0.015 | 0.056 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | eq_c2_0 | 3 | 0.211 | 0.206 | 0.223 | 0.035 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | fetch_medium | 3 | 1.535 | 1.372 | 1.616 | 0.039 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | group_c200 | 3 | 2.164 | 2.096 | 2.262 | 0.035 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | group_filtered | 3 | 1.282 | 1.279 | 1.535 | 0.045 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | in_c20k_100 | 3 | 0.412 | 0.383 | 0.437 | 0.125 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | is_null | 3 | 0.058 | 0.057 | 0.060 | 0.033 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | ordered_limit | 3 | 46.010 | 45.053 | 46.366 | 0.034 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring | clean | default | 64MB | range_random | 3 | 26.980 | 25.733 | 29.196 | 0.050 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring | dirty_scattered | default | 64MB | and2 | 3 | 0.192 | 0.190 | 0.205 | 0.074 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 3 | 0.021 | 0.020 | 0.021 | 0.039 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 3 | 35.399 | 33.687 | 37.358 | 0.039 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring | dirty_scattered | default | 64MB | fetch_medium | 3 | 1.873 | 1.651 | 2.287 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring | dirty_scattered | default | 64MB | group_c200 | 3 | 77.789 | 72.023 | 78.499 | 0.048 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 3 | 2.366 | 2.193 | 2.398 | 0.034 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | low_work_mem | prefer_index | 64kB | eq_c2_0 | 3 | 0.207 | 0.206 | 0.216 | 0.037 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring | low_work_mem | prefer_index | 64kB | group_c200 | 3 | 2.204 | 2.056 | 2.407 | 0.034 | RoaringCount |  |  |  |  |
| scalar | 500000 | roaring_bitmap | after_maintenance | default | 64MB | and2 | 3 | 1.593 | 1.295 | 2.045 | 0.062 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 3 | 1.353 | 1.228 | 1.580 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 3 | 80.371 | 74.769 | 80.531 | 0.034 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | after_maintenance | default | 64MB | is_null | 3 | 12.016 | 11.682 | 13.598 | 0.031 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | and2 | 3 | 1.326 | 1.315 | 1.332 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | and3 | 3 | 1.358 | 1.152 | 1.411 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 3 | 1.319 | 1.317 | 1.337 | 0.034 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 3 | 0.112 | 0.034 | 0.188 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 3 | 33.636 | 32.424 | 35.072 | 0.041 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 3 | 1.469 | 1.362 | 1.677 | 0.038 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | group_c200 | 3 | 77.468 | 75.393 | 79.072 | 0.036 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | group_filtered | 3 | 1.684 | 1.498 | 1.686 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 3 | 1.534 | 1.457 | 1.822 | 0.088 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | is_null | 3 | 11.042 | 10.762 | 11.622 | 0.030 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | ordered_limit | 3 | 46.096 | 40.859 | 46.255 | 0.036 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | clean | default | 64MB | range_random | 3 | 28.568 | 27.402 | 28.591 | 0.044 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 3 | 1.485 | 1.320 | 1.645 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 3 | 1.517 | 1.459 | 1.580 | 0.033 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 3 | 32.545 | 30.925 | 36.063 | 0.034 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | dirty_scattered | default | 64MB | fetch_medium | 3 | 1.509 | 1.405 | 1.542 | 0.055 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 3 | 77.943 | 72.662 | 78.275 | 0.100 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 3 | 77.313 | 75.711 | 87.294 | 0.054 | Seq Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | low_work_mem | prefer_index | 64kB | eq_c2_0 | 3 | 51.430 | 49.367 | 52.080 | 0.058 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 500000 | roaring_bitmap | low_work_mem | prefer_index | 64kB | group_c200 | 3 | 73.926 | 72.761 | 76.632 | 0.047 | Seq Scan |  |  |  |  |

