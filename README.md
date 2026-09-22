# pg_lion — a roaring-bitmap inverted index AM for PostgreSQL (prototype)

Formerly `roaring_index`; renamed to pg_lion on 2026-09-21 (a roaring bitmap index, hence the lion). The on-disk
format is unchanged: the meta-page magic still spells `RBI1`, so indexes built before the rename remain readable.

`CREATE INDEX ... USING lion (col)` builds one posting set of roaring-style containers
(array / bitset / run, ≤ 4104 bytes each, one per 64 heap pages) or sparse (container key, offset)
segments per distinct key, plus one reserved entry for the rows whose key is NULL. The index serves
Bitmap Index Scans through `amgetbitmap` for `col = v`, `col = ANY (list)`, `col IS NULL` and
`col IS NOT NULL`, and a CustomScan (`LionCount`) answers
`SELECT count(*) [, k] FROM t WHERE k1 = c1 [AND ...] [GROUP BY k]` from the containers plus the
visibility map, visiting the heap only for pages that are not all-visible. `DESIGN.md` is the spec:
on-disk format, locking protocol, the VACUUM/visibility-map interlock argument (§9, §11), and the
planner integration (§10).

Status: prototype against PostgreSQL master (20devel). Latest validation passes 17 SQL regression
files, 6 isolation specs (including an injection-point check of the VACUUM/container-page interlock),
660k container and 290k sparse unit checks, and the crash-recovery/hot-standby harness.
See the [latest review](FOLLOWUP_REVIEW.md) for evidence and remaining limitations.

## When to use Lion

Use the roaring index (`USING lion`) for read-heavy dashboards, facet counts, and aggregations over
large tables: equality or NULL counts, intersections of indexed predicates, and grouping by a column
with relatively few distinct values. It can count compressed posting sets without fetching every
matching row when the visibility map allows it. Array membership and simple full-text AND/OR counts
benefit from the same mechanism. Keep tables vacuumed and statistics current so the planner can
estimate that benefit; dirty pages require visibility checks in the heap.

| Your workload | Index choice and tradeoff |
| --- | --- |
| Count many matches, combine equality filters, or count groups | Consider Lion on the columns used by these queries. The largest measured gains come from `LionCount` pushdown. |
| Fetch a handful of rows, or count a very selective key | B-tree is a strong default. The latest run shows practical parity for tiny equality counts and no consistent heap-fetch advantage from Lion. |
| Range predicates, ordering, or uniqueness | Keep B-tree. Lion supports neither ordered access nor unique indexes; its range queries in the benchmark fall back to sequential scans. |
| Array membership or exact-lexeme counts | Consider Lion when counts dominate; compare against GIN on your predicates and result sizes. |
| Full-text phrase/prefix search, or searches returning documents | Prefer GIN for the measured phrase/prefix cases; ordinary document fetching shows no clear Lion advantage. |
| Frequent inserts or indexed updates | B-tree/GIN were cheaper to build and maintain in this run. Lion's count gains must justify its extra write latency and WAL. GIN uses deferred updates, so include VACUUM costs in comparisons. |

Lion, B-tree and GIN can coexist. Add Lion for queries that benefit, retain B-tree for transactional
access/ranges and GIN for richer text search, and account for the storage and write cost of every
additional index. The [latest quick results](#latest-benchmarks) below show where Lion wins, where
there is practical parity, and where it loses. This remains a prototype; the measurements describe
the tested workloads, not a general replacement recommendation.

## Build and test

    ./dev.sh reset                                   # initdb a private cluster in .local/data (needs .local/pg)
    make PG_CONFIG=.local/pg/bin/pg_config && make PG_CONFIG=.local/pg/bin/pg_config install
    eval "$(./dev.sh env)"
    make PG_CONFIG=.local/pg/bin/pg_config installcheck   # 17 regress files + 6 isolation specs
    make unit PG_CONFIG=.local/pg/bin/pg_config           # container and sparse libraries, no server needed

`.local/pg` must be a PostgreSQL master install; for the isolation specs it needs
`--enable-injection-points` and the `injection_points` test module installed, and
`pg_isolation_regress` installed from `src/test/isolation`.

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

-- Retain B-tree for a different access pattern: time ranges and ordered retrieval.
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

## Source layout

    src/lion_tid.h          TID <-> (container key, 15-bit lo) encoding; 9 offset bits at 8K pages
    src/lion_container.[ch] container library (array/bitset/run), set algebra, unit-tested standalone
    src/lion.h, lion_pages.c on-disk structs; meta/entry/leaf primitives, page splits, generic WAL
    src/lion_dir.c          the sorted entry directory: a Lehman & Yao B-tree keyed by the index key
    src/lion_posting.c      the per-key posting tree: a B-tree over container keys, GIN's shape
    src/lion_am.c           handler, reloptions, amvalidate, cost estimate, buildempty, _PG_init hook/GUC
    src/lion_build.c        ambuild via tuplesort (hash, key, tid code); INLINE entries or per-key posting trees
    src/lion_scan.c         amgetbitmap
    src/lion_insert.c       aminsert (serialised on the directory leaf; bitset in-place fast path)
    src/lion_vacuum.c       ambulkdelete with cleanup locks on every page, two-pass cancellable protocol
    src/lion_funcs.c        lion_index_stats(), lion_index_verify()
    src/lion_count.[ch]     lion_count_keys(): VM-interlocked counting, per-block batched heap recheck
    src/lion_customscan.c   create_upper_paths_hook -> CustomPath/CustomScan "LionCount"
    src/lion_multikey.c     array_ops/tsvector_ops: GIN-style extraction and tsquery key trees
    test/sql, test/isolation, test/unit

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
choose a sequential scan instead; GIN wins the measured phrase/prefix cases below. The reloption `max_entries` (default 0 = unlimited) makes the index warn once per backend when it grows
past that many distinct keys; it never rejects a row.

## Reloptions

`fillfactor` (10 .. 100, default 90): how full the build packs a directory leaf.
`max_entries` (0 = unlimited; the advisory cardinality guard above) and
`inline_limit` (64 .. 4096 bytes, default 4096): how large a key's posting set may be before it
moves out of its entry tuple onto container pages of its own.
`buckets` is accepted and ignored since format 4 - the entry directory is a B-tree keyed by the
index key, and it grows by splitting instead of being sized once.
`lion_index_stats()` reports ONE ROW PER KEY COLUMN (DESIGN.md §24), with a leading `attno`: the
directory's height, its leaf and internal pages and whether that column is `ordered` (false for a
key type with no btree opclass, whose entries are then in a complete but arbitrary order), the
column's entries, its containers by kind, its sparse segments, its posting trees' internal pages and
tallest height, `null_tids`, the number of rows whose key in that column is NULL, and `empty_tids`,
the number of rows a multi-key opclass extracted no key from.  The counters that describe the
relation rather than a column - the directory's shape, free and deleted pages - are repeated on
every row.

## Known limitations

Equality, `IN` lists and the multi-key operators above (no ranges), no
`amgettuple`/index-only scans, no INCLUDE columns, no
parallel build or scan, no reclaim of an emptied directory leaf or of an emptied posting-tree leaf
(both wait for the whole set or the whole index to go). Inserts serialise on the directory
leaf that holds the key; see the measured
[write costs](#writes-and-maintenance-5m-rows) below. Count pushdown supports constants and parameters
but no multi-column GROUP BY. `IN` lists of more than 1000
values are left to the ordinary plan, a multi-key index can never drive a `GROUP BY` or a
sum-over-all-entries count (its entries are keys, not row values), and the cost model inherits the
stale `relallvisible` blind spot of index-only scans. Indexes built before NULL keys existed (meta page version 1) are refused
with an error and have to be rebuilt with REINDEX.
On a hot standby the count paths recheck every candidate TID in the heap instead of trusting the
visibility map (generic WAL replay does not take the cleanup locks the pin interlock relies on), so
they stay correct there but are no longer O(1) per container. The SQL count functions require SELECT
on the table or on the indexed columns and refuse tables where row-level security applies to the
caller; the pushdown only uses an index whose collation matches the clause or grouping collation.

## Latest benchmarks

Measured on **2026-09-22 at `3906538`**, using the [quick benchmark](bench/QUICK.md):
**1M and 5M scalar rows, 200k documents, 160 exact-result checks and 320 timings**, all passing.
The entire run, including private-cluster setup and shutdown, took **227.0 seconds (3m 47s)**.
[Full report](bench/results/quick/2026-09-22-3906538/REPORT.md) ·
[HTML tables](bench/results/quick/2026-09-22-3906538/index.html) ·
[CSV](bench/results/quick/2026-09-22-3906538/summary.csv) ·
[Audit](bench/results/quick/2026-09-22-3906538/AUDIT.json).

Lion's largest gains are in count pushdown: at 5M rows, the clean dense count is about **92× faster**
and 200-group aggregation **21× faster** than the B-tree portfolio. Tiny B-tree/Lion equality probes
are at practical parity. B-tree wins on ranges; the 100-value IN case is now Lion's at both scales; GIN wins on phrase/prefix
searches. Lion costs more to build and write, and its
five-index portfolio is smaller than B-tree's but larger than GIN's.

### Setup and interpretation

Release PostgreSQL 20devel (`-O2`), AMD Ryzen 7 5700X3D, 16 logical CPUs, WSL2;
512 MB shared buffers and 64 MB work_mem. Durability is enabled; parallel query, JIT and autovacuum
are disabled. Each query has one warmup and two timed rounds. Tables below show median EXPLAIN
execution time in **milliseconds**; planning time is separate in the full report. These are progress
measurements, not confidence intervals. Warm means no deliberate cache eviction, not that all pages
fit in shared buffers. [Recorded environment](bench/results/quick/2026-09-22-3906538/metadata.json).

The scalar portfolios each have five single-column indexes: `c2`, `c20`, `c200`, `c20k`, and
`nullable`. GIN uses `btree_gin` with `fastupdate=on`. Lion and **Lion (pushdown off)** share the same
`USING lion` indexes; their artifact labels are `roaring` and `roaring_bitmap`. All query timings below
use the default planner, so **† marks an actual sequential scan**, not index performance. Other cells may
use LionCount, index-only scans or bitmap scans; every route is recorded in the full report.

### Where Lion wins: counts and grouping

These are default-planner results. Clean rows follow VACUUM; dirty rows follow scattered payload
updates to 1% of rows and ANALYZE, without VACUUM. Row-update percentage is not the percentage of
heap pages that lose all-visible status.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms | Speedup vs B-tree / GIN |
| --- | --- | --- | --- | --- | --- |
| 1M scalar | Count ~50% of rows / clean | 33.817 | 93.520 | 0.356 | 94.9× / 262.3× |
| 1M scalar | Count ~0.5% of rows / clean | 0.346 | 3.200 | 0.027 | 12.8× / 118.5× |
| 1M scalar | Count two equality predicates / clean | 2.639 | 32.556 | 0.372 | 7.1× / 87.5× |
| 1M scalar | Count per 200 groups / clean | 80.819 | 140.827 † | 3.753 | 21.5× / 37.5× |
| 1M scalar | Count per 200 groups / dirty | 80.267 | 148.520 † | 3.641 | 22.0× / 40.8× |
| 5M scalar | Count ~50% of rows / clean | 166.330 | 923.293 | 1.811 | 91.8× / 509.8× |
| 5M scalar | Count ~0.5% of rows / clean | 1.522 | 108.290 | 0.119 | 12.7× / 906.2× |
| 5M scalar | Count two equality predicates / clean | 15.588 | 183.444 | 1.792 | 8.7× / 102.4× |
| 5M scalar | Count per 200 groups / clean | 414.257 | 1,231.842 † | 19.688 | 21.0× / 62.6× |
| 5M scalar | Count per 200 groups / dirty | 415.103 | 993.291 † | 17.846 | 23.3× / 55.7× |
| 200k documents | Array contains `t1` | — | 3.994 | 0.025 | — / 159.8× |
| 200k documents | Array contains `t1` and `t17` | — | 0.869 | 0.149 | — / 5.8× |
| 200k documents | Array overlaps `t1`, `t17`, `t123` | — | 6.617 | 0.244 | — / 27.1× |
| 200k documents | Full-text `w1 & w17` count | — | 1.030 | 0.135 | — / 7.7× |

The speedup column gives **B-tree / GIN** multipliers, calculated as competitor median divided
by Lion median using the unrounded measurements. Higher is better for Lion; the comparison includes
the actual sequential fallbacks marked †. A dash means there is no matching B-tree measurement.

At 5M rows, clean equality/intersection/grouping cases above are roughly **13–92× faster than
B-tree**. In the document cases shown, Lion's membership counts are roughly **6–160× faster than
GIN**. B-tree has no matching document index in this workload (—). This does not extend to arbitrary
aggregates or retrieving all matching rows: the optimization answers the supported count shapes.

The same Lion indexes with pushdown disabled show why the query shape matters:

| 5M-row query / clean | Lion ms | Lion, pushdown off ms |
| --- | --- | --- |
| Count ~50% of rows | 1.811 | 624.255 |
| Count two equality predicates | 1.792 | 39.047 |
| Count per 200 groups | 19.688 | 971.446 † |

### Where it is at practical parity: tiny probes and fetching rows

For very selective equality counts, B-tree and Lion both finish in tens of microseconds. The
absolute gap is too small to justify another index from these two samples alone. Fetching data
requires heap access, so the count shortcut no longer applies.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 1M scalar | Count ~0.005% of rows | 0.022 | 0.050 | 0.014 |
| 5M scalar | Count ~0.005% of rows | 0.040 | 0.978 | 0.025 |
| 1M scalar | Sum ID/payload length for `c200=17` | 3.611 | 3.132 | 3.196 |
| 200k documents | Sum ID/payload length for full-text `w1` | — | 4.457 | 4.068 |

“Practical parity” here means similar scale or no demonstrated useful advantage, not statistical
equivalence. The 5M payload-fetch measurements vary sharply even between Lion variants using the
same bitmap plan (15.664 ms with pushdown enabled versus 129.808 ms disabled). That difference is
not evidence of a pushdown benefit for fetching rows. Inspect the full report and repeat such cases.

### Where Lion loses: ranges, phrase and prefix search

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 1M scalar | Count `c20k BETWEEN 100 AND 199` | 0.313 | 41.853 | 111.055 † |
| 5M scalar | Count `c20k BETWEEN 100 AND 199` | 1.962 | 379.028 | 705.309 † |
| 200k documents | Phrase `common <-> w1` count | — | 7.088 | 35.391 † |
| 200k documents | Prefix `rare12:*` count | — | 1.673 | 27.441 † |

B-tree directly supports the range predicate, which Lion cannot index. GIN is about
**5× faster for the phrase** and **16× faster for the prefix** here.
Lion cannot narrow phrase/prefix predicates through its index (it stores no positions or sorted
lexemes); its full-walk fallback is now priced as such, so the planner picks the sequential scan (†)
rather than the slower index route the `481f876` run showed. GIN narrows these through its index.
Retain B-tree/GIN for these
access patterns. The 100-value IN case that B-tree won at `481f876` is now Lion's: 0.160 vs
0.323 ms at 1M rows and 0.869 vs 1.607 ms at 5M, after the disjoint-sum change
to IN-list counting.

The full report also includes NULL counts, three-predicate intersections, all dirty-read cases,
and reads after maintenance. Exact SQL and data distributions are in the
[workload manifest](bench/results/quick/2026-09-22-3906538/queries.json).

### Build time and index storage

Times are the sum of one build per index; sizes are measured after build, before mutations.
Scalar portfolios contain five indexes; document portfolios contain two. The pushdown setting
changes neither construction nor storage. At 5M rows, Lion uses **44% less index space than B-tree**
but **50% more than GIN**, and its build takes **2.7×** the B-tree time and **4.9×** the GIN time.
These are portfolio totals, not a claim that Lion is smaller for every column or distribution.

| Dataset | Family | Build seconds | Index MiB |
| --- | --- | --- | --- |
| 1M scalar | B-tree | 1.096 | 33.516 |
| 1M scalar | GIN | 0.771 | 18.945 |
| 1M scalar | Lion | 2.744 | 22.305 |
| 5M scalar | B-tree | 5.991 | 166.812 |
| 5M scalar | GIN | 3.281 | 62.703 |
| 5M scalar | Lion | 16.108 | 94.008 |
| 200k documents | GIN | 0.335 | 6.375 |
| 200k documents | Lion | 1.782 | 7.672 |

### Writes and maintenance: 5M rows

Each portfolio inserts 50k rows, updates `c200` in 50k existing rows, then runs VACUUM ANALYZE.
A checkpoint precedes each measured operation. Times and WAL volumes are single observations,
including all five indexes. GIN's fast updates defer work, so its subsequent VACUUM cost is shown too.

| Family | Insert ms | Insert WAL MiB | Update ms | Update WAL MiB | VACUUM ms | VACUUM WAL MiB |
| --- | --- | --- | --- | --- | --- | --- |
| B-tree | 548.301 | 59.688 | 1205.313 | 77.010 | 435.219 | 20.739 |
| GIN | 285.447 | 46.051 | 792.083 | 62.568 | 584.741 | 56.255 |
| Lion | 1704.076 | 191.936 | 2231.910 | 257.974 | 437.463 | 20.729 |

In this run, Lion's insert took **3.1×** and indexed update **1.9×** the B-tree time, with roughly
**3.2× / 3.3×** the WAL. Against GIN with `fastupdate=on`, Lion's insert/update times are about
**6.0× / 2.8×** higher, with **4.2× / 4.1×** the WAL; GIN's subsequent VACUUM is slower and writes more
WAL than Lion's. The quick suite does not measure sustained concurrent write throughput.
Since `481f876` the entry directory is a B-tree (no bucket warnings), each key's containers form a
posting tree, VACUUM deletes empty entries and recycles pages, and IN lists count by disjoint sum;
the pre-rename 20M-row and comprehensive results still describe the older layout.

### Reproduce, review, and earlier measurements

Build/install the current extension, then run `python3 bench/quick.py --prefix /path/to/postgresql`.
See [QUICK.md](bench/QUICK.md) for setup and saved-baseline comparisons. The pre-rename quick run has
a different workload hash and is not an automatic comparison baseline for this run.

The [follow-up review](FOLLOWUP_REVIEW.md) records the verified fixes, fresh regression/isolation
and recovery/standby checks, and the remaining P2 issue in the legacy benchmark cleanup path.
The quick runner uses its own disposable cluster and does not use that cleanup helper.

The [comprehensive suite](bench/comprehensive/README.md) also compares hash, GiST, BRIN, tuned
B-tree and timed sequential execution, with broader query, memory, maintenance and concurrency
coverage. Its [comparison results](bench/COMPARISON.md) and
[growth/churn/write supplement](bench/results/2026-09-21-stress/REPORT.md) describe older commits;
those larger suites were not rerun at `481f876` or `3906538`. The former README's 20M-row experiments and
pre-implementation simulation are preserved in the
[historical benchmark archive](bench/HISTORICAL_README_BENCHMARKS.md).

## License

Copyright (c) 2026, Chinmay Kanchi.

Licensed under the [PostgreSQL License](LICENSE) (SPDX: `PostgreSQL`).
