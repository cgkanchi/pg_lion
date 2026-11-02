# pg_lion benchmarks

Times and relative speeds from a synthetic benchmark. Absolute times depend on the machine; the
ratios carry over better. "5× faster" means the other index took five times as long as lion. Times
are milliseconds of query execution.

**Setup.** The extension at `59f2bac` (so `pg_lion.recheck_group_tid_cost` at 1; the default of 2 picks the same plan for the one query it affects), PostgreSQL 18.6 (packaged, `-O2`), an 8-core Ryzen 7 5700X3D
under WSL2 with 23 GB RAM, focused profile. Lion preloaded, so its indexes use lion's own WAL format.
Shared buffers 2 GB, which holds the tables and every index, `work_mem` 64 MB, parallel query and JIT
off (index builds may still use parallel workers), warm cache. Each query is the median of seven
timed runs after two warmups, measured as EXPLAIN ANALYZE execution time. Every result is checked against a sequential scan, and all 394 checks
passed. The default planner chooses the plan.

**Data.** The scalar table has 5M rows (1M-row results are in the full report and show the same
pattern; where lion and B-tree are close, the winner can differ between the two sizes) and one single-column index per column: B-tree, GIN (`btree_gin`) or lion.
Columns hold 2, 20, 200, 20,000 and 1M distinct values, plus a skewed column (90% one value) and a
column that is 10% NULL. The documents table has 200k rows with a `text[]` and a `tsvector`, indexed
by GIN or lion (`store_positions = true`).

Full report: [REPORT.md](../bench/results/2026-11-01-59f2bac-pg18-focused/REPORT.md) ·
[HTML](../bench/results/2026-11-01-59f2bac-pg18-focused/index.html) ·
[CSV](../bench/results/2026-11-01-59f2bac-pg18-focused/summary.csv).

## Counts (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 0.21 | 168 | 515 | 787× faster | 2,419× faster |
| Count the hot value of a skewed column (90%) | 0.23 | 312 | 486 | 1,368× faster | 2,130× faster |
| Count a value in 0.5% of rows | 0.13 | 1.67 | 22.6 | 13× faster | 174× faster |
| Count a value in 0.005% of rows | 0.025 | 0.035 | 0.27 | 1.4× faster | 11× faster |
| Count a value of a 1M-value column | 0.017 | 0.022 | 0.036 | 1.3× faster | 2.1× faster |
| Two equality filters ANDed | 0.43 | 19.8 | 205 | 46× faster | 477× faster |
| Three selective equality filters ANDed | 0.073 | 1.77 | 3.24 | 24× faster | 44× faster |
| Two `IN` lists ANDed | 2.42 | 44.1 | 92.5 | 18× faster | 38× faster |
| Two columns ORed | 0.94 | 111 | 124 | 119× faster | 132× faster |
| `IN` list of 10 values | 0.10 | 0.19 | 2.82 | 1.8× faster | 27× faster |
| `IN` list of 1,000 values | 9.04 | 17.6 | 119 | 2.0× faster | 13× faster |
| `IS NULL` (10% of rows) | 0.24 | 32.3 | 333 | 133× faster | 1,371× faster |
| Range over 100 of 20,000 values | 0.88 | 1.70 | 288 | 1.9× faster | 328× faster |

Lion's lead grows with the number of rows a count matches. On the most selective values, a few to a
few hundred rows, all three finish in under a millisecond and B-tree is within a few hundredths of
a millisecond of lion.

## GROUP BY (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count per group, 2 groups | 0.45 | 439 | 724 | 985× faster | 1,624× faster |
| Count per group, 200 groups | 21.6 | 443 | 843 | 21× faster | 39× faster |
| Count per group, 20 groups, with a filter | 13.8 | 21.0 | 23.9 | 1.5× faster | 1.7× faster |

## Unvacuumed tables (5M rows)

The same counts after updating 1% of rows, scattered, without a VACUUM. Lion has to check the
affected pages in the heap, so its lead shrinks. It doesn't disappear.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| Count a value in 50% of rows | 86.2 | 372 | 525 | 4.3× faster | 6.1× faster |
| Count a value in 0.5% of rows | 3.91 | 7.45 | 24.1 | 1.9× faster | 6.2× faster |
| Two equality filters ANDed | 3.39 | 17.4 | 212 | 5.1× faster | 62× faster |
| Count per group, 200 groups | 334 | 854 | 876 | 2.6× faster | 2.6× faster |

## Fetching rows (5M rows, vacuumed)

Queries that read the matching rows (`sum(id), sum(length(payload))`), so every index has to visit
the heap. Lion is level with B-tree on the single key of the 1M-value column and up to 1.8× faster on
the rest, and faster than GIN in every case.

| Query | Lion ms | B-tree ms | GIN ms | Lion vs B-tree | Lion vs GIN |
| --- | --- | --- | --- | --- | --- |
| A value of a 1M-value column | 0.031 | 0.031 | 0.042 | same | 1.4× faster |
| A value in 0.005% of rows | 0.21 | 0.27 | 0.29 | 1.2× faster | 1.4× faster |
| A value in 0.5% of rows | 10.9 | 19.7 | 24.9 | 1.8× faster | 2.3× faster |
| A value in 5% of rows | 83.7 | 117 | 133 | 1.4× faster | 1.6× faster |
| Two filters ANDed | 11.9 | 13.8 | 30.8 | 1.2× faster | 2.6× faster |
| Three filters ANDed | 12.3 | 15.1 | 31.7 | 1.2× faster | 2.6× faster |
| Two `IN` lists ANDed | 37.1 | 42.5 | 93.8 | 1.1× faster | 2.5× faster |

## Ordered queries with LIMIT (5M rows, vacuumed)

| Query | Lion ms | B-tree ms | Lion vs B-tree |
| --- | --- | --- | --- |
| `WHERE a = .. AND b = .. ORDER BY c LIMIT 10` (0.25% match) | 1.31 | 4.99 | 3.8× faster |
| The same, `ORDER BY c DESC` | 1.17 | 3.43 | 2.9× faster |
| `WHERE b = .. ORDER BY c LIMIT 10` (50% match) | 0.040 | 0.041 | same |
| `WHERE c >= .. ORDER BY c LIMIT 100` on one column | 0.073 | 0.020 | **3.6× slower** |

Lion walks the `ORDER BY` column's index in order and skips rows its filter rules out
(`LionOrdered`). A B-tree's index-only scan on the one column wins the last case: keep a B-tree for
plain "first N by this column".

## Arrays and full-text search (200k documents, vacuumed)

| Query | Lion ms | GIN ms | Lion vs GIN |
| --- | --- | --- | --- |
| Count `tags @> '{t1}'` (5% of rows) | 0.031 | 4.35 | 140× faster |
| Count `tags @> '{t1,t17}'` | 0.073 | 1.05 | 14× faster |
| Count `tags && '{t1,t17,t123}'` | 0.11 | 7.20 | 67× faster |
| Count `tsv @@ 'common'` (nearly every row) | 0.039 | 31.8 | 816× faster |
| Count `tsv @@ 'w1 & w17'` | 0.075 | 1.14 | 15× faster |
| Count `tsv @@ '(w1 \| w2) & (w17 \| w18)'` | 0.18 | 3.86 | 22× faster |
| Count a rare word | 0.019 | 0.038 | 2.0× faster |
| Count a phrase, `common <-> w1` | 2.52 | 7.60 | 3.0× faster |
| Count a weight, `w1:D` | 0.44 | 5.40 | 12× faster |
| Count a NOT, `common & !w1` | 9.54 | 39.0 | 4.1× faster |
| Count a prefix, `rare12:*` (111 words) | 0.63 | 1.72 | 2.7× faster |
| Fetch the rows matching `w1` | 2.62 | 4.83 | 1.8× faster |

Lion answers a prefix as the OR of the indexed words that start with it, up to 1000 words. A prefix
that matches more words is rechecked row by row; this benchmark does not measure that case.

## Ranked search: top 10 by score

Top 10 rows for an OR of words, over 500k synthetic documents of 20–200 words from a Zipf-like
50,000-word vocabulary ([script](../bench/ranking/run.py),
[results](../bench/results/2026-11-01-59f2bac-ranking/RESULTS.md)). GIN finds every match, scores it
with `ts_rank` and sorts. Lion's `LionBm25` scan scores from the index and skips rows that can't
reach the top 10. Each run is on a new connection.

| Query | Matches | Lion BM25 ms | GIN + ts_rank ms | Lion vs GIN |
| --- | --- | --- | --- | --- |
| One common word and two rare words | 492,031 | 11.9 | 622 | 52× faster |
| Two mid-frequency words | 32,934 | 6.88 | 97.6 | 14× faster |
| Three common words | 499,985 | 52.2 | 686 | 13× faster |
| Two rare words | 584 | 3.74 | 3.25 | **1.2× slower** |
| Two rare words ANDed | 18 | 0.57 | 0.56 | same |

When the query's words together hold under about 0.75% of the rows, as in the last two queries, sorting
the matches beats the ranked walk, so lion does not offer it (`pg_lion.bm25_walk_min_share`, less when
the tables and index outgrow `effective_cache_size`), and both sides are a scan and a sort.

## Nearest neighbours: lionvec against HNSW

The ten nearest of a query by cosine distance, `ORDER BY embedding <=> $1 LIMIT 10`, over 975,000
OpenAI ada-002 embeddings of DBpedia entities (1536 dimensions; Qdrant's
`dbpedia-openai-1M-1536-angular` set and the first 100 of its test queries), optionally under
`WHERE u < s`, a uniform random column with a lion index that keeps a share `s` of the rows
([script](../bench/vector/knn.py), [results](../bench/results/2026-10-14-lionvec-partitions/)).
lionvec built with `partitions = 1974` (`2 sqrt(n)`) and `spill = off` (the defaults are
`8 sqrt(n)` partitions with each row in a second partition as well, `spill = on`: on 115,000
text-embedding-3-large rows those read 1.8 times fewer code pages for 0.95 recall, in the same
time, and 1.25 times fewer for 0.98, taking 1.3 times longer, and make the index about 18% larger;
[results](../bench/results/2026-10-14-lionvec-dbpedia/)), against pgvector 0.8.6's HNSW on
`embedding::halfvec(1536)` (m 16, ef_construction 64). PostgreSQL 16, 4 vCPUs, 16 GB RAM,
`shared_buffers` 5 GB, and pgvector's `vector` distance functions at `COST 100`, as lionvec's setup
asks. Recall is the share of the true ten returned, against an exact numpy search. Both indexes
are approximate.

**Warm** (everything in memory), median of 100 queries, 95th percentile and recall. lionvec at its
defaults (margin 3, patience 30), at margins 2 and 4, and with the pause off:

| Filter | lionvec (defaults) | margin 2 | margin 4 | no pause | HNSW ef_search 40 | HNSW ef_search 100 |
| --- | --- | --- | --- | --- | --- | --- |
| none | 7.9 ms (16), 0.979 | 6.6 (15), 0.964 | 9.1 (18), 0.983 | 14 (68), 0.984 | 4.7 (6.5), 0.900 | 8.1 (11), 0.954 |
| 10% | 13.5 (29), 0.978 | 15 (29), 0.974 | 15 (28), 0.979 | 60 (123), 0.996 | 8.7 (14), 0.931 | 8.0 (15), 0.939 |
| 1% | 24 (40), 0.981 | 27 (41), 0.981 | 27 (38), 0.981 | 109 (136), 1.000 | 37 (62), 0.947 | 41 (66), 0.950 |
| 0.1% | 9.9 (11), 1.000 | | | | 13 (18), 0.999 | |

HNSW under a filter uses its iterative scan; without it, it returns 3.9 of the ten rows at 10% and
0.4 at 1%. At 10% the planner chose lionvec's plain scan with a `Filter`, at 1% `LionOrdered`,
and at 0.1% a sort of the 953 matching rows, for either index (the plain scan with a `Filter` is
a plan only under `pg_lion.vector_postfilter = allow`, DESIGN.md §44.6).
The pause stops a scan once 30
times `sqrt(n) / 2` rows, here 30 partitions' worth, have brought nothing nearer; without it a query whose nearest rows lie
far from every centroid reads hundreds of partitions (a mean of 254 of the 1,974 unfiltered, 77
with it), and the 95th percentile was five times the median, where with it it is twice.

**Cold** (PostgreSQL restarted and the OS page cache dropped before each query), median of 5:

| Filter | lionvec | HNSW ef_search 40 (iterative under a filter) |
| --- | --- | --- |
| none | 85 ms | 118 ms |
| 1% | 196 ms | 1,254 ms |
| 0.1% | 210 ms | 251 ms (both: the planner sorts the filter's rows) |

**Memory-capped** (a stream of queries, PostgreSQL and its page cache held to 1 or 2 GB by a
memory cgroup with `shared_buffers` at 128 MB, the cache filled first), median of 100 and the
pages each query read from disk:

| Memory | Filter | lionvec | HNSW ef_search 40 (iterative under a filter) |
| --- | --- | --- | --- |
| 1 GB | none | 19 ms, 100 pages | 51 ms, 767 pages |
| 1 GB | 1% | 28 ms, 0 pages | 691 ms, 9,229 pages |
| 2 GB | none | 13 ms, 41 pages | 41 ms, 672 pages |
| 2 GB | 1% | 30 ms, 0 pages | 465 ms, 7,853 pages |

lionvec's working set is the centroids, the one-bit codes of the partitions queries probe, and the
few rows they refine and fetch; under the 1% filter it never filled the cgroup. HNSW reads 650 to
800 random pages a query without a filter and 8,000 to 9,000 under one. This machine's disk
answers a random read in well under a tenth of a millisecond; on slower storage the page counts
are what to compare.

**Size and build.** lionvec is 1.7 GB, of which an unfiltered query reads about 9 MB of one-bit
codes; HNSW is 3.8 GB. lionvec builds in 19 minutes with the table cached, nearly all of it k-means
and the assignment of every row to its nearest of the 1,974 centroids.

Times vary between runs by up to a fifth.

## Size, build and writes (5M rows, all seven indexes)

| | B-tree | GIN | Lion |
| --- | --- | --- | --- |
| Index size | 256 MiB | 157 MiB | 118 MiB |
| Build time | 11.7 s | 8.4 s | 5.4 s |
| Insert 50k rows | 1.1 s, 120 MiB WAL | 0.4 s, 61 MiB WAL | 1.5 s, 104 MiB WAL |
| Update an indexed column in 50k rows | 1.5 s, 139 MiB WAL | 0.6 s, 78 MiB WAL | 1.7 s, 125 MiB WAL |
| Delete 50k rows | 0.7 s | 0.6 s | 0.7 s |
| VACUUM | 1.4 s | 1.8 s | 1.5 s |

Lion is 54% smaller than B-tree and 25% smaller than GIN here, and builds in under half B-tree's
time. Inserts are about 1.4× slower than B-tree but write about 13% less WAL; an indexed update is
about 1.2× slower and writes about 10% less WAL. GIN's writes look cheap because it defers work to
its pending list.

On the documents table, lion's `tsvector` index with positions is 2.3× the size of GIN's
(10.0 vs 4.3 MiB). Lion's `text[]` index is the same size as GIN's (2.1 MiB).

## Reproducing

Install pg_lion into a release PostgreSQL build, then:

```sh
python3 bench/comprehensive/run.py --prefix /path/to/postgresql --preload --repeats 7 --warmups 2 --output bench/results/my-run
python3 bench/comprehensive/report.py bench/results/my-run      # needs matplotlib
python3 bench/comprehensive/audit.py bench/results/my-run
python3 bench/ranking/run.py --dsn 'dbname=rank' --docs 500000 --repeats 9 # needs pg_lion in that database
```

For the nearest-neighbour benchmark, load the vectors into `items(id int, u float8, embedding
vector(1536))` with `u` uniform in [0, 1), build pgvector's HNSW index on
`(embedding::halfvec(1536)) halfvec_cosine_ops`, and save the queries and their exact similarities to
every row as `Q.npy` and `S.npy` (see the script's header). Then:

```sh
python3 bench/vector/knn.py setup     # the lionvec and lion indexes
python3 bench/vector/knn.py warm 100  # needs pg_prewarm
python3 bench/vector/knn.py cold 5    # restarts the cluster; run as root to drop the page cache
# as root, with KNN_CGROUP a memory cgroup directory it may use:
python3 bench/vector/knn.py capped 1024 100   # PostgreSQL held to 1 GB of memory
```

The suite runs its own temporary cluster and must run as a non-root user. It takes about 10 minutes,
and the ranking script about 3. See
[bench/comprehensive/README.md](../bench/comprehensive/README.md) for options.
