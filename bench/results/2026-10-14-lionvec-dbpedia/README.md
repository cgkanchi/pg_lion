# lionvec and HNSW on DBpedia, text-embedding-3-large (2026-10-14)

`bench/vector/knn_dbpedia.py`, DESIGN.md §44.9 ("DBpedia").

**Data.** Shards 0-2 of the Hugging Face dataset
`Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-1M`: 115,386 DBpedia abstracts embedded
by OpenAI's text-embedding-3-large at 1536 dimensions (unit length to within 1.4e-7). The queries
are 200 rows, `np.random.default_rng(0).choice(115386, 200, replace=False)` over the shards in
sorted filename order, held out of the table, so the table `dbpedia(id int primary key, u float8,
embedding vector(1536))` has 115,186 rows (id = the row's place in the shards). `u` is
`np.random.default_rng(42).random(115386)[id]`, with a lion index on it, so `u < s` keeps a share s
unrelated to the vectors: 11,377 rows at 10%, 3,472 at 3%, 1,204 at 1%. The truth is the exact ten
nearest by cosine among the table's rows (the float4 values it holds, in float64), under each
filter; no query's tenth and eleventh lie within 2.8e-5 of each other.

**Machine and settings.** PostgreSQL 16.15, pgvector 0.8.7, 4 vCPUs (Intel Xeon @ 2.10GHz,
AVX-512 with VPOPCNTDQ; lionvec uses its POPCNT kernel), 15 GB RAM, `shared_buffers` 2 GB (which
holds every relation here), `work_mem` 64 MB, `maintenance_work_mem` 1 GB (so the k-means sample
is every row), no parallel query, `jit` off, pgvector's three `vector` distance functions at
`COST 100`. Warm: every relation `pg_prewarm`ed into shared buffers, each method's 200 queries run
once untimed first, then timed from the client (`median_ms`, `p95_ms`: parse, plan, execute and
fetch), then once more under `EXPLAIN (ANALYZE, TIMING OFF)` for the server's execution time
(`exec_median_ms`). `LIMIT 10`; recall is the share of the true ten returned. The partitions
probed and CODE pages read are the means of the scan's `DEBUG1` line.

**Files.** Each holds `meta` (the index's build: DDL, seconds, size, partitions, sample, k-means
iterations; the commit and library it was built and run with; settings; sizes) and `results`, one
row a method and filter:

| File | Index | Built from | Partitions | Build | Size |
| --- | --- | --- | --- | --- | --- |
| `hnsw.json` | HNSW on `embedding::halfvec(1536)`, m 16, ef_construction 64 | 1de7d73 (d498fba) | | 187 s | 450 MB |
| `lionvec-p678.json` | lionvec, `partitions = 678` (= the default then, 2 sqrt(n)) | 1de7d73 (d498fba) | 678 | 161 s | 210 MB |
| `lionvec-p2715.json` | lionvec, `partitions = 2715` (8 sqrt(n)) | 1de7d73 (d498fba) | 2,715 | 347 s | 221 MB |

The table is 7.6 MB and its TOAST 900 MB. 1de7d73 is the last commit to `src/` on the
`claude/lionvec` branch, whose head d498fba these ran on. An index built with `partitions = 0` on
that code had the same 678 partitions and gave the same numbers, query for query (build 159 s).
lionvec's builds ran all ten k-means iterations at both sizes. 2715 is `(int) (8 * sqrt(115186))`,
what the new default chooses, so that the default's run can be compared exactly.

## Warm, the planner's plans

Median server time in ms (client median, 95th percentile), recall, and for lionvec the partitions
probed / CODE pages read; patience 30 throughout. The plan: under 10% the plain lionvec scan with a
`Filter` (HNSW: its scan with a `Filter`, iterative); under 3% LionOrdered in 678 partitions but a
sort of the lion bitmap scan's rows in 2,715, and for HNSW a sort; under 1% a sort of the 1,204
rows for both (exact).

| Filter | lionvec 678, margin 3 | margin 2 | margin 4 | lionvec 2,715, margin 3 | margin 2 | margin 4 | HNSW ef 40 | HNSW ef 100 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| none | 1.1 (2.1, 3.8), 0.843, 15 / 88 | 0.9 (2.2, 3.4), 0.809, 10 / 60 | 1.4 (2.4, 4.3), 0.881, 22 / 124 | 1.4 (2.6, 4.5), 0.835, 37 / 68 | 1.2 (2.3, 3.6), 0.799, 25 / 47 | 1.6 (2.6, 4.5), 0.870, 55 / 99 | 1.9 (2.9, 3.6), 0.951 | 4.1 (5.1, 6.6), 0.979 |
| 10% | 7.8 (8.4, 14.8), 0.879, 55 / 301 | 5.4 (6.3, 10.7), 0.838, 38 / 212 | 6.1 (7.0, 11.3), 0.910, 75 / 411 | 5.4 (7.2, 11.7), 0.872, 129 / 226 | 5.3 (6.3, 10.5), 0.825, 100 / 176 | 5.7 (7.0, 11.2), 0.897, 151 / 264 | 4.7 (6.1, 9.3), 0.930 | 83 (84, 104), 1.000: sort |
| 3% | 5.0 (5.9, 8.0), 0.917, 101 / 551 | 4.0 (5.3, 7.7), 0.878, 73 / 397 | 5.6 (6.6, 9.4), 0.949, 140 / 756 | 20 (22, 29), 1.000: sort | 20 (22, 30), sort | 17 (17, 27), sort | 25 (29, 39), 1.000: sort | 28 (28, 38), sort |
| 1% | 5.3 (6.3, 7.7), 1.000: sort | | | 5.5 (6.4, 9.5), 1.000: sort | | | 8.0 (8.6, 12), 1.000: sort | 8.3 (8.7, 10.5), sort |

The index's own paths where the planner sorted (`enable_sort = off`, and `enable_indexscan = off`
for LionOrdered): lionvec 2,715 at 3% through LionOrdered, margin 3, 5.8 ms (6.9, 9.8), 0.908, 280
/ 487; under 1% LionOrdered computes all 1,204 members' distances, 5.6 ms (678) and 5.5 ms (2,715),
recall 1; HNSW's iterative scan at 3% 10.7 ms (ef 40, 0.953) and 11.1 ms (ef 100, 0.957), at 1%
26.7 ms (0.959) and 28.8 ms (0.960), at 10% ef 100 5.2 ms (0.945).

Times vary between runs of the same index by up to 2.5 ms here (678 partitions at 10%, margin 3:
5.2 ms in an earlier run of the same index, 7.8 in this file's; unfiltered within 0.5 ms); recall,
partitions probed and pages read are the same from run to run.

## Equal recall, along the margins

The default margin of 3 is far short of ada-002's recall here (0.84 against 0.98), so every lionvec
run went on through margins 6, 8, 12 and 16, and at patiences 30 and 120 (`pg_lion.vector_patience`
counts partitions' worth of rows: 30 in 2,715 partitions is the rows of 7.5 in 678, 120 the rows of
30). Interpolated between margins, unfiltered; server ms; rows scored is about the partitions times
the rows a partition holds (170 and 42):

| Recall | Partitions, patience | CODE pages | Partitions probed | Rows scored, about | Server ms |
| --- | --- | --- | --- | --- | --- |
| 0.95 | 678, 30 (margin 7.1) | 358 | 65 | 11,000 | 2.4 |
| 0.95 | 678, 120 | 383 | 70 | 11,900 | 2.6 |
| 0.95 | 2,715, 30 | 234 | 134 | 5,700 | 2.9 |
| 0.95 | 2,715, 120 | 310 | 177 | 7,500 | 2.8 |
| 0.98 | 678, 30 (margin 10.8) | 578 | 106 | 18,000 | 3.2 |
| 0.98 | 678, 120 | 916 | 169 | 28,700 | 4.6 |
| 0.98 | 2,715, 30 | not reached: 0.966 at margins 12 and 16 | | | |
| 0.98 | 2,715, 120 | 566 | 327 | 13,900 | 4.5 |

So the finer partitions read 1.5 times fewer CODE pages at 0.95 (1.9 times fewer rows, against the
numpy model's 1.78) and about as many at 0.98, and are no faster at 115,186 rows: each query ranks
four times the centroids (85 pages of them), and a partition of 42 rows spans two 35-row pages, so a
probe reads 1.75 pages for 1.2 pages of rows. Under the filters, through LionOrdered at 3%: 0.95
took 771 pages in 678 partitions (5.6 ms) and 646 in 2,715 (7.2 ms); through the plain scan at
10%, 589 (6.7 ms) and, at patience 120, 585 (8.3 ms). `knn_dbpedia.py table` prints all of it.

## Re-running

    export LION_SOCK=/tmp/lion-sock-a LION_PORT=54329; eval "$(./dev.sh env)"
    python3 bench/vector/knn_dbpedia.py prepare         # once: shards -> .local/dbpedia
    python3 bench/vector/knn_dbpedia.py load            # once: database dbpedia
    python3 bench/vector/knn_dbpedia.py hnsw            # once
    python3 bench/vector/knn_dbpedia.py lionvec 0       # (re)build at the default
    python3 bench/vector/knn_dbpedia.py warm lionvec NAME     # margins 2-16, patience 30 and 120
    python3 bench/vector/knn_dbpedia.py warm hnsw hnsw
    python3 bench/vector/knn_dbpedia.py table bench/results/2026-10-14-lionvec-dbpedia/*.json
