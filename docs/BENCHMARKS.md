# pg_lion benchmarks

Relative speeds from a synthetic benchmark. Absolute times depend on the machine; the ratios are what
to take away. "5× faster" means the other index took five times as long as lion.

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

| Query | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- |
| Count a value in 50% of rows | 536× faster | 2,805× faster |
| Count the hot value of a skewed column (90%) | 452× faster | 902× faster |
| Count a value in 0.5% of rows | 12× faster | 353× faster |
| Count a value in 0.005% of rows | 1.8× faster | 23× faster |
| Count a value of a 1M-value column | 1.7× faster | 4× faster |
| Two equality filters ANDed | 39× faster | 215× faster |
| Three selective equality filters ANDed | 24× faster | 34× faster |
| Two `IN` lists ANDed | 27× faster | 43× faster |
| Two columns ORed | 438× faster | 440× faster |
| `IN` list of 10 values | 2.1× faster | 61× faster |
| `IN` list of 1,000 values | 3.8× faster | 48× faster |
| `IS NULL` (10% of rows) | 70× faster | 995× faster |
| Range over 100 of 20,000 values | 1.9× faster | 517× faster |

## GROUP BY (5M rows, vacuumed)

| Query | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- |
| Count per group, 2 groups | 368× faster | 733× faster |
| Count per group, 200 groups | 16× faster | 34× faster |
| Count per group, 20 groups, with a filter | 2.4× faster | 3.3× faster |

## Unvacuumed tables (5M rows)

The same counts after updating 1% of rows, scattered, without a VACUUM. Lion has to check the
affected pages in the heap, so its lead shrinks. It doesn't disappear.

| Query | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- |
| Count a value in 50% of rows | 5.1× faster | 5.6× faster |
| Count a value in 0.5% of rows | 1.1× faster | 2.6× faster |
| Two equality filters ANDed | 7.2× faster | 26× faster |
| Count per group, 200 groups | 1.9× faster | 1.9× faster |

## Fetching rows (5M rows, vacuumed)

Queries that read the matching rows (`sum(id), sum(length(payload))`), so every index has to visit
the heap. Lion is a little faster, because its bitmaps are cheaper to combine.

| Query | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- |
| One rare value (a few rows) | 1.3–1.8× faster | 1.5–1.8× faster |
| A value in 0.5% of rows | 1.1× faster | 1.9× faster |
| A value in 5% of rows | 1.4× faster | 1.4× faster |
| Two or three filters ANDed | 1.4–1.5× faster | 2.2–2.3× faster |

## Ordered queries with LIMIT (5M rows, vacuumed)

| Query | Lion vs B-tree |
| --- | --- |
| `WHERE a = .. AND b = .. ORDER BY c LIMIT 10` (0.25% match) | 6.6–9.6× faster |
| `WHERE b = .. ORDER BY c LIMIT 10` (50% match) | about the same |
| `WHERE c >= .. ORDER BY c LIMIT 100` on one column | **17× slower** |

Lion walks the `ORDER BY` column's index in order and skips rows its filter rules out
(`LionOrdered`). A B-tree's index-only scan on the one column wins the last case: keep a B-tree for
plain "first N by this column".

## Arrays and full-text search (200k documents, vacuumed)

| Query | Lion vs GIN |
| --- | --- |
| Count `tags @> '{t1}'` (5% of rows) | 218× faster |
| Count `tags @> '{t1,t17}'` | 10× faster |
| Count `tags && '{t1,t17,t123}'` | 83× faster |
| Count `tsv @@ 'common'` (nearly every row) | 748× faster |
| Count `tsv @@ 'w1 & w17'` | 9.5× faster |
| Count `tsv @@ '(w1 \| w2) & (w17 \| w18)'` | 17× faster |
| Count a rare word | 2.4× faster |
| Count a phrase, `common <-> w1` | 3.9× faster |
| Count a weight, `w1:D` | 21× faster |
| Count a NOT, `common & !w1` | 4.8× faster |
| Count a prefix, `rare12:*` | **12× slower** |
| Fetch the rows matching `w1` | 1.1× faster |

Lion answers prefix queries by checking every row, so GIN wins them.

## Ranked search: top 10 by score

Top 10 rows for an OR of words, over 500k synthetic documents of 20–200 words from a Zipf-like
50,000-word vocabulary ([script](../bench/ranking/run.py),
[results](../bench/results/2026-10-08-ranking/RESULTS.md)). GIN finds every match, scores it with
`ts_rank` and sorts. Lion's `LionBm25` scan scores from the index and skips rows that can't reach
the top 10.

| Query | Matches | Lion BM25 vs GIN + ts_rank |
| --- | --- | --- |
| One common word and two rare words | 492,031 | 39× faster |
| Two mid-frequency words | 32,934 | 27× faster |
| Three common words | 499,985 | 11× faster |
| Two rare words | 584 | **3.7× slower** |
| Two rare words ANDed | 18 | about the same |

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
