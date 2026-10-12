# pg_lion benchmarks

Times and relative speeds from a synthetic benchmark. Absolute times depend on the machine; the
ratios carry over better. "5× faster" means the other index took five times as long as lion. Times
are milliseconds of query execution.

**Setup.** The extension at `3947945`, PostgreSQL 18.6 (built from source with `-O2`), 4 vCPUs,
16 GB RAM, focused profile. Lion preloaded, so its indexes use lion's own WAL format. Shared buffers
512 MB, `work_mem` 64 MB, parallel query and JIT off (index builds may still use parallel workers),
warm cache. Each query is the median of three timed runs after one warmup, measured as EXPLAIN
ANALYZE execution time. Every result is checked against a sequential scan, and all 394 checks
passed. The default planner chooses the plan.

**Data.** The scalar table has 5M rows (1M-row results are in the full report and show the same
pattern; where lion and B-tree are close, the winner can differ between the two sizes) and one single-column index per column: B-tree, GIN (`btree_gin`) or lion.
Columns hold 2, 20, 200, 20,000 and 1M distinct values, plus a skewed column (90% one value) and a
column that is 10% NULL. The documents table has 200k rows with a `text[]` and a `tsvector`, indexed
by GIN or lion (`store_positions = true`).

Full report: [REPORT.md](../bench/results/2026-10-11-3947945-pg18-focused/REPORT.md) ·
[HTML](../bench/results/2026-10-11-3947945-pg18-focused/index.html) ·
[CSV](../bench/results/2026-10-11-3947945-pg18-focused/summary.csv).

## Counts (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 0.34 | 133 | 703 | 392× faster | 2,073× faster |
| Count the hot value of a skewed column (90%) | 0.66 | 245 | 643 | 374× faster | 981× faster |
| Count a value in 0.5% of rows | 0.14 | 1.35 | 62.9 | 9.7× faster | 453× faster |
| Count a value in 0.005% of rows | 0.043 | 0.044 | 0.90 | same | 21× faster |
| Count a value of a 1M-value column | 0.025 | 0.066 | 0.39 | 2.6× faster | 15× faster |
| Two equality filters ANDed | 0.87 | 27.6 | 195 | 32× faster | 224× faster |
| Three selective equality filters ANDed | 0.098 | 1.96 | 2.85 | 20× faster | 29× faster |
| Two `IN` lists ANDed | 2.84 | 84.4 | 125 | 30× faster | 44× faster |
| Two columns ORed | 1.12 | 302 | 411 | 270× faster | 367× faster |
| `IN` list of 10 values | 0.11 | 0.19 | 8.93 | 1.7× faster | 81× faster |
| `IN` list of 1,000 values | 10.6 | 14.4 | 363 | 1.4× faster | 34× faster |
| `IS NULL` (10% of rows) | 0.58 | 34.8 | 479 | 60× faster | 821× faster |
| Range over 100 of 20,000 values | 1.05 | 1.46 | 512 | 1.4× faster | 486× faster |

Lion's lead grows with the number of rows a count matches. On the most selective values, a few to a
few hundred rows, all three finish in under a millisecond and B-tree is within a few hundredths of
a millisecond of lion: at parity for the 0.005% value.

## GROUP BY (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count per group, 2 groups | 1.26 | 328 | 813 | 260× faster | 645× faster |
| Count per group, 200 groups | 25.1 | 330 | 940 | 13× faster | 37× faster |
| Count per group, 20 groups, with a filter | 20.1 | 36.7 | 88.9 | 1.8× faster | 4.4× faster |

## Unvacuumed tables (5M rows)

The same counts after updating 1% of rows, scattered, without a VACUUM. Lion has to check the
affected pages in the heap, so its lead shrinks. It doesn't disappear.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 116 | 616 | 820 | 5.3× faster | 7.1× faster |
| Count a value in 0.5% of rows | 11.7 | 12.8 | 29.9 | 1.1× faster | 2.6× faster |
| Two equality filters ANDed | 6.55 | 79.9 | 157 | 12× faster | 24× faster |
| Count per group, 200 groups | 480 | 908 | 1,015 | 1.9× faster | 2.1× faster |

## Fetching rows (5M rows, vacuumed)

Queries that read the matching rows (`sum(id), sum(length(payload))`), so every index has to visit
the heap. Lion is close to B-tree here, from 1.5× faster to 1.3× slower: slower on the single key
of the 1M-value column and on the value in 0.5% of rows, faster or the same on the rest. It is
faster than GIN in every case.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| A value of a 1M-value column | 0.091 | 0.070 | 0.23 | **1.3× slower** | 2.5× faster |
| A value in 0.005% of rows | 0.66 | 0.99 | 0.78 | 1.5× faster | 1.2× faster |
| A value in 0.5% of rows | 32.8 | 28.0 | 72.9 | **1.2× slower** | 2.2× faster |
| A value in 5% of rows | 385 | 518 | 430 | 1.3× faster | 1.1× faster |
| Two filters ANDed | 16.4 | 17.7 | 29.3 | 1.1× faster | 1.8× faster |
| Three filters ANDed | 16.1 | 19.6 | 27.3 | 1.2× faster | 1.7× faster |
| Two `IN` lists ANDed | 50.3 | 52.2 | 103 | same | 2.0× faster |

## Ordered queries with LIMIT (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | Lion vs B-tree |
| --- | --- | --- | --- |
| `WHERE a = .. AND b = .. ORDER BY c LIMIT 10` (0.25% match) | 1.67 | 15.4 | 9.2× faster |
| The same, `ORDER BY c DESC` | 1.63 | 12.4 | 7.6× faster |
| `WHERE b = .. ORDER BY c LIMIT 10` (50% match) | 0.10 | 0.10 | same |
| `WHERE c >= .. ORDER BY c LIMIT 100` on one column | 0.42 | 0.028 | **15× slower** |

Lion walks the `ORDER BY` column's index in order and skips rows its filter rules out
(`LionOrdered`). A B-tree's index-only scan on the one column wins the last case: keep a B-tree for
plain "first N by this column".

## Arrays and full-text search (200k documents, vacuumed)

| Query | Lion ms | GIN ms | Lion vs GIN |
| --- | --- | --- | --- |
| Count `tags @> '{t1}'` (5% of rows) | 0.047 | 8.96 | 191× faster |
| Count `tags @> '{t1,t17}'` | 0.12 | 1.26 | 11× faster |
| Count `tags && '{t1,t17,t123}'` | 0.14 | 13.6 | 94× faster |
| Count `tsv @@ 'common'` (nearly every row) | 0.13 | 41.4 | 331× faster |
| Count `tsv @@ 'w1 & w17'` | 0.11 | 1.37 | 12× faster |
| Count `tsv @@ '(w1 \| w2) & (w17 \| w18)'` | 0.37 | 5.32 | 14× faster |
| Count a rare word | 0.030 | 0.073 | 2.4× faster |
| Count a phrase, `common <-> w1` | 4.58 | 16.1 | 3.5× faster |
| Count a weight, `w1:D` | 0.63 | 12.1 | 19× faster |
| Count a NOT, `common & !w1` | 13.3 | 48.6 | 3.7× faster |
| Count a prefix, `rare12:*` (111 words) | 0.97 | 3.23 | 3.3× faster |
| Fetch the rows matching `w1` | 8.96 | 9.79 | 1.1× faster |

Lion answers a prefix as the OR of the indexed words that start with it, up to 1000 words. A prefix
that matches more words is rechecked row by row; this benchmark does not measure that case.

## Ranked search: top 10 by score

Top 10 rows for an OR of words, over 500k synthetic documents of 20–200 words from a Zipf-like
50,000-word vocabulary ([script](../bench/ranking/run.py),
[results](../bench/results/2026-10-11-ranking/RESULTS.md)). GIN finds every match, scores it with
`ts_rank` and sorts. Lion's `LionBm25` scan scores from the index and skips rows that can't reach
the top 10. For the AND query the planner does not pick the ranked scan, so both sides are a bitmap
scan and a sort.

| Query | Matches | Lion BM25 ms | GIN + ts_rank ms | Lion vs GIN |
| --- | --- | --- | --- | --- |
| One common word and two rare words | 492,031 | 17.1 | 857 | 50× faster |
| Two mid-frequency words | 32,934 | 6.82 | 112 | 16× faster |
| Three common words | 499,985 | 57.0 | 955 | 17× faster |
| Two rare words | 584 | 4.83 | 2.06 | **2.3× slower** |
| Two rare words ANDed | 18 | 0.74 | 0.67 | **1.1× slower** |

With only a few hundred matches, scoring them all is cheaper than lion's ranked walk, but the
planner still picks the walk. Set `pg_lion.enable_bm25_scan = off` for queries you know are this
selective.

## Size, build and writes (5M rows, all seven indexes)

| | B-tree | GIN | Lion |
| --- | --- | --- | --- |
| Index size | 256 MiB | 157 MiB | 113 MiB |
| Build time | 10.4 s | 9.8 s | 5.8 s |
| Insert 50k rows | 1.1 s, 120 MiB WAL | 0.4 s, 61 MiB WAL | 1.6 s, 104 MiB WAL |
| Update an indexed column in 50k rows | 2.5 s, 139 MiB WAL | 0.9 s, 78 MiB WAL | 2.2 s, 128 MiB WAL |
| Delete 50k rows | 1.7 s | 1.3 s | 1.2 s |
| VACUUM | 2.8 s | 3.3 s | 2.7 s |

Lion is 56% smaller than B-tree and 28% smaller than GIN here, and builds in a little over half
B-tree's time. Inserts are about 1.5× slower than B-tree but write about 13% less WAL; an indexed
update is slightly faster than B-tree's and writes about 7% less WAL. GIN's writes look cheap
because it defers work to its pending list.

On the documents table, lion's `tsvector` index with positions is 2.3× the size of GIN's
(9.8 vs 4.3 MiB). Lion's `text[]` index is slightly smaller than GIN's (1.9 vs 2.1 MiB).

## Reproducing

Install pg_lion into a release PostgreSQL build, then:

```sh
python3 bench/comprehensive/run.py --prefix /path/to/postgresql --preload --output bench/results/my-run
python3 bench/comprehensive/report.py bench/results/my-run      # needs matplotlib
python3 bench/comprehensive/audit.py bench/results/my-run
python3 bench/ranking/run.py --dsn 'dbname=rank' --docs 500000 --repeats 9 # needs pg_lion in that database
```

The suite runs its own temporary cluster and must run as a non-root user. On a 4-vCPU machine it
takes about 7 minutes, and the ranking script about 3. See
[bench/comprehensive/README.md](../bench/comprehensive/README.md) for options.
