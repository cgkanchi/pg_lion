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
| Count matches within a scalar range | Consider Lion count pushdown when the column has few distinct values (days, statuses, small integers): the measured clean range count beats B-tree at both scales. Fetching matching rows has different costs. |
| Range filters on high-cardinality columns (timestamps, ids, prices) | Build the index `WITH (summaries = auto)` (or `on`). Without it a range is answered one distinct key at a time, so its cost grows with the number of distinct values in the range (or outside it, whichever is smaller), and a time window over a timestamp column is slow. With it the column keeps one summary posting set per bucket of about 4096 rows, and a range counts whole buckets at once: a million-key range went from 346 ms to 3.9 ms on an assert build (DESIGN.md §32). When the column's rows lie in no heap order, a range beside a clause with far fewer rows reads its buckets only at that clause's rows (DESIGN.md §32, "Summed ranges: dense and probed"). Bitmap and plain index scans of a range read the buckets too, and on a multicolumn index a range beside other columns bounds the bitmap exactly, however little `work_mem` it has for the range alone (DESIGN.md §28). Summaries cost inserts CPU - 31% more for single-row INSERTs and 73% for a bulk INSERT in that measurement, with two of the index's three columns summarized - and a few percent of index size. |
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

- **contrib: `citext`, `pageinspect`, `pg_buffercache` and `pg_walinspect`.**  The regression
  suite creates all four.  Without `pageinspect` the `build`, `corrupt` and `summary` files fail,
  without `pg_buffercache` the `corrupt`, `pinbudget` and `range` files and the `count_batch_race`
  and `gettuple_pause` specs fail, without `pg_walinspect` `walrecords` fails, and most files use
  `citext`.  The PGDG packages (`postgresql-N`) include contrib; a source build needs
  `make -C contrib install`.
- **`pg_isolation_regress`** for the isolation specs: a source build installs it with
  `make -C src/test/isolation install`, and the packages ship it in `postgresql-server-dev-N`.
- **`injection_points`** for the specs that park a backend on an injection point: a server
  configured with `--enable-injection-points` (17 or later) and the test module installed with
  `make -C src/test/modules/injection_points install`.  Where the module is missing those specs are
  skipped and the run says which (`INJECTION_POINTS=1` forces them, and makes a missing module an
  error instead).
- **The recovery harness** (`make recovery-check`) needs a whole installation to initdb clusters
  in, named by `RECOVERY_PREFIX`, which it builds and installs the extension into
  (`RECOVERY_SKIP_INSTALL=1` uses the one already installed there instead); as root it also
  needs `RECOVERY_RUN_AS=<unprivileged user>` (`test/recovery/README.md`).  Its phases 1b-1e and 3
  need `injection_points`, and are skipped without it unless `INJECTION_POINTS=1` makes that an
  error.

The regression files VACUUM with `FREEZE` wherever what they print depends on the VACUUM having
done all its work - index statistics after a DELETE, a plan the visibility map prices, a count
that must skip every heap block.  A plain VACUUM that cannot get a heap page's cleanup lock at
once (the checkpointer and the background writer pin the pages they write) checks that page
without pruning it: its dead TIDs stay in every index and its visibility-map bit stays clear.  An
aggressive VACUUM waits for the lock instead.  The isolation specs keep plain VACUUMs, whose waits
are part of what they test.

`dev.sh` puts its cluster's socket in `$XDG_RUNTIME_DIR/pg_lion-<user>` (or `/tmp/pg_lion-<user>`)
on port 54329, and `LION_SOCK` / `LION_PORT` move it.  The cluster trusts local connections, so
`dev.sh` makes a missing socket directory mode 0700, and refuses the default one if it is a symlink,
belongs to someone else or is open to others.  The cluster runs with `wal_level = replica`, so
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

A `GROUP BY` with several filters (`SELECT country, count(*) FROM events WHERE event_type =
'purchase' AND ... GROUP BY country`) collects the rows the filters select once per table, or per
partition, and counts each group against that one set, so the filters' index pages are read once
rather than once per group; `EXPLAIN ANALYZE` prints `WHERE Sets Collected` when it did (DESIGN.md
§10). The set stays within a hash table's memory (`work_mem` times `hash_mem_multiplier`) and goes
to a temporary file past it. The groups are then counted against it up to a few hundred at a time,
in one pass over its containers for all of them (`Group Batches` in `EXPLAIN ANALYZE`). With
parallel query enabled (`max_parallel_workers_per_gather`) such a `GROUP BY` over one table can
run in parallel: the workers divide the table's blocks into ranges, each collects the filters of
its ranges and counts every group there, and a `Finalize HashAggregate` above the `Gather` adds
the groups' partial counts up (`Parallel Custom Scan (LionCount)`, `Key Ranges` in `EXPLAIN
ANALYZE`; DESIGN.md §10, "A GROUP BY in parallel").

On a partitioned table the pushdown counts each partition the planner keeps, with that partition's
own Lion indexes (DESIGN.md §16), and every `WHERE` clause needs one in every partition counted -
except a clause the partition's bounds imply. `kind = 'a'` over a table partitioned by `kind`,
once pruning has left only the `kind = 'a'` partitions (sub-partitions included), is true of every
row they hold: it needs no index there and is left out of their counts, and `EXPLAIN` lists it
under `Implied by Partition Bounds`. The proof is PostgreSQL's own, the one partial indexes use, so
a partition that also takes NULLs, a default partition, or a generic plan's parameter implies
only what that proof can show. An `OR` with an arm per kind - `(kind = 'a' AND tags && '{x}') OR
(kind = 'b' AND tags && '{y}')` - is narrowed the same way: each partition leaves out the arms its
bounds rule out and, in the arm it keeps, the `kind = ...` they imply, so the partition of `'a'`
counts `tags && '{x}'` alone (`Refuted by Partition Bounds` in `EXPLAIN`).

A count over a fact table joined to a filtered dimension (DESIGN.md §27) is pushed down too, when the
fact's foreign-key column has a Lion index and its own filters are ones Lion answers: the dimension
side runs as an ordinary plan (its own Lion index serves its filters through a bitmap scan), and for
each dimension row the node looks the key up in the fact's FK index and counts, or for `EXISTS` tests,
that key's rows ANDed with the fact filters, which it collects once for the whole scan:

```sql
CREATE INDEX orders_customer_lion ON orders USING lion (customer_id);
CREATE INDEX orders_status_lion ON orders USING lion (status);

-- orders of the matching customers (also as a JOIN, or with customer_id IN (...))
SELECT count(*) FROM orders o
WHERE o.status = 'open'
  AND EXISTS (SELECT 1 FROM customers cu WHERE cu.id = o.customer_id AND cu.country = 'NZ');

-- customers with at least one matching order (NOT EXISTS: with none)
SELECT count(*) FROM customers cu
WHERE cu.country = 'NZ'
  AND EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = cu.id AND o.status = 'open');
```

Written as a JOIN, the first form needs `customers.id` unique (a primary key). As `EXISTS` or `IN`
it does not: over a key that repeats - `o.customer_id IN (SELECT customer_id FROM visits WHERE
...)` - the node sorts the subquery's keys and counts each distinct key's orders once (DESIGN.md
§27; five million fact rows against 9,776 rows over 8,848 distinct keys took 117 ms, against 585
for the hash semi join, on an assert build). The JOIN and the second form may group by the
dimension's columns. To count each customer's orders, the JOIN may also group by the join key,
`cu.id` or `o.customer_id`, and the first form by `o.customer_id`. Each customer the node counts is
then a group of its own, which PostgreSQL's aggregate takes as it comes, with no sort and no hash
table (DESIGN.md §27, "Grouped by the join key"). The JOIN, and the form with `IN`, may also group
by one column of the fact table, `o.status`, with a lion index on it: each customer is then counted
once per status. Over a fact table partitioned by that column, a partition whose bounds give it one
value needs no index on it (DESIGN.md §27, "Grouped by a fact column"). A query that aggregates
those per-key counts in turn gets the node at its inner level:

```sql
WITH n AS (SELECT o.customer_id, count(*) AS n
           FROM orders o JOIN customers cu ON cu.id = o.customer_id
           WHERE o.status = 'open' AND cu.country = 'NZ'
           GROUP BY o.customer_id)
SELECT count(*) AS customers, sum(LEAST(n, 3)) AS capped FROM n;
```

`GROUP BY o.customer_id` needs both key columns of one type, compared with that type's own `=`,
whose equal values are always spelled alike: integers, text under a deterministic collation, or
uuid, for example, but not numeric, where 1.0 and 1.00 are equal.

`count(DISTINCT o.customer_id)` or `count(DISTINCT cu.city)` in place of `count(*)` is pushed down
too: the node emits the matching customers, and PostgreSQL's own aggregate counts their distinct
values. So are, in place of `count(*)` or beside it, `min`, `max`,
`bool_and`, `bool_or`, `every`, `bit_and` and `bit_or` of the customers' columns, `count` of them,
and `sum` of their `smallint` and `integer` ones (DESIGN.md §27, "Every aggregate over the node's
rows"): where one of those stands beside a count of the join's rows, each customer the node emits
carries its count of orders, and the counts and sums become sums of those counts
(`lion_join_count()`, `lion_join_sum()`, two aggregates of the extension's). The node pays a lookup
per dimension row (per distinct key), so the
cost model leaves a large dimension set to the hash join. With parallel query enabled
(`max_parallel_workers_per_gather`) it can run in parallel, each worker taking its share of the
dimension rows, or of the distinct keys, which every worker sorts. The fact filters are collected
once per process into memory bounded like a hash join's (`work_mem` × `hash_mem_multiplier`); past
that the copy spills to a temporary file, as a hash join's table would. Where the planner expects
few dimension rows it may instead probe the fact filters at each dimension row's count; a run that
meets far more rows, or far heavier keys, than it expected keeps an account of what probing has cost
and collects the filters part way through once that reaches what collecting them costs, if the copy
is expected to fit that memory (DESIGN.md §27, "Probed, then collected"; `EXPLAIN ANALYZE` prints
`Fact Filters: probed, then collected`, `Fact Filter Switches` and `Fact Filter Keys Probed`, the
keys counted by probing before each switch). Their values may be
parameters and stable expressions as for a single table, an `IN` list whose array is a parameter
(`o.status = ANY ($1)`) included: every process evaluates them once per scan.

The fact table may be partitioned, when every partition the planner keeps has a Lion index on the
foreign-key column (one on the partitioned table gives each partition its own) and on each fact
filter its bounds do not imply. The node then takes each batch of dimension keys to every partition
in turn and adds the partitions' counts up per dimension row - for `EXISTS` a match in any of them,
for `NOT EXISTS` in none (DESIGN.md §27, "A partitioned fact table"). `EXPLAIN` lists the partitions
and what their bounds imply, and its counters are summed over them: `Join Keys Looked Up` counts a
key once per partition it was looked up in. The partitions are the ones plan-time pruning keeps: a
partition that only run-time pruning would remove, as with `ts >= now() - interval '1 year'` over
partitions by year, is counted too, and contributes nothing.

The dimension may itself be a join: one table that carries the key, and other tables joined to it
only by `EXISTS`, `IN` or `NOT EXISTS`, which never repeat its rows (DESIGN.md §27, "A dimension
that is a join"). A dimension `d` with an `EXISTS` on each of two fact tables `f1` and `f2` is
then `f2` counted against `d` semi-joined to `f1`, or `f1` against `d` semi-joined to `f2`,
whichever the cost model prices lower; and `SELECT count(*) FROM f2 WHERE ... AND f2.fk IN (SELECT
d.pk FROM dim d WHERE ... AND EXISTS (SELECT 1 FROM f1 WHERE f1.fk = d.pk AND ...))` counts `f2`
against the dimension rows that pass. The node's child is PostgreSQL's own plan of that join. An
inner or outer join inside the dimension, which could repeat a row, keeps the ordinary plan.

The rows of such a semi or anti join themselves - `d` semi-joined to `f1`, where a sort, another
join, an aggregate the node does not answer, or the node above as its joined dimension needs them -
can come from the same lookups: `Custom Scan (LionSemiJoin)` emits each row of its outer side whose
key the fact's FK index has rows for under the fact filters, `Custom Scan (LionAntiJoin)` each row
that has none, a NULL key included (DESIGN.md §27, "The semi and anti join as a join path"). The
outer side can be any table or join, its key need not be unique - each of its rows is tested and
emitted once - and the fact table may be partitioned. It keeps the outer side's order, so a sort
above it may not be needed, and runs in parallel over a parallel scan of the outer side. It is
offered for `EXISTS`, `IN` and `NOT EXISTS` whose subquery is the fact alone, correlated on one
column; `NOT IN` is not an anti join and keeps the ordinary plan. The cost model chooses between it
and PostgreSQL's own joins; `pg_lion.enable_semijoin` turns it off.

`EXPLAIN ANALYZE` of such a join says where each dimension row's time went (DESIGN.md §27, "Where a
key's time goes"): `Join Child Rows` from the dimension's plan, `Join Keys Looked Up` and `Without
Entry`, the containers the counts read from each key's FK set (`Join Key Containers Read`), from the
collected copy of the fact filters (`Fact Filter Copy Containers Read`, `Seeks`, and `File Reads`
when the copy spilled), the posting-tree pages they read (`Join Posting Pages Read`) and the
visibility map (`Visibility Map Checks`, `Pages Pinned`); with `TIMING` on, also `Join Child Time`,
`Join Lookup Time`, `Join Count Time`, `Fact Filter Locate Time` (the fact filters located, a range
among them collected into memory, which can be most of a run) and `Fact Filter Collect Time`, summed
over parallel workers and the partitions of a partitioned fact table.
A join whose result is counts alone (no `GROUP BY`, no dimension column in the output) adds the
dimension rows' counts up inside the node and hands up one row per process.

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
force for the caller.  A count under a collation the index does not compare in is an error, never
a different number; on a column of a nondeterministic collation that includes a key of the default
collation - a literal's, which the call cannot tell from an explicit `COLLATE "default"` - so there
the key names the column's collation (`'abc'::text COLLATE case_insensitive`).  They take one INDEX, and an index belongs to one table, so they count that
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
    src/lion.h              on-disk structs, the relation state, and the page layer's and the AM's functions
    src/lion_pages.c        index pages: initializing, deleting, recycling; the items of container pages
    src/lion_meta.c         the meta page
    src/lion_state.c        per-relation state: columns, opclass procedures and order; usability checks; keys
    src/lion_entry.c        entry tuples, and the max_entries cardinality guard
    src/lion_posting_put.c  writing container chains: putting items, spilling an INLINE entry, page splits
    src/lion_compat.h       the differences between PostgreSQL 16, 17, 18, 19 and master
    src/lion_wal.[ch]       the WAL shim every write path calls, and the custom resource manager
    src/lion_dir.c          the sorted entry directory: a Lehman & Yao B-tree keyed by the index key
    src/lion_posting.c      the per-key posting tree: a B-tree over container keys, GIN's shape
    src/lion_am.c           handler, reloptions, amvalidate, table AMs, buildempty, _PG_init hook/GUC
    src/lion_amcost.c       amcostestimate: the cost of plain and bitmap scans
    src/lion_build.c        ambuild via tuplesort (hash, key, tid code); INLINE entries or per-key posting trees
    src/lion_scan.c         amgetbitmap, and amgettuple for plain index scans
    src/lion_insert.c       aminsert (serialised on the directory leaf; bitset in-place fast path)
    src/lion_vacuum.c       ambulkdelete with cleanup locks on every page, two-pass cancellable protocol
    src/lion_funcs.[ch]     lion_index_stats() and the other diagnostics; lion_funcs.h is private to them
    src/lion_verify*.c      lion_index_verify(): pages and posting sets, the directory, rechecks and
                            heapallindexed, summary posting sets
    src/lion_count.h        the count engine's interface - lion_count_keys(): VM-interlocked counting,
                            per-block batched heap recheck - and lion_count_int.h what its files share:
    src/lion_set.c          locating posting sets, the list pin budget, the lookup walk
    src/lion_set_copy.c     private copies of posting sets, spilled to a file when too big
    src/lion_cursor.c       container cursors over one posting set
    src/lion_expr.c         expression cursors over boolean expressions of sets; the stream the scans read
    src/lion_vis.c          the visibility map a container at a time, and the per-query visibility cache
    src/lion_count.c        counting a container against the map and the heap; the merge
    src/lion_count_groups.c the groups of a walk counted together
    src/lion_count_shared.c the copy the participants of a parallel plan share
    src/lion_rangesrc.c     a range as a source: collected, or probed at the other sources' rows
    src/lion_range.c        range restrictions, and iterating every entry of an index
    src/lion_count_sql.c    the SQL count functions (lion_index_count() and the others)
    src/lion_customscan.h   the LionCount custom scan's private header, shared by its planner half:
    src/lion_plan_*.c       create_upper_paths_hook -> CustomPath/CustomScan "LionCount"; set_join_pathlist_hook
                            -> "LionSemiJoin"/"LionAntiJoin"; index matching, partitions, the cost model
    src/lion_exec_*.c       and its executor half: begin, locating the WHERE sets, counting, the FK-side
                            join, the run and parallel DSM, EXPLAIN
    src/lion_costs.[ch]     the cost model's constants as planner settings (pg_lion.*_cost)
    src/lion_fkjoin.[ch]    the FK-side joins a LionCount answers (fact JOIN dim, EXISTS / NOT EXISTS), and the semi/anti join paths
    src/lion_ordered.c      CustomScan "LionOrdered": lion-filtered scans in a btree's or a lion column's order
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

Several such quals on one column (`tags && '{a}' AND tags && '{b}'`), like two lists on a scalar
column, are ANDed in the index, as quals on several columns are, so the heap gets only the rows they
select together (DESIGN.md §5, SCAN step 5).
`count(*)` over `@>`, `&&` and an AND/OR tsquery is pushed down like any other clause, and can be
combined with a `GROUP BY` on a scalar roaring column. Everything a plain AND/OR of key sets cannot
express - `<@`, `@> '{}'`, a NULL element, and a tsquery with `!`, `<->`, `foo:*` or weights - falls
back to scanning every indexed row and rechecking it if the Lion index is used. The planner may
choose a sequential scan instead; GIN wins the measured phrase/prefix cases below.

A literal query is only pushed down when it is exact. A query the count only sees at run time - a
prepared statement's generic plan (`tags @> $1`, `tsv @@ to_tsquery($1)`) or a stable expression
(`tsv @@ to_tsquery(current_setting('app.q'))`) - is pushed down whatever it turns out to be
(DESIGN.md §17): exactly when the key sets answer it; from a superset of its rows, each rechecked in
the heap, when they do not (`@>` with a NULL element counts the other elements' rows, a phrase the
rows with all of its lexemes, `a & !b` the rows with `a`); and when no key narrows it at all (`@>
'{}'`, `!a`, `foo:*`), by rechecking the rows the other clauses select, or every row by a
sequential scan when there is no other clause. `EXPLAIN ANALYZE` reports the rows the recheck
turned away as `Rows Removed by Recheck`. A value the planner cannot estimate is priced as the case
that rechecks the most, so a lone `tags @> $1` in a generic plan usually goes to the ordinary plan
and a custom plan of the literal to the pushdown. Such a query is not taken under an `OR`.

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
`summaries` (`off` | `on` | `auto`, default `off`): summary posting sets for ranges (DESIGN.md §32).
With `on` every ordered scalar key column keeps, after its keys, one posting set per bucket of
consecutive keys, and a count over a range sums the buckets it covers whole instead of walking their
keys; `auto` gives them only to the columns whose keys are small next to a bucket (many distinct
values), which is where ranges are slow.  Every insert into a summarized column also updates its
bucket's set.  `summary_tids` (16 .. 16777216, default 4096) is the rows a bucket closes at.  Both
are read at build time: `ALTER INDEX ... SET (summaries = ...)` takes effect at the next REINDEX.
An index with summaries is format 7, which an older build refuses; one without is format 6, as
before.  A range on a column that does not drive a count (`g, count(*) ... WHERE ts >= $1 GROUP BY
g`, a range in an OR, a join's fact filter) is answered too, collected once from the same walk.
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
be READ on any server but can only be WRITTEN where the resource manager is registered (an unlogged
or temporary one writes no WAL and is written anywhere), which needs

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
the number of rows a multi-key opclass extracted no key from, and `ndistinct`, the distinct keys
the planner is given for the column (DESIGN.md §33; NULL where none is recorded).  `slack_bytes` and
`inline_slack_bytes` are the growth slack inserts leave inside items and inside INLINE entry
payloads (DESIGN.md §4), which is space a later insert into the same key grows into for free; a
bulk-built index has none of either.  The counters that describe the
relation rather than a column - the directory's shape, free and deleted pages - are repeated on
every row.

## Settings

Planner switches, each on by default and settable per session (`SET`), for comparing plans and
working around a bad choice:

- `pg_lion.enable_count_pushdown`: answer `count(*)` from Lion indexes with the `LionCount`
  custom scan (DESIGN.md §10). Off, it turns off `pg_lion.enable_semijoin`'s joins too.
- `pg_lion.enable_semijoin`: offer `LionSemiJoin` and `LionAntiJoin`, a semi or anti join over a
  fact table answered from the Lion index on its foreign key (DESIGN.md §27, "The semi and anti
  join as a join path"). PostgreSQL's `enable_hashjoin`, `enable_mergejoin` and `enable_nestloop`
  do not disable them.
- `pg_lion.enable_ordered_scan`: offer `LionOrdered` for an `ORDER BY`: a walk of a B-tree filtered
  by a Lion set, or a walk of a Lion index's own ordered scalar column in either direction (`ORDER
  BY ts DESC LIMIT n` over a Lion index on `ts`), filtered by a Lion set of the other clauses or by
  the clauses themselves; on a table, or on each partition of one (DESIGN.md §30, §30.11). EXPLAIN
  names a column's walk `Ordered By: <index> (<column>[, backward])`.
- `pg_lion.enable_plain_scan`: let the planner use plain and index-only scans of Lion indexes
  (`amgettuple`, DESIGN.md §29). Off, Lion indexes are planned for bitmap scans only, as GIN
  indexes are, and every other index's scans are unaffected - where `enable_indexscan = off` would
  also take away B-tree index scans. The count pushdown and `LionOrdered` still use Lion indexes.
- `pg_lion.enable_intersection_probe`: let Lion measure, at plan time, how many rows the AND of
  two or more equality, `IN` or multi-key clauses on one Lion index really selects, when the
  planner's product of their selectivities may be far off - correlated filters - and price its
  own paths (its index scans and the count pushdown) from that (DESIGN.md §29.11, "Correlated
  sets"). A bounded sample of the index, about 1,000 buffer accesses a planner run at most; the
  planner's own row counts are left alone. Off, Lion prices its paths from the planner's estimate.
- `pg_lion.enable_index_ndistinct`: give the planner a column's number of distinct values from a
  Lion index on it - the index's count of its keys, taken at build, by every VACUUM that deletes
  rows (in the walk it makes anyway) and by an ANALYZE when the index's directory is no larger than
  ANALYZE's own sample - in place of the number ANALYZE estimates from its sample, which a column
  with many rare values gets far too low (DESIGN.md §33). It applies to a column with statistics, a
  non-partial index with a scalar opclass on exactly that column (or an expression index on the
  expression), while the table holds within a factor of 2 of the rows the count was taken over, and
  never where the column's `n_distinct` has been set by hand. `pg_stats` still shows ANALYZE's
  number.

Testing knobs rather than tuning ones: `pg_lion.scan_window_floor` (4 MB), the least memory a plain
scan's window of container keys takes (DESIGN.md §29.3), `pg_lion.parallel_range_keys` (16), the
fewest container keys - of 64 heap blocks each - a range of a parallel count covers when it runs
(DESIGN.md §10, "A GROUP BY in parallel"; the planner prices the default),
`pg_lion.enable_union_probe` (on), whether an AND of posting sets may look its few rows up in the
containers of an `IN` list's or a multi-key query's union rather than build the union (DESIGN.md
§29.11, "Unions probed"; the answers are the same either way; `EXPLAIN ANALYZE` of a count prints
`Unions Built` and `Unions Probed`), `pg_lion.enable_tree_probe` (on), the same for a nested tree -
a tsquery `(a | b) & (c | d)`, an `OR` of `AND`s across columns - evaluated for the intersection's
few rows rather than built (DESIGN.md §29.11, "Trees probed"; `Trees Built` and `Trees Probed`),
`pg_lion.enable_filter_switch` (on), whether an FK-side join that probes its fact filters may
collect them part way through (DESIGN.md §27, "Probed, then collected"), and
`pg_lion.vacuum_barrier_ranges` (superuser), how many visited-block ranges VACUUM batches in rmgr
mode (DESIGN.md §25).
`pg_lion.rmgr_id` is described under `wal_mode` above.

Cost settings, for calibrating Lion's cost model on your own workload the way `random_page_cost`
calibrates core's (DESIGN.md §31, "The settings"). Each is the price of one operation as a multiple
of a core cost setting, so Lion's prices still scale with core's; the defaults are what the model
was fitted at, and changing one changes plans, not results. Settable per session, and shown by
`EXPLAIN (SETTINGS)` when changed:

| `pg_lion.` | default | unit | the operation it prices |
|---|---|---|---|
| `plain_fetch_row_cost` | 1.0 | `cpu_tuple_cost` | a plain scan's heap fetch of a row past the first on its page, beyond a bitmap heap scan's |
| `bitmap_row_cost` | 0.1 | `cpu_operator_cost` | a row's bitmap entry, which a plain scan is charged as a bitmap heap scan is |
| `walk_pass_cost` | 5.0 | `cpu_tuple_cost` | a plain scan's walk of an entry past the first, a heap pass each |
| `container_cost` | 8.0 | `cpu_operator_cost` | a container of a posting set read and counted |
| `member_cost` | 0.15 | `cpu_operator_cost` | a member of it, up to 1,024 a container |
| `probe_cost` | 40 | `cpu_operator_cost` | a seek of a posting tree to a container key |
| `memory_probe_cost` | 30 | `cpu_operator_cost` | the same into a set copied into memory |
| `and_member_cost` | 0.8 | `cpu_operator_cost` | a member of an intersection ANDed with what a seek found, or looked up in a container of a union's |
| `union_key_cost` | 30 | `cpu_operator_cost` | the union of an `IN` list's or a multi-key query's containers built at a container key |
| `union_member_cost` | 0.25 | `cpu_operator_cost` | a member of such a union, or of the intersection ANDed with it |
| `descent_cost` | 120 | `cpu_operator_cost` | a level of an entry directory descended |
| `union_set_cost` | 100 | `cpu_tuple_cost` | a set of an `IN` list or `OR` rebuilt by each count of a GROUP BY |
| `recheck_tid_cost` | 1.5 | `cpu_tuple_cost` | a candidate row of a count's heap recheck |
| `recheck_group_tid_cost` | 6.0 | `cpu_tuple_cost` | the same in a grouped count |
| `entry_count_cost` | 50 | `cpu_tuple_cost` | a count of a GROUP BY: an entry, or a pair of two |
| `list_group_cost` | 18 | `cpu_tuple_cost` | a count of a group an `IN` list drives |
| `distinct_test_cost` | 50 | `cpu_tuple_cost` | a test of a `count(DISTINCT)` walk |
| `range_entry_cost` | 40 | `cpu_tuple_cost` | an entry of a range walk counted on its own |
| `range_union_entry_cost` | 12 | `cpu_tuple_cost` | a small entry of a summed range, counted with its leaf |
| `probe_step_cost` | 2.0 | `cpu_operator_cost` | a container of a set a summed range probes |
| `range_fold_cost` | 420 | `cpu_operator_cost` | a fold into a container of a range's union, collected as a source, that is not a bitset |
| `fkjoin_count_cost` | 14 | `cpu_tuple_cost` | an FK-side join's count that probes the fact filters, a dimension row |
| `fkjoin_row_cost` | 20 | `cpu_tuple_cost` | a row the FK-side join hands up |
| `fkjoin_probe_cost` | 10 | `cpu_operator_cost` | a probe of such a count into a fact filter |
| `fkjoin_probe_page_cost` | 54 | `cpu_operator_cost` | a posting page such a probe reads |
| `fkjoin_set_cost` | 11 | `cpu_tuple_cost` | a set of a fact filter that is a union (an `IN` list, an `OR`), a count that probes it |
| `fkjoin_lookup_cost` | 75 | `cpu_operator_cost` | a key looked up on the directory leaf a lookup in key order stands on |
| `fkjoin_collect_container_cost` | 2.0 | `cpu_operator_cost` | a container of the driving filter, read to collect the fact filters |
| `fkjoin_copy_count_cost` | 33 | `cpu_tuple_cost` | a count against the collected copy |
| `fkjoin_copy_probe_cost` | 2.0 | `cpu_operator_cost` | a lookup of the copy and its AND, an fk container |
| `fkjoin_copy_member_cost` | 3.0 | `cpu_operator_cost` | a member of that container |
| `fkjoin_copy_container_cost` | 20 | `cpu_operator_cost` | a container of the copy made |
| `fkjoin_batch_row_cost` | 75 | `cpu_operator_cost` | a dimension row's place in a batch looked up in key order |
| `fkjoin_sort_compare_cost` | 0.25 | `cpu_operator_cost` | a comparison in the sort that makes the dimension's keys distinct |
| `fkjoin_sort_key_cost` | 6.0 | `cpu_operator_cost` | a key into and out of that sort |
| `fkjoin_sort_seq_page_cost` | 0.75 | `seq_page_cost` | a page that sort writes or reads past `work_mem`, the sequential share |
| `fkjoin_sort_random_page_cost` | 0.25 | `random_page_cost` | ... and the random share |

## Known limitations

Equality, `IN` lists, scalar ranges and the multi-key operators above are supported, through bitmap
scans and plain index scans (`amgettuple`, DESIGN.md §29). There are no ordered scans of the
access method itself (an `ORDER BY` is `LionOrdered`'s: a B-tree walked with a lion filter, or a
lion index's own ordered column walked, §30), no index-only scans
that return a column (only those that need none, like `count(*)`), no INCLUDE columns, no
parallel build or scan, no reclaim of an emptied directory leaf or of an emptied posting-tree leaf
(both wait for the whole set or the whole index to go). Inserts serialise on the directory
leaf that holds the key; see the measured
[write costs](#writes-and-maintenance-5m-rows) below. Count pushdown supports constants, parameters
and stable expressions such as `now() - interval '30 days'` or `current_date - 30` (evaluated once
per execution; a volatile one like `random()` goes to the ordinary plan) on any indexed column, enum
columns included, a boolean column tested by itself (`flag`, `NOT flag`, `flag IS TRUE`, `flag IS
NOT FALSE`), an `OR` of such clauses and of `AND`s of them, nested as deep as the query writes it
(`(a = 1 AND flag IS NOT TRUE) OR b = 2` is distributed into the arms it stands for, up to 1000
clauses in all), a `GROUP BY` of one or two indexed columns - or of `coalesce(col, constant)` of
one, whose NULL rows are counted in the constant's group, merged with that key's rows when the
column has it - and a `HAVING` over the counts it computes (a `HAVING` with a correlated subquery,
or a `GROUP BY` of three or more columns, goes to the ordinary plan). `count(DISTINCT col)` is
answered for an indexed column (DESIGN.md §26), and for a column a unique index proves unique - a
primary key - as the `count(col)` it equals: `count(*)` where the column is NOT NULL, whether or not
it has a lion index (a single-column, immediate, non-partial btree index under the `DISTINCT`'s
collation; on a partitioned table, one on the parent, and then only without a `GROUP BY`). `IN`
lists of more than 1000 values are left to the ordinary plan, a multi-key index can never drive a
`GROUP BY` or a sum-over-all-entries count (its entries are keys, not row values), and the cost
model inherits the
stale `relallvisible` blind spot of index-only scans. Indexes built before NULL keys existed (meta page version 1) are refused
with an error and have to be rebuilt with REINDEX.
On a hot standby a GENERIC-mode index's count paths recheck every candidate TID in the heap instead
of trusting the visibility map, because generic WAL replay does not take the cleanup locks the pin
interlock relies on; they stay correct there but are no longer O(1) per container.  Index-only
scans of such an index (of a query that needs no column, such as `SELECT count(*)`) look every TID
up in the heap there for the same reason.  An rmgr-mode
index does not pay that: its removal records replay under a cleanup lock, so the standby uses the
visibility map again (DESIGN.md §25) - at the price that a standby reader holding a pin makes replay
wait, which `max_standby_streaming_delay` resolves as a recovery conflict.  A count that reads even
one generic-mode index falls back to rechecking everything, because the interlock has to hold for
every source it intersects. The SQL count functions require SELECT
on the table or on the indexed columns and refuse tables where row-level security applies to the
caller; the pushdown only uses an index whose collation matches the clause or grouping collation.
On PostgreSQL 16 a server with `old_snapshot_threshold` set (it is -1, off, by default; 17 removed
the setting) does not read lion indexes at all: lion does not detect "snapshot too old", so the
planner prices them out and declines the count pushdown and `LionOrdered`, and a scan or SQL count
that reaches one anyway fails with an error rather than return a different answer than the
snapshot's (DESIGN.md §9).  Inserts and VACUUM work as usual.

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
