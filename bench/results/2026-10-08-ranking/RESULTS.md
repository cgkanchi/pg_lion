# Ranked full-text search: lion BM25 vs GIN + ts_rank

`python3 bench/ranking/run.py --docs 500000 --repeats 9`, extension code at `6bf193b`, PostgreSQL
18.6 (built from source with `-O2`), `shared_preload_libraries = pg_lion`, shared buffers 512 MB,
`work_mem` 64 MB, parallel query and JIT off. 4 vCPU / 16 GB, warm cache, median of 9 runs after one
warmup, EXPLAIN ANALYZE execution time.

Top 10 rows by score for each query. GIN: bitmap scan, `ts_rank`, sort. Lion: the planner's
`LionBm25` scan (`ORDER BY lion_bm25_score(...) DESC LIMIT 10`), except the AND query, where the
planner kept the bitmap scan.

| Query | Matches | GIN + ts_rank ms | Lion BM25 ms | Lion vs GIN |
| --- | --- | --- | --- | --- |
| two rare words | 584 | 3.73 | 7.14 | 1.9x slower |
| one common, two rare words | 492,031 | 986.1 | 18.5 | 53x faster |
| two mid-frequency words | 32,934 | 121.1 | 9.9 | 12x faster |
| three common words | 499,985 | 1,073.6 | 63.2 | 17x faster |
| rare AND | 18 | 0.54 | 0.61 | 1.1x slower |
