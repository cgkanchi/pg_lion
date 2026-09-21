# Quick index progress benchmark

Commit `0e21a9754f4a63f1c0dd257d938a4d46d49a4990`. Profile `quick-v2`. Completed in **268.1 seconds**, with **160 exact-result checks** and **320 timings**. All checks pass.

Only B-tree, GIN, roaring, and roaring_bitmap are timed. Documents use GIN and roaring; B-tree has no matching array/full-text index here. Sequential scans supply untimed correctness references. 2 timing rounds, 1 warmup(s), one index build; warm cache, single-client queries, parallel query/JIT off. Small-sample medians/min/max are progress signals, not confidence intervals or production capacity estimates. Build and maintenance times are single observations.

Read timings are EXPLAIN execution time; planning is separate. Positive change means slower than the baseline. Inspect plan changes and repeat suspicious differences, especially sub-millisecond results. Different commits/extension binaries are expected; incompatible workloads/server settings are rejected.

## Build and size

| Suite | Rows | Family | Build ms | Indexes MiB |
| --- | --- | --- | --- | --- |
| scalar | 1000000 | gin | 795.288 | 18.945 |
| scalar | 1000000 | roaring | 3364.948 | 22.617 |
| scalar | 1000000 | btree | 1187.212 | 33.516 |
| scalar | 5000000 | roaring | 21910.684 | 91.406 |
| scalar | 5000000 | btree | 6814.943 | 166.812 |
| scalar | 5000000 | gin | 3505.583 | 62.703 |
| documents | 200000 | roaring | 2051.922 | 8.055 |
| documents | 200000 | gin | 500.993 | 6.375 |

## Maintenance

| Rows | Family | Operation | Elapsed ms | WAL MiB |
| --- | --- | --- | --- | --- |
| 1000000 | gin | insert | 59.049 | 9.234 |
| 1000000 | gin | indexed_update | 106.326 | 12.547 |
| 1000000 | gin | vacuum | 170.419 | 14.830 |
| 1000000 | roaring | insert | 307.165 | 43.206 |
| 1000000 | roaring | indexed_update | 401.344 | 58.049 |
| 1000000 | roaring | vacuum | 116.679 | 4.204 |
| 1000000 | btree | insert | 105.997 | 14.041 |
| 1000000 | btree | indexed_update | 225.490 | 18.106 |
| 1000000 | btree | vacuum | 129.064 | 4.225 |
| 5000000 | roaring | insert | 1927.197 | 229.708 |
| 5000000 | roaring | indexed_update | 2507.070 | 288.006 |
| 5000000 | roaring | vacuum | 478.904 | 20.729 |
| 5000000 | btree | insert | 619.090 | 59.688 |
| 5000000 | btree | indexed_update | 1304.490 | 77.017 |
| 5000000 | btree | vacuum | 472.010 | 20.733 |
| 5000000 | gin | insert | 337.748 | 46.051 |
| 5000000 | gin | indexed_update | 930.423 | 62.566 |
| 5000000 | gin | vacuum | 642.909 | 56.256 |

## Query timings and baseline comparison

| Suite | Rows | Variant | State | Planner | Memory | Query | n | Median ms | Min ms | Max ms | Planning ms | Plan | Baseline ms | Change % | Baseline plan | Plan changed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| documents | 200000 | gin | clean | default | 64MB | array_and | 2 | 0.933 | 0.843 | 1.024 | 0.056 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | array_common | 2 | 4.447 | 4.132 | 4.762 | 0.066 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | array_or | 2 | 10.684 | 9.862 | 11.505 | 0.088 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_and | 2 | 1.095 | 1.034 | 1.157 | 0.069 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_common | 2 | 46.995 | 44.577 | 49.413 | 0.068 | Seq Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_fetch | 2 | 6.946 | 6.167 | 7.725 | 0.070 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_phrase | 2 | 11.111 | 9.950 | 12.273 | 0.073 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | gin | clean | default | 64MB | ts_prefix | 2 | 2.148 | 2.136 | 2.160 | 0.065 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_and | 2 | 0.119 | 0.118 | 0.121 | 0.077 | RoaringCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_common | 2 | 0.034 | 0.024 | 0.043 | 0.095 | RoaringCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | array_or | 2 | 0.308 | 0.291 | 0.326 | 0.121 | RoaringCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_and | 2 | 0.114 | 0.112 | 0.116 | 0.080 | RoaringCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_common | 2 | 0.037 | 0.031 | 0.043 | 0.066 | RoaringCount |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_fetch | 2 | 5.386 | 5.363 | 5.408 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_phrase | 2 | 83.315 | 82.993 | 83.637 | 0.080 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring | clean | default | 64MB | ts_prefix | 2 | 73.245 | 72.260 | 74.231 | 0.061 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_and | 2 | 0.248 | 0.236 | 0.260 | 0.074 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_common | 2 | 5.588 | 5.549 | 5.627 | 0.075 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | array_or | 2 | 8.050 | 7.801 | 8.299 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_and | 2 | 0.247 | 0.221 | 0.274 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_common | 2 | 39.395 | 38.778 | 40.011 | 0.065 | Seq Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_fetch | 2 | 5.766 | 5.408 | 6.124 | 0.090 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_phrase | 2 | 91.118 | 89.232 | 93.005 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| documents | 200000 | roaring_bitmap | clean | default | 64MB | ts_prefix | 2 | 76.352 | 71.097 | 81.608 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.383 | 0.361 | 0.405 | 0.055 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 93.219 | 89.353 | 97.085 | 0.040 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | and2 | 2 | 3.119 | 2.915 | 3.324 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | and3 | 2 | 3.244 | 3.073 | 3.416 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 0.493 | 0.351 | 0.635 | 0.053 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.026 | 0.023 | 0.030 | 0.068 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 36.283 | 34.794 | 37.772 | 0.051 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | fetch_medium | 2 | 4.420 | 3.650 | 5.191 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | group_c200 | 2 | 93.645 | 90.133 | 97.158 | 0.043 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 0.358 | 0.331 | 0.385 | 0.100 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | is_null | 2 | 7.277 | 7.097 | 7.458 | 0.032 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | clean | default | 64MB | range_random | 2 | 0.361 | 0.313 | 0.409 | 0.123 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 3.738 | 3.402 | 4.073 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.338 | 0.326 | 0.350 | 0.046 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 77.584 | 75.112 | 80.057 | 0.048 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 158.756 | 151.151 | 166.361 | 0.149 | Seq Scan |  |  |  |  |
| scalar | 1000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 90.308 | 88.974 | 91.641 | 0.056 | Index Only Scan |  |  |  |  |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 3.305 | 2.807 | 3.804 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 148.534 | 148.292 | 148.776 | 0.041 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | and2 | 2 | 34.258 | 34.004 | 34.511 | 0.052 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | and3 | 2 | 5.299 | 5.272 | 5.325 | 0.051 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 3.197 | 3.122 | 3.272 | 0.064 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 0.057 | 0.047 | 0.067 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 93.519 | 91.872 | 95.166 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | fetch_medium | 2 | 3.178 | 3.105 | 3.251 | 0.045 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | group_c200 | 2 | 145.746 | 141.006 | 150.486 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 3.683 | 3.661 | 3.706 | 0.137 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | is_null | 2 | 57.492 | 56.947 | 58.037 | 0.024 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | clean | default | 64MB | range_random | 2 | 41.405 | 40.589 | 42.221 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 37.157 | 36.045 | 38.270 | 0.069 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 3.989 | 3.512 | 4.465 | 0.078 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 100.066 | 99.292 | 100.841 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 148.649 | 147.497 | 149.800 | 0.033 | Seq Scan |  |  |  |  |
| scalar | 1000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 143.484 | 143.168 | 143.801 | 0.052 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.054 | 0.054 | 0.055 | 0.063 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 154.238 | 146.498 | 161.978 | 0.047 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | and2 | 2 | 0.345 | 0.338 | 0.351 | 0.041 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | and3 | 2 | 0.320 | 0.260 | 0.379 | 0.094 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.030 | 0.028 | 0.033 | 0.049 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.012 | 0.011 | 0.013 | 0.034 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 0.425 | 0.403 | 0.448 | 0.034 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 3.182 | 2.709 | 3.655 | 0.049 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | group_c200 | 2 | 4.107 | 3.976 | 4.239 | 0.032 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 0.940 | 0.589 | 1.290 | 0.191 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | is_null | 2 | 0.100 | 0.099 | 0.100 | 0.030 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | clean | default | 64MB | range_random | 2 | 118.934 | 116.458 | 121.409 | 0.035 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 0.384 | 0.361 | 0.408 | 0.044 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.033 | 0.033 | 0.033 | 0.064 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 68.031 | 67.192 | 68.871 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 150.352 | 148.756 | 151.948 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 4.083 | 3.901 | 4.266 | 0.032 | RoaringCount |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 3.423 | 3.225 | 3.622 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 150.530 | 150.266 | 150.795 | 0.032 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 3.535 | 3.284 | 3.786 | 0.064 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 2.657 | 2.466 | 2.847 | 0.050 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 3.169 | 2.932 | 3.406 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 0.055 | 0.048 | 0.062 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 71.770 | 69.986 | 73.554 | 0.059 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 3.362 | 2.925 | 3.800 | 0.042 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 160.453 | 152.752 | 168.154 | 0.035 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 4.024 | 3.674 | 4.375 | 0.085 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 29.104 | 27.150 | 31.058 | 0.028 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 58.576 | 57.973 | 59.179 | 0.041 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 3.684 | 3.604 | 3.765 | 0.067 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 2.959 | 2.543 | 3.375 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 69.611 | 67.628 | 71.594 | 0.034 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 156.409 | 151.980 | 160.839 | 0.043 | Seq Scan |  |  |  |  |
| scalar | 1000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 149.261 | 147.964 | 150.558 | 0.052 | Seq Scan |  |  |  |  |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | eq_c200_17 | 2 | 1.946 | 1.922 | 1.971 | 0.048 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | after_maintenance | default | 64MB | group_c200 | 2 | 450.201 | 450.173 | 450.228 | 0.052 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | and2 | 2 | 20.590 | 19.946 | 21.234 | 0.073 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | and3 | 2 | 14.901 | 14.361 | 15.440 | 0.054 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c200_17 | 2 | 1.855 | 1.568 | 2.143 | 0.038 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c20k_123 | 2 | 0.033 | 0.032 | 0.033 | 0.033 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | eq_c2_0 | 2 | 177.649 | 177.605 | 177.693 | 0.051 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | fetch_medium | 2 | 21.122 | 19.962 | 22.282 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | group_c200 | 2 | 439.428 | 433.019 | 445.837 | 0.056 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | in_c20k_100 | 2 | 1.658 | 1.606 | 1.710 | 0.109 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | is_null | 2 | 42.142 | 36.465 | 47.818 | 0.035 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | clean | default | 64MB | range_random | 2 | 1.723 | 1.578 | 1.868 | 0.072 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | and2 | 2 | 223.118 | 221.272 | 224.963 | 0.068 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 2.083 | 1.809 | 2.356 | 0.060 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 677.087 | 658.452 | 695.722 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | default | 64MB | group_c200 | 2 | 1032.383 | 981.890 | 1082.876 | 0.048 | Seq Scan |  |  |  |  |
| scalar | 5000000 | btree | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 425.329 | 419.520 | 431.138 | 0.067 | Index Only Scan |  |  |  |  |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | eq_c200_17 | 2 | 23.085 | 22.236 | 23.934 | 0.070 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | after_maintenance | default | 64MB | group_c200 | 2 | 1091.954 | 1054.425 | 1129.484 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | and2 | 2 | 191.776 | 187.624 | 195.928 | 0.069 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | and3 | 2 | 27.898 | 27.680 | 28.116 | 0.086 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c200_17 | 2 | 113.357 | 72.988 | 153.726 | 0.073 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c20k_123 | 2 | 1.000 | 0.257 | 1.744 | 0.063 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | eq_c2_0 | 2 | 957.933 | 950.697 | 965.168 | 0.064 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | fetch_medium | 2 | 83.210 | 22.849 | 143.572 | 0.053 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | group_c200 | 2 | 1268.598 | 1192.391 | 1344.805 | 0.041 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | in_c20k_100 | 2 | 252.372 | 223.305 | 281.439 | 0.141 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | is_null | 2 | 791.272 | 713.610 | 868.933 | 0.047 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | clean | default | 64MB | range_random | 2 | 406.913 | 312.350 | 501.475 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | and2 | 2 | 193.280 | 189.025 | 197.535 | 0.072 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 104.066 | 67.727 | 140.405 | 0.073 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 879.526 | 828.481 | 930.571 | 0.084 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | default | 64MB | group_c200 | 2 | 975.308 | 924.626 | 1025.990 | 0.051 | Seq Scan |  |  |  |  |
| scalar | 5000000 | gin | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 1009.835 | 1001.840 | 1017.830 | 0.052 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | eq_c200_17 | 2 | 0.179 | 0.156 | 0.203 | 0.049 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | after_maintenance | default | 64MB | group_c200 | 2 | 1038.439 | 1020.120 | 1056.759 | 0.046 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | and2 | 2 | 1.686 | 1.624 | 1.747 | 0.043 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | and3 | 2 | 1.144 | 1.140 | 1.148 | 0.045 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c200_17 | 2 | 0.117 | 0.106 | 0.128 | 0.048 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c20k_123 | 2 | 0.025 | 0.021 | 0.028 | 0.047 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | eq_c2_0 | 2 | 2.020 | 2.019 | 2.021 | 0.035 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | fetch_medium | 2 | 17.375 | 16.167 | 18.582 | 0.036 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | group_c200 | 2 | 19.678 | 19.625 | 19.731 | 0.033 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | in_c20k_100 | 2 | 2.527 | 2.454 | 2.599 | 0.117 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | is_null | 2 | 0.464 | 0.464 | 0.464 | 0.032 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | clean | default | 64MB | range_random | 2 | 721.793 | 720.145 | 723.441 | 0.035 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | and2 | 2 | 2.661 | 1.761 | 3.561 | 0.058 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 0.125 | 0.109 | 0.141 | 0.064 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 532.472 | 514.546 | 550.397 | 0.066 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | default | 64MB | group_c200 | 2 | 911.392 | 882.334 | 940.451 | 0.035 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 21.180 | 20.073 | 22.287 | 0.035 | RoaringCount |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | eq_c200_17 | 2 | 19.319 | 14.662 | 23.976 | 0.044 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | after_maintenance | default | 64MB | group_c200 | 2 | 1088.812 | 1037.140 | 1140.483 | 0.042 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and2 | 2 | 43.733 | 39.905 | 47.562 | 0.066 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | and3 | 2 | 15.236 | 12.558 | 17.914 | 0.068 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c200_17 | 2 | 113.782 | 20.991 | 206.573 | 0.047 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c20k_123 | 2 | 2.457 | 2.044 | 2.869 | 0.067 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | eq_c2_0 | 2 | 652.584 | 506.672 | 798.496 | 0.043 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | fetch_medium | 2 | 129.713 | 22.471 | 236.954 | 0.060 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | group_c200 | 2 | 1012.828 | 995.841 | 1029.814 | 0.053 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | in_c20k_100 | 2 | 225.888 | 223.553 | 228.223 | 0.094 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | is_null | 2 | 532.471 | 461.737 | 603.206 | 0.037 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | clean | default | 64MB | range_random | 2 | 533.793 | 518.662 | 548.925 | 0.043 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | and2 | 2 | 18.959 | 16.364 | 21.555 | 0.062 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c200_17 | 2 | 117.474 | 18.521 | 216.426 | 0.057 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | eq_c2_0 | 2 | 742.606 | 702.826 | 782.385 | 0.034 | Bitmap Heap Scan, Bitmap Index Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | default | 64MB | group_c200 | 2 | 889.454 | 809.285 | 969.623 | 0.043 | Seq Scan |  |  |  |  |
| scalar | 5000000 | roaring_bitmap | dirty_scattered | prefer_index | 64MB | group_c200 | 2 | 1058.197 | 1058.100 | 1058.294 | 0.053 | Seq Scan |  |  |  |  |

