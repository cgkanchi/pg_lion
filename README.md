# roaring_index — a roaring-bitmap inverted index AM for PostgreSQL (prototype)

`CREATE INDEX ... USING roaring (col)` builds one posting set of roaring-style containers
(array / bitset / run, ≤ 4104 bytes each, one per 64 heap pages) or sparse (container key, offset)
segments per distinct key, plus one reserved entry for the rows whose key is NULL. The index serves
Bitmap Index Scans through `amgetbitmap` for `col = v`, `col = ANY (list)`, `col IS NULL` and
`col IS NOT NULL`, and a CustomScan (`RoaringCount`) answers
`SELECT count(*) [, k] FROM t WHERE k1 = c1 [AND ...] [GROUP BY k]` from the containers plus the
visibility map, visiting the heap only for pages that are not all-visible. `DESIGN.md` is the spec:
on-disk format, locking protocol, the VACUUM/visibility-map interlock argument (§9, §11), and the
planner integration (§10).

Status: prototype against PostgreSQL master (20devel). 10k lines of C, 13 pg_regress files, 4 isolation
specs (including an injection-point proof that VACUUM waits behind a pinned container page and stays
cancellable), a 660k-check unit suite for the container library, a crash-recovery check of the
generic-WAL records, and concurrent insert/delete/vacuum/read stress runs. Nothing is committed to git.

## Build and test

    ./dev.sh reset                                   # initdb a private cluster in .local/data (needs .local/pg)
    make PG_CONFIG=.local/pg/bin/pg_config && make PG_CONFIG=.local/pg/bin/pg_config install
    eval "$(./dev.sh env)"
    make PG_CONFIG=.local/pg/bin/pg_config installcheck   # 13 regress files + 4 isolation specs
    make unit PG_CONFIG=.local/pg/bin/pg_config           # container library, no server needed

`.local/pg` must be a PostgreSQL master install; for the isolation specs it needs
`--enable-injection-points` and the `injection_points` test module installed, and
`pg_isolation_regress` installed from `src/test/isolation`.

    CREATE EXTENSION roaring_index;
    CREATE INDEX ON fact USING roaring (country) WITH (buckets = 256, inline_limit = 4096);
    SELECT * FROM roaring_index_stats('fact_country_idx');
    SELECT roaring_index_verify('fact_country_idx', heapallindexed => true);
    SELECT roaring_index_count('fact_country_idx', 'Japan');          -- VM-interlocked count
    SET roaring_index.enable_count_pushdown = on;                       -- default on
    EXPLAIN (ANALYZE) SELECT country, count(*) FROM fact GROUP BY country;   -- Custom Scan (RoaringCount)

## Source layout

    src/rbi_tid.h          TID <-> (container key, 15-bit lo) encoding; 9 offset bits at 8K pages
    src/rbi_container.[ch] container library (array/bitset/run), set algebra, unit-tested standalone
    src/rbi.h, rbi_pages.c on-disk structs; meta/bucket/entry/chain primitives, page splits, generic WAL
    src/rbi_am.c           handler, reloptions, amvalidate, cost estimate, buildempty, _PG_init hook/GUC
    src/rbi_build.c        ambuild via tuplesort (hash, key, tid code); INLINE entries or per-key page chains
    src/rbi_scan.c         amgetbitmap
    src/rbi_insert.c       aminsert (bucket-serialised; bitset in-place fast path)
    src/rbi_vacuum.c       ambulkdelete with cleanup locks on every page, two-pass cancellable protocol
    src/rbi_funcs.c        roaring_index_stats(), roaring_index_verify()
    src/rbi_count.[ch]     rbi_count_keys(): VM-interlocked counting, per-block batched heap recheck
    src/rbi_customscan.c   create_upper_paths_hook -> CustomPath/CustomScan "RoaringCount"
    src/rbi_multikey.c     array_ops/tsvector_ops: GIN-style extraction and tsquery key trees
    test/sql, test/isolation, test/unit

## Key types

One default operator class per type, reusing the hash access method's hash functions: integers
(`int2/int4/int8`, one family with cross-type equality), `float4/float8`, `numeric`, `bool`, `"char"`,
`name`, `text` (and `varchar` through it), `char(n)`, `bytea`, `uuid`, `date`, `time`, `timetz`,
`timestamp`, `timestamptz`, `interval`, `macaddr`, `inet`, `jsonb`, `pg_lsn`, `xid`, `cid`, `tid`,
`oid`, and any enum. Case-insensitive text: `CREATE EXTENSION roaring_index_citext` (requires
`citext`) adds `citext_ops`. Domains resolve to their base type. Keys over 2000 bytes are rejected.

## Multi-key columns: arrays and tsvector (DESIGN.md §17)

`array_ops` (any array type) and `tsvector_ops` index one row under many keys, reusing GIN's own
extraction functions, and answer

    CREATE INDEX ON doc USING roaring (tags);       -- text[], int[], ...
    CREATE INDEX ON doc USING roaring (tsv);        -- tsvector

    tags @> '{a,b}'    the intersection of the elements' posting sets, exact
    tags && '{a,b}'    their union, exact
    tags <@ '{a,b}'    every indexed row, rechecked in the heap
    tsv  @@ 'a & (b | c)'   an AND/OR tree over the lexemes, exact

`count(*)` over `@>`, `&&` and an AND/OR tsquery is pushed down like any other clause, and can be
combined with a `GROUP BY` on a scalar roaring column. Everything a plain AND/OR of key sets cannot
express - `<@`, `@> '{}'`, a NULL element, and a tsquery with `!`, `<->`, `foo:*` or weights - falls
back to scanning every indexed row and rechecking it, which is correct but no faster than GIN. The
reloption `max_entries` (default 0 = unlimited) makes the index warn once per backend when it grows
past that many distinct keys; it never rejects a row.

## Reloptions

`buckets` (0 = chosen by the build, otherwise the exact number of hash buckets, max 65536),
`max_entries` (0 = unlimited; the advisory cardinality guard above) and
`inline_limit` (64 .. 4096 bytes, default 4096): how large a key's posting set may be before it
moves out of its entry tuple onto container pages of its own. The build chooses the bucket count
from the BYTES its entries need, at three quarters of a page per bucket - not from the number of
distinct keys - because every bucket owns a head page whether it needs one or not. Bucket counts
are no longer rounded to a power of two; a hash is mapped to its bucket with a modulo.
`roaring_index_stats()` reports the bucket count, the pages and entries, the containers by kind,
the sparse segments, `null_tids`, the number of rows whose key is NULL, and `empty_tids`, the number
of rows a multi-key opclass extracted no key from.

## Known limitations

Single column, equality, `IN` lists and the multi-key operators above (no ranges), no
`amgettuple`/index-only scans, no
parallel build or scan, no page recycling, inserts serialise per hash bucket and cost about 4x a
btree insert (a container is copied out and back per insert, except bitsets), count pushdown handles
`Const` keys only (no `Param` or multi-column GROUP BY), `IN` lists of more than 1000
values are left to the ordinary plan, a multi-key index can never drive a `GROUP BY` or a
sum-over-all-entries count (its entries are keys, not row values), and the cost model inherits the
stale `relallvisible` blind spot of index-only scans. Indexes built before NULL keys existed (meta page version 1) are refused
with an error and have to be rebuilt with REINDEX.

## Index size after the v1 build policy (5M rows, 2026-09-20)

Sparse segments (DESIGN.md §13) shrank the posting sets, but the index only got smaller once the
space policy followed: `inline_limit` now defaults to its maximum of 4096 bytes (a key that spills
owns whole container pages, so spilling a 1.5 KB posting set wastes a page), and the build sizes the
bucket array by the bytes its entries need rather than by the number of distinct keys.

| column (5M rows) | roaring before | roaring after | GIN (btree_gin) | btree |
|---|---|---|---|---|
| c2 (2 keys) | 8488 kB | 8488 kB | 5280 kB | 33 MB |
| c200 | 13 MB | 14 MB | 13 MB | 33 MB |
| c20k | 164 MB | **37 MB** | 26 MB | 34 MB |
| c1m (993k keys) | 256 MB | **116 MB** | 87 MB | 56 MB |

## Results with the real index (20M rows, optimized build, 15-bit containers)

Index sizes. Roaring pays 12 bytes of header per container plus item alignment, and one 4104-byte
bitset per page leaves half of each page empty, so dense random keys cost more than GIN; sparse
high-cardinality keys cost 2-3x GIN because every member becomes its own container. Clustered keys
are 10x smaller than GIN and 60x smaller than btree.

| column | btree | GIN | roaring, 15-bit containers | roaring, 14-bit (experiment) |
|---|---|---|---|---|
| c2 | 132 MB | 21 MB | 56 MB | 38 MB |
| c10 | 132 MB | 23 MB | 41 MB | 41 MB |
| c200 | 132 MB | 39 MB | 51 MB | 60 MB |
| c20k | 138 MB | 157 MB | 475 MB | 477 MB |
| c1m | 152 MB | 199 MB | 337 MB (all INLINE entries) | 337 MB |
| c200_clustered | 132 MB | 23 MB | 2.1 MB | 2.1 MB |
| c_skew | 133 MB | 26 MB | 41 MB | 48 MB |

Build time per roaring index: 16-17 s (btree 4-10 s, GIN 4-21 s). The 14-bit variant helps only the
2-valued column and hurts mid-cardinality keys, so 15 bits stays; the real fixes are better bitset page
packing and a sparse posting representation for keys with few members per container.

Bitmap-scan path (amgetbitmap, count pushdown disabled), pgbench average ms:

| query | btree | GIN | roaring |
|---|---|---|---|
| count(*) where c2 = 1 (10M rows) | 539 (index-only) | 1630 | 1062 |
| count(*) where c200 = 17 (100k) | 5.4 (index-only) / 69 (bitmap) | 77 | 73 |
| count(*) where c200_clustered = 17 | 5.2 | 12 | 6.8 |
| count(*) where c20k = 123 (1k) | 0.17 | 0.86 | 0.82 |
| count(*) where c1m = 12345 (20) | 0.12 | 0.13 | 0.13 |
| 3-column AND, 5k rows | 84 | 176 | 59 |
| sum(c1m) where c200 = 17 (fetch 100k rows) | 72 | 79 | 71 |
| count(*) where c20k between 100 and 199 | 5.6 | 810 | unsupported (equality only) |

The roaring bitmap-index-scan node itself is fast (500k TIDs in 4 ms) but every path that goes
through Bitmap Heap Scan is heap-bound, exactly as the feasibility study predicted (the 10M-row
count varied between 1.06 and 1.44 s across runs).

Count pushdown (`Custom Scan (RoaringCount)`: containers + visibility map, heap visited only for
the ~120 pages of this table that are not all-visible), pgbench average ms, same 20M-row table:

| query | seqscan / hashagg | btree index-only | RoaringCount | speedup vs btree |
|---|---|---|---|---|
| count(*) where c2 = 1 (10M rows) | ~2000 | 539 | **2.6** | 200x |
| count(*) where c200 = 17 (100k) | | 5.4 | **0.35** | 15x |
| count(*) where c200_clustered = 17 | | 5.2 | **0.11** | 45x |
| count(*) where c20k = 123 (1k) | | 0.17 | 0.15 | 1x |
| count(*) where c1m = 12345 (20) | | 0.12 | 0.12 | 1x |
| count(*) where c10 = 3 and c200 = 17 and c2 = 1 | | 84 (BitmapAnd) | **4.9** | 17x |
| c200, count(*) group by c200 | 2354 | 1337 | **50** | 27x (47x vs seqscan) |
| c2, count(*) group by c2 | 1989 | 1347 | **5.3** | 250x |

These are the numbers the 2022 demo hinted at, now produced by a transactionally correct index: the
count honours the caller's snapshot, rechecks TIDs on non-all-visible pages in the heap, and holds
the container page pin across the visibility-map check so VACUUM cannot overtake it (proven by an
isolation test with an injection point). The cost is O(containers) in the all-visible case; before
the per-container visibility-map read it was O(heap blocks × keys) and the GROUP BY took 1034 ms.

Where it does not help: anything that must fetch rows (heap-bound, parity with btree), high-cardinality
keys (parity with btree, 2-3x the space of GIN), range predicates (unsupported), and tables where
few pages are all-visible (the recheck path is 1.8x faster than a seqscan at best and the cost model
falls back to the seqscan when `relallvisible` says so).

Raw logs: `results/06b_bits15_*` (bitmap phases, 15-bit), `results/07_bits14_*` (14-bit experiment),
`results/08_vmmask_*` (final run with the per-container visibility-map read).

---

# Feasibility benchmark (pre-implementation)

Reproducible material behind the 2026-09-20 assessment of the idea posted to pgsql-hackers in June 2022
("An inverted index using roaring bitmaps"). Everything here ran against PostgreSQL master
(20devel, commit 9e17d25e, built from source with `-O2`) and pg_roaringbitmap 1.3 (CRoaring 4.3.11),
on WSL2, 16 cores, 23 GB RAM, private cluster with `fsync=off`, `jit=off`, no parallel query,
`work_mem=256MB`, `autovacuum=off`.

## Layout

- `bench/gen.sql` — builds the 20M-row `fact` table (1.8 GB heap, 227,328 pages, max offset 88),
  one btree and one GIN (`btree_gin`) index per column, and a `fact_tids` helper table.
- `bench/sim.sql` — builds simulated roaring "indexes" (`rb_<col>` tables: key -> bitmap) under four
  TID encodings, prints sizes next to the real indexes, and builds two visibility-map masks.
- `bench/tidconv/` — 20-line C extension (`int8_to_tid`, `tid_to_int8`) so the roaring->heap paths
  are not dominated by text parsing of ctids.
- `bench/q/*.sql` — pgbench query files. `*_idx.sql` run against the real indexes,
  `*_rb_naive.sql` read the bitmap only, `*_rb_vmall.sql` AND the bitmap with an all-visible VM mask,
  `*_rb_vm95.sql` AND with a mask where 5% of heap pages are randomly "dirty" and recheck the
  leftover TIDs in the heap via a TID scan (the honest cost model for a real index).
- `bench/bench.sh` — runs each query once under EXPLAIN (ANALYZE, BUFFERS) and then under
  `pgbench -M prepared -T <secs>`; forces btree-only / GIN-only phases by flipping `pg_index.indisvalid`.
- `bench/w.sql` — write-path comparison on a 2M-row copy: 200k inserts and ~20k non-HOT updates
  with 7 btree indexes, 7 GIN indexes (`fastupdate=off`), 7 GIN indexes (`fastupdate=on`).
- `results/` — raw logs and result tables from the run.

## Columns in `fact`

| column | distinct keys | distribution |
|---|---|---|
| c2 | 2 | uniform random |
| c10 | 10 | uniform random |
| c200 | 200 | uniform random |
| c20k | 20,000 | uniform random |
| c1m | 1,000,000 | uniform random (~20 rows/key) |
| c200_clustered | 200 | monotone in insertion order (runs of 100k rows) |
| c_skew | 1,000 | 90% one value, rest uniform |

## TID encodings tried for the roaring bitmaps

| name | bits | mapping | notes |
|---|---|---|---|
| rb32_2048 | 32 | block*2048 + offset | the 2022 demo; caps a table at 2^21 pages = 16 GB |
| rb32_292 | 32 | block*292 + offset | densest legal 32-bit packing (MaxHeapTuplesPerPage=291 at 8K); caps at ~112 GB |
| rb64_11 | 64 | block<<11 \| offset | GIN's own encoding, no cap; each roaring container spans 32 heap pages |
| rb64_16 | 64 | block<<16 \| offset | raw ItemPointer layout; one container per heap page |

## Index size (20M rows)

| column | btree | GIN | rb32_2048 raw | rb32_2048 run-opt | rb32_292 run-opt | rb64_11 run-opt | rb64_16 run-opt |
|---|---|---|---|---|---|---|---|
| c2 | 132 MB | 21 MB | 38 MB | 38 MB | 16 MB | 38 MB | 40 MB |
| c10 | 132 MB | 23 MB | 39 MB | 39 MB | 38 MB | 39 MB | 56 MB |
| c200 | 132 MB | 39 MB | 49 MB | 49 MB | 40 MB | 49 MB | 162 MB |
| c20k | 138 MB | 157 MB | 181 MB | 181 MB | 135 MB | 181 MB | 191 MB |
| c1m | 152 MB | 199 MB | 198 MB | 198 MB | 197 MB | 210 MB | 243 MB |
| c200_clustered | 132 MB | 23 MB | 38 MB | **0.96 MB** | 0.90 MB | 0.96 MB | 3.1 MB |
| c_skew | 133 MB | 26 MB | 52 MB | 25 MB | 18 MB | 25 MB | 29 MB |

Roaring sizes are the sum of the portable serialized bitmaps (no page overhead, no entry tree), so they
flatter roaring slightly. GIN sizes include its entry tree and posting trees; btree sizes include
pivot tuples.

## Write path (2M-row copy, 7 single-column indexes)

| index set | insert 200k rows | update ~20k rows (indexed column changes, non-HOT) |
|---|---|---|
| none | 0.27 s | 0.34 s |
| 7 btree | 2.44 s | 0.93 s |
| 7 GIN, fastupdate=off | 4.03 s | 0.64 s |
| 7 GIN, fastupdate=on | 1.66 s (work deferred to pending-list cleanup) | 1.03 s |

## Query latency

See `results/03_queries_*.txt` (pgbench average latency, single client, warm cache) and the
"Query latency" section appended below after the 10 GB shared_buffers re-run.

### Query latency, 10 GB shared_buffers, all buffers hit (pgbench average, ms)

Point `count(*)` on one key:

| predicate (rows) | btree index-only | GIN bitmap heap scan | roaring, bitmap only | roaring AND all-visible mask | roaring AND 95% mask + heap recheck of rest |
|---|---|---|---|---|---|
| c2 = 1 (10.0M) | 562 | 1571 | 2.2 | 3.0 | 184 |
| c200 = 17 (100k, scattered) | 5.4 | 80 | 0.16 | 0.65 | 5.0 |
| c200_clustered = 17 (100k, contiguous) | 5.2 | 12 | 0.14 | 0.18 | 2.0 |
| c20k = 123 (1k) | 0.18 | 0.94 | 0.12 | 0.20 | 0.43 |
| c1m = 12345 (20) | 0.13 | 0.13 | 0.12 | 0.15 | 0.20 |

Other shapes:

| query | btree | GIN | roaring, bitmap only | roaring, 95% mask + recheck |
|---|---|---|---|---|
| count where c10=3 and c200=17 and c2=1 (5k rows; planner ANDs two indexes, filters the third in the heap) | 84 | 169 | 6.0 | 6.9 |
| c200, count(*) group by c200 (seqscan+hashagg: 2427) | 1359 index-only + GroupAggregate | n/a | 7.3 | 94 (all-visible mask) / 891 (95% mask) |
| c2, count(*) group by c2 (seqscan+hashagg: 2080) | 1340 | n/a | 4.7 | 6.5 / 352 |
| count where c20k between 100 and 199 (100k rows, 100 keys) | 5.5 | 789 (btree_gin turns `>=` into a scan of every key above the bound) | 11.2 (OR of 100 bitmaps) | n/a |
| sum(c1m) where c200 = 17 (must fetch 100k rows) | 76 bitmap heap scan | 84 | 55 via TID scan | n/a |

The 4 GB shared_buffers run (`results/03_*`) gave the same numbers within noise, so none of the
bitmap-heap-scan cost is buffer misses; it is heap page visits (81,398 pages for c200 = 17,
227,273 pages for c2 = 1).

Caveats of the simulation: the roaring "index" has no page structure, no entry tree beyond a btree on
`key`, and must detoast whole bitmaps (the 19 MB c2 bitmap costs ~2 ms just to read), so it
overstates roaring's cost on huge keys and understates it on tiny ones. The recheck path uses a TID
scan, which is slightly cheaper per row than a bitmap heap scan. None of the roaring variants pay
for the VACUUM/visibility-map interlock that a real index-only bitmap count would need.
