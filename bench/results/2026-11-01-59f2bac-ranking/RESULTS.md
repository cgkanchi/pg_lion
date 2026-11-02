# Ranked full-text search: lion BM25 vs GIN + ts_rank

`python3 bench/ranking/run.py --dsn 'dbname=rank' --docs 500000 --repeats 9`, extension code at `59f2bac`, PostgreSQL 18.6 (packaged, `-O2`), shared buffers 2 GB, `work_mem` 64 MB, parallel query and JIT off, `pg_lion` preloaded. Ryzen 7 5700X3D, 8 cores, 23 GB RAM, WSL2. Warm cache, median of 9 runs after one warmup, each on a new connection, EXPLAIN ANALYZE execution time.

```
{"query": "two rare words", "tsquery": "w20000 | w31000", "matches": 584, "gin_ms": 3.25, "lion_ms": 3.742, "lion_scan": false}
{"query": "one common, two rare words", "tsquery": "w3 | w20000 | w31000", "matches": 492031, "gin_ms": 622.131, "lion_ms": 11.907, "lion_scan": true}
{"query": "two mid-frequency words", "tsquery": "w300 | w700", "matches": 32934, "gin_ms": 97.602, "lion_ms": 6.884, "lion_scan": true}
{"query": "three common words", "tsquery": "w2 | w5 | w9", "matches": 499985, "gin_ms": 685.672, "lion_ms": 52.171, "lion_scan": true}
{"query": "rare AND", "tsquery": "w2000 & w3000", "matches": 18, "gin_ms": 0.563, "lion_ms": 0.569, "lion_scan": false}
```
