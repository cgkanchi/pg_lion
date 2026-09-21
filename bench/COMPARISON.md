> Note: this document predates the rename to **pg_lion** (2026-09-21). Names map as: extension `roaring_index` -> `pg_lion`, access method `roaring` -> `lion`, functions `roaring_index_*` -> `lion_index_*`, GUC `roaring_index.*` -> `pg_lion.*`, C prefix `rbi_`/`RBI` -> `lion_`/`LION`/`Lion`, files `src/rbi_*.c` -> `src/lion_*.c`.

# Roaring versus PostgreSQL index types: measured comparison

Roaring's strongest result is counting and grouping a vacuumed, read-heavy table without visiting the heap. It is a specialized addition to a PostgreSQL indexing strategy. Composite and covering B-tree indexes win several selective intersections and heap-fetch workloads here, while B-tree also supplies range and ordered access that roaring does not implement. Writes, visibility-map changes, prepared-plan selection, and how the index was built materially change the comparison.

This report reviews extension commit `7db8f11`. The accompanying [code and architecture review](../FOLLOWUP_REVIEW.md) identifies four remaining P2 findings. No extension implementation changes were made during this review.

The [main searchable report](results/2026-09-20-comparison/index.html) contains every query/configuration, actual access route, median, p95, confidence interval, and planning time. The [complete Markdown tables](results/2026-09-20-comparison/REPORT.md), [growth/churn/write supplement](results/2026-09-21-stress/REPORT.md), and [reproduction instructions](comprehensive/README.md) accompany this interpretation. Raw samples, full EXPLAIN plans, exact-result checks, SQL, settings, and artifact audits are retained beside each report. Presentation was inspired by the [Biscuit benchmark](https://biscuit.readthedocs.io/en/latest/benchmark.html); these are independent PostgreSQL measurements on different workloads.

Both runs are finished and their matrix/artifact audits pass: **32,768 main observations plus 2,500 supplemental observations**, **3,516 successful exact-query checks**, and **42 indexed post-write checks**. There were no query errors or result mismatches. Five hash builds and one hash REINDEX timed out; hash portfolios are explicitly partial, and four dependent post-maintenance configurations are unmeasured. The [main audit](results/2026-09-20-comparison/AUDIT.json) and [supplement audit](results/2026-09-21-stress/AUDIT.json) preserve that distinction. No failed or missing measurement is counted as a successful timing.

## What was compared

The main experiment uses one million and five million synthetic scalar rows, plus 200,000 array/full-text documents. It compares sequential scans, B-tree, tuned B-tree, hash, GIN, GiST, BRIN, and roaring. A separate `roaring_bitmap` variant disables count pushdown while using the same indexes. Each ordinary scalar portfolio attempts eight single-column indexes. Tuned B-tree adds a composite `(c200,c20,c2)` and a covering `(c200) INCLUDE (id,payload)` index and pays their storage/build/write costs.

Scalar columns independently span two to one million distinct values, clustered values, a 90% hot-key skew, and NULLs. Queries include equality, IN lists, AND/OR combinations, heap fetches, range scans, ordered LIMIT, grouping, DISTINCT, arrays, and full-text expressions. There are ten timed rounds per warm configuration, two warmups, and three index-build trials. The supplement examines construction order, five changing-key/VACUUM cycles at fixed live cardinality, fully dirty counts at three memory budgets, typed prepared queries, and concurrent inserts.

The server is PostgreSQL **20devel**, built with GCC 13.3 and `-O2`, without assertions. The host is an AMD Ryzen 7 5700X3D with 16 logical CPUs and 23.47 GiB visible RAM under WSL2 Linux. Shared buffers are 512 MB, ordinary work_mem is 64 MB, and maintenance_work_mem is 512 MB. Durability is enabled. Parallel query and JIT are disabled; this comparison does not establish the best achievable parallel B-tree or sequential-scan performance. Autovacuum is disabled so each visibility transition is explicit. Exact configuration and version strings are in [metadata.json](results/2026-09-20-comparison/metadata.json).

## Count acceleration and its limits

Selected **one-million-row**, clean-table, default-planner execution medians, in milliseconds:

| Portfolio | Dense count (50%) | Count (0.5%) | Three-column AND | Group 200 keys | Fetch payload | Narrow range |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 68.613 | 48.056 | 47.770 | 152.871 | 49.407 | 53.552 |
| B-tree | 33.862 | 0.332 | 2.755 | 83.216 | 3.946 | 0.303 |
| Tuned B-tree | 30.785 | 0.330 | 0.029 | 83.801 | 0.605 | 0.305 |
| Hash, partial | 69.787 | 3.450 | 3.098 | 150.981 | 3.641 | 54.707 |
| GIN | 93.802 | 3.147 | 5.205 | 152.269 | 3.820 | 41.002 |
| GiST | 72.101 | 0.749 | 3.880 | 203.514 | 3.934 | 0.825 |
| BRIN | 67.819 | 75.964 | 71.752 | 154.025 | 71.120 | 81.407 |
| Roaring | 0.411 | 0.028 | 0.261 | 3.881 | 3.452 | 56.209 |
| Roaring, bitmap only | 66.156 | 3.112 | 3.663 | 153.425 | 3.218 | 54.865 |

These correspond to `eq_c2_0`, `eq_c200_17`, `and3`, `group_c200`, `fetch_medium`, and `range_random` in the saved SQL. Fetching aggregates `id` and payload length for `c200=17`; the narrow range counts `c20k BETWEEN 100 AND 199`. Portfolio labels describe available indexes: the actual plan can be a fallback, particularly for dense reads, grouping, and unsupported ranges. Hash is missing its dense-key index after a build timeout.

Roaring can combine compressed memberships and use visibility-map bits to count without fetching every matching tuple. The dense-count result is where that distinction matters most. The tuned B-tree intersection benefits from the composite key; its heap-fetch query benefits from the covering index. Roaring's range result uses a sequential fallback and is not a measurement of a roaring range-scan implementation. The small difference between that fallback and the sequential portfolio should not be treated as an algorithmic difference.

At five million rows, roaring's clean dense count is **2.123 ms**, its `c200=17` count is **0.100 ms**, and grouping all 200 keys is **25.133 ms**. Disabling count pushdown changes the dense-count median to **515.517 ms** with a bitmap heap scan. Thus the speedup is substantially a property of the integrated count executor and visibility handling, not merely the choice of bitmap compression.

The same queries at **five million rows**, again clean/default execution medians in milliseconds:

| Portfolio | Dense count (50%) | Count (0.5%) | Three-column AND | Group 200 keys | Fetch payload | Narrow range |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 407.717 | 313.492 | 307.090 | 829.691 | 309.698 | 347.109 |
| B-tree | 159.250 | 1.499 | 13.722 | 411.209 | 18.251 | 1.558 |
| Tuned B-tree | 159.373 | 1.551 | 0.071 | 411.875 | 3.005 | 1.542 |
| Hash, partial | 831.547 | 19.033 | 16.810 | 1,237.843 | 19.809 | 648.892 |
| GIN | 802.219 | 109.315 | 26.349 | 998.124 | 20.881 | 467.131 |
| GiST | 362.875 | 3.410 | 20.687 | 994.946 | 19.722 | 3.898 |
| BRIN | 409.197 | 310.606 | 549.239 | 862.079 | 309.397 | 354.631 |
| Roaring | 2.123 | 0.100 | 1.363 | 25.133 | 19.941 | 590.883 |
| Roaring, bitmap only | 515.517 | 192.986 | 26.910 | 1,138.092 | 178.188 | 600.596 |

This larger heap exceeds shared buffers. The randomized mixed-query sequence is not a sequence of individually timed queries each immediately preceded by its own warmup. Cache residency varies: GIN's 0.5% count ranges from **17.857 to 229.138 ms** across ten rounds (CV 74%); bitmap-only roaring ranges from **18.399 to 322.862 ms** (CV 67%). Their median differences do not establish a stable index-algorithm advantage. Even sequential fallbacks differ substantially across portfolio runs. Use the raw buffer evidence and full intervals, and repeat on the intended workload before treating small or cache-sensitive differences as decisive.

Very selective sub-millisecond results need restraint. At one million rows, the rare `c1m=12345` count has a roaring execution median of 0.011 ms versus 0.0195 ms for ordinary B-tree; adding recorded planning time gives medians of about 0.048 and 0.056 ms. Planning, instrumentation, and application/network latency can overwhelm such a small execution difference.

## Visibility and planner behavior

The visibility map is part of the workload. Updating only a non-indexed payload can remove the ability to count index membership without checking heap visibility.

| Five-million-row query, roaring portfolio | Clean | First 5% of rows updated | Additional scattered updates |
| --- | ---: | ---: | ---: |
| Dense count, default planner (ms) | 2.123 | 13.665 | 522.885 |
| `c200=17` count, default planner (ms) | 0.100 | 0.668 | 1.325 |
| Three-column intersection, default planner (ms) | 1.363 | 1.292 | 3.458 |
| Group 200 keys, default planner (ms) | 25.133 | 1,173.510 | 892.878 |
| Group 200 keys, sequential scans discouraged (ms) | — | 148.085 | 132.675 |

The last dirty dense-count result uses a fallback; the selective count and intersection retain RoaringCount. The heap has 96,154 of 96,192 pages all-visible in the clean state, 91,345 of 100,407 after the first update, and 43,846 of 100,407 after the scattered update. Row-update percentages are not page-visibility percentages.

The grouped-count discrepancy is a code-review finding. The planner charges repeated visits to the same dirty pages as random I/O, even when their working set fits in buffers. In a selected custom-plan sample, 418,819 buffer hits and zero reads cost much less than the estimate implies. Both planner modes produce the same exact results, yet the default plan is about 7.9 times slower after the first update. See the [review and full-plan evidence](../FOLLOWUP_REVIEW.md#p2-grouped-count-costing-still-rejects-a-much-faster-cached-recheck-path). Disabling sequential scans is a diagnostic, not a blanket production recommendation.

## Arrays and full-text search

Selected **200,000-document**, clean/default execution medians in milliseconds:

| Portfolio | Array contains one key | Array contains two keys | Common lexeme count | Two-lexeme AND | Phrase | Prefix | Full-text payload fetch |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 30.883 | 32.852 | 28.250 | 24.334 | 31.119 | 23.608 | 22.736 |
| GIN | 3.729 | 0.855 | 28.312 | 0.952 | 6.765 | 1.572 | 4.029 |
| GiST | 30.994 | 31.604 | 28.268 | 9.128 | 16.723 | 42.712 | 13.389 |
| Roaring | 0.023 | 0.103 | 0.029 | 0.102 | 68.028 | 59.537 | 3.712 |
| Roaring, bitmap only | 3.276 | 0.177 | 28.060 | 0.180 | 68.684 | 60.775 | 3.673 |

The saved cases are `array_common`, `array_and`, `ts_common`, `ts_and`, `ts_phrase`, `ts_prefix`, and `ts_fetch`. GiST's array columns are sequential fallbacks: it has no applicable text-array containment index in this portfolio. A very common lexeme can also lead GIN/GiST to choose sequential scanning; consult the recorded route.

Roaring's exact-key count acceleration is strong here. Native GIN is substantially better for phrase and prefix search. Roaring does not store the positions or sorted-key access needed to resolve those predicates directly; its current fallback walks all key postings and performs heap rechecks. The generic cost estimator underprices that work. A separate experiment on the **same warm table**, with randomized interleaving, reproduced default roaring phrase/prefix plans at **68.560/62.291 ms**, versus sequential plans at **31.762/23.834 ms**. This is the fourth [review finding](../FOLLOWUP_REVIEW.md#p2-generic-index-costing-treats-full-multikey-fallback-scans-as-selective-lookups), rather than merely a comparison against a more capable competing index.

## Prepared statements and dirty rechecks

On the supplement's clean **100,000-row** table, forcing a custom prepared plan permits roaring's dense-count shortcut; forcing a generic plan retains a parameter and loses it:

| Dense prepared count | Custom plan median | Generic plan median | Generic route |
| --- | ---: | ---: | --- |
| Roaring | 0.025 ms | 5.598 ms | Sequential scan |
| B-tree | 3.010 ms | 2.913 ms | Index-only scan |

For a unique-key point count, roaring's generic plan uses a bitmap scan: its execution median is 0.019 ms versus 0.009 ms custom, while plan+execution medians are 0.026 versus 0.049 ms. Reduced planning can outweigh tiny execution differences. These forced modes show the available paths; they do not reproduce PostgreSQL's automatic custom/generic-plan heuristic. Applications that use prepared statements should inspect their actual cached plans before expecting literal-query speedups.

After updating every payload, the same table has **zero all-visible pages**. The direct SQL count diagnostic returns 50,000 after rechecking 95,587 candidate TIDs, skipping no heap blocks. Its medians are **2.704, 2.696, and 2.696 ms** at 64 kB, 4 MB, and 64 MB work_mem. At 64 kB the ordinary bitmap route instead records 2,062 lossy heap blocks and a 7.226 ms median. This exercises the bounded recheck batches with more candidates than fit in the small buffer. It is not a measurement of whole-backend peak memory, and the separate partition-group hash still lacks a runtime memory limit.

## Storage, construction, and maintenance

For the one-million-row scalar portfolio:

| Family | Indexes MiB | Median full-portfolio build seconds |
| --- | ---: | ---: |
| BRIN | 0.44 | 0.454 |
| GIN | 60.26 | 2.281 |
| B-tree | 65.64 | 1.695 |
| Roaring | 72.71 | 6.707 |
| GiST | 155.14 | 5.210 |
| Tuned B-tree | 180.08 | 2.677 |
| Hash, partial portfolio | 250.21 | Not a complete build |

Roaring is not the smallest or fastest-building portfolio in this dataset. BRIN is tiny because it summarizes page ranges, but its usefulness depends on physical locality and the selectivity of those summaries. GIN and GiST scalar comparisons use the official `btree_gin` and `btree_gist` opclasses. They are not native B-tree implementations, nor do these integer results determine their performance on their usual array, full-text, or geometric workloads.

Maintenance also has a cost. These **single observations** follow the main visibility mutations, with a checkpoint before each operation:

| One-million-row portfolio | Insert 10,000 rows: ms / WAL MiB | Indexed UPDATE: ms / WAL MiB | VACUUM: ms / WAL MiB |
| --- | ---: | ---: | ---: |
| Roaring | 570.828 / 90.55 | 702.732 / 112.30 | 1,140.706 / 316.49 |
| B-tree | 169.300 / 35.46 | 488.182 / 40.48 | 380.279 / 137.03 |
| Tuned B-tree | 252.447 / 46.03 | 668.229 / 56.40 | 704.067 / 225.02 |
| GIN | 84.162 / 13.84 | 131.221 / 17.08 | 616.514 / 144.86 |

WAL includes full-page images after the checkpoint. GIN's default fastupdate can defer maintenance into its pending list; the VACUUM and supplement's post-writer drain measurements help expose that cost. These figures have no between-run confidence intervals and should not be read as sustained write-throughput estimates.

Native hash builds hit the equal 60-second statement limit on low-cardinality/skewed columns: `c2` and `skew` at one million rows, and `c2`, `c20`, and `skew` at five million. Surviving hash indexes are retained, and every report labels that portfolio **partial**. Queries for omitted indexes can fall back. The timeouts are measured outcomes under this limit; they do not establish that hash cannot build those indexes given more time.

The five-million-row hash portfolio also timed out during `REINDEX TABLE`, after its read matrix and preceding maintenance operations had completed. The failed rebuild is reported as an attempt, and its four dependent post-maintenance query configurations are explicitly unmeasured. The run resumed with the completed reads retained; both failed-attempt metadata/logs and resume history are preserved. No timeout is presented as a successful completion or a zero-latency result.

## Growth, key churn, and concurrent writes

Construction order changes roaring's hash-directory layout. With 100,000 unique bigint keys, bulk build chooses **847 buckets**; creating the index empty and then inserting keeps **64 buckets**, with overflow pages absorbing growth. The recorded load-plus-build time is 468.558 ms for the bulk route, while inserting into the already-created indexes takes 1,191.573 ms. These timings exclude the empty-index DDL and subsequent VACUUM, so they are not complete settled-construction costs. The 100-key IN median after VACUUM rises from **0.104 to 0.321 ms** for the incrementally grown index, despite its slightly smaller portfolio. Size alone misses the longer lookup chains.

Five cycles of replacing every unique key and vacuuming, at **100,000 live rows throughout**, produce these two-index portfolio sizes:

| Family | Initial MiB | After five cycles MiB | After REINDEX MiB |
| --- | ---: | ---: | ---: |
| B-tree | 2.836 | 9.797 | 2.836 |
| Hash | 9.938 | 16.523 | 9.938 |
| GIN | 5.492 | 36.438 | 5.492 |
| GiST | 4.641 | 28.594 | 4.641 |
| BRIN | 0.094 | 0.094 | 0.094 |
| Roaring | 7.594 | 34.133 | 7.672 |

Roaring retains **600,000 historical key entries for 100,000 live TIDs** after the fifth VACUUM; REINDEX returns the entry count to 100,000. Its fifth-cycle UPDATE takes 1,228.464 ms and VACUUM 1,017.270 ms, versus B-tree's 346.357 and 41.186 ms. Other families also retain storage; the experiment does not support a claim that roaring uniquely bloats. These are deterministic churn observations, not independent repeats of an identical state.

Short durable insert bursts use a new table and one index on a two-valued key, with **100 rows per transaction**:

| Family | 1 client: transactions/s | 4 clients | 8 clients | 8-client p95 transaction latency |
| --- | ---: | ---: | ---: | ---: |
| No index | 572.5 | 1,210.3 | 2,332.7 | 4.191 ms |
| B-tree | 508.6 | 1,193.1 | 1,629.1 | 8.023 ms |
| Hash | 370.6 | 774.9 | 1,024.2 | 11.009 ms |
| GIN | 512.9 | 1,136.6 | 1,561.7 | 8.061 ms |
| GiST | 486.4 | 1,153.4 | 2,260.9 | 4.840 ms |
| BRIN | 497.9 | 1,184.7 | 2,387.1 | 3.970 ms |
| Roaring | 454.8 | 731.6 | 844.8 | 13.476 ms |

Roaring scales less well in this hot-key insert experiment, consistent with its bucket-lock design; the experiment does not isolate lock waits as the only cause. Each point is one three-second burst, and faster methods finish with more rows. Small ranking differences—including BRIN versus no index—are not statistically established. The supplemental report includes WAL and post-burst VACUUM/drain cost; GIN's deferred work is not assumed free. Final row counts and both indexed key counts pass for all 21 bursts. This is a short insert-throughput experiment, not sustained mixed-workload endurance.

## How to use these results

Choose the matching query, scale, visibility state, and actual plan. Do not pool all operations into a single winner. A tuned composite/covering index trades additional space and write work for specific reads; a bitmap count engine trades deeper executor integration and visibility sensitivity for fast aggregates. The workload determines which trade is useful.

The full tables show bootstrap intervals, p95, and sample variation. They describe within-run variation, not uncertainty across machines or independent deployments. Families ran sequentially in shuffled order, rather than in interleaved crossover trials. Five-million-row heaps exceed shared buffers, and changing cache residency can strongly affect latency. "Warm" means no deliberate cache eviction. The shared-buffer-cold points restart the dedicated server but retain the operating-system cache; they are not cold-device I/O measurements.

Actual query outputs are checked as result multisets against forced sequential execution. Correct output on this finite matrix does not prove crash safety, standby replay correctness, or behavior for every opclass and SQL expression. The benchmark is exhaustive over its published matrix; long-running mixed workloads, geometric/vector/JSON cases, alternate PostgreSQL versions, and tuning sweeps remain separate studies.
