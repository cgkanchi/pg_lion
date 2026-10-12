# Ranked full-text search: lion BM25 vs GIN + ts_rank

`python3 bench/ranking/run.py --docs 500000 --repeats 9`, extension code at `3947945`, PostgreSQL
18.6 (built from source with `-O2`), shared buffers 512 MB, `work_mem` 64 MB, parallel query and JIT
off. pg_lion was not in `shared_preload_libraries` on this server; preloading changes only lion's WAL
format, and the timed queries are read-only. 4 vCPU / 16 GB, warm cache, median of 9 runs after one
warmup, EXPLAIN ANALYZE execution time. Run 2026-10-11, 11:07–11:10 UTC.

Top 10 rows by score for each query. GIN: bitmap scan, `ts_rank`, sort. Lion: the planner's
`LionBm25` scan (`ORDER BY lion_bm25_score(...) DESC LIMIT 10`), except the AND query, where the
planner kept the bitmap scan.

| Query | Matches | GIN + ts_rank ms | Lion BM25 ms | Lion vs GIN |
| --- | --- | --- | --- | --- |
| two rare words | 584 | 2.06 | 4.83 | 2.3x slower |
| one common, two rare words | 492,031 | 857.4 | 17.1 | 50x faster |
| two mid-frequency words | 32,934 | 112.2 | 6.8 | 16x faster |
| three common words | 499,985 | 954.8 | 57.0 | 17x faster |
| rare AND | 18 | 0.67 | 0.74 | 1.1x slower |

Raw output (`tsquery`, exact milliseconds, and whether the plan used `LionBm25`):

```
{"query": "two rare words", "tsquery": "w20000 | w31000", "matches": 584, "gin_ms": 2.055, "lion_ms": 4.829, "lion_scan": true}
{"query": "one common, two rare words", "tsquery": "w3 | w20000 | w31000", "matches": 492031, "gin_ms": 857.405, "lion_ms": 17.112, "lion_scan": true}
{"query": "two mid-frequency words", "tsquery": "w300 | w700", "matches": 32934, "gin_ms": 112.164, "lion_ms": 6.821, "lion_scan": true}
{"query": "three common words", "tsquery": "w2 | w5 | w9", "matches": 499985, "gin_ms": 954.822, "lion_ms": 57.046, "lion_scan": true}
{"query": "rare AND", "tsquery": "w2000 & w3000", "matches": 18, "gin_ms": 0.665, "lion_ms": 0.738, "lion_scan": false}
```

A second timing pass over the same table (`--skip-setup`) put lion at 2.6x slower, 61x, 11x and
14x faster, and 1.1x slower on the five queries: the order of the results is stable, the ratios
move by up to about a third between passes.
