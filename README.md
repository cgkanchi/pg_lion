# pg_lion — a roaring-bitmap inverted index AM for PostgreSQL (prototype)

Formerly `roaring_index`; renamed to pg_lion on 2026-09-21 (a roaring bitmap index, hence the lion). The on-disk
format is unchanged: the meta-page magic still spells `RBI1`, so indexes built before the rename remain readable.

`CREATE INDEX ... USING lion (col)` builds one posting set of roaring-style containers
(array / bitset / run, ≤ 4104 bytes each, one per 64 heap pages) or sparse (container key, offset)
segments per distinct key, plus one reserved entry for the rows whose key is NULL. The index serves
Bitmap Index Scans through `amgetbitmap` for equality, `IN`/`ANY`, scalar ranges
(`<`, `<=`, `>=`, `>`), `col IS NULL` and `col IS NOT NULL`, and a CustomScan (`LionCount`) answers
`SELECT count(*) [, k] FROM t WHERE k1 = c1 [AND ...] [GROUP BY k]` from the containers plus the
visibility map, visiting the heap only for pages that are not all-visible. `DESIGN.md` is the spec:
on-disk format, locking protocol, the VACUUM/visibility-map interlock argument (§9, §11), and the
planner integration (§10).

Status: prototype.  Builds against PostgreSQL 16, 17, 18, 19 and master (20devel); the version
differences live in `src/lion_compat.h`, and the first validation across all of them (16.15, 17.11,
18.6, 19beta4 and master) was at `eb1579e`.  CI (`.github/workflows/ci.yml`) builds and tests every
one of those majors on each push: the container and sparse unit tests, every SQL regression file in
`test/sql` and every isolation spec in `test/isolation` in both WAL modes, and, on the 19 and master
source builds, the crash-recovery/hot-standby harness in both modes.  One gap is deliberate and worth
knowing: the isolation specs that park a backend on an injection point (`grep -l injection_points
test/isolation/*.spec`), which are the race tests of the VACUUM/count/split interlocks, need a server
configured with `--enable-injection-points` and the `injection_points` test module.  PostgreSQL 16
has no injection points, and the PGDG packages of 17 and 18 ship without the module, so CI runs those
specs only on its 19 and master source builds; on 16-18 the interlocks are covered by the same code
and by those specs on the newer majors, not by a test on that major.  See the
[latest review](FOLLOWUP_REVIEW.md) for evidence and remaining limitations.

## When to use Lion

Use the roaring index (`USING lion`) for read-heavy dashboards, facet counts, and aggregations over
large tables: equality, range or NULL counts, intersections of indexed predicates, and grouping by a column
with relatively few distinct values. It can count compressed posting sets without fetching every
matching row when the visibility map allows it. Array membership and simple full-text AND/OR counts
benefit from the same mechanism. Keep tables vacuumed and statistics current so the planner can
estimate that benefit; dirty pages require visibility checks in the heap.

| Your workload | Index choice and tradeoff |
| --- | --- |
| Count many matches, combine equality filters, or count groups | Consider Lion on the columns used by these queries. The largest measured gains come from `LionCount` pushdown. |
| Fetch a handful of rows, or count a very selective key | B-tree is a strong default. The latest run shows practical parity for tiny equality counts and no consistent heap-fetch advantage from Lion. |
| Count matches within a scalar range | Consider Lion count pushdown: the measured clean range count beats B-tree at both scales. Fetching matching rows has different costs. |
| Ordered retrieval or uniqueness | Keep B-tree. Lion supplies bitmap and plain index scans and count pushdown, not ordered row retrieval or unique indexes. |
| Array membership or exact-lexeme counts | Consider Lion when counts dominate; compare against GIN on your predicates and result sizes. |
| Full-text phrase/prefix search, or searches returning documents | Prefer GIN for the measured phrase/prefix cases; ordinary document fetching shows no clear Lion advantage. |
| Frequent inserts or indexed updates | B-tree/GIN build more cheaply. Write results are mixed: Lion's inserts are slower and emit more WAL, but its indexed UPDATE is faster than B-tree in this run. GIN defers work, so include VACUUM costs. |

Lion, B-tree and GIN can coexist. Add Lion for queries that benefit, retain B-tree for transactional
access and ordered retrieval, and GIN for richer text search, and account for the storage and write cost of every
additional index. The [latest focused results](#latest-benchmarks) below show where Lion wins, where
there is practical parity, and where it loses. This remains a prototype; the measurements describe
the tested workloads, not a general replacement recommendation.

## Build and test

    ./dev.sh reset                                   # initdb a private cluster in .local/data (needs .local/pg)
    make PG_CONFIG=.local/pg/bin/pg_config && make PG_CONFIG=.local/pg/bin/pg_config install
    eval "$(./dev.sh env)"
    make PG_CONFIG=.local/pg/bin/pg_config installcheck   # regress files + isolation specs
    make unit PG_CONFIG=.local/pg/bin/pg_config           # container and sparse libraries, no server needed

Lion logs through one of two WAL resource managers (DESIGN.md §25), chosen per index at
CREATE INDEX, so the suite has to pass in both:

    make PG_CONFIG=.local/pg/bin/pg_config installcheck           # generic WAL (no preload)
    make PG_CONFIG=.local/pg/bin/pg_config installcheck-rmgr      # the custom resource manager
    make PG_CONFIG=.local/pg/bin/pg_config recovery-check       RECOVERY_PREFIX=<prefix>
    make PG_CONFIG=.local/pg/bin/pg_config recovery-check-rmgr  RECOVERY_PREFIX=<prefix>

`installcheck-rmgr` restarts the dev cluster with `shared_preload_libraries = 'pg_lion'` on
pg_ctl's command line, proves the restart took - the server lists pg_lion as a WAL resource manager
and a freshly built index reports `rmgr` - and only then runs the same suite (`walrecords` has an
expected file for each mode, so the suite alone would pass in either), and afterwards puts the
cluster back as it found it: restarted in generic mode, or stopped.  Nothing is written into
postgresql.conf, so an interrupted run leaves no trace; `make hookcheck` works the same way.
`recovery-check-rmgr` is the recovery
harness with the resource manager registered AND `wal_consistency_checking = 'pg_lion'`, which is
where "replay reproduces every page" is actually proved - the comparison only happens during
replay, so turning it on for a primary that never replays proves nothing.

`.local/pg` can be any PostgreSQL 16 or later install.  What the suites need from it:

- **contrib: `citext`, `pg_buffercache` and `pg_walinspect`.**  The regression suite creates all
  three.  Without `pg_buffercache` the `pinbudget` and `range` files and the `gettuple_pause` spec
  fail, without `pg_walinspect` `walrecords` fails, and most files use `citext`.  The PGDG packages
  (`postgresql-N`) include contrib; a source build needs `make -C contrib install`.
- **`pg_isolation_regress`** for the isolation specs: a source build installs it with
  `make -C src/test/isolation install`, and the packages ship it in `postgresql-server-dev-N`.
- **`injection_points`** for the specs that park a backend on an injection point: a server
  configured with `--enable-injection-points` (17 or later) and the test module installed with
  `make -C src/test/modules/injection_points install`.  Where the module is missing those specs are
  skipped and the run says which (`INJECTION_POINTS=1` forces them, and makes a missing module an
  error instead).
- **The recovery harness** (`make recovery-check`) needs a whole installation to initdb clusters
  in, named by `RECOVERY_PREFIX`, which it builds and installs the extension into; as root it also
  needs `RECOVERY_RUN_AS=<unprivileged user>` (`test/recovery/README.md`).

`dev.sh` puts its cluster's socket in `$XDG_RUNTIME_DIR/pg_lion-<user>` (or `/tmp/pg_lion-<user>`)
on port 54329, and `LION_SOCK` / `LION_PORT` move it; the cluster runs with `wal_level = replica`, so
that index builds and the write paths of indexes created in the same transaction are WAL-logged in
the suite as they are in production.

## How to use it

For an existing `events` table with `country text`, `event_type text`, and
`created_at timestamptz`, create separate Lion indexes on the count/filter columns. The default
index options are the right ones to start with.

```sql
CREATE EXTENSION pg_lion;
CREATE INDEX events_country_lion ON events USING lion (country);
CREATE INDEX events_type_lion ON events USING lion (event_type);

-- Refresh statistics and visibility after loading data; run outside a transaction block.
VACUUM (ANALYZE) events;

-- Equality count and intersection of two indexed filters.
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)
SELECT count(*) FROM events WHERE country = 'Japan';

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)
SELECT count(*) FROM events
WHERE country = 'Japan' AND event_type = 'purchase';

-- Counts per group.
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)
SELECT country, count(*) FROM events GROUP BY country;

-- Retain B-tree for ordered retrieval.
CREATE INDEX events_created_at_btree ON events (created_at);
```

Count pushdown is enabled by default. Look for **`Custom Scan (LionCount)`** in EXPLAIN to confirm
it was selected. The planner can choose an ordinary scan when pushdown is unsupported or estimated
to cost more; measure the default plan before changing planner settings. A Bitmap Heap Scan still
fetches heap tuples and does not have the same count shortcut. Normal SQL is sufficient; direct
`lion_index_count()` calls are optional. Use `lion_index_stats('events_country_lion')` to inspect
storage and `lion_index_verify('events_country_lion', heapallindexed => true)` for verification.

For an existing `docs(tags text[], tsv tsvector)` table, a count-oriented array example is:

```sql
CREATE INDEX docs_tags_lion ON docs USING lion (tags);
VACUUM (ANALYZE) docs;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)
SELECT count(*) FROM docs WHERE tags @> ARRAY['t1', 't17'];

-- Choose GIN for phrase/prefix searches on the text-search column.
CREATE INDEX docs_tsv_gin ON docs USING gin (tsv);
```

## SQL functions

Ordinary SQL is the interface - the planner uses the index and the `LionCount` pushdown on its own -
and these functions are for testing, diagnostics and the occasional direct count:

    lion_index_count(idx, key)                      count(*) WHERE col = key, through one index
    lion_index_count(idx1, key1, idx2, key2)        ... AND col2 = key2, two indexes on one table
    lion_index_count_any(idx, keys)                 count(*) WHERE col = ANY (keys)
    lion_index_count_stats(idx, key)                lion_index_count() plus the heap it visited
    lion_index_count_group_stats(idx [, use_cache, attno])   every group's count, as GROUP BY does
    lion_index_stats(idx)                           the index's shape, one row per key column
    lion_index_verify(idx [, heapallindexed])       structural check, optionally against the heap
    lion_index_wal_mode(idx)                        'generic' or 'rmgr' (DESIGN.md §25)
    lion_index_posting_root(idx, key)               a key's posting-tree root block, for tests

The counts answer exactly what the equivalent `SELECT count(*)` answers under the same snapshot,
and ask for what it would: SELECT on the table or its indexed columns, and no row-level security in
force for the caller.  They take one INDEX, and an index belongs to one table, so they count that
table's own rows and nothing else.  On an inheritance parent that is the parent's rows alone - the
count of `SELECT count(*) FROM ONLY parent WHERE ...`, never the children's, even though a plain
`FROM parent` includes them.  A partitioned table's index has no storage and is refused
(`"..." is not an index`); pass a partition's own index, or write the `SELECT count(*)` against the
partitioned table and let the pushdown count every partition (DESIGN.md §16).  The pushdown is not
attempted for an old-style inheritance parent without `ONLY`, whose children need not even share
its columns: that query takes the ordinary plan.  The four diagnostic functions below the counts
are not executable by PUBLIC, as with pageinspect and amcheck; `lion_index_stats()` is granted to
`pg_stat_scan_tables`.
`lion_index_verify()` checks the index while it is being written to, the way `CREATE INDEX
CONCURRENTLY` builds one: it takes ShareUpdateExclusiveLock on the table and the index, so INSERT,
UPDATE and DELETE go on while it runs, and VACUUM, ANALYZE, DDL and a second verify wait for it.
What a concurrent insert could make look wrong it checks again once the statements that were writing
the index have ended, so it may wait for them - for as long as statement_timeout and lock_timeout
allow - but never reports their changes as damage.  On a hot standby it takes AccessShareLock and
is exact only while replay leaves the index alone.  With `heapallindexed` it evaluates the index's
expressions as the table's owner (DESIGN.md §7).

## Source layout

    src/lion_tid.h          TID <-> (container key, 15-bit lo) encoding; 9 offset bits at 8K pages
    src/lion_container.[ch] container library (array/bitset/run), set algebra, unit-tested standalone
    src/lion_sparse.[ch]    sparse (container key, offset) segments, unit-tested standalone
    src/lion.h, lion_pages.c on-disk structs; meta/entry/leaf primitives, page splits
    src/lion_compat.h       the differences between PostgreSQL 16, 17, 18, 19 and master
    src/lion_wal.[ch]       the WAL shim every write path calls, and the custom resource manager
    src/lion_dir.c          the sorted entry directory: a Lehman & Yao B-tree keyed by the index key
    src/lion_posting.c      the per-key posting tree: a B-tree over container keys, GIN's shape
    src/lion_am.c           handler, reloptions, amvalidate, cost estimate, buildempty, _PG_init hook/GUC
    src/lion_build.c        ambuild via tuplesort (hash, key, tid code); INLINE entries or per-key posting trees
    src/lion_scan.c         amgetbitmap, and amgettuple for plain index scans
    src/lion_insert.c       aminsert (serialised on the directory leaf; bitset in-place fast path)
    src/lion_vacuum.c       ambulkdelete with cleanup locks on every page, two-pass cancellable protocol
    src/lion_funcs.c        lion_index_stats(), lion_index_verify() and the other diagnostics
    src/lion_count.[ch]     lion_count_keys(): VM-interlocked counting, per-block batched heap recheck
    src/lion_customscan.c   create_upper_paths_hook -> CustomPath/CustomScan "LionCount"
    src/lion_fkjoin.[ch]    the FK-side join a LionCount answers (GROUP BY dim.attr over a fact table)
    src/lion_ordered.c      CustomScan "LionOrdered": lion-filtered, btree-ordered scans
    src/lion_multikey.c     array_ops/tsvector_ops: GIN-style extraction and tsquery key trees
    test/sql, test/isolation, test/unit, test/recovery, test/modules

## Key types

One default operator class per type, reusing the hash access method's hash functions: integers
(`int2/int4/int8`, one family with cross-type equality), `float4/float8`, `numeric`, `bool`, `"char"`,
`name`, `text` (and `varchar` through it), `char(n)`, `bytea`, `uuid`, `date`, `time`, `timetz`,
`timestamp`, `timestamptz`, `interval`, `macaddr`, `inet`, `jsonb`, `pg_lsn`, `xid`, `cid`, `tid`,
`oid`, and any enum. Case-insensitive text: `CREATE EXTENSION pg_lion_citext` (requires
`citext`) adds `citext_ops`. Domains resolve to their base type. Keys over 2000 bytes are rejected.

## Multi-key columns: arrays and tsvector (DESIGN.md §17)

`array_ops` (any array type) and `tsvector_ops` index one row under many keys, reusing GIN's own
extraction functions, and answer

    CREATE INDEX ON doc USING lion (tags);       -- text[], int[], ...
    CREATE INDEX ON doc USING lion (tsv);        -- tsvector

    tags @> '{a,b}'    the intersection of the elements' posting sets, exact
    tags && '{a,b}'    their union, exact
    tags <@ '{a,b}'    every indexed row, rechecked in the heap
    tsv  @@ 'a & (b | c)'   an AND/OR tree over the lexemes, exact

`count(*)` over `@>`, `&&` and an AND/OR tsquery is pushed down like any other clause, and can be
combined with a `GROUP BY` on a scalar roaring column. Everything a plain AND/OR of key sets cannot
express - `<@`, `@> '{}'`, a NULL element, and a tsquery with `!`, `<->`, `foo:*` or weights - falls
back to scanning every indexed row and rechecking it if the Lion index is used. The planner may
choose a sequential scan instead; GIN wins the measured phrase/prefix cases below.

## Reloptions

`fillfactor` (10 .. 100, default 90): how full the build packs a directory leaf.
`max_entries` (default 0 = unlimited): an advisory cardinality guard.  The index warns once per
backend when it has grown past that many distinct keys - counted exactly by a build, estimated by an
insert that adds a key as the entries on its directory leaf times the number of leaves - and never
rejects a row.  For a multi-key column the keys are the extracted elements or lexemes.
`inline_limit` (64 .. 4096 bytes, default 4096): how large a key's posting set may be before it
moves out of its entry tuple onto container pages of its own.
`buckets` is accepted and ignored since format 4 - the entry directory is a B-tree keyed by the
index key, and it grows by splitting instead of being sized once.
`wal_mode` (`auto` | `generic` | `rmgr`, default `auto`): which WAL resource manager this index is
logged through (DESIGN.md §25).  Measured on a release build: the 8-client hot-key insert burst goes
from 765 to 1275 tps (p95 14.8 to 9.9 ms, against btree's 1632 / 7.8), 10,000 inserts into the
1M-row eight-index portfolio from 922 ms / 88 MiB to 254 ms / 65 MiB, and an insert into a key whose
posting set is still inside its entry tuple from 2919 to 422 bytes of WAL.  One case is worse: a
dense key whose container has to be rewritten whole costs ~1.6 KB where a generic page diff cost
~122 B, which is +40% of WAL on that path and is the first thing §25 lists to fix.  `auto` is `rmgr` when the server registered Lion's own resource
manager and `generic` otherwise, so one CREATE INDEX script works on a cluster that preloads the
library and on one that does not.  The mode is fixed at build time and recorded on the meta page;
`SELECT lion_index_wal_mode(idx)` reports it and REINDEX is what changes it.  An rmgr-mode index can
be READ on any server but can only be WRITTEN where the resource manager is registered, which needs

    shared_preload_libraries = 'pg_lion'      # and a restart
    # optional: pg_lion.rmgr_id = 128         # the id to register under

`pg_lion.rmgr_id` defaults to 128, `RM_EXPERIMENTAL_ID`, the id the PostgreSQL project reserves for
development: two extensions that both take it cannot be loaded together, so a production cluster
should check `pg_get_wal_resource_managers()` and move one of them.  Once an index has been written
in rmgr mode the library must stay in `shared_preload_libraries` for as long as WAL that mentions it
may still be replayed - the rule core states for every custom resource manager.
`lion_index_stats()` reports ONE ROW PER KEY COLUMN (DESIGN.md §24), with a leading `attno`: the
directory's height, its leaf and internal pages and whether that column is `ordered` (false for a
key type with no btree opclass, whose entries are then in a complete but arbitrary order), the
column's entries, its containers by kind, its sparse segments, its posting trees' internal pages and
tallest height, `null_tids`, the number of rows whose key in that column is NULL, and `empty_tids`,
the number of rows a multi-key opclass extracted no key from.  `slack_bytes` and
`inline_slack_bytes` are the growth slack inserts leave inside items and inside INLINE entry
payloads (DESIGN.md §4), which is space a later insert into the same key grows into for free; a
bulk-built index has none of either.  The counters that describe the
relation rather than a column - the directory's shape, free and deleted pages - are repeated on
every row.

## Known limitations

Equality, `IN` lists, scalar ranges and the multi-key operators above are supported, through bitmap
scans and plain index scans (`amgettuple`, DESIGN.md §29). There are no ordered index scans (an
`ORDER BY` needs a B-tree, which `LionOrdered` combines with a lion filter, §30), no index-only scans
that return a column (only those that need none, like `count(*)`), no INCLUDE columns, no
parallel build or scan, no reclaim of an emptied directory leaf or of an emptied posting-tree leaf
(both wait for the whole set or the whole index to go). Inserts serialise on the directory
leaf that holds the key; see the measured
[write costs](#writes-and-maintenance-5m-rows) below. Count pushdown supports constants and parameters,
a `GROUP BY` of one or two indexed columns, and a `HAVING` over the counts it computes (a `HAVING`
with a correlated subquery, or a `GROUP BY` of three or more columns, goes to the ordinary plan). `IN` lists of more than 1000
values are left to the ordinary plan, a multi-key index can never drive a `GROUP BY` or a
sum-over-all-entries count (its entries are keys, not row values), and the cost model inherits the
stale `relallvisible` blind spot of index-only scans. Indexes built before NULL keys existed (meta page version 1) are refused
with an error and have to be rebuilt with REINDEX.
On a hot standby a GENERIC-mode index's count paths recheck every candidate TID in the heap instead
of trusting the visibility map, because generic WAL replay does not take the cleanup locks the pin
interlock relies on; they stay correct there but are no longer O(1) per container.  An rmgr-mode
index does not pay that: its removal records replay under a cleanup lock, so the standby uses the
visibility map again (DESIGN.md §25) - at the price that a standby reader holding a pin makes replay
wait, which `max_standby_streaming_delay` resolves as a recovery conflict.  A count that reads even
one generic-mode index falls back to rechecking everything, because the interlock has to hold for
every source it intersects. The SQL count functions require SELECT
on the table or on the indexed columns and refuse tables where row-level security applies to the
caller; the pushdown only uses an index whose collation matches the clause or grouping collation.

## Latest benchmarks

Measured on **2026-09-24 (UTC), PostgreSQL 18.6, commit `118f623`**, using the
[focused benchmark](bench/comprehensive/README.md): **1M and 5M scalar rows, 200k documents,
286 exact-result checks and 858 timings**, all passing. The run took **394.0 seconds
(6m 34s)** including cluster setup and shutdown; extension compilation and report generation
are separate. [Full report](bench/results/2026-09-24-118f623-pg18-focused/REPORT.md) · [HTML](bench/results/2026-09-24-118f623-pg18-focused/index.html) ·
[CSV](bench/results/2026-09-24-118f623-pg18-focused/summary.csv) · [Audit](bench/results/2026-09-24-118f623-pg18-focused/AUDIT.json).

At 5M rows, Lion's clean dense count is **90.0× faster** than B-tree,
200-group aggregation is **23.8× faster**, and the 1,000-value IN count is
**2.5× faster**. Range-count pushdown now wins too: **0.663 ms versus B-tree's
1.629 ms (2.5×)**. Tiny equality probes are at practical parity. Keep B-tree for
ordered retrieval and GIN for phrase/prefix search; dirty pages and build/write costs
still matter.

These timings include the range changes through `118f623`. The subsequent comparator-guard
fix at `07f8fcb` passed correctness verification but was not separately benchmarked.

### Setup and interpretation

Release PostgreSQL 18.6 (`-O2`, assertions off), AMD Ryzen 7 5700X3D, 16 logical CPUs, WSL2;
512 MB shared buffers and 64 MB work_mem. Durability is enabled; parallel query, JIT and autovacuum
are disabled. Lion is preloaded and uses its **custom WAL resource manager**. Each query has one
warmup and three timed rounds. Tables show median EXPLAIN execution time in **milliseconds**;
planning is separate. Three samples characterize a progress run, not statistical equivalence.
Warm means no deliberate cache eviction, not that every page fits in shared buffers.
[Environment and build provenance](bench/results/2026-09-24-118f623-pg18-focused/metadata.json) · [Archived source](bench/results/2026-09-24-118f623-pg18-focused/source.tar.gz).

Each scalar portfolio has **seven** single-column indexes: `c2`, `c20`, `c200`, `c20k`, `c1m`,
`skew`, and `nullable`. Documents have `tags` and `tsv` indexes. GIN uses `btree_gin` for scalars
with `fastupdate=on`. Lion and **Lion (pushdown off)** share the same `USING lion` indexes;
their artifact labels are `roaring` and `roaring_bitmap`. The tables use the default planner:
**† marks an actual sequential scan**, not index performance. The report also contains forced-index
and 64 kB work_mem diagnostics. This profile has more indexes and different query/mutation coverage
than the earlier quick runs; their portfolio costs and timings are not a like-for-like change series.

### Where Lion wins: counts and grouping

Clean measurements follow VACUUM FREEZE. Speedups are competitor median divided by Lion median,
computed before rounding; higher is better for Lion. A dash means B-tree has no matching document
measurement. Ratios include any sequential fallbacks marked †.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms | Speedup vs B-tree / GIN |
| --- | --- | --- | --- | --- | --- |
| 1M scalar | Count ~50% / clean | 30.464 | 80.748 | 0.350 | 87.0× / 230.7× |
| 1M scalar | Count ~0.5% / clean | 0.334 | 3.095 | 0.028 | 11.9× / 110.5× |
| 1M scalar | Two equality predicates / clean | 2.619 | 31.742 | 0.362 | 7.2× / 87.7× |
| 1M scalar | 1,000-value IN / clean | 3.327 | 18.954 | 1.608 | 2.1× / 11.8× |
| 1M scalar | Count per 200 groups / clean | 79.972 | 139.114 † | 3.648 | 21.9× / 38.1× |
| 1M scalar | Count `c20k BETWEEN 100 AND 199` / clean | 0.304 | 37.070 | 0.190 | 1.6× / 195.1× |
| 5M scalar | Count ~50% / clean | 157.028 | 665.041 | 1.744 | 90.0× / 381.3× |
| 5M scalar | Count ~0.5% / clean | 1.612 | 18.097 | 0.095 | 17.0× / 190.5× |
| 5M scalar | Two equality predicates / clean | 16.380 | 171.158 | 1.778 | 9.2× / 96.3× |
| 5M scalar | 1,000-value IN / clean | 17.107 | 373.333 | 6.736 | 2.5× / 55.4× |
| 5M scalar | Count per 200 groups / clean | 405.735 | 1,045.313 † | 17.074 | 23.8× / 61.2× |
| 5M scalar | Count `c20k BETWEEN 100 AND 199` / clean | 1.629 | 428.312 | 0.663 | 2.5× / 646.0× |
| 200k documents | Array contains `t1` | — | 4.045 | 0.024 | — / 168.5× |
| 200k documents | Array contains `t1` and `t17` | — | 0.820 | 0.125 | — / 6.6× |
| 200k documents | Array overlaps three tags | — | 6.449 | 0.227 | — / 28.4× |
| 200k documents | Full-text `w1 & w17` count | — | 0.952 | 0.118 | — / 8.1× |

These gains apply to the supported count shapes, not arbitrary aggregates or retrieving every match.
The same Lion indexes with count pushdown disabled show the difference:

| 5M-row query / clean | Lion ms | Lion, pushdown off ms |
| --- | --- | --- |
| Count ~50% | 1.744 | 509.001 |
| Two equality predicates | 1.778 | 46.238 |
| Count per 200 groups | 17.074 | 1,059.507 † |
| Range count | 0.663 | 152.083 |

The 5M range count fell from **712.504 ms** at `7cb7711` (sequential fallback) to
**0.663 ms** with `LionCount`. This is an aggregate-pushdown gain: with pushdown off,
Lion's bitmap range count takes **152.083 ms**, versus B-tree's **1.629 ms** index-only
scan. Do not generalize the count result to fetching the matching rows. Small changes in
other cases are not established improvements from three timing rounds.

### Dirty data: the visibility-map trade-off

After scattered payload updates to 1% of rows and ANALYZE, without VACUUM, Lion must check
visibility in the heap for affected pages. That 1% is a row fraction, not a heap-page fraction;
the [raw operations](bench/results/2026-09-24-118f623-pg18-focused/operations.jsonl) record actual all-visible coverage.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms | Speedup vs B-tree / GIN |
| --- | --- | --- | --- | --- | --- |
| 5M scalar | Count ~50% / dirty | 711.865 | 829.709 | 81.315 | 8.8× / 10.2× |
| 5M scalar | Count ~0.5% / dirty | 5.940 | 61.288 | 5.608 | 1.1× / 10.9× |
| 5M scalar | Two equality predicates / dirty | 18.284 | 171.558 | 3.962 | 4.6× / 43.3× |
| 5M scalar | Count per 200 groups / dirty | 925.271 † | 962.271 † | 283.387 | 3.3× / 3.4× |

Vacuum cadence and update distribution matter: do not apply the clean-data speedups to a frequently
updated table. The dirty 0.5%-selectivity count is effectively at parity with B-tree here
(5.608 versus 5.940 ms), despite its large clean-data advantage. The suite also checks results
after indexed writes, deletes, and VACUUM.

**Version caveat — earlier PG16–20 runs (`eb1579e`):** ordinary reads on PG19/20 can restore
all-visible pages during heap pruning. In that scattered-update fixture, reference scans and
warmups restored almost all visibility before timing on **19beta4 and 20devel**.
The 5M-row dense count then took about **2 ms**, versus
**74–80 ms on PG16–18**. These are warmed post-update reads, not equivalent dirty-page conditions
or a general 40× Lion speedup; the first read's cleanup cost is outside the timed samples.
Clean dense counts in that earlier comparison were about **1.8–2.1 ms** across all five versions.
See the [per-sample visibility evidence from those earlier runs](bench/results/2026-09-24-118f623-pg18-focused/earlier-version-visibility.csv).
The headline tables above remain PostgreSQL 18 measurements.

### Where it is at practical parity: tiny probes and fetching rows

Selective equality counts finish in tens of microseconds for both B-tree and Lion. The absolute
gap is too small to justify an additional index on these three samples alone. Fetching payloads
requires heap access and does not benefit from count pushdown.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 1M scalar | Count ~0.005% | 0.020 | 0.056 | 0.013 |
| 5M scalar | Count ~0.005% | 0.034 | 0.975 | 0.017 |
| 5M scalar | Rare equality (up to 1M keys) | 0.035 | 0.382 | 0.019 |
| 1M scalar | Sum ID/payload length for `c200=17` | 3.445 | 3.482 | 3.053 |
| 5M scalar | Sum ID/payload length for `c200=17` | 14.828 | 57.216 | 16.280 |
| 200k documents | Sum ID/payload length for full-text `w1` | — | 4.179 | 4.325 |

“Practical parity” means similar scale or no demonstrated useful advantage, not statistical
equivalence. Heap-fetch results are cache-sensitive, especially at 5M rows; differences between
Lion and pushdown-off executions of the same bitmap plan are not a count-pushdown benefit.

### Where Lion loses: ordering, phrase and prefix search

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 5M scalar | Ordered LIMIT 100 | 0.021 | 720.948 † | 857.024 † |
| 200k documents | Phrase `common <-> w1` | — | 6.997 | 32.481 † |
| 200k documents | Prefix `rare12:*` | — | 1.552 | 25.077 † |

Lion does not provide ordered row retrieval and stores no full-text positions or sorted lexemes
for phrase/prefix pruning. These queries use sequential fallbacks here. Keep B-tree for ordered
retrieval and GIN for richer full-text search. The report additionally includes skew,
OR/IN intersections, selective three-predicate AND, NULL counts, and low-memory cases.
FK-side join pushdown is not timed by this benchmark.
[Exact SQL and workload manifest](bench/results/2026-09-24-118f623-pg18-focused/queries.json).

### Build time and index storage

One build per index; times are summed across each portfolio and sizes are measured before mutations.
Scalar portfolios contain seven indexes and documents two. Pushdown changes neither construction
nor storage. These are portfolio totals: high-cardinality columns can change the space trade-off,
and Lion is not smaller for every distribution.

At 5M rows, Lion uses about **25% less space than B-tree** and **23% more than GIN**;
at 1M, it uses about **15% more than either**. Its scalar build takes roughly **2.8–2.9×**
the B-tree time. The additional high-cardinality and skew indexes matter to these totals.

| Dataset | Family | Build seconds | Index MiB |
| --- | --- | --- | --- |
| 1M scalar | B-tree | 1.692 | 58.984 |
| 1M scalar | GIN | 2.000 | 58.680 |
| 1M scalar | Lion | 4.656 | 67.797 |
| 5M scalar | B-tree | 8.958 | 256.289 |
| 5M scalar | GIN | 7.622 | 156.961 |
| 5M scalar | Lion | 25.723 | 192.484 |
| 200k documents | GIN | 0.304 | 6.375 |
| 200k documents | Lion | 1.732 | 7.672 |

### Writes and maintenance: 5M rows

Each portfolio inserts 50k rows, updates `c200` in 50k existing rows, deletes rows where
`id % 100 = 1`, then runs VACUUM ANALYZE. A checkpoint precedes each operation. Cells show
**wall time in ms / WAL in MiB**, including all seven indexes. These are single observations;
GIN's fast updates defer work, so include its VACUUM/drain cost.

| Operation | B-tree ms / MiB | GIN ms / MiB | Lion ms / MiB |
| --- | --- | --- | --- |
| INSERT | 983.2 / 119.58 | 344.6 / 61.37 | 1,228.0 / 176.91 |
| Indexed UPDATE | 2,533.0 / 138.53 | 756.4 / 77.89 | 1,675.6 / 209.36 |
| DELETE | 1,871.6 / 366.12 | 1,007.5 / 366.12 | 1,139.8 / 366.12 |
| VACUUM ANALYZE | 3,439.5 / 634.38 | 3,554.4 / 587.33 | 3,750.8 / 569.33 |

This run uses Lion's custom WAL manager; earlier quick measurements used generic WAL and a smaller
portfolio. Do not attribute their difference solely to code changes. The focused suite does not
measure sustained concurrent-write throughput.

Here Lion's insert is **1.25× slower than B-tree**, while its indexed UPDATE takes about
**0.66× the time**; both generate roughly **1.5× the WAL**. GIN's insert/update steps are faster
than Lion's, before its deferred work is drained. Lion's VACUUM is slower than both B-tree
and GIN in this run.

### Reproduce and earlier measurements

Build/install against the desired release PostgreSQL version, then:

```sh
python3 bench/comprehensive/run.py --prefix /path/to/postgresql --preload \
  --output bench/results/my-focused-run
python3 bench/comprehensive/report.py bench/results/my-focused-run
python3 bench/comprehensive/audit.py bench/results/my-focused-run
```

The defaults retain 1M/5M rows and 200k documents. See the [focused suite documentation](bench/comprehensive/README.md)
for coverage and `--profile full`; [quick.py](bench/QUICK.md) remains the smaller progress check.
The [previous quick report](bench/results/quick/2026-09-22-3906538/REPORT.md),
[older comparison](bench/COMPARISON.md), [growth/churn supplement](bench/results/2026-09-21-stress/REPORT.md),
and [historical README archive](bench/HISTORICAL_README_BENCHMARKS.md) describe earlier commits and workloads.
The [follow-up review](FOLLOWUP_REVIEW.md) records correctness and compatibility validation.

## License

Copyright (c) 2026, Chinmay Kanchi.

Licensed under the [PostgreSQL License](LICENSE) (SPDX: `PostgreSQL`).
