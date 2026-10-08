# pg_lion benchmarks

Times and relative speeds from a synthetic benchmark. Absolute times depend on the machine; the
ratios carry over better. "5× faster" means the other index took five times as long as lion. Times
are milliseconds of query execution.

**Setup.** The extension at `6bf193b` (the run itself is recorded as `85c991c`, the same code plus this
benchmark's own changes), PostgreSQL 16.15 (Ubuntu release build), 4 vCPUs, 16 GB RAM. Lion
preloaded, so its indexes use lion's own WAL format. Shared buffers 512 MB, `work_mem` 64 MB,
parallel query and JIT off, warm cache. Each query is the median of three timed runs after one
warmup, measured as EXPLAIN ANALYZE execution time. Every result is checked against a sequential
scan, and all 394 checks passed. The default planner chooses the plan.

**Data.** The scalar table has 5M rows (1M-row results are in the full report and tell the same
story) and one single-column index per column: B-tree, GIN (`btree_gin`) or lion. Columns hold 2,
20, 200, 20,000 and 1M distinct values, plus a skewed column (90% one value) and a column that is
10% NULL. The documents table has 200k rows with a `text[]` and a `tsvector`, indexed by GIN or lion
(`store_positions = true`).

Full report: [REPORT.md](../bench/results/2026-10-08-85c991c-pg16-focused/REPORT.md) ·
[HTML](../bench/results/2026-10-08-85c991c-pg16-focused/index.html) ·
[CSV](../bench/results/2026-10-08-85c991c-pg16-focused/summary.csv).

## Counts (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 0.37 | 196 | 1,027 | 536× faster | 2,805× faster |
| Count the hot value of a skewed column (90%) | 0.85 | 383 | 764 | 452× faster | 902× faster |
| Count a value in 0.5% of rows | 0.17 | 1.98 | 58.9 | 12× faster | 353× faster |
| Count a value in 0.005% of rows | 0.038 | 0.070 | 0.87 | 1.8× faster | 23× faster |
| Count a value of a 1M-value column | 0.027 | 0.046 | 0.11 | 1.7× faster | 4.0× faster |
| Two equality filters ANDed | 1.07 | 41.3 | 231 | 39× faster | 215× faster |
| Three selective equality filters ANDed | 0.11 | 2.62 | 3.75 | 24× faster | 34× faster |
| Two `IN` lists ANDed | 3.31 | 89.2 | 143 | 27× faster | 43× faster |
| Two columns ORed | 1.26 | 550 | 552 | 438× faster | 440× faster |
| `IN` list of 10 values | 0.14 | 0.30 | 8.84 | 2.1× faster | 61× faster |
| `IN` list of 1,000 values | 11.7 | 44.2 | 561 | 3.8× faster | 48× faster |
| `IS NULL` (10% of rows) | 0.59 | 41.8 | 591 | 70× faster | 995× faster |
| Range over 100 of 20,000 values | 1.05 | 1.97 | 544 | 1.9× faster | 517× faster |

## GROUP BY (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count per group, 2 groups | 1.35 | 495 | 986 | 368× faster | 733× faster |
| Count per group, 200 groups | 35.0 | 563 | 1,188 | 16× faster | 34× faster |
| Count per group, 20 groups, with a filter | 21.2 | 50.1 | 70.2 | 2.4× faster | 3.3× faster |

## Unvacuumed tables (5M rows)

The same counts after updating 1% of rows, scattered, without a VACUUM. Lion has to check the
affected pages in the heap, so its lead shrinks. It doesn't disappear.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 172 | 875 | 969 | 5.1× faster | 5.6× faster |
| Count a value in 0.5% of rows | 13.5 | 14.7 | 35.7 | 1.1× faster | 2.6× faster |
| Two equality filters ANDed | 8.32 | 60.3 | 213 | 7.2× faster | 26× faster |
| Count per group, 200 groups | 586 | 1,125 | 1,137 | 1.9× faster | 1.9× faster |

## Fetching rows (5M rows, vacuumed)

Queries that read the matching rows (`sum(id), sum(length(payload))`), so every index has to visit
the heap. Lion is a little faster, because its bitmaps are cheaper to combine.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| A value of a 1M-value column | 0.051 | 0.091 | 0.093 | 1.8× faster | 1.8× faster |
| A value in 0.005% of rows | 0.57 | 0.72 | 0.84 | 1.3× faster | 1.5× faster |
| A value in 0.5% of rows | 39.5 | 42.5 | 74.1 | 1.1× faster | 1.9× faster |
| A value in 5% of rows | 401 | 549 | 542 | 1.4× faster | 1.4× faster |
| Two filters ANDed | 17.8 | 25.9 | 39.4 | 1.5× faster | 2.2× faster |
| Three filters ANDed | 17.3 | 24.6 | 40.1 | 1.4× faster | 2.3× faster |
| Two `IN` lists ANDed | 56.5 | 72.7 | 150 | 1.3× faster | 2.7× faster |

## Ordered queries with LIMIT (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | Lion vs B-tree |
| --- | --- | --- | --- |
| `WHERE a = .. AND b = .. ORDER BY c LIMIT 10` (0.25% match) | 1.81 | 17.4 | 9.6× faster |
| The same, `ORDER BY c DESC` | 1.61 | 10.6 | 6.6× faster |
| `WHERE b = .. ORDER BY c LIMIT 10` (50% match) | 0.084 | 0.079 | **1.1× slower** |
| `WHERE c >= .. ORDER BY c LIMIT 100` on one column | 0.52 | 0.031 | **17× slower** |

Lion walks the `ORDER BY` column's index in order and skips rows its filter rules out
(`LionOrdered`). A B-tree's index-only scan on the one column wins the last case: keep a B-tree for
plain "first N by this column".

## Arrays and full-text search (200k documents, vacuumed)

| Query | Lion ms | GIN ms | Lion vs GIN |
| --- | --- | --- | --- |
| Count `tags @> '{t1}'` (5% of rows) | 0.040 | 8.74 | 218× faster |
| Count `tags @> '{t1,t17}'` | 0.11 | 1.16 | 10× faster |
| Count `tags && '{t1,t17,t123}'` | 0.15 | 12.7 | 83× faster |
| Count `tsv @@ 'common'` (nearly every row) | 0.063 | 47.1 | 748× faster |
| Count `tsv @@ 'w1 & w17'` | 0.14 | 1.28 | 9.5× faster |
| Count `tsv @@ '(w1 \| w2) & (w17 \| w18)'` | 0.25 | 4.39 | 17× faster |
| Count a rare word | 0.028 | 0.066 | 2.4× faster |
| Count a phrase, `common <-> w1` | 3.59 | 14.1 | 3.9× faster |
| Count a weight, `w1:D` | 0.52 | 10.8 | 21× faster |
| Count a NOT, `common & !w1` | 11.5 | 55.4 | 4.8× faster |
| Count a prefix, `rare12:*` | 41.2 | 3.32 | **12× slower** |
| Fetch the rows matching `w1` | 9.36 | 10.4 | 1.1× faster |

Lion answers prefix queries by checking every row, so GIN wins them.

## Ranked search: top 10 by score

Top 10 rows for an OR of words, over 500k synthetic documents of 20–200 words from a Zipf-like
50,000-word vocabulary ([script](../bench/ranking/run.py),
[results](../bench/results/2026-10-08-ranking/RESULTS.md)). GIN finds every match, scores it with
`ts_rank` and sorts. Lion's `LionBm25` scan scores from the index and skips rows that can't reach
the top 10.

| Query | Matches | Lion BM25 ms | GIN + ts_rank ms | Lion vs GIN |
| --- | --- | --- | --- | --- |
| One common word and two rare words | 492,031 | 16.6 | 639 | 39× faster |
| Two mid-frequency words | 32,934 | 9.0 | 244 | 27× faster |
| Three common words | 499,985 | 59.4 | 662 | 11× faster |
| Two rare words | 584 | 6.3 | 1.7 | **3.7× slower** |
| Two rare words ANDed | 18 | 0.49 | 0.46 | same |

With only a few hundred matches, scoring them all is cheaper than lion's ranked walk, but the
planner still picks the walk. Set `pg_lion.enable_bm25_scan = off` for queries you know are this
selective.

## Size, build and writes (5M rows, all seven indexes)

| | B-tree | GIN | Lion |
| --- | --- | --- | --- |
| Index size | 256 MiB | 157 MiB | 180 MiB |
| Build time | 16.7 s | 25.9 s | 16.0 s |
| Insert 50k rows | 1.3 s, 120 MiB WAL | 0.6 s, 61 MiB WAL | 1.9 s, 177 MiB WAL |
| Update an indexed column in 50k rows | 2.2 s, 132 MiB WAL | 1.0 s, 71 MiB WAL | 2.3 s, 202 MiB WAL |
| Delete 50k rows | 1.4 s | 1.6 s | 1.6 s |
| VACUUM | 4.0 s | 4.0 s | 4.3 s |

Lion is 30% smaller than B-tree and 15% larger than GIN here, and builds as fast as B-tree.
Inserts are about 1.4× slower than B-tree and write about 1.5× the WAL. GIN's writes look cheap
because it defers work to its pending list.

On the documents table, lion's `tsvector` index with positions is 2.8× the size of GIN's
(12.0 vs 4.3 MiB). Lion's `text[]` index is 1.3× the size of GIN's.

## Reproducing

Install pg_lion into a release PostgreSQL build, then:

```sh
python3 bench/comprehensive/run.py --prefix /path/to/postgresql --preload --output bench/results/my-run
python3 bench/comprehensive/report.py bench/results/my-run      # needs matplotlib
python3 bench/comprehensive/audit.py bench/results/my-run
python3 bench/ranking/run.py --dsn 'dbname=rank' --docs 500000 # needs pg_lion in that database
```

The suite runs its own temporary cluster and must run as a non-root user. On a 4-vCPU machine it
takes about 11 minutes, and the ranking script about 4. See
[bench/comprehensive/README.md](../bench/comprehensive/README.md) for options.
