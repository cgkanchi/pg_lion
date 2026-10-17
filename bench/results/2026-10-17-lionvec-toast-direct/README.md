# lionvec reading vectors in TOAST by their places, on DBpedia, PostgreSQL 18 (2026-10-17)

`bench/vector/knn_readahead.py` on `knn_dbpedia.py`'s DBpedia rows with their vectors in TOAST
(`dbpedia_toast`, as in `2026-10-17-lionvec-read-streams/`) and in their rows (`dbpedia`): what
reading a vector in TOAST from its chunks' pages - the locator and the REFINE items' places of
DESIGN.md §44.13, index format 5 - does to a query, under `pg_lion.vector_toast_direct = off` and
`on`; what the locator costs the index, its build and inserts (`knn_toast_insert.py`); and that
the inline table's queries take main's time.

**Summary.** From the disk the exact path read its members' vectors in 0.38 times its time at 1%
(68.3 ms against 179.7) and 0.34 at 3% (158.6 against 457.2), reading 1,631 pages instead of 2,717
and 3,915 instead of 5,620 and waiting 12.7 and 12.2 ms on them instead of 152.7 and 395.9: within
12 ms of the inline table's exact path at 1% and level with it at 3%. Warm, 0.55 and 0.52 (7.9 ms
against 14.1; 16.4 against 31.8). The probing kinds, reading only the ten or so rows they rerank
by place, were 6 to 7% faster from the disk and the same warm. Every query returned the same rows
in the same order under both settings. The locator took 1.26% more index (3.3 MB of 259 MB), its
build no measurable time, and an insert 121 bytes more WAL a row and no time beyond the runs'
spread. On the inline table, whose index keeps no locator, the branch took main's time.

## The vectors in TOAST, direct reads off and on

`toast-direct-final.json`, the branch at 7fec486. The index REINDEXed by the branch (format 5: a
locator of 115,186 entries, and the REFINE items' places). The server's median ms over the 10
queries, `off / on`, and in brackets the median of each query's `on` time over its `off` time;
then the pages read, the wait on them and what the disk read, `off`, `on`.

| Query | Device | pages read | wait (ms) | disk MB | Warm |
| --- | --- | --- | --- | --- | --- |
| unfiltered, margin 3 | 30.5 / 27.2 (0.94) | 430, 429 | 22.1, 19.5 | 6.0, 5.4 | 2.6 / 2.6 (0.96) |
| unfiltered, margin 8 | 53.7 / 44.8 (0.93) | 1,024, 1,024 | 34.0, 28.5 | 11.1, 11.5 | 6.2 / 6.3 (0.99) |
| 3%, probed | 70.8 / 63.9 (0.93) | 1,446, 1,445 | 45.1, 40.4 | 14.9, 15.0 | 10.2 / 10.0 (0.99) |
| 1%, exact | 179.7 / 68.3 (0.38) | 2,717, 1,631 | 152.7, 12.7 | 29.5, 15.0 | 14.1 / 7.9 (0.55) |
| 3%, exact | 457.2 / 158.6 (0.34) | 5,620, 3,915 | 395.9, 12.2 | 48.5, 32.9 | 31.8 / 16.4 (0.52) |

The exact path's DEBUG1 line: at 1% 1,204 read by place over 394 locator pages, none failing, 10
checked against their heap rows on the way out; at 3% 3,472 over 396. Of its 1,631 pages at 1%:
the LOCDIR page, the 394 LOC pages (the members' entries lie on all but two of the 396 that hold
entries), the 1,204 vectors' pages (each value's four chunks on one page of its own in this bulk
load), the 10 rows' heap pages, and 22 of the TOAST index's - the query's projection detoasting
each row returned, after the scan, to compute `embedding <=> q` for the `ORDER BY` column
(DESIGN.md §44.13, "Open questions"). The inline table's exact path took 56.2 and 158.3 ms the
same morning (`inline-worker-3.json`, the branch), the prototype's 55.2 and 141.5.

The probing kinds each read 10 rows by their REFINE items' places, none failing, and asked the
kernel for 1 and 16 chunk pages at margins of 3 and 8, each beside its row's heap page, and none
under the 3% filter, where the inline table's scan asks for no heap page either. They read as many
pages as `off` - the projection's detoast of the rows returned reads the TOAST index either way -
and gained by the order of the reads.

`toast-direct-worker-3.json` is the same run with the branch at 3864d11, before the exact path
searched the build's LOC pages for its members (it read every entry of every LOC page it read and
looked each up among the members): from the disk 69.9 and 197.2 ms at 1% and 3%, against 179.0 and
472.1 `off`; warm 16.5 and 22.4, against 17.8 and 35.1. `toast-warm-sorted.json` is the change
measured warm, each library's `on` and `off` in its session: 7fec486's (`on2`, `off2`) 7.2 and 15.5
ms against 13.6 and 30.4; 3864d11's (`on`, `off`) 11.4 and 21.0 against 12.0 and 30.1. Reading all
115,186 entries cost a warm query about 4 ms at 1% and 5.5 at 3%. `off` warm, the same code in every
run, took 11.3 ms at 1% in the read-streams run, 17.8, 12.0, 13.6 and 14.1 here: a warm session's
times move by up to half again from one session to the next, which is why each run's `off` and `on`
share sessions.

## The index, its build, and inserts

`dbpedia_lv` REINDEXed serially at `maintenance_work_mem` 1 GB, its pages counted by type
(`pageinspect`):

| | main (1be037c), format 4 | the branch, format 5 |
| --- | --- | --- |
| bytes | 258,949,120 | 262,209,536 (+3,260,416, +1.26%) |
| CODE / REFINE / PART / REGION pages | 7,940 / 23,581 / 85 / 2 | the same |
| LOC / LOCDIR pages | - | 397 (396 of entries and the inserts' empty tail) / 1 |
| REINDEX | 137.1 s | 131.0 s, "a locator of 115186 rows in TOAST, its refine items with their places" |

The REFINE pages hold five items each with their 20-byte slots as without. One build each: the
TOAST index lookup a row is lost in the build's time.

Inserts (`insert-final.json`, 8 repetitions; `insert.json`, 5, with the branch at 3864d11, whose
insert path is 7fec486's): each repetition loads 20,000 rows into a new table, builds the index
with the library measured, checkpoints, then times `INSERT ... SELECT` of the next 5,000 rows and
the WAL it wrote; the configurations take turns, reversed every other repetition.

| | main, format 4 | the branch, format 5 |
| --- | --- | --- |
| ms, `insert-final.json` (median of 8; all) | 2,211 (2,113 to 2,825) | 2,275 (2,143 to 2,457) |
| ms, `insert.json` (median of 5) | 2,274 (2,070 to 2,770) | `on` 2,596 (2,139 to 2,878), `off` 2,351 (2,250 to 2,620) |
| WAL, MB (5,000 rows) | 50.19 | 50.79 (+121 bytes a row, +1.2%) |
| index after, bytes | 59,244,544 | 59,965,440 |
| build of 20,000 rows, s (medians) | 19.0, 19.7 | 19.3; 19.8 `on`, 19.3 `off` |

Each repetition's pair of the final run (branch over main): 0.83 to 1.13, the median 1.00. The
spread of the runs - a 50 MB write each, its TOAST 30 MB - is wider than any difference between
the libraries; the setting does not touch an insert.

## The inline table

`dbpedia`, the vectors in their rows, its index built by main (format 4) and read as one without a
locator by the branch - the same code a format-5 index without a locator takes. From the disk
(`inline-worker-3.json`, the branch at 3864d11, whose inline path is 7fec486's; `main / branch`,
each query's branch time over its main time):

| Query | Device | Warm, one session each (main first) |
| --- | --- | --- |
| unfiltered, margin 3 | 32.5 / 30.6 (0.90) | 3.3 / 2.3 (0.79) |
| unfiltered, margin 8 | 55.2 / 48.4 (0.90) | 6.0 / 6.3 (1.02) |
| 3%, probed | 77.2 / 75.0 (0.99) | 14.4 / 12.9 (1.05) |
| 1%, exact | 57.3 / 56.2 (1.07) | 4.4 / 4.3 (0.98) |
| 3%, exact | 164.6 / 158.3 (0.98) | 11.1 / 13.0 (1.16) |

Warm, a run gives each library one session, and the session moved the times more than the library:
three more runs (`inline-warm-final.json` and `inline-warm-branch-first.json`, the branch's session
first; `inline-warm-main-first.json`) put the branch at 1.59, 0.98 and 1.00 of main at a margin of
3, and at 1.06, 1.01 and 0.98 at 3% exact. So `inline-warm-alternating.json` (`alternate.py`)
gives each library four sessions, in the order main, branch, branch, main, main, branch, branch,
main, each session's own median per kind from 10 queries of 5 runs each:

| Query | main | branch | each query's branch over main (quartiles) | sessions' medians, main; branch |
| --- | --- | --- | --- | --- |
| unfiltered, margin 3 | 2.20 | 2.37 | 1.00 (0.97-1.06) | 2.2-2.5; 2.0-2.7 |
| 3%, probed | 10.22 | 10.40 | 0.97 (0.95-1.01) | 9.8-11.0; 9.6-10.5 |
| 1%, exact | 4.38 | 4.33 | 0.98 (0.96-1.02) | 4.0-4.8; 4.2-4.5 |
| 3%, exact | 11.34 | 11.54 | 1.00 (0.98-1.04) | 10.9-11.5; 11.2-12.4 |

## Machine, settings, method

As in `2026-10-17-lionvec-read-streams/`: PostgreSQL 18.6 built from source without assertions,
pgvector 0.8.7, pg_lion built with `WERROR=1` (each library's sha256 is in its file's
`meta.configs`; `main` is 1be037c, `branch` 7fec486 in the `-final` files and `inline-warm-*.json`,
3864d11 in `toast-direct-worker-3.json` and `inline-worker-3.json`, and in `toast-warm-sorted.json`
`branch2` is 7fec486's code as built before its comments were rewrapped, and `branch` 3864d11). 4
vCPUs (Intel Xeon @ 2.80GHz), 15 GB of RAM, Linux 6.18, the data on a virtio disk (`vda`).
`shared_buffers` 2 GB, `work_mem` 64 MB, `maintenance_work_mem` 1 GB, no parallel query, `jit` off,
`fsync` off, `random_page_cost` 1.1, `effective_io_concurrency` 200, `maintenance_io_concurrency`
200, `io_combine_limit` 128 kB, `track_io_timing` on, `io_method = worker` with 3 workers.
pgvector's distance functions at `COST 100`. The cluster is a copy of the read-streams run's.

`dbpedia_toast` is `knn_dbpedia.py load` with `KNN_INLINE=0`: the 115,186 rows of `vector(1536)`
with the default `toast_tuple_target`, so each vector lies in TOAST, its four chunks on a page of
their own, the TOAST blocks in the heap's order; the heap 7.6 MB, its TOAST 900 MB. Its index,
`dbpedia_lv` (`partitions = 0`: 2,715, spilling), REINDEXed by each library in turn above, by the
branch for every query here. The kinds, the states and how a query is measured are the
read-streams README's: `device` the server restarted and the kernel's page cache dropped before
each query, two seconds for the flush to settle; `warm` every relation in shared buffers
(`pg_prewarm`, the TOAST relation and its index too), each query once first and then 5 times (3 in
`toast-direct-worker-3.json` and `inline-worker-3.json`), the median kept, the configurations of
one library taking turns in one session. `off` and `on` are `pg_lion.vector_toast_direct`, the
fourth field of `KNN_RA_CONFIGS` (`label:library:prefetch:direct`), with `vector_prefetch =
stream`. Each run's `meta.mismatches` is empty: every configuration returned the same rows in the
same order.

`knn_toast_insert.py` made `insert.json` and `insert-final.json` (as a first draft whose
variables were named `INS_*`, not `KNN_INS_*`); `alternate.py` made
`inline-warm-alternating.json`, importing `knn_readahead.py` for its restarts and measurements.

## Files

| File | What |
| --- | --- |
| `toast-direct-final.json` | `dbpedia_toast`, `off` and `on`, device and warm, the branch at 7fec486 |
| `toast-direct-worker-3.json` | the same at 3864d11, before the LOC pages were searched |
| `toast-warm-sorted.json` | warm, 7fec486's scan (`on2`, `off2`) and 3864d11's (`on`, `off`) |
| `inline-worker-3.json` | `dbpedia`, main and the branch (3864d11), device and warm |
| `inline-warm-final.json`, `inline-warm-branch-first.json`, `inline-warm-main-first.json` | warm, one session a library, either first |
| `inline-warm-alternating.json`, `alternate.py` | warm, four sessions a library, alternating |
| `insert.json`, `insert-final.json` | inserts into a toasted table under the index, main and the branch |
| `tables.md` | `knn_readahead.py table` of the `knn_readahead.py` files |
