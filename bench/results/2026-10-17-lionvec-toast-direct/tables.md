### toast-direct-final.json: io_method worker, io_workers 3

**device**

| Query | off | on |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 30.5 (26.7-33.3), 430 p, 22.1 w, 6.0 MB | 27.2 (23.6-33.2), 429 p, 19.5 w, 5.4 MB |
| unfiltered, margin 8 (0.980) | 53.7 (42.6-56.6), 1024 p, 34.0 w, 11.1 MB | 44.8 (35.0-53.2), 1024 p, 28.5 w, 11.5 MB |
| 3%, probed (0.970) | 70.8 (49.6-72.1), 1446 p, 45.1 w, 14.9 MB | 63.9 (40.5-76.6), 1445 p, 40.4 w, 15.0 MB |
| 1%, exact (1.000) | 179.7 (170.5-192.1), 2717 p, 152.7 w, 29.5 MB | 68.3 (62.8-75.6), 1631 p, 12.7 w, 15.0 MB |
| 3%, exact (1.000) | 457.2 (423.6-530.4), 5620 p, 395.9 w, 48.5 MB | 158.6 (155.1-171.5), 3915 p, 12.2 w, 32.9 MB |

*each query against itself under off, or its own library's off where named: the median ratio (quartiles)*

| Query | on |
| --- | --- |
| unfiltered, margin 3 | 0.94 (0.81-1.04) |
| unfiltered, margin 8 | 0.93 (0.89-1.02) |
| 3%, probed | 0.93 (0.83-1.03) |
| 1%, exact | 0.38 (0.36-0.43) |
| 3%, exact | 0.34 (0.32-0.37) |

**warm**

| Query | off | on |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 2.6 (2.0-3.1) | 2.6 (1.9-3.1) |
| unfiltered, margin 8 (0.980) | 6.2 (4.3-7.5) | 6.3 (4.1-7.5) |
| 3%, probed (0.970) | 10.2 (7.5-10.8) | 10.0 (7.5-11.0) |
| 1%, exact (1.000) | 14.1 (13.4-15.0) | 7.9 (7.5-8.4) |
| 3%, exact (1.000) | 31.8 (30.8-32.3) | 16.4 (15.9-16.7) |

*each query against itself under off, or its own library's off where named: the median ratio (quartiles)*

| Query | on |
| --- | --- |
| unfiltered, margin 3 | 0.96 (0.94-1.01) |
| unfiltered, margin 8 | 0.99 (0.96-1.02) |
| 3%, probed | 0.99 (0.97-1.01) |
| 1%, exact | 0.55 (0.53-0.57) |
| 3%, exact | 0.52 (0.51-0.53) |

### toast-direct-worker-3.json: io_method worker, io_workers 3

**device**

| Query | off | on |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 34.8 (27.7-41.4), 430 p, 23.9 w, 5.4 MB | 32.2 (23.5-39.9), 429 p, 23.6 w, 5.5 MB |
| unfiltered, margin 8 (0.980) | 55.3 (35.2-65.9), 1024 p, 36.2 w, 11.2 MB | 53.7 (37.8-60.4), 1024 p, 35.0 w, 11.3 MB |
| 3%, probed (0.970) | 72.8 (57.3-78.1), 1446 p, 48.6 w, 14.9 MB | 70.9 (59.5-82.8), 1445 p, 44.9 w, 15.0 MB |
| 1%, exact (1.000) | 179.0 (170.5-201.9), 2717 p, 153.7 w, 29.3 MB | 69.9 (67.4-74.1), 1631 p, 7.9 w, 15.0 MB |
| 3%, exact (1.000) | 472.1 (456.5-489.9), 5620 p, 406.3 w, 48.1 MB | 197.2 (194.8-203.2), 3915 p, 10.3 w, 32.9 MB |

*each query against itself under off, or its own library's off where named: the median ratio (quartiles)*

| Query | on |
| --- | --- |
| unfiltered, margin 3 | 0.89 (0.81-1.06) |
| unfiltered, margin 8 | 0.88 (0.80-1.10) |
| 3%, probed | 1.10 (0.97-1.15) |
| 1%, exact | 0.37 (0.36-0.39) |
| 3%, exact | 0.41 (0.40-0.43) |

**warm**

| Query | off | on |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 3.2 (2.8-3.5) | 3.4 (2.5-3.5) |
| unfiltered, margin 8 (0.980) | 9.1 (6.3-10.8) | 8.9 (6.2-9.8) |
| 3%, probed (0.970) | 9.4 (7.5-10.1) | 9.3 (7.4-9.9) |
| 1%, exact (1.000) | 17.8 (17.8-17.9) | 16.5 (16.3-17.1) |
| 3%, exact (1.000) | 35.1 (31.0-36.2) | 22.4 (21.1-26.0) |

*each query against itself under off, or its own library's off where named: the median ratio (quartiles)*

| Query | on |
| --- | --- |
| unfiltered, margin 3 | 0.98 (0.93-1.04) |
| unfiltered, margin 8 | 0.98 (0.98-1.09) |
| 3%, probed | 1.00 (0.98-1.01) |
| 1%, exact | 0.95 (0.93-0.97) |
| 3%, exact | 0.68 (0.65-0.70) |

### toast-warm-sorted.json: io_method worker, io_workers 3

**warm**

| Query | on2 | off2 | on | off |
| --- | --- | --- | --- | --- |
| unfiltered, margin 3 (0.920) | 2.8 (2.1-3.1) | 2.8 (2.1-3.3) | 3.0 (2.5-3.5) | 3.1 (2.1-3.4) |
| 3%, probed (0.970) | 9.5 (7.6-13.3) | 9.6 (7.9-12.5) | 8.1 (6.9-10.1) | 8.4 (6.9-10.3) |
| 1%, exact (1.000) | 7.2 (6.5-8.1) | 13.6 (12.1-16.6) | 11.4 (11.2-13.0) | 12.0 (11.9-12.7) |
| 3%, exact (1.000) | 15.5 (15.3-15.7) | 30.4 (29.4-30.7) | 21.0 (20.5-21.1) | 30.1 (29.8-30.6) |

*each query against itself under off, or its own library's off where named: the median ratio (quartiles)*

| Query | on2 | off2 | on |
| --- | --- | --- | --- |
| unfiltered, margin 3 | 0.94 (0.90-1.01) | 0.93 (0.89-1.01) | 1.06 (1.01-1.11) |
| 3%, probed | 1.23 (1.06-1.41) | 1.08 (1.04-1.36) | 0.99 (0.97-1.02) |
| 1%, exact | 0.57 (0.54-0.66) | 1.08 (1.01-1.35) | 0.95 (0.94-0.95) |
| 3%, exact | 0.52 (0.51-0.53) | 1.00 (0.98-1.03) | 0.70 (0.69-0.70) |

### inline-worker-3.json: io_method worker, io_workers 3

**device**

| Query | main | branch |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 32.5 (26.9-42.5), 402 p, 24.2 w, 5.1 MB | 30.6 (22.4-34.8), 402 p, 23.2 w, 5.1 MB |
| unfiltered, margin 8 (0.980) | 55.2 (41.1-62.1), 996 p, 32.0 w, 11.0 MB | 48.4 (35.0-56.0), 996 p, 27.6 w, 11.2 MB |
| 3%, probed (0.970) | 77.2 (53.9-93.3), 1418 p, 50.5 w, 14.8 MB | 75.0 (62.7-82.8), 1418 p, 44.0 w, 15.1 MB |
| 1%, exact (1.000) | 57.3 (53.3-60.1), 1212 p, 3.1 w, 11.6 MB | 56.2 (55.5-64.5), 1212 p, 3.6 w, 11.8 MB |
| 3%, exact (1.000) | 164.6 (152.8-174.2), 3492 p, 5.8 w, 29.7 MB | 158.3 (146.3-169.0), 3492 p, 4.6 w, 29.7 MB |

*each query against itself under main, or its own library's off where named: the median ratio (quartiles)*

| Query | branch |
| --- | --- |
| unfiltered, margin 3 | 0.90 (0.76-1.08) |
| unfiltered, margin 8 | 0.90 (0.82-1.02) |
| 3%, probed | 0.99 (0.96-1.23) |
| 1%, exact | 1.07 (0.91-1.17) |
| 3%, exact | 0.98 (0.86-1.01) |

**warm**

| Query | main | branch |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 3.3 (2.3-3.5) | 2.3 (1.8-3.0) |
| unfiltered, margin 8 (0.980) | 6.0 (4.0-7.3) | 6.3 (4.2-7.9) |
| 3%, probed (0.970) | 14.4 (11.5-15.6) | 12.9 (9.6-16.3) |
| 1%, exact (1.000) | 4.4 (4.4-4.5) | 4.3 (4.2-4.5) |
| 3%, exact (1.000) | 11.1 (11.0-11.2) | 13.0 (11.9-16.3) |

*each query against itself under main, or its own library's off where named: the median ratio (quartiles)*

| Query | branch |
| --- | --- |
| unfiltered, margin 3 | 0.79 (0.68-0.81) |
| unfiltered, margin 8 | 1.02 (0.98-1.04) |
| 3%, probed | 1.05 (0.79-1.09) |
| 1%, exact | 0.98 (0.93-1.00) |
| 3%, exact | 1.16 (1.06-1.47) |

### inline-warm-final.json: io_method worker, io_workers 3

**warm**

| Query | branch | main |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 3.4 (2.2-4.2) | 2.2 (1.6-2.6) |
| unfiltered, margin 8 (0.980) | 8.1 (4.7-8.6) | 6.8 (4.9-9.4) |
| 3%, probed (0.970) | 11.9 (8.6-13.1) | 10.8 (9.5-11.5) |
| 1%, exact (1.000) | 4.8 (4.4-4.9) | 4.5 (4.4-4.8) |
| 3%, exact (1.000) | 12.1 (11.6-12.6) | 11.1 (10.7-12.4) |

*each query against itself under branch, or its own library's off where named: the median ratio (quartiles)*

| Query | main |
| --- | --- |
| unfiltered, margin 3 | 0.63 (0.57-0.84) |
| unfiltered, margin 8 | 1.09 (0.99-1.16) |
| 3%, probed | 0.91 (0.87-0.91) |
| 1%, exact | 0.99 (0.93-1.05) |
| 3%, exact | 0.94 (0.89-1.05) |

### inline-warm-main-first.json: io_method worker, io_workers 3

**warm**

| Query | main | branch |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 2.6 (1.8-3.2) | 2.4 (1.9-2.8) |
| unfiltered, margin 8 (0.980) | 6.3 (4.3-10.4) | 6.3 (4.3-7.7) |
| 3%, probed (0.970) | 11.8 (8.9-13.0) | 13.2 (10.4-15.4) |
| 1%, exact (1.000) | 4.6 (4.3-5.1) | 5.6 (5.1-5.8) |
| 3%, exact (1.000) | 13.8 (11.5-15.4) | 13.1 (11.4-14.8) |

*each query against itself under main, or its own library's off where named: the median ratio (quartiles)*

| Query | branch |
| --- | --- |
| unfiltered, margin 3 | 1.00 (0.84-1.04) |
| unfiltered, margin 8 | 0.90 (0.71-1.09) |
| 3%, probed | 1.26 (1.07-1.30) |
| 1%, exact | 1.11 (1.08-1.22) |
| 3%, exact | 0.98 (0.91-1.05) |

### inline-warm-branch-first.json: io_method worker, io_workers 3

**warm**

| Query | branch | main |
| --- | --- | --- |
| unfiltered, margin 3 (0.920) | 2.8 (1.9-3.5) | 2.9 (2.1-3.7) |
| unfiltered, margin 8 (0.980) | 8.2 (5.4-9.1) | 6.6 (5.3-7.2) |
| 3%, probed (0.970) | 10.9 (10.6-12.8) | 12.0 (8.2-12.7) |
| 1%, exact (1.000) | 5.5 (4.7-5.6) | 4.3 (4.2-4.5) |
| 3%, exact (1.000) | 13.1 (11.7-14.9) | 12.8 (11.8-14.4) |

*each query against itself under branch, or its own library's off where named: the median ratio (quartiles)*

| Query | main |
| --- | --- |
| unfiltered, margin 3 | 1.02 (0.94-1.40) |
| unfiltered, margin 8 | 0.87 (0.76-0.98) |
| 3%, probed | 1.05 (0.79-1.15) |
| 1%, exact | 0.89 (0.79-0.96) |
| 3%, exact | 0.99 (0.79-1.23) |

