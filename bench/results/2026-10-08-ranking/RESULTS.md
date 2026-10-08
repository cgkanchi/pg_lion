# Ranked full-text search: lion BM25 vs GIN + ts_rank

`python3 bench/ranking/run.py --docs 500000`, extension code at `6bf193b`, PostgreSQL 16.15 (release build),
4 vCPU / 16 GB, warm cache, median of 5 runs after one warmup, EXPLAIN ANALYZE execution time.
Top 10 rows by score for each query. GIN: bitmap scan, `ts_rank`, sort. Lion: the planner's
`LionBm25` scan (`ORDER BY lion_bm25_score(...) DESC LIMIT 10`), except the AND query, where the
planner kept the bitmap scan.

| Query | Matches | GIN + ts_rank ms | Lion BM25 ms | Lion speedup |
| --- | --- | --- | --- | --- |
| two rare words | 584 | 1.7 | 6.3 | 0.3x |
| one common, two rare words | 492,031 | 638.8 | 16.6 | 38.6x |
| two mid-frequency words | 32,934 | 244.4 | 9.0 | 27.0x |
| three common words | 499,985 | 662.0 | 59.4 | 11.1x |
| rare AND | 18 | 0.5 | 0.5 | 1.0x |
