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
    src/lion.h, lion_pages.c on-disk structs; meta/entry/chain primitives, page splits, generic WAL
    src/lion_dir.c          the sorted entry directory: a Lehman & Yao B-tree keyed by the index key
    src/lion_am.c           handler, reloptions, amvalidate, cost estimate, buildempty, _PG_init hook/GUC
    src/lion_build.c        ambuild via tuplesort (hash, key, tid code); INLINE entries or per-key page chains
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
`lion_index_stats()` reports the directory's height, its leaf and internal pages and whether it is
`ordered` (false for a key type with no btree opclass, whose entries are then in a complete but
arbitrary order), the entries, the containers by kind, the sparse segments, `null_tids`, the number
of rows whose key is NULL, and `empty_tids`, the number of rows a multi-key opclass extracted no key
from.

## Known limitations

Single column, equality, `IN` lists and the multi-key operators above (no ranges), no
`amgettuple`/index-only scans, no
parallel build or scan, no reclaim of an emptied directory leaf. Inserts serialise on the directory
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

Measured on **2026-09-21 at `481f876`**, using the [quick benchmark](bench/QUICK.md):
**1M and 5M scalar rows, 200k documents, 160 exact-result checks and 320 timings**, all passing.
The entire run, including private-cluster setup and shutdown, took **231.4 seconds (3m 51s)**.
[Full report](bench/results/quick/2026-09-21-481f876/REPORT.md) ·
[HTML tables](bench/results/quick/2026-09-21-481f876/index.html) ·
[CSV](bench/results/quick/2026-09-21-481f876/summary.csv) ·
[Audit](bench/results/quick/2026-09-21-481f876/AUDIT.json).

Lion's largest gains are in count pushdown: at 5M rows, the clean dense count is about **82× faster**
and 200-group aggregation **22× faster** than the B-tree portfolio. Tiny B-tree/Lion equality probes
are at practical parity. B-tree wins on ranges and the 100-value IN case; GIN wins on phrase/prefix
searches. Lion costs more to build and write, and its
five-index portfolio is smaller than B-tree's but larger than GIN's.

### Setup and interpretation

Release PostgreSQL 20devel (`-O2`), AMD Ryzen 7 5700X3D, 16 logical CPUs, WSL2;
512 MB shared buffers and 64 MB work_mem. Durability is enabled; parallel query, JIT and autovacuum
are disabled. Each query has one warmup and two timed rounds. Tables below show median EXPLAIN
execution time in **milliseconds**; planning time is separate in the full report. These are progress
measurements, not confidence intervals. Warm means no deliberate cache eviction, not that all pages
fit in shared buffers. [Recorded environment](bench/results/quick/2026-09-21-481f876/metadata.json).

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
| 1M scalar | Count ~50% of rows / clean | 32.336 | 93.011 | 0.408 | 79.3× / 228.0× |
| 1M scalar | Count ~0.5% of rows / clean | 0.318 | 2.827 | 0.028 | 11.2× / 99.2× |
| 1M scalar | Count two equality predicates / clean | 2.764 | 33.469 | 0.357 | 7.7× / 93.8× |
| 1M scalar | Count per 200 groups / clean | 85.142 | 142.057 † | 3.874 | 22.0× / 36.7× |
| 1M scalar | Count per 200 groups / dirty | 142.221 † | 146.727 † | 3.699 | 38.5× / 39.7× |
| 5M scalar | Count ~50% of rows / clean | 167.163 | 958.400 | 2.034 | 82.2× / 471.3× |
| 5M scalar | Count ~0.5% of rows / clean | 1.530 | 110.119 | 0.103 | 14.9× / 1,074.3× |
| 5M scalar | Count two equality predicates / clean | 16.193 | 178.657 | 1.643 | 9.9× / 108.7× |
| 5M scalar | Count per 200 groups / clean | 417.244 | 1252.029 † | 18.645 | 22.4× / 67.2× |
| 5M scalar | Count per 200 groups / dirty | 956.909 † | 934.408 † | 18.465 | 51.8× / 50.6× |
| 200k documents | Array contains `t1` | — | 4.909 | 0.025 | — / 200.3× |
| 200k documents | Array contains `t1` and `t17` | — | 0.956 | 0.115 | — / 8.3× |
| 200k documents | Array overlaps `t1`, `t17`, `t123` | — | 8.040 | 0.277 | — / 29.1× |
| 200k documents | Full-text `w1 & w17` count | — | 1.069 | 0.116 | — / 9.2× |

The speedup column gives **B-tree / GIN** multipliers, calculated as competitor median divided
by Lion median using the unrounded measurements. Higher is better for Lion; the comparison includes
the actual sequential fallbacks marked †. A dash means there is no matching B-tree measurement.

At 5M rows, clean equality/intersection/grouping cases above are roughly **10–82× faster than
B-tree**. In the document cases shown, Lion's membership counts are roughly **8–200× faster than
GIN**. B-tree has no matching document index in this workload (—). This does not extend to arbitrary
aggregates or retrieving all matching rows: the optimization answers the supported count shapes.

The same Lion indexes with pushdown disabled show why the query shape matters:

| 5M-row query / clean | Lion ms | Lion, pushdown off ms |
| --- | --- | --- |
| Count ~50% of rows | 2.034 | 617.308 |
| Count two equality predicates | 1.643 | 17.598 |
| Count per 200 groups | 18.645 | 976.453 † |

### Where it is at practical parity: tiny probes and fetching rows

For very selective equality counts, B-tree and Lion both finish in tens of microseconds. The
absolute gap is too small to justify another index from these two samples alone. Fetching data
requires heap access, so the count shortcut no longer applies.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 1M scalar | Count ~0.005% of rows | 0.021 | 0.051 | 0.017 |
| 5M scalar | Count ~0.005% of rows | 0.033 | 1.146 | 0.025 |
| 1M scalar | Sum ID/payload length for `c200=17` | 3.578 | 3.228 | 3.029 |
| 200k documents | Sum ID/payload length for full-text `w1` | — | 4.903 | 5.700 |

“Practical parity” here means similar scale or no demonstrated useful advantage, not statistical
equivalence. The 5M payload-fetch measurements vary sharply even between Lion variants using the
same bitmap plan (15.239 ms with pushdown enabled versus 127.818 ms disabled). That difference is
not evidence of a pushdown benefit for fetching rows. Inspect the full report and repeat such cases.

### Where Lion loses: ranges, larger IN lists, phrase and prefix search

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 1M scalar | Count `c20k IN (0,...,99)` | 0.325 | 3.417 | 0.577 |
| 1M scalar | Count `c20k BETWEEN 100 AND 199` | 0.298 | 38.532 | 119.384 † |
| 5M scalar | Count `c20k IN (0,...,99)` | 1.635 | 255.224 | 2.454 |
| 5M scalar | Count `c20k BETWEEN 100 AND 199` | 1.542 | 381.769 | 713.062 † |
| 200k documents | Phrase `common <-> w1` count | — | 7.893 | 46.614 † |
| 200k documents | Prefix `rare12:*` count | — | 1.720 | 29.463 † |

B-tree wins the tested 100-value IN queries and directly supports the range predicate, which
Lion cannot index. GIN is about **6× faster for the phrase** and **17× faster for the prefix** here.
Lion currently handles phrase/prefix predicates through full-index walks and heap rechecking;
GIN can narrow the candidates through its index. Lion's default plan in this run is sequential (†).
Retain B-tree/GIN for these
access patterns. This single IN-list size does not establish a universal crossover point.

The full report also includes NULL counts, three-predicate intersections, all dirty-read cases,
and reads after maintenance. Exact SQL and data distributions are in the
[workload manifest](bench/results/quick/2026-09-21-481f876/queries.json).

### Build time and index storage

Times are the sum of one build per index; sizes are measured after build, before mutations.
Scalar portfolios contain five indexes; document portfolios contain two. The pushdown setting
changes neither construction nor storage. At 5M rows, Lion uses **45% less index space than B-tree**
but **46% more than GIN**, and its build takes **3.3×** the B-tree time and **5.6×** the GIN time.
These are portfolio totals, not a claim that Lion is smaller for every column or distribution.

| Dataset | Family | Build seconds | Index MiB |
| --- | --- | --- | --- |
| 1M scalar | B-tree | 1.062 | 33.516 |
| 1M scalar | GIN | 0.742 | 18.945 |
| 1M scalar | Lion | 3.277 | 22.617 |
| 5M scalar | B-tree | 5.839 | 166.812 |
| 5M scalar | GIN | 3.401 | 62.703 |
| 5M scalar | Lion | 19.013 | 91.406 |
| 200k documents | GIN | 0.347 | 6.375 |
| 200k documents | Lion | 1.606 | 8.055 |

### Writes and maintenance: 5M rows

Each portfolio inserts 50k rows, updates `c200` in 50k existing rows, then runs VACUUM ANALYZE.
A checkpoint precedes each measured operation. Times and WAL volumes are single observations,
including all five indexes. GIN's fast updates defer work, so its subsequent VACUUM cost is shown too.

| Family | Insert ms | Insert WAL MiB | Update ms | Update WAL MiB | VACUUM ms | VACUUM WAL MiB |
| --- | --- | --- | --- | --- | --- | --- |
| B-tree | 473.005 | 59.688 | 1111.647 | 77.010 | 421.067 | 20.733 |
| GIN | 320.169 | 46.051 | 777.381 | 62.567 | 564.815 | 56.255 |
| Lion | 1600.515 | 208.808 | 2020.713 | 271.021 | 429.944 | 20.729 |

In this run, Lion's insert took **3.4×** and indexed update **1.8×** the B-tree time, with roughly
**3.5×** the WAL for both. Against GIN with `fastupdate=on`, Lion's insert/update times are about
**5.0× / 2.6×** higher, with **4.5× / 4.3×** the WAL; GIN's subsequent VACUUM is slower and writes more
WAL than Lion's. The quick suite does not measure sustained concurrent write throughput.
One bucket-directory growth warning for the 5M `ix_c20k` index was recorded; all checks still passed.

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
those larger suites were not rerun at `481f876`. The former README's 20M-row experiments and
pre-implementation simulation are preserved in the
[historical benchmark archive](bench/HISTORICAL_README_BENCHMARKS.md).

## License

Copyright (c) 2026, Chinmay Kanchi.

Licensed under the [PostgreSQL License](LICENSE) (SPDX: `PostgreSQL`).
