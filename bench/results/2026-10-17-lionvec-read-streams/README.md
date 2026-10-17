# lionvec through read streams, on DBpedia, PostgreSQL 18 (2026-10-17)

`bench/vector/knn_readahead.py` on `knn_dbpedia.py`'s DBpedia table, queries and truth: how a
lionvec scan reads ahead (DESIGN.md §44.4, "Prefetch" and "Read streams"; §44.9, "Read streams"),
under `pg_lion.vector_prefetch = off`, `fadvise` (the `PrefetchBuffer()` asks of 2026-10-16) and
`stream` (read streams, the default from 17), against main at 64e7443 (whose `on` is `fadvise`,
without a9a27c5's fix), in three states of the caches and under each of 18's `io_method`s.

**Summary.** From the disk `stream` was faster than `fadvise` on 18 of the 21 query kinds and
`io_method`s, by 6 to 35% query by query, and than `off` on all of them, by 9 to 67%; the exact
path, a list of heap blocks known whole, gained most (its wait on reads 3.5 ms against 19.3 at 1%
under `worker`; 112 ms against 168 at 3% under `io_uring`). From the kernel's page cache `stream`
cost more than `fadvise`, 0 to 40% on the index's pages under `worker` and `io_uring`, and up to
twice on the exact path under `io_uring`: 18 hands each streamed read to an I/O worker or an
io_uring kernel thread, where `ReadBuffer()` copies the page in the backend. Warm, `stream` cost 2
to 11% over `off`, `fadvise` within 5%. Recall and rows were the same in every configuration.

## From the disk (`device`)

The server's median ms over the 10 queries, `off / fadvise / stream`, and in brackets the median of
each query's `stream` time over its `fadvise` time. Every file's own tables
(`knn_readahead.py table`, in `tables.md`) have the quartiles, pages read, the wait on reads and the
disk's MB.

| Query | `worker`, 3 workers | `sync` | `io_uring` | `worker`, 16 workers |
| --- | --- | --- | --- | --- |
| unfiltered, margin 3 | 25.3 / 20.9 / 21.9 (1.12) | 25.3 / 20.8 / 18.8 (0.94) | 27.8 / 19.1 / 20.3 (1.04) | 25.3 / 20.3 / 23.4 (1.13) |
| unfiltered, margin 8 | 67.2 / 47.6 / 43.2 (0.93) | 67.2 / 48.1 / 41.0 (0.79) | 58.9 / 46.4 / 39.8 (0.86) | 62.4 / 45.4 / 37.1 (0.87) |
| 10%, probed | 58.0 / 58.1 / 46.8 (0.94) | 60.9 / 51.5 / 41.7 (0.87) | 62.4 / 51.6 / 47.9 (0.85) |  |
| 3%, probed | 95.9 / 72.9 / 57.9 (0.79) | 101.3 / 76.9 / 61.6 (0.79) | 93.4 / 76.0 / 68.1 (0.87) |  |
| 1%, probed | 100.9 / 82.8 / 62.0 (0.80) | 103.1 / 93.0 / 72.2 (0.81) | 87.7 / 88.2 / 72.9 (0.85) | 107.6 / 84.3 / 60.3 (0.77) |
| 1%, exact | 108.3 / 58.9 / 53.3 (0.90) | 119.3 / 56.1 / 67.2 (1.17) | 114.9 / 64.9 / 43.8 (0.66) | 117.3 / 61.9 / 37.2 (0.59) |
| 3%, exact | 318.3 / 172.8 / 151.6 (0.88) | 310.3 / 168.1 / 158.2 (0.93) | 333.2 / 168.3 / 111.8 (0.65) | 316.2 / 170.0 / 99.5 (0.58) |

The streams read more pages than `fadvise` - 402 against 346 at a margin of 3, 996 against 861 at 8,
1,708 against 1,500 at 1% probed, under `worker` - the partitions' chains the refill predicted and
the scan did not probe, released unread (a median of 64, 228 and 677 pages a query), and the disk
read 4 to 19% more on the index's kinds. Their streams ended and started again 35 to 72 times a
query. The exact path reads the same 1,212 pages under each; under `sync` a stream's reads are the
backend's own `preadv()` after `posix_fadvise()` advice, and at 1% it was slower there (67.2 ms
against 56.1). Main's `on` asked for none of this table's heap pages (its raised
`toast_tuple_target` not counted until a9a27c5), so its exact path took `off`'s time.

## From the kernel's cache (`os`)

| Query | `worker`, 3 workers | `sync` | `io_uring` | `worker`, 16 workers |
| --- | --- | --- | --- | --- |
| unfiltered, margin 3 | 6.3 / 6.4 / 11.5 (1.40) | 6.4 / 7.3 / 7.1 (1.02) | 6.4 / 8.2 / 8.9 (1.31) | 7.0 / 6.2 / 10.9 (1.72) |
| unfiltered, margin 8 | 18.2 / 17.9 / 21.7 (1.18) | 17.9 / 19.8 / 18.5 (1.11) | 16.4 / 16.9 / 18.9 (1.22) | 16.2 / 16.0 / 22.4 (1.33) |
| 10%, probed | 20.2 / 23.8 / 23.6 (1.00) | 21.7 / 19.5 / 21.5 (1.08) | 24.2 / 19.8 / 30.4 (1.23) |  |
| 3%, probed | 28.0 / 26.1 / 29.8 (1.10) | 26.2 / 28.3 / 30.8 (1.14) | 27.0 / 27.0 / 31.1 (1.19) |  |
| 1%, probed | 29.2 / 28.4 / 26.5 (1.05) | 26.5 / 31.0 / 31.1 (1.02) | 26.4 / 27.6 / 36.4 (1.10) | 26.9 / 29.1 / 38.8 (1.37) |
| 1%, exact | 18.4 / 19.1 / 8.6 (0.44) | 18.9 / 19.8 / 21.6 (1.10) | 17.7 / 22.3 / 34.0 (1.54) | 18.9 / 22.9 / 25.6 (1.17) |
| 3%, exact | 52.5 / 55.1 / 23.2 (0.45) | 56.1 / 56.6 / 61.4 (1.07) | 54.0 / 55.8 / 107.9 (1.96) | 54.9 / 56.1 / 71.4 (1.16) |

Sixteen I/O workers read the disk faster and the kernel's cache slower, sixteen processes on four
CPUs.

## Warm

The matrices' own warm runs (`mx-*.json`, a session for each library, the configurations taking
turns query by query):

| Query | `worker`, 3 workers | `sync` | `io_uring` |
| --- | --- | --- | --- |
| unfiltered, margin 3 | 2.0 / 2.2 / 2.3 (1.01) | 2.0 / 2.1 / 2.2 (1.03) | 1.7 / 1.9 / 2.0 (1.03) |
| unfiltered, margin 8 | 4.4 / 4.8 / 5.1 (1.00) | 5.5 / 5.6 / 5.8 (1.01) | 6.1 / 6.4 / 5.7 (0.99) |
| 10%, probed | 9.9 / 10.2 / 10.1 (1.00) | 10.4 / 10.7 / 11.1 (1.02) | 13.2 / 12.2 / 11.7 (1.00) |
| 3%, probed | 11.7 / 13.3 / 13.1 (0.93) | 11.6 / 12.0 / 11.8 (0.99) | 11.4 / 11.6 / 11.6 (1.01) |
| 1%, probed | 9.1 / 10.0 / 10.1 (1.02) | 9.5 / 10.3 / 10.3 (1.03) | 10.5 / 10.7 / 10.9 (1.01) |
| 1%, exact | 4.6 / 4.7 / 4.9 (1.04) | 4.2 / 4.2 / 4.4 (1.03) | 4.5 / 4.6 / 4.6 (1.02) |
| 3%, exact | 12.2 / 12.5 / 12.6 (1.01) | 12.7 / 12.9 / 13.0 (1.01) | 12.7 / 13.0 / 13.3 (1.02) |

In one session, 20 queries, 5 runs each, each query against itself under its own library's `off`
(`warm-rest3.json`): `stream` 1.02 to 1.11, `fadvise` 0.98 to 1.05; a build whose streams never
rested (`norest`, r3) 1.06 to 1.15 against its own `off` (`roff`); main's `on` 0.99 to 1.09 against
its `off` (`moff`). Resting (after 32 reads in a row that read nothing in, 256 reads with no stream)
roughly halves what a warm stream costs.

| Query | fadvise | stream | norest / roff | main / moff |
| --- | --- | --- | --- | --- |
| unfiltered, margin 3 | 1.05 (0.99-1.16) | 1.11 (1.03-1.21) | 1.15 (1.09-1.23) | 1.09 (1.02-1.13) |
| unfiltered, margin 8 | 1.05 (1.03-1.06) | 1.07 (1.05-1.09) | 1.13 (1.10-1.17) | 1.07 (1.01-1.11) |
| 10%, probed | 1.02 (1.01-1.05) | 1.03 (1.01-1.04) | 1.06 (1.04-1.10) | 1.02 (1.00-1.04) |
| 3%, probed | 1.03 (1.02-1.04) | 1.02 (1.01-1.04) | 1.11 (1.10-1.13) | 1.03 (1.03-1.04) |
| 1%, probed | 1.04 (1.03-1.05) | 1.05 (1.04-1.08) | 1.15 (1.14-1.16) | 1.04 (1.02-1.08) |
| 1%, exact | 0.98 (0.96-1.01) | 1.02 (0.99-1.05) | 1.06 (1.03-1.07) | 0.99 (0.98-1.03) |
| 3%, exact | 1.01 (0.99-1.02) | 1.02 (1.00-1.03) | 1.06 (1.04-1.08) | 1.01 (0.98-1.04) |

## The parts

5 queries, `io_method = worker`, device (`ab-restart.json`, `ab-list.json`): `r3` is the ring's
stream as it is now, begun anew from a full I/O ahead each time it ends; `r2` reset instead, from
one block ahead; `nopred` without predicting the next partitions' chains, each probe's stream ending
with its run (`r3a`, in the file, an earlier `r3`). The exact path through the ring (`ring`),
through a list stream with `READ_STREAM_FULL` (`full`, as it is now) and without (`nofull`). The
median ms, then `w` the median wait on reads.

| Query | fadvise | r2 | r3 | nopred |
| --- | --- | --- | --- | --- |
| unfiltered, margin 3 | 24.6, 14.9 w | 25.4, 20.7 w | 19.3, 13.7 w | 31.7, 23.2 w |
| unfiltered, margin 8 | 44.8, 24.9 w | 42.2, 30.8 w | 42.8, 26.2 w | 68.4, 50.2 w |
| 10%, probed | 43.5, 19.2 w | 62.7, 35.4 w | 49.6, 25.6 w | 65.5, 41.0 w |
| 1%, probed | 80.2, 49.2 w | 61.8, 39.1 w | 62.8, 36.6 w | 146.9, 104.5 w |

| Query | fadvise | ring | full | nofull |
| --- | --- | --- | --- | --- |
| 1%, exact | 57.8, 18.1 w | 65.9, 45.4 w | 50.9, 3.2 w | 53.7, 4.1 w |
| 3%, exact | 166.0, 52.3 w | 163.8, 121.8 w | 132.2, 3.8 w | 146.4, 5.7 w |

## The vectors in TOAST

`toast-worker-3.json`. pgvector stores a `vector` of more than 506 dimensions in TOAST unless its
column or table says otherwise, so most tables of real embeddings keep theirs there. The same rows,
loaded by `knn_dbpedia.py load` with `KNN_INLINE=0` into another database (`dbpedia_toast`) - the
heap 7.6 MB, its TOAST 900 MB - with an index of their own (`knn_dbpedia.py lionvec 0`: 2,715
partitions, spilling, 247 MB, built by the branch's library in 128 s); `io_method = worker`, 3
workers; the 10 queries from the disk and warm; `off / fadvise / stream` (02:49 to 02:54 UTC).
Nothing was built for TOAST: the index's streams are as above, and the exact distances' TOAST
fetches are not read ahead under any setting - a vector's chunks are found through the TOAST index
from its heap tuple's pointer, which reading them ahead would have to read first - nor, the vectors
not lying in the heap's pages, are the members' heap pages (DESIGN.md §44.4, "Prefetch").

| Device | TOAST: off / fadvise / stream (stream / fadvise) | TOAST: pages read, `fadvise`, `stream` | inline (`mx-worker-3.json`) |
| --- | --- | --- | --- |
| unfiltered, margin 3 | 28.7 / 26.3 / 29.5 (1.20) | 374, 430 | 25.3 / 20.9 / 21.9 (1.12) |
| unfiltered, margin 8 | 57.9 / 47.7 / 43.0 (0.93) | 887, 1,024 | 67.2 / 47.6 / 43.2 (0.93) |
| 3%, probed | 97.4 / 81.2 / 67.4 (0.82) | 1,342, 1,446 | 95.9 / 72.9 / 57.9 (0.79) |
| 1%, exact | 156.5 / 159.0 / 167.6 (1.03) | 2,717, 2,717 | 108.3 / 58.9 / 53.3 (0.90) |

| Warm | TOAST: off / fadvise / stream (stream / fadvise) | inline (`mx-worker-3.json`) |
| --- | --- | --- |
| unfiltered, margin 3 | 2.6 / 2.4 / 2.6 (1.06) | 2.0 / 2.2 / 2.3 (1.01) |
| unfiltered, margin 8 | 6.3 / 6.5 / 6.9 (1.00) | 4.4 / 4.8 / 5.1 (1.00) |
| 3%, probed | 8.7 / 9.5 / 9.4 (1.03) | 11.7 / 13.3 / 13.1 (0.93) |
| 1%, exact | 11.3 / 11.2 / 11.2 (1.00) | 4.6 / 4.7 / 4.9 (1.04) |

The index's pages gain what they gained inline: at a margin of 8 43.0 ms streaming against 47.7
asking the kernel and 57.9 with neither, at 3% probed 67.4 against 81.2 and 97.4; a margin of 3, 374
pages, about even again (each query against itself, `stream` 1.06 times `off`, quartiles 0.97 to
1.10; `fadvise` 0.89). The exact path at 1% read its 2,717 pages - the members' heap pages, the
TOAST index's and their chunks - one at a time under every setting, 157 to 168 ms, 135 to 146 of
them waiting on reads, where with the vectors inline and streamed it took 53: that is what reading
the chunks ahead could win back. Warm, detoasting its 1,204 vectors took it from 4.6 ms to 11.2. The
answers were the same under each setting (`meta.mismatches`), and the recall the inline table's.

## Machine, settings, method

PostgreSQL 18.6 built from source without assertions
(`--with-icu --with-openssl --with-lz4 --with-zstd --with-liburing`, gcc's `-O2`), pgvector 0.8.7
built against it, pg_lion built against it with `WERROR=1` (each library's sha256 is in its file's
`meta.configs`): `main` at 64e7443, `branch` at 52ed0ff with the next commit's list rest less that
commit's fix for a stream whose backlog outlasts a rest, which no query here met (the fix changes no
count of them). 4 vCPUs (Intel Xeon @ 2.80GHz), 15 GB of RAM, Linux 6.18; the data on a virtio disk
(`vda`, the kernel's readahead 128 kB). `shared_buffers` 2 GB, `work_mem` 64 MB,
`maintenance_work_mem` 1 GB, no parallel query, `jit` off, `fsync` off, `random_page_cost` 1.1,
`effective_io_concurrency` 200 (so 64 pages ahead), `maintenance_io_concurrency` 200,
`effective_cache_size` 10 GB, `io_combine_limit` 128 kB (16 pages), `track_io_timing` on,
`io_method` as each file says (`meta.settings`), with 3 I/O workers under `worker` (18's default)
and 16 in `mx-worker-16.json`. pgvector's distance functions at `COST 100`.

The table is `knn_dbpedia.py load`'s: 115,186 rows of `vector(1536)` loaded with
`toast_tuple_target = 8160` (`KNN_INLINE`, the default), so each vector stays in its row, one
row a heap page: the heap 900 MB, its TOAST empty. The index is `dbpedia_lv`, lionvec on
`vector_cosine_ops` with `partitions = 0` (2,715, spilling), 247 MB, built at main. The queries
are the first 10 of the 200, the truth the numpy ten nearest under each filter (`u < 0.1`, `0.03`,
`0.01`: 11,377, 3,472 and 1,204 rows). The kinds:

| Kind | Query | Settings |
| --- | --- | --- |
| u3, u8 | unfiltered, `ORDER BY embedding <=> q LIMIT 10` | `pg_lion.vector_margin` 3, 8 |
| f10, f3, f1 | `WHERE u < 0.1` (`0.03`, `0.01`), LionOrdered probing | margin 3, the exact path off (`vector_exact_rows` and `vector_exact_per_partition` 0), `enable_sort` and `enable_indexscan` off |
| x1 | `WHERE u < 0.01`, LionOrdered's exact path (1,204 members) | `enable_sort` and `enable_indexscan` off |
| x3 | `WHERE u < 0.03`, LionOrdered's exact path (3,472 members) | `vector_exact_rows` 200,000, the same |

The states:

- **device**: the server restarted and the kernel's page cache dropped (`echo 3 >
  /proc/sys/vm/drop_caches`, which each row records as `dropped`) before each query, two seconds
  for the flush to settle: every page from the disk.
- **os**: the server restarted before each query, the relations read into the page cache once
  beforehand (`pg_prewarm(..., 'read')`): every page from the kernel's cache, none in shared
  buffers.
- **warm**: every relation in shared buffers (`pg_prewarm`), each query once first, then 3 times
  (the median kept); the configurations of one library take turns in one session.

In the cold states the configurations take turns query by query, so that a slow stretch of the
disk falls on all of them. Each query runs under `EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS
OFF)` with its DEBUG1 lines caught: the server's time; `p`, the pages read from outside shared
buffers (`Buffers: shared read`); `w`, the time waited on them (`I/O Timings: shared read`, which
for a stream is the wait in `WaitReadBuffers()`, not the reads' own time); `MB`, what the disk
read (`/proc/diskstats`, which counts the kernel's readahead and the asks of the kernel too).
Then once more for its rows: the recall in brackets is the share of the true ten returned, and
every configuration must return the same rows in the same order - none differed in any file here
(`meta.mismatches`).

## Files

| File | What |
| --- | --- |
| `mx-worker-3.json` | the matrix under `io_method = worker`, 3 workers: main, off, fadvise, stream; device, os, warm; every kind |
| `mx-sync-3.json` | the same under `io_method = sync` |
| `mx-io_uring-3.json` | the same under `io_method = io_uring` |
| `mx-worker-16.json` | off, fadvise, stream under `worker` with 16 workers; device and os; u3, u8, f1, x1, x3 |
| `warm-rest3.json` | warm, 20 queries, 5 repetitions: off, fadvise, stream of the branch; `roff` and `norest`, off and stream of a build before the streams rested (r3: c12d8a5c), in one session; `moff` and `main`, off and on of main, in one session |
| `warm-r3.json` | warm, before the streams rested (r3), each configuration in its own session |
| `ab-restart.json` | device and warm, 5 queries, u3, u8, f10, f1: how a ring's stream starts again (r2: reset; r3a, r3: begun anew; nopred: no prediction of the next partitions' chains) |
| `ab-list.json` | device and warm, 5 queries, x1, x3: the exact path through the ring (`ring`), through a list stream with `READ_STREAM_FULL` (`full`) and without (`nofull`) |
| `toast-worker-3.json` | the vectors in TOAST (`dbpedia_toast`): off, fadvise, stream under `worker`, 3 workers; device and warm; u3, u8, f3, x1 |
| `tables.md` | `knn_readahead.py table` of every file above |

Each file is `meta` (settings, sizes, the configurations and their libraries' sha256, the
mismatches), `results` (each state, kind and configuration: medians, means, quartiles, every
query's time, pages and wait) and, from `warm-r3.json` on, `raw` (every query's own row and its
ids). `knn_readahead.py table FILE...` prints the tables below, the per-query ratios included.

## Re-running

As root, with a cluster of the settings above whose `pg_lion.so` the script may replace (the
libraries in `KNN_RA_SO_DIR`, `NAME.so` each; `main.so` built at 64e7443, `branch.so` at the
branch), `KNN_RESTART` a command that restarts it as its owner, and `KNN_CACHE` the cache of
`knn_dbpedia.py load`:

    export PGHOST=... PGPORT=... KNN_CACHE=... KNN_OUT=... KNN_RESTART=...
    psql -c "ALTER SYSTEM SET io_method = 'worker'" -c "ALTER SYSTEM SET io_workers = 3"; $KNN_RESTART
    python3 bench/vector/knn_readahead.py mx-worker-3
    # and sync, io_uring; then io_workers = 16:
    KNN_RA_CONFIGS=off:branch:off,fadvise:branch:fadvise,stream:branch:stream KNN_RA_STATES=device,os \
      KNN_RA_KINDS=u3,u8,f1,x1,x3 python3 bench/vector/knn_readahead.py mx-worker-16
    KNN_RA_CONFIGS=off:branch:off,fadvise:branch:fadvise,stream:branch:stream,roff:r3:off,norest:r3:stream,moff:main:off,main:main:on \
      KNN_RA_STATES=warm KNN_RA_NQ=20 KNN_RA_WARM_REPS=5 python3 bench/vector/knn_readahead.py warm-rest3
    # the vectors in TOAST, into their own database (KNN_CACHE a cache of its own):
    KNN_DSN='dbname=dbpedia_toast' KNN_INLINE=0 python3 bench/vector/knn_dbpedia.py load
    KNN_DSN='dbname=dbpedia_toast' python3 bench/vector/knn_dbpedia.py lionvec 0
    KNN_DSN='dbname=dbpedia_toast' KNN_RA_CONFIGS=off:branch:off,fadvise:branch:fadvise,stream:branch:stream \
      KNN_RA_STATES=device,warm KNN_RA_KINDS=u3,u8,f3,x1 python3 bench/vector/knn_readahead.py toast-worker-3
    python3 bench/vector/knn_readahead.py table $KNN_OUT/mx-*.json
