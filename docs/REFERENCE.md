# pg_lion reference

SQL functions, key types, multi-key operators, index options, settings and known limitations. Section numbers (§) refer to [DESIGN.md](../DESIGN.md).

## SQL functions

Ordinary SQL is the interface - the planner uses the index and the `LionCount` pushdown on its own -
and these functions are for testing, diagnostics and the occasional direct count:

    lion_bm25(idx, query, k [, k1, b])              the k best rows by BM25, best first (store_positions indexes)
    lion_bm25_score(tsv, query, idx [, k1, b])      one row's BM25 score; ORDER BY it for the LionBm25 scan
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
allow - but never reports their changes as damage.  On a hot standby it takes AccessShareLock, holds
no page while it waits for another that replay may hold, and is exact only while replay leaves the
index alone; a posting set that replay keeps changing is checked page by page, with a WARNING that
its totals were not compared.  With `heapallindexed` it evaluates the index's expressions as the
table's owner (DESIGN.md §7).


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
combined with a `GROUP BY` on a scalar roaring column.

An index built `WITH (store_positions = true)` also stores every lexeme's positions and weights per row, so phrases (`a <-> b`,
`a <3> b`), weights (`a:A`) and NOTs (`a & !b`, `a <-> !b`) are answered exactly from the index:
bitmap and index scans need no recheck, and counts skip the heap on all-visible pages, under a
`GROUP BY`, `count(DISTINCT)` or the FK-side join too, under an OR, and for `@@ ANY (array)`
in scans. It costs about three bytes per lexeme and
row on top of a plain lion index, more than doubling it, so it is off by default.

    CREATE INDEX ON doc USING lion (tsv) WITH (store_positions = true);

Such an index also ranks. `lion_bm25(index, query, k [, k1, b])` returns the `k` rows with the
highest BM25 score for the query's lexemes, best first, scored entirely from the index (term
frequencies, document frequencies and each row's length, which the index keeps under its own key);
the heap is read only to check that the rows returned are visible. Join on `ctid` for the rows:

    SELECT d.*, s.score
      FROM lion_bm25('doc_tsv_idx', to_tsquery('english', 'cat | dog'), 10) s
      JOIN doc d ON d.ctid = s.ctid
     ORDER BY s.score DESC;

The score is Lucene's BM25 (`k1` 1.2 and `b` 0.75 by default) over the query's lexemes, each
counted once; lexemes under a NOT do not score, and operators, phrases and weights do not change
it, so add `WHERE d.tsv @@ query` to keep only rows that match the query as a whole. As in a search
engine, the statistics count deleted rows until VACUUM removes them, and a backend keeps the mean
row length it read until the row count moves by more than 1/64. Rows that cannot reach the top `k`
are skipped (MaxScore), so a very common lexeme next to rarer ones costs little. Like the counts,
`lion_bm25()` and `lion_bm25_score()` need SELECT on the table and refuse a table where row-level
security applies to the caller: the statistics count every row, so a score would reveal what the
hidden rows hold.

`lion_bm25_score(tsv, query, index [, k1, b])` is the same score for one row, from its tsvector and
the index's statistics. Ordered by it, with a `WHERE` that matches on the same column, the planner
returns the rows best first straight from the index (a `LionBm25` scan) instead of scoring and
sorting every match:

    SELECT d.*, lion_bm25_score(d.tsv, 'cat | dog', 'doc_tsv_idx') AS score
      FROM doc d
     WHERE d.tsv @@ 'cat | dog'
     ORDER BY lion_bm25_score(d.tsv, 'cat | dog', 'doc_tsv_idx') DESC
     LIMIT 10;

The scan is offered when the query and index are constants and every row the `WHERE`'s tsquery can
match has one of the scored lexemes - the usual case of the same query in both places. Other
`WHERE` clauses filter its rows, and a second sort key is an incremental sort above it. Anywhere
else, the function scores row by row. `pg_lion.enable_bm25_scan` turns the scan off.

Without positions, everything a plain AND/OR of key sets cannot express is rechecked in the heap. A
NULL element and a tsquery phrase, weight or `a & !b` are answered from the rows of their keys (the
other elements, the phrase's lexemes, `a`), then rechecked, in counts and scans alike. `<@`, `@> '{}'` and a bare `!a` fall back to scanning
every indexed row and rechecking it if the Lion index is used; the planner may choose a sequential
scan instead.

A prefix lexeme (`foo:*`) is replaced at scan time by the OR of the indexed lexemes starting with
`foo`, with the same weight mask, so it is answered like any other lexeme: inside phrases and NOTs
too, and from stored positions when the index has them. A prefix with no indexed match matches no
row. When the expansion would pass 1000 lexemes for the whole query, the query is answered as
before: every indexed row is a candidate and the heap rechecks it. GIN is faster for such very
common prefixes.

A query the count only sees at run time - a
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

## jsonb containment, existence and jsonpath (DESIGN.md §42, §43)

`jsonb_contains_ops` (not the default; the default `jsonb_ops` indexes whole documents for `=`)
indexes a document under one key per path to a scalar (with the scalar), one per object or array
below the root, the root's type, and one per top-level key, string element or string scalar:

    CREATE INDEX ON docs USING lion (doc jsonb_contains_ops);

    doc @> '{"a": {"b": 1}}'   the AND of the query's paths, exact
    doc @> '{"a": [{"b": 1, "c": 2}]}'   the AND, rechecked: an array element with two or more
                               fields or elements, which must be found in one element of the row
    doc ? 'k'                  one key, exact
    doc ?| '{k,l}'             their OR, exact; NULL elements are skipped
    doc ?& '{k,l}'             their AND, exact; `?& '{}'` is every row, rechecked
    doc @@ '$.a[*].b == 1'     the keys under a.b with the value 1, rechecked
    doc @? '$.a ? (@.b > 1)'   the keys under a.b (a range only requires the path), rechecked

Numbers compare by value (`1`, `1.0` and `1e0` are one key) and strings by their bytes, as jsonb
equality does. A key longer than 2000 bytes is stored as its SHA-256, and a query that needs one is
rechecked. Counts are pushed down, the rechecked queries with each candidate checked in the heap,
except `?& '{}'`, which stays with the ordinary plan. `<@` is not indexed.

A jsonpath is always a superset, rechecked. `path == scalar` narrows to the documents with that
scalar under the path's object keys, at any array levels. A path that must yield something
(`exists`, either side of another comparison, `starts with`, `like_regex`, `@?`'s path) narrows to
the documents with something under its keys. Filters add what they require. `&&` keeps what either
side requires, `||` needs both sides to narrow, and under `!` comparisons and `exists` narrow
nothing. A path stops at `.*`, `.**` or an item method. A path's prefix that more than 1000 keys
start with narrows nothing, nor does one that could start a hashed key once the index has more
than 1000 hashed keys. Counts are pushed down with every candidate checked in the heap, but not
under an OR.

## Reloptions

`store_positions` (boolean, default `false`): on a `tsvector` column, also store every lexeme's
positions and weights, so phrase, weight and NOT queries are answered from the index and BM25
ranking works (see "Multi-key columns" above). Read at build time, like `summaries`.
`fillfactor` (10 .. 100, default 90): how full the build packs a directory leaf.
`max_entries` (default 0 = unlimited): an advisory cardinality guard.  The index warns once per
backend when it has grown past that many distinct keys - counted exactly by a build, estimated by an
insert that adds a key as the entries on its directory leaf times the number of leaves - and never
rejects a row.  For a multi-key column the keys are the extracted elements or lexemes.
`inline_limit` (64 .. 4096 bytes, default 4096): how large a key's posting set may be before it
moves out of its entry tuple onto container pages of its own.
`summaries` (`off` | `on` | `auto`, default `off`): summary posting sets for ranges (DESIGN.md §32).
With `on` every ordered scalar key column keeps, after its keys, one posting set per bucket of
consecutive keys, and a count over a range sums the buckets it covers whole instead of walking their
keys; `auto` gives them only to the columns whose keys are small next to a bucket (many distinct
values), which is where ranges are slow.  Every insert into a summarized column also updates its
bucket's set.  `summary_tids` (16 .. 16777216, default 4096) is the rows a bucket closes at.  Both
are read at build time: `ALTER INDEX ... SET (summaries = ...)` takes effect at the next REINDEX.
A range on a column that does not drive a count (`g, count(*) ... WHERE ts >= $1 GROUP BY
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
the number of rows a multi-key opclass extracted no key from, `ndistinct`, the distinct keys
the planner is given for the column (DESIGN.md §33; NULL where none is recorded), and, last,
`narrow_containers`, the column's NARROW containers (DESIGN.md §38).  `slack_bytes` and
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
  names a column's walk `Ordered By: <index> (<column>[, backward])`. A filter on the walked column
  alone (`WHERE s LIKE 'p1%' ORDER BY s`) is decided for a whole key by its first visible row, and
  a key that fails is skipped unread (`Filter per Value`, DESIGN.md §35). When the B-tree holds
  every column the query reads, the rows' values come from its index tuples and the heap is read
  only for pages not all-visible (`Ordered By: <index> (index only)`, DESIGN.md §40); off, no order
  is claimed and the covering walks below are the only ones left.
- `pg_lion.enable_btree_scan`: offer `LionBtreeScan`, a Lion-filtered walk of a covering B-tree
  that claims no order (`SELECT grp, sum(x) FROM t WHERE tags @> '{t}' GROUP BY grp` over a B-tree
  on `(grp) INCLUDE (x, tags)`): the values come from the index tuples, the heap is read only for
  pages not all-visible, and the planner prices the whole walk against the bitmap scan (DESIGN.md
  §40). EXPLAIN names the index it walks `Index: <index> (index only)`. `enable_indexonlyscan =
  off` takes these walks away too, and the index-only mode of the ordered ones.
- `pg_lion.enable_lazy_set`: let `LionOrdered` evaluate its Lion set only at the ranges of 64 heap
  blocks its walk reaches, and build it for the whole table only once that has cost what the build
  would (DESIGN.md §30.4, "The set, lazily"). Off, the set is built before the walk starts.
- `pg_lion.enable_decoded_walk`: count a `GROUP BY` of several Lion-indexed columns by decoding,
  at each range of 64 heap blocks, which value of each column every row has (DESIGN.md §34): three
  or more columns, and two where that is cheaper than the nested loop over their entries. Off, a
  `GROUP BY` of three or more columns goes to the ordinary plan and one of two to the nested loop.
- `pg_lion.enable_topk`: for `GROUP BY g ORDER BY count(*) DESC LIMIT k`, count only the groups
  that can be among the first `k`: each key's entry records how many rows it holds, which bounds
  its count, so the largest of those are counted first and the rest are never read once they cannot
  catch up (DESIGN.md §36). Off, every group is counted.
- `pg_lion.enable_bm25_scan`: offer `LionBm25` for `ORDER BY lion_bm25_score(col, query, index)
  DESC` with a `WHERE col @@ ...` it covers: the rows best first from the index's BM25 walk
  (DESIGN.md §17, "Ranking"). Off, every match is scored and sorted.
- `pg_lion.enable_plain_scan`: let the planner use plain and index-only scans of Lion indexes
  (`amgettuple`, DESIGN.md §29). Off, Lion indexes are planned for bitmap scans only, as GIN
  indexes are, and every other index's scans are unaffected - where `enable_indexscan = off` would
  also take away B-tree index scans. The count pushdown and `LionOrdered` still use Lion indexes.
- `pg_lion.enable_intersection_probe`: let Lion measure, at plan time, how many rows the AND of
  two or more equality, `IN` or multi-key clauses on one Lion index really selects, when the
  planner's product of their selectivities may be far off - correlated filters - and price its
  own paths (its index scans and the count pushdown) from that (DESIGN.md §29.11, "Correlated
  sets"). A bounded sample of the index, about 1,000 buffer accesses a planner run at most. Off,
  Lion prices its paths from the planner's estimate.
- `pg_lion.enable_rows_correction`: let what the intersection probe measures of a table's own
  filters correct the planner's row estimate for that table as well, so that joins above it are
  sized for the rows its filters really leave (DESIGN.md §29.11, "The relation's rows"). Off, the
  probe corrects Lion's own estimates only and the planner keeps its own row count.
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
| `resident_page_cost` | 120 | `cpu_operator_cost` | a page of a Lion index a count reads while the index fits in `effective_cache_size` with the query's tables (DESIGN.md §39); a page that does not is priced as I/O |
| `cold_page_cost` | 50 | `seq_page_cost` | a page read from the device, not the cache, one synchronous read at a time: a nested loop's inner index scan competing with an FK-side join, and that join's own reads, in the share of the tables `effective_cache_size` cannot hold (DESIGN.md §39, "A nested loop's cold reads"); 0 turns the charge off |
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

Rate settings (DESIGN.md §39, "The competitor's units"). The cost settings above are fitted at 500
cost units a millisecond, the rate of PostgreSQL's own sequential and index-only scans; its other
plans run at rates of their own - a hash aggregate over a whole table at about 200, a hash join
at about 250. A kind of plan with no setting below (scans, nested loops) is priced at the
reference, 1. A `LionCount`, `LionSemiJoin` or `LionAntiJoin` path is priced in the units of the
cheapest PostgreSQL plan it competes with: its own price, pages and CPU alike, times that kind of
plan's rate below, so its cost in `EXPLAIN` is that plan's units; which of its own forms Lion runs
is decided before, in its own units. Set a rate to 1 to price Lion as fitted against that kind of
plan; `SET client_min_messages = debug2` logs which kind each path was priced against, at which
rate and margin. A plan forced with PostgreSQL's `enable_*` settings, with nothing of PostgreSQL's
left enabled to compete with, is priced in Lion's own units. To read Lion's own cost units a
millisecond when calibrating the cost settings above, force its plans that way, or set every rate
below to 1. `bench/calib/matrix.py` measures each kind of plan's units a millisecond, and the
planner's mispicks, on synthetic tables (DESIGN.md §39, "The matrix").

| `pg_lion.` | default | the PostgreSQL plans it is the rate of, as a multiple of 500 units a millisecond |
|---|---|---|
| `hashagg_rate` | 0.1 | an aggregate that hashes: its own cost, less its input's; the rest of its plan is priced at 1 |
| `agg_rate` | 1.0 | a plain or sorted aggregate over a scan |
| `hashjoin_rate` | 0.5 | a hash join |
| `mergejoin_rate` | 1.0 | a merge join |
| `bitmap_rate` | 1.0 | a bitmap heap scan |

`pg_lion.pushdown_margin` (1, no margin): the share of the cheapest competing plan's cost a Lion
custom path (`LionCount`, `LionSemiJoin`, `LionAntiJoin`, `LionOrdered`, `LionBtreeScan`) must be
priced at to be chosen. Set below 1, its own price is divided by it, so a near tie goes to
PostgreSQL's plan, and its cost in `EXPLAIN` is marked up by it. It tips `LionBtreeScan` against
the sequential scan the same way: the covering walk's marked-up price must beat the sequential
scan's for the walk to be kept. It is not applied where it hedges nothing: to a plan
forced with nothing of PostgreSQL's left enabled, to a count whose cheapest competitor is the
access method's own scan of a Lion index (both prices Lion's), and to a count whose multi-key query
is a generic plan's parameter (priced at its dearest already). PostgreSQL's `enable_*` settings and Lion's
switches above still force a plan either way.

`pg_lion.ordered_switch_ratio` (32, 1 to 1,000,000): how many B-tree entries `LionOrdered` walks per
member it has not met yet before it stops walking, fetches the members left in TID order and sorts
them (DESIGN.md §30.4, "When the walk is not paying"; `Switched to Fetch and Sort` in `EXPLAIN
ANALYZE`). In index-only mode the switch is also held back while the walk meets members at the rate
a uniform spread predicts, since there it would read heap pages the walk never touches (DESIGN.md
§40.4). A large value keeps a walk from switching at all.

## Known limitations

Equality, `IN` lists, scalar ranges and the multi-key operators above are supported, through bitmap
scans and plain index scans (`amgettuple`, DESIGN.md §29). There are no ordered scans of the
access method itself (an `ORDER BY` is `LionOrdered`'s: a B-tree walked with a lion filter, or a
lion index's own ordered column walked, §30), no index-only scans of a lion index
that return a column (only those that need none, like `count(*)`; the rows of a Lion-filtered
query come from a covering B-tree instead, §40), no INCLUDE columns, no
parallel scan (a parallel build is supported from PostgreSQL 17), no reclaim of an emptied directory leaf or of an emptied posting-tree leaf
(both wait for the whole set or the whole index to go). Inserts serialise on the directory
leaf that holds the key; see the measured
[write costs](BENCHMARKS.md#size-build-and-writes-5m-rows-all-seven-indexes). Count pushdown supports constants, parameters
and stable expressions such as `now() - interval '30 days'` or `current_date - 30` (evaluated once
per execution; a volatile one like `random()` goes to the ordinary plan) on any indexed column, enum
columns included, or on an indexed expression such as `data->>'key'` treated as a column (on a
table that is not partitioned, outside joins and aggregates over keys; DESIGN.md §41), a boolean column tested by itself (`flag`, `NOT flag`, `flag IS TRUE`, `flag IS
NOT FALSE`), an `OR` of such clauses and of `AND`s of them, nested as deep as the query writes it
(`(a = 1 AND flag IS NOT TRUE) OR b = 2` is distributed into the arms it stands for, up to 1000
clauses in all), a `GROUP BY` of up to eight indexed columns - three or more, and two where the
walk is cheaper, over one table that is not partitioned, with a WHERE of no range (DESIGN.md §34) - or of `coalesce(col, constant)` of
one, whose NULL rows are counted in the constant's group, merged with that key's rows when the
column has it - and a `HAVING` over the counts it computes (a `HAVING` with a correlated subquery
goes to the ordinary plan). `count(DISTINCT col)` is
answered for an indexed column (DESIGN.md §26), and for a column a unique index proves unique - a
primary key - as the `count(col)` it equals: `count(*)` where the column is NOT NULL, whether or not
it has a lion index (a single-column, immediate, non-partial btree index under the `DISTINCT`'s
collation; on a partitioned table, one on the parent, and then only without a `GROUP BY`). `IN`
lists of more than 1000 values are left to the ordinary plan, a multi-key index can never drive a
`GROUP BY` or a sum-over-all-entries count (its entries are keys, not row values), and the cost
model inherits the
stale `relallvisible` blind spot of index-only scans.
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
