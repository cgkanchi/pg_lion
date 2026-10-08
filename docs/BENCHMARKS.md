# pg_lion benchmarks

Times and relative speeds from a synthetic benchmark. Absolute times depend on the machine; the
ratios carry over better. "5× faster" means the other index took five times as long as lion. Times
are milliseconds of query execution.

**Setup.** The extension at `6bf193b` (the run itself is recorded as `85c991c`, the same code plus this
benchmark's own changes), PostgreSQL 18.6 (built from source with `-O2`), 4 vCPUs, 16 GB RAM. Lion
preloaded, so its indexes use lion's own WAL format. Shared buffers 512 MB, `work_mem` 64 MB,
parallel query and JIT off (index builds may still use parallel workers), warm cache. Each query is the median of three timed runs after one
warmup, measured as EXPLAIN ANALYZE execution time. Every result is checked against a sequential
scan, and all 394 checks passed. The default planner chooses the plan.

**Data.** The scalar table has 5M rows (1M-row results are in the full report and tell the same
story) and one single-column index per column: B-tree, GIN (`btree_gin`) or lion. Columns hold 2,
20, 200, 20,000 and 1M distinct values, plus a skewed column (90% one value) and a column that is
10% NULL. The documents table has 200k rows with a `text[]` and a `tsvector`, indexed by GIN or lion
(`store_positions = true`).

Full report: [REPORT.md](../bench/results/2026-10-08-85c991c-pg18-focused/REPORT.md) ·
[HTML](../bench/results/2026-10-08-85c991c-pg18-focused/index.html) ·
[CSV](../bench/results/2026-10-08-85c991c-pg18-focused/summary.csv).

## Counts (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 0.41 | 189 | 933 | 464× faster | 2,287× faster |
| Count the hot value of a skewed column (90%) | 0.82 | 358 | 1,092 | 433× faster | 1,324× faster |
| Count a value in 0.5% of rows | 0.18 | 1.86 | 89.2 | 10× faster | 485× faster |
| Count a value in 0.005% of rows | 0.039 | 0.069 | 1.24 | 1.8× faster | 32× faster |
| Count a value of a 1M-value column | 0.024 | 0.048 | 0.32 | 2.0× faster | 14× faster |
| Two equality filters ANDed | 1.21 | 34.9 | 296 | 29× faster | 245× faster |
| Three selective equality filters ANDed | 0.11 | 2.47 | 4.13 | 22× faster | 36× faster |
| Two `IN` lists ANDed | 3.45 | 113 | 180 | 33× faster | 52× faster |
| Two columns ORed | 1.25 | 448 | 548 | 358× faster | 438× faster |
| `IN` list of 10 values | 0.18 | 0.23 | 12.8 | 1.3× faster | 71× faster |
| `IN` list of 1,000 values | 11.5 | 20.4 | 587 | 1.8× faster | 51× faster |
| `IS NULL` (10% of rows) | 0.81 | 38.5 | 687 | 48× faster | 853× faster |
| Range over 100 of 20,000 values | 1.02 | 1.91 | 666 | 1.9× faster | 653× faster |

## GROUP BY (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count per group, 2 groups | 1.64 | 474 | 1,164 | 289× faster | 711× faster |
| Count per group, 200 groups | 31.8 | 493 | 1,275 | 15× faster | 40× faster |
| Count per group, 20 groups, with a filter | 23.9 | 36.5 | 103 | 1.5× faster | 4.3× faster |

## Unvacuumed tables (5M rows)

The same counts after updating 1% of rows, scattered, without a VACUUM. Lion has to check the
affected pages in the heap, so its lead shrinks. It doesn't disappear.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 183 | 882 | 1,021 | 4.8× faster | 5.6× faster |
| Count a value in 0.5% of rows | 13.4 | 14.3 | 37.0 | 1.1× faster | 2.8× faster |
| Two equality filters ANDed | 7.92 | 125 | 212 | 16× faster | 27× faster |
| Count per group, 200 groups | 577 | 1,225 | 1,212 | 2.1× faster | 2.1× faster |

## Fetching rows (5M rows, vacuumed)

Queries that read the matching rows (`sum(id), sum(length(payload))`), so every index has to visit
the heap. Lion is a little faster, because its bitmaps are cheaper to combine.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| A value of a 1M-value column | 0.059 | 0.095 | 0.28 | 1.6× faster | 4.8× faster |
| A value in 0.005% of rows | 0.84 | 1.64 | 1.26 | 1.9× faster | 1.5× faster |
| A value in 0.5% of rows | 35.5 | 37.6 | 102 | 1.1× faster | 2.9× faster |
| A value in 5% of rows | 531 | 599 | 525 | 1.1× faster | same |
| Two filters ANDed | 20.8 | 22.1 | 39.9 | 1.1× faster | 1.9× faster |
| Three filters ANDed | 20.4 | 24.7 | 47.1 | 1.2× faster | 2.3× faster |
| Two `IN` lists ANDed | 64.0 | 66.9 | 146 | same | 2.3× faster |

## Ordered queries with LIMIT (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | Lion vs B-tree |
| --- | --- | --- | --- |
| `WHERE a = .. AND b = .. ORDER BY c LIMIT 10` (0.25% match) | 1.88 | 20.7 | 11× faster |
| The same, `ORDER BY c DESC` | 1.57 | 13.4 | 8.6× faster |
| `WHERE b = .. ORDER BY c LIMIT 10` (50% match) | 0.11 | 0.097 | **1.2× slower** |
| `WHERE c >= .. ORDER BY c LIMIT 100` on one column | 0.45 | 0.030 | **15× slower** |

Lion walks the `ORDER BY` column's index in order and skips rows its filter rules out
(`LionOrdered`). A B-tree's index-only scan on the one column wins the last case: keep a B-tree for
plain "first N by this column".

## Arrays and full-text search (200k documents, vacuumed)

| Query | Lion ms | GIN ms | Lion vs GIN |
| --- | --- | --- | --- |
| Count `tags @> '{t1}'` (5% of rows) | 0.043 | 12.0 | 280× faster |
| Count `tags @> '{t1,t17}'` | 0.12 | 1.25 | 10× faster |
| Count `tags && '{t1,t17,t123}'` | 0.17 | 20.0 | 114× faster |
| Count `tsv @@ 'common'` (nearly every row) | 0.092 | 52.9 | 575× faster |
| Count `tsv @@ 'w1 & w17'` | 0.16 | 1.33 | 8.4× faster |
| Count `tsv @@ '(w1 \| w2) & (w17 \| w18)'` | 0.26 | 4.84 | 19× faster |
| Count a rare word | 0.031 | 0.073 | 2.4× faster |
| Count a phrase, `common <-> w1` | 3.90 | 14.7 | 3.8× faster |
| Count a weight, `w1:D` | 0.52 | 18.1 | 35× faster |
| Count a NOT, `common & !w1` | 11.3 | 66.5 | 5.9× faster |
| Count a prefix, `rare12:*` (111 words) | 1.20 | 2.57 | 2.1× faster |
| Count a prefix, `rare1:*` (1,111 words) | 44.2 | 16.2 | **2.7× slower** |
| Fetch the rows matching `w1` | 9.51 | 16.6 | 1.8× faster |

Lion answers a prefix as the OR of the indexed words that start with it, up to 1000 words. Past
that it rechecks rows, and GIN wins. The prefix rows were measured separately from the rest of the
table, on the same data and server.

## Ranked search: top 10 by score

Top 10 rows for an OR of words, over 500k synthetic documents of 20–200 words from a Zipf-like
50,000-word vocabulary ([script](../bench/ranking/run.py),
[results](../bench/results/2026-10-08-ranking/RESULTS.md)). GIN finds every match, scores it with
`ts_rank` and sorts. Lion's `LionBm25` scan scores from the index and skips rows that can't reach
the top 10.

| Query | Matches | Lion BM25 ms | GIN + ts_rank ms | Lion vs GIN |
| --- | --- | --- | --- | --- |
| One common word and two rare words | 492,031 | 18.5 | 986 | 53× faster |
| Two mid-frequency words | 32,934 | 9.9 | 121 | 12× faster |
| Three common words | 499,985 | 63.2 | 1,074 | 17× faster |
| Two rare words | 584 | 7.1 | 3.7 | **1.9× slower** |
| Two rare words ANDed | 18 | 0.61 | 0.54 | **1.1× slower** |

With only a few hundred matches, scoring them all is cheaper than lion's ranked walk, but the
planner still picks the walk. Set `pg_lion.enable_bm25_scan = off` for queries you know are this
selective.

## Size, build and writes (5M rows, all seven indexes)

| | B-tree | GIN | Lion |
| --- | --- | --- | --- |
| Index size | 256 MiB | 157 MiB | 180 MiB |
| Build time | 15.9 s | 12.8 s | 8.4 s |
| Insert 50k rows | 1.4 s, 120 MiB WAL | 0.5 s, 61 MiB WAL | 1.9 s, 177 MiB WAL |
| Update an indexed column in 50k rows | 3.0 s, 139 MiB WAL | 1.2 s, 78 MiB WAL | 2.5 s, 209 MiB WAL |
| Delete 50k rows | 2.3 s | 1.8 s | 1.7 s |
| VACUUM | 4.2 s | 4.8 s | 4.4 s |

Lion is 30% smaller than B-tree and 15% larger than GIN here, and builds about twice as fast as
B-tree because its build runs in parallel. Inserts are about 1.3× slower than B-tree and write about
1.5× the WAL. GIN's writes look cheap
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
