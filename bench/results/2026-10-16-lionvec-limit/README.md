# lionvec's pause and a known LIMIT, DBpedia 115k, PostgreSQL 16 (2026-10-16)

A restricted scan (LionOrdered over a lion filter) told its query's LIMIT, DESIGN.md §44.4, "A known
LIMIT": `before` is main at 1c85107, `after` the commit that keeps no tier of the pause below the
LIMIT, `batch` the one after it, which makes a LIMIT past `pg_lion.vector_batch` the scan's L.

**Data and queries.** `bench/results/2026-10-14-lionvec-dbpedia/`'s: the table `dbpedia`, 115,186
rows of `vector(1536)`; the same 200 queries; `u < s` for filters of 10%, 3% and 1% through the lion
index `dbpedia_u`. The truth is the exact LIMIT nearest by cosine distance among the rows the filter
keeps, computed in numpy (and checked against the cache's ten nearest). The lionvec index
`dbpedia_lv` was built once, at 1c85107, with `partitions = 0` (2,715 partitions, spill), in 69 s,
258,572,288 bytes, and every run used it.

**Method.** One session a run, through psycopg: every relation `pg_prewarm`ed, then for each LIMIT,
filter, margin (3, 8, 16) and patience (30, 0): `RESET ALL`, `SET pg_lion.vector_margin`,
`pg_lion.vector_patience`, `pg_lion.vector_exact_rows = 0` (so that the 1% filter probes too),
`enable_sort = off` and `enable_indexscan = off` (the plan is `Limit -> Custom Scan (LionOrdered)`),
and the 200 queries three times. Recall, the partitions probed, passes, CODE pages read, pauses and
rows skipped out of order are means over the queries, from the scan's DEBUG1 line; ms is the
median over the queries of each query's median of three. The three rounds returned the same rows
every time. "Same rows" counts the queries whose ids, in order, match between the two builds.

**Machine and settings.** The 4 vCPUs (Intel Xeon @ 2.10GHz) and 15 GB of RAM of the earlier
results; PostgreSQL 16.15 built from source, pgvector 0.8.1, pg_lion built against it
(`WERROR=1`). `shared_buffers` 2.5 GB, `work_mem` 64 MB, `maintenance_work_mem` 1 GB, no parallel
query, `jit` off, `fsync` off. The three builds ran one after another on the same server, the
library replaced and the server restarted between them.

**Files.** `before.jsonl`, `after.jsonl` and `batch.jsonl`, a line a plan, LIMIT, filter, margin and
patience: `recall`, `ms`, `ms_p95`, `round_ms`, the plan's `shape`, and the DEBUG1 counts' means
(`probes` the partitions, `pages` the CODE pages, `skipped` the rows skipped out of order).
`batch.jsonl` (LIMITs 10 and 100 in one round, 1000 in three) was measured with L =
`Max(vector_batch, Min(LIMIT, 100000))`; the commit bounds it by an eighth of `work_mem`'s rows
instead (`lv_cap()`), 174,762 at 64 MB, the same L at these LIMITs. The committed build, run again
at LIMITs 10, 100 and 1000, 10% and 3%, margins 3 and 8, patience 30 and 0, one round, returned the
same rows with the same DEBUG1 counts as `batch` for every query at all 24 settings.

## LIMIT 100 (`before` -> `after`)

| filter | margin | patience | recall | partitions | CODE pages | pauses | skipped | ms | same rows |
|---|---|---|---|---|---|---|---|---|---|
| 10% | 3 | 30 | 0.9677 -> 0.9679 | 556 -> 556 | 1683 -> 1682 | 0.66 -> 0.20 | 1.79 -> 1.77 | 22.1 -> 23.7 | 198 |
| 10% | 3 | 0 | 0.9680 -> 0.9680 | 574 -> 574 | 1735 -> 1735 | 0.00 -> 0.00 | 1.81 -> 1.81 | 22.8 -> 22.7 | 200 |
| 10% | 8 | 30 | 0.9913 -> 0.9942 | 888 -> 887 | 2646 -> 2644 | 4.84 -> 1.00 | 0.32 -> 0.03 | 27.6 -> 27.6 | 161 |
| 10% | 8 | 0 | 0.9990 -> 0.9990 | 2087 -> 2087 | 6127 -> 6127 | 0.00 -> 0.00 | 0.08 -> 0.08 | 50.2 -> 50.5 | 200 |
| 10% | 16 | 30 | 0.9974 -> 0.9982 | 1368 -> 1367 | 4044 -> 4041 | 5.00 -> 1.00 | 0.09 -> 0.01 | 35.4 -> 35.4 | 184 |
| 10% | 16 | 0 | 1.0000 -> 1.0000 | 2715 -> 2715 | 7895 -> 7895 | 0.00 -> 0.00 | 0.01 -> 0.01 | 54.8 -> 57.8 | 200 |
| 3% | 3 | 30 | 0.9722 -> 0.9741 | 947 -> 946 | 2820 -> 2818 | 2.57 -> 0.63 | 1.60 -> 1.42 | 23.2 -> 25.0 | 171 |
| 3% | 3 | 0 | 0.9756 -> 0.9756 | 1080 -> 1080 | 3210 -> 3210 | 0.00 -> 0.00 | 1.67 -> 1.67 | 25.4 -> 26.6 | 200 |
| 3% | 8 | 30 | 0.9884 -> 0.9940 | 1202 -> 1202 | 3563 -> 3561 | 4.99 -> 1.00 | 0.56 -> 0.01 | 26.0 -> 27.8 | 127 |
| 3% | 8 | 0 | 0.9997 -> 0.9997 | 2541 -> 2541 | 7419 -> 7419 | 0.00 -> 0.00 | 0.04 -> 0.04 | 48.1 -> 48.0 | 200 |
| 3% | 16 | 30 | 0.9963 -> 0.9987 | 1738 -> 1738 | 5120 -> 5118 | 4.97 -> 0.98 | 0.24 -> 0.01 | 38.0 -> 38.8 | 159 |
| 3% | 16 | 0 | 0.9999 -> 0.9999 | 2715 -> 2715 | 7895 -> 7895 | 0.00 -> 0.00 | 0.01 -> 0.01 | 52.2 -> 50.0 | 200 |
| 1% | 3 | 30 | 0.9727 -> 0.9823 | 1365 -> 1364 | 4039 -> 4037 | 4.51 -> 0.96 | 1.75 -> 0.79 | 25.1 -> 26.2 | 99 |
| 1% | 3 | 0 | 0.9868 -> 0.9868 | 1769 -> 1769 | 5210 -> 5210 | 0.00 -> 0.00 | 1.05 -> 1.05 | 32.1 -> 31.9 | 200 |
| 1% | 8 | 30 | 0.9845 -> 0.9941 | 1546 -> 1545 | 4564 -> 4563 | 5.00 -> 1.00 | 0.97 -> 0.01 | 28.9 -> 27.9 | 84 |
| 1% | 8 | 0 | 0.9999 -> 0.9999 | 2693 -> 2693 | 7838 -> 7838 | 0.00 -> 0.00 | 0.01 -> 0.01 | 42.5 -> 44.9 | 200 |
| 1% | 16 | 30 | 0.9970 -> 0.9988 | 2088 -> 2087 | 6129 -> 6128 | 4.92 -> 0.94 | 0.18 -> 0.00 | 37.8 -> 36.5 | 167 |
| 1% | 16 | 0 | 1.0000 -> 1.0000 | 2715 -> 2715 | 7895 -> 7895 | 0.00 -> 0.00 | 0.00 -> 0.00 | 45.2 -> 43.1 | 200 |

`batch` leaves a LIMIT 100 alone, L being 100 already: the same rows and DEBUG1 counts as `after` for
every query at all 18 settings.

## LIMIT 10

The same rows, in order, and the same DEBUG1 counts for all 200 queries at every one of the 18
settings, `before` against `after` and against `batch`. `before`'s:

| filter | margin | patience | recall | partitions | CODE pages | pauses | ms (before, after) |
|---|---|---|---|---|---|---|---|
| 10% | 3 | 30 | 0.9560 | 138 | 433 | 0.05 | 9.8, 9.5 |
| 10% | 3 | 0 | 0.9560 | 140 | 442 | 0.00 | 10.1, 9.8 |
| 10% | 8 | 30 | 0.9935 | 385 | 1172 | 0.89 | 14.8, 16.0 |
| 10% | 8 | 0 | 0.9960 | 797 | 2381 | 0.00 | 23.3, 22.8 |
| 10% | 16 | 30 | 0.9975 | 733 | 2196 | 0.99 | 21.1, 20.8 |
| 10% | 16 | 0 | 1.0000 | 2631 | 7664 | 0.00 | 54.2, 53.4 |
| 3% | 3 | 30 | 0.9640 | 256 | 790 | 0.29 | 10.5, 9.2 |
| 3% | 3 | 0 | 0.9670 | 283 | 872 | 0.00 | 11.3, 10.1 |
| 3% | 8 | 30 | 0.9895 | 434 | 1319 | 0.99 | 14.3, 12.6 |
| 3% | 8 | 0 | 0.9990 | 1372 | 4060 | 0.00 | 28.9, 28.3 |
| 3% | 16 | 30 | 0.9965 | 785 | 2348 | 1.00 | 18.8, 18.2 |
| 3% | 16 | 0 | 1.0000 | 2709 | 7880 | 0.00 | 47.3, 47.3 |
| 1% | 3 | 30 | 0.9610 | 385 | 1172 | 0.79 | 8.7, 8.9 |
| 1% | 3 | 0 | 0.9690 | 542 | 1639 | 0.00 | 11.6, 11.3 |
| 1% | 8 | 30 | 0.9795 | 499 | 1511 | 1.00 | 10.3, 11.3 |
| 1% | 8 | 0 | 0.9985 | 2010 | 5908 | 0.00 | 35.2, 32.3 |
| 1% | 16 | 30 | 0.9925 | 887 | 2644 | 1.00 | 16.5, 16.4 |
| 1% | 16 | 0 | 1.0000 | 2715 | 7895 | 0.00 | 42.4, 40.8 |

## LIMIT 1000 (`before`, `after`, `batch`)

| filter | margin | patience | recall | partitions | passes | CODE pages | pauses | skipped | ms |
|---|---|---|---|---|---|---|---|---|---|
| 10% | 3 | 30 | 0.9838, 0.9840, 0.9863 | 1778, 1794, 1811 | 2.50, 2.50, 1.00 | 11822, 11866, 5331 | 10.41, 4.07, 0.06 | 13.19, 13.12, 11.04 | 97, 96, 65 |
| 10% | 3 | 0 | 0.9863, 0.9863, 0.9863 | 1816, 1816, 1816 | 2.50, 2.50, 1.00 | 11900, 11900, 5346 | 0.00, 0.00, 0.00 | 11.06, 11.06, 11.06 | 95, 96, 66 |
| 10% | 8 | 30 | 0.9984, 0.9987, 0.9997 | 2691, 2690, 2327 | 2.48, 2.48, 1.00 | 19253, 19251, 6816 | 15.79, 6.66, 0.85 | 1.59, 1.29, 0.12 | 137, 142, 75 |
| 10% | 8 | 0 | 0.9999, 0.9999, 0.9999 | 2698, 2698, 2698 | 2.48, 2.48, 1.00 | 19272, 19272, 7850 | 0.00, 0.00, 0.00 | 0.13, 0.13, 0.13 | 141, 143, 78 |
| 10% | 16 | 30 | 0.9997, 0.9998, 1.0000 | 2715, 2715, 2608 | 2.48, 2.48, 1.00 | 19580, 19580, 7603 | 6.46, 2.47, 0.38 | 0.31, 0.23, 0.03 | 141, 141, 76 |
| 10% | 16 | 0 | 1.0000, 1.0000, 1.0000 | 2715, 2715, 2715 | 2.48, 2.48, 1.00 | 19580, 19580, 7895 | 0.00, 0.00, 0.00 | 0.03, 0.03, 0.03 | 138, 143, 77 |
| 3% | 3 | 30 | 0.9895, 0.9899, 0.9940 | 2519, 2539, 2524 | 2.75, 2.75, 1.00 | 18932, 18987, 7372 | 15.79, 5.83, 0.17 | 10.23, 9.86, 5.74 | 119, 128, 64 |
| 3% | 3 | 0 | 0.9941, 0.9941, 0.9941 | 2543, 2543, 2543 | 2.75, 2.75, 1.00 | 18999, 18999, 7422 | 0.00, 0.00, 0.00 | 5.76, 5.76, 5.76 | 120, 124, 65 |
| 3% | 8 | 30 | 0.9977, 0.9983, 0.9999 | 2715, 2715, 2677 | 2.75, 2.75, 1.00 | 21729, 21729, 7793 | 11.09, 5.18, 0.38 | 2.31, 1.74, 0.07 | 135, 137, 67 |
| 3% | 8 | 0 | 0.9999, 0.9999, 0.9999 | 2715, 2715, 2715 | 2.75, 2.75, 1.00 | 21729, 21729, 7895 | 0.00, 0.00, 0.00 | 0.07, 0.07, 0.07 | 136, 138, 65 |
| 3% | 16 | 30 | 0.9996, 0.9998, 1.0000 | 2715, 2715, 2715 | 2.75, 2.75, 1.00 | 21751, 21751, 7895 | 5.91, 1.92, 0.00 | 0.42, 0.18, 0.04 | 135, 139, 66 |
| 3% | 16 | 0 | 1.0000, 1.0000, 1.0000 | 2715, 2715, 2715 | 2.75, 2.75, 1.00 | 21751, 21751, 7895 | 0.00, 0.00, 0.00 | 0.04, 0.04, 0.04 | 136, 139, 66 |

The tiers alone do little for a LIMIT 1000: it has no tier past L = 100 either way, so it still
pauses on L, by count, in each of two or three passes. With L the LIMIT it makes one pass, reads
2.2 to 2.8 times fewer pages and finds more of its rows at a patience of 30; with the pause off it
returns the same rows for every query.

## The plain plan

An Index Scan with a Filter (`enable_bitmapscan`, `pg_lion.enable_ordered_scan` and `enable_sort`
off), LIMIT 100, 3%, margin 8, patience 30, one round: 0.9987, 2,486 partitions, 24,688 pages,
35.23 pauses and 3.85 passes a query, `before` and `after` alike - it is never told its LIMIT. At
that setting LionOrdered returned the plain plan's rows for 39% of the queries before and 57% after.
