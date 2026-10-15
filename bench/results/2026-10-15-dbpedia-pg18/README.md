# lionvec, HNSW, VectorChord and pgvectorscale on DBpedia, PostgreSQL 18 (2026-10-15)

`bench/vector/knn_dbpedia.py`; the data, the queries, the truth, the filters and the method are those
of `bench/results/2026-10-14-lionvec-dbpedia/` (its README), run again on PostgreSQL 18 with tuned
settings, which is how every benchmark runs from here on (a decision of 2026-10-15).

**Machine and settings.** PostgreSQL 18.6 built from source (`REL_18_6` from the GitHub mirror, gcc
13.3, `--with-icu --with-openssl --with-libxml --with-lz4 --with-zstd`), pgvector 0.8.7 built from
source against it (its default `-march=native`), pg_lion 9acb71a built against it (`WERROR=1`),
VectorChord 1.1.1 and pgvectorscale 0.9.1 from their PostgreSQL 18 Debian packages unpacked into the
installation. The same 4 vCPUs (Intel Xeon @ 2.10GHz, AVX-512 with VPOPCNTDQ) and 15 GB of RAM.
`shared_buffers` 2 GB, `work_mem` 64 MB, `maintenance_work_mem` 1 GB, no parallel query or build
(lionvec's build and scan are not parallel, the other three can build in parallel), `jit` off,
`fsync` off, `random_page_cost` 1.1, `effective_io_concurrency` 200, `maintenance_io_concurrency`
200, `effective_cache_size` 10 GB, `io_method` at 18's default `worker` (3 workers), `vchord.so`
preloaded, pgvector's three `vector` distance functions at `COST 100`. Each result file records the
settings it ran under (`meta.settings`).

**Files.** As before: `meta` (the index's build, the commit and library, settings, sizes) and
`results`, one row a method and filter. The table is 7.6 MB and its TOAST 900 MB.

| File | Index | Build | Size |
| --- | --- | --- | --- |
| `lionvec.json` | lionvec, `partitions = 0` (2,715, spill), margins 2 to 16, patience 30 | 161 s | 247 MB |
| `hnsw.json` | HNSW on `embedding::halfvec(1536)`, m 16, ef_construction 64; ef 20 to 400 | 186 s | 450 MB |
| `vchord-l1357.json` | VectorChord `vchordrq`, 1,357 lists; probes 10 to 1,357 | 49 s | 975 MB |
| `vchord-l339.json` | VectorChord `vchordrq`, 339 lists; probes 10 to 320 | 22 s | 940 MB |
| `diskann.json` | pgvectorscale `diskann`, its defaults; search list 10 to 400, rescore 50 | 338 s | 75 MB |
| `cold.json` | the four indexes cold (`knn_dbpedia.py cold`), 7 queries a method | | |

## Warm

Server ms at equal recall (0.95 / 0.98), interpolated along each method's own setting
(`knn_dbpedia.py table`), the four from one sitting (06:55 to 07:25 UTC). Under a filter: lionvec
through the plan the planner takes and through LionOrdered, HNSW through its iterative scan,
VectorChord with `vchordrq.prefilter`, diskann through its streaming scan.

| Filter | lionvec | HNSW | VectorChord, 339 lists | VectorChord, 1,357 lists | diskann |
| --- | --- | --- | --- | --- | --- |
| none | 1.8 / 3.4 | 2.3 / 4.9 | 1.2 / 1.9 | 1.5 / 3.1 | 3.2 at 0.966 (list 10, the smallest run) / 6.6 |
| 10% | 6.5 / 7.8 (the plain scan, the planner's plan) | 5.6 / 12.7 (the planner's plan, iterative) | 2.3 / 3.8 | 3.4 / 5.1 | 0.816 at most |
| 3% | 6.1 / 9.4 through LionOrdered; the planner's plan 12.8 / 16.9 | 10.3 at 0.951 (ef 20), 0.968 at most | 2.9 / 4.0 | 4.3 / 7.5 | 0.746 at most |
| 1% | 5.8, exact (LionOrdered); the planner's sort 6.1, exact | the planner's sort 7.9, exact; iterative 0.967 at most | 6.0 / 9.1 | 6.8 / 8.6 | 0.704 at most |

Against the PostgreSQL 16 sitting of 2026-10-15 05:52 to 06:34 (`2026-10-14-lionvec-dbpedia/`):
lionvec the same (1.8 / 3.4 unfiltered, 6.1 / 9.4 against 6.0 / 8.8 at 3%), VectorChord's 339 lists
back to their better PG 16 sitting (1.2 / 1.9 against 1.4 / 2.5), HNSW about 15% slower (its index
was rebuilt here, 0.945 at ef 40 against 0.951), diskann the same; so 18 itself changes nothing warm
that the noise between sittings does not. What the tuned settings change is the planner: with
`random_page_cost` 1.1 its plan at 3% is the plain lionvec scan with a `Filter` (12.8 ms for 0.95,
844 CODE pages; 16.9 for 0.98) rather than 16's sort of the lion bitmap scan's rows (20 ms, exact),
and still not LionOrdered (6.1 ms, 671 pages), and for HNSW the iterative scan rather than the sort.
At 1% both planners still sort. The standing decision holds: no planner fix, `COST 100` on the
distance functions, and the 3% gap is documented.

## Cold

PostgreSQL restarted and the page cache dropped before each query, two seconds for the flush to
settle, then the query once under `EXPLAIN (ANALYZE, BUFFERS)`: the server's time and the pages it
read from the kernel, the median of 7 queries (07:25 to 07:34 UTC). The recall in brackets is the
warm run's for the same settings.

| Filter | Method | Median ms | Pages read |
| --- | --- | --- | --- |
| none | lionvec margin 3 (0.917) | 25 | 478 |
| none | lionvec margin 7 (0.95) | 52 | 1,314 |
| none | lionvec margin 12 (0.993) | 69 | 1,378 |
| none | HNSW ef 40 (0.945) | 54 | 977 |
| none | HNSW ef 100 (0.976) | 108 | 1,936 |
| none | VectorChord 339 lists, probes 40 (0.966) | 44 | 914 |
| none | VectorChord 339 lists, probes 80 (0.983) | 73 | 1,521 |
| none | VectorChord 339 lists, probes 160 (0.994) | 126 | 2,690 |
| none | diskann list 10 (0.966) | 129 | 2,238 |
| none | diskann list 200 (0.981) | 285 | 4,996 |
| 3% | lionvec, the planner's plan (the plain scan with a Filter, 0.957) | 276 | 4,732 |
| 3% | lionvec LionOrdered margin 3 (0.967) | 76 | 1,607 |
| 3% | lionvec LionOrdered margin 7 (0.99) | 121 | 2,285 |
| 3% | HNSW ef 40 iterative, the planner's plan (0.953) | 325 | 5,381 |
| 3% | HNSW ef 40 iterative, enable_sort off (0.953) | 299 | 5,381 |
| 3% | VectorChord 339 lists, prefilter, probes 40 (0.912) | 96 | 1,864 |
| 3% | VectorChord 339 lists, prefilter, probes 80 (0.960) | 124 | 2,473 |
| 3% | VectorChord 339 lists, prefilter, probes 160 (0.987) | 172 | 3,640 |
| 3% | diskann list 10 (0.746) | 425 | 6,933 |
| 3% | diskann list 200 (0.746) | 487 | 8,102 |
| 1% | lionvec, the planner's plan (sort, exact) | 112 | 2,720 |
| 1% | lionvec LionOrdered (exact) | 101 | 2,721 |
| 1% | HNSW, the planner's plan (sort, exact) | 109 | 2,720 |
| 1% | HNSW ef 40 iterative, enable_sort off (0.962) | 639 | 11,861 |
| 1% | VectorChord 339 lists, prefilter, probes 40 (0.848) | 100 | 1,799 |
| 1% | VectorChord 339 lists, prefilter, probes 80 (0.932) | 130 | 2,475 |
| 1% | VectorChord 339 lists, prefilter, probes 160 (0.976) | 187 | 3,642 |
| 1% | diskann list 10 (0.704) | 564 | 9,272 |
| 1% | diskann list 200 (0.704) | 595 | 9,873 |

Unfiltered the three are close: around 0.95 to 0.97, VectorChord 44 ms and 914 pages, lionvec 52
and 1,314, HNSW 54 and 977; around 0.98 to 0.99, lionvec 69 ms and 1,378 pages for 0.993,
VectorChord 73 and 1,521 for 0.983, HNSW 108 and 1,936 for 0.976. Under the filters lionvec reads
the matching rows' codes and the others walk or probe past the rows the filter rejects: at 3%,
lionvec LionOrdered 76 ms and 1,607 pages for 0.967 against VectorChord's 124 and 2,473 for 0.960
(1.6 times) and HNSW's 299 and 5,381 (3.9 times); at 1%, lionvec exact in 101 ms and 2,721 pages
against VectorChord's 187 and 3,642 for 0.976 (1.8 times) and HNSW's 639 for 0.962. diskann reads
2 to 4 times lionvec's pages everywhere and its filtered recall stays at its ceilings. The planner's
3% plan for lionvec costs 3.6 times LionOrdered cold (276 ms, 4,732 pages), as the plain scan probes
on through partitions whose rows the filter rejects. The same table on PostgreSQL 16 (stock knobs)
had lionvec and HNSW only: lionvec 50 ms and 1,314 pages for 0.95 unfiltered against HNSW's 50 and
926, 64 against 293 at 3%, 90 against 539 at 1%; the 18 numbers for the two are within the disk's
variation of those.

## Re-running

PostgreSQL 18 built from source into a prefix of its own (here `/home/claude/pg18`; its `pg_config`
puts extensions under `share/postgresql/extension` and `lib/postgresql`), pgvector and pg_lion built
with `PG_CONFIG` pointing at it, VectorChord's and pgvectorscale's PostgreSQL 18 Debian packages
unpacked (`dpkg-deb -x`) and their `.so` and `extension/*` files copied into those two directories,
`.local/pg` pointed at the prefix, then:

    export LION_SOCK=/tmp/lion-sock-d LION_PORT=55435
    pgdev reset              # then append the settings above to .local/data/postgresql.conf
    pgdev restart
    eval "$(./dev.sh env)"
    KNN_CACHE=... python3 bench/vector/knn_dbpedia.py load     # the cache of the 16 run: X.npy, qidx.npy, truth.npz, u.npy
    psql -d dbpedia -c "CREATE EXTENSION vchord" -c "CREATE EXTENSION vectorscale"
    python3 bench/vector/knn_dbpedia.py hnsw
    python3 bench/vector/knn_dbpedia.py lionvec 0
    python3 bench/vector/knn_dbpedia.py diskann
    python3 bench/vector/knn_dbpedia.py vchord 1357
    KNN_PROBES=10,20,40,80,160,320,640,1357 python3 bench/vector/knn_dbpedia.py warm vchord vchord-l1357
    python3 bench/vector/knn_dbpedia.py vchord 339
    python3 bench/vector/knn_dbpedia.py warm lionvec lionvec
    python3 bench/vector/knn_dbpedia.py warm hnsw hnsw
    python3 bench/vector/knn_dbpedia.py warm vchord vchord-l339
    python3 bench/vector/knn_dbpedia.py warm diskann diskann
    python3 bench/vector/knn_dbpedia.py table bench/results/2026-10-15-dbpedia-pg18/*.json
    KNN_RESTART="runuser -u pglion -- env LION_SOCK=/tmp/lion-sock-d LION_PORT=55435 ./dev.sh restart" \
      KNN_COLD_VCHORD=40,80,160 KNN_COLD_DISKANN=10,200 python3 bench/vector/knn_dbpedia.py cold   # as root

with `KNN_OUT` set to this directory and `KNN_SRC` to the checkout built, so that the files record
its commit.
