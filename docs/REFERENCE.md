# pg_lion reference

SQL functions, key types, multi-key operators, index options, settings and known limitations. Section numbers (§) refer to [DESIGN.md](../DESIGN.md).

## SQL functions

Ordinary SQL is the interface - the planner uses the index and the `LionCount` pushdown on its own -
and these functions are for testing, diagnostics and the occasional direct count:

    lion_bm25(idx, query, k [, k1, b, col])         the k best rows by BM25, best first (an index with positions)
    lion_bm25_score(tsv, query, idx [, k1, b, col]) one row's BM25 score; ORDER BY it for the LionBm25 scan
    lion_index_count(idx, key)                      count(*) WHERE col = key, through one index
    lion_index_count(idx1, key1, idx2, key2)        ... AND col2 = key2, two indexes on one table
    lion_index_count_any(idx, keys)                 count(*) WHERE col = ANY (keys)
    lion_index_count_stats(idx, key)                lion_index_count() plus the heap it visited
    lion_index_count_group_stats(idx [, use_cache, attno])   every group's count, as GROUP BY does
    lion_index_facets(idx [, k, filter, attno])     the k keys with the most rows under a filter (below)
    lion_index_slice_agg(idx [, filter, attno])     count, sum, min and max of a bit-sliced column (below)
    lion_index_stats(idx)                           the index's shape, one row per key column
    lion_index_verify(idx [, heapallindexed])       structural check, optionally against the heap
    lion_index_wal_mode(idx)                        'generic' or 'rmgr' (DESIGN.md §25)
    lion_index_posting_root(idx, key)               a key's posting-set head block (the root, or the first leaf), for tests

The counts answer exactly what the equivalent `SELECT count(*)` answers under the same snapshot,
and take the key as `col = key` takes it: the same operator, and the key converted as the query
converts it - an untyped literal (`lion_index_count('idx', 'abc')`, `lion_index_count_any('idx',
'{1,2}')`) as the column's type, `1` to numeric on a numeric column, `0.1` to float8 on a float4 one
(where, as in the query, no float4 value equals it). Where the query would convert the column
instead - `intcol = 1.5` compares `intcol::numeric` - the count is an error that says to cast the
key to the column's type. A NULL key, or a NULL array, counts 0, as `col = NULL` selects no row; a
NULL index answers NULL. The counts ask for what the query would: SELECT on the table or its indexed columns, and no row-level security in
force for the caller.  A count under a collation the index does not compare in is an error, never
a different number; on a column of a nondeterministic collation that includes a key of the default
collation - a literal's, which the call cannot tell from an explicit `COLLATE "default"` - so there
the key names the column's collation (`'abc'::text COLLATE case_insensitive`).  They take one INDEX, and an index belongs to one table, so they count that
table's own rows and nothing else.  On an inheritance parent that is the parent's rows alone - the
count of `SELECT count(*) FROM ONLY parent WHERE ...`, never the children's, even though a plain
`FROM parent` includes them.  A partitioned table's index has no storage and is refused, as by
every function here (below); pass a partition's own index, or write the `SELECT count(*)` against
the partitioned table and let the pushdown count every partition (DESIGN.md §16).  The pushdown is not
attempted for an old-style inheritance parent without `ONLY`, whose children need not even share
its columns: that query takes the ordinary plan.  The four diagnostic functions below the counts
are not executable by PUBLIC, as with pageinspect and amcheck; `lion_index_stats()` is granted to
`pg_stat_scan_tables`.
`lion_index_verify()` checks the index while it is being written to, the way `CREATE INDEX
CONCURRENTLY` builds one: it takes ShareUpdateExclusiveLock on the table and the index, so INSERT,
UPDATE and DELETE go on while it runs, and VACUUM, ANALYZE, DDL and a second verify wait for it;
a relation that is not a lion index is refused before either is locked.  CREATE INDEX
CONCURRENTLY on the same table can deadlock with it, whatever column it indexes - its last phases
wait out the snapshot the statement calling verify took - so run the two apart.  What a concurrent
insert could make look wrong it checks again once the statements that were writing the index have
ended, so it may wait for them - for as long as statement_timeout and lock_timeout allow - but never
reports their changes as damage.  On a hot standby it takes AccessShareLock, holds no page while it
waits for another that replay may hold, and tells what replay changed under it from damage by the
pages' LSNs, which it holds to the replay position it noted before reading them; a posting set or
position tree that replay changes under every one of several recounts is checked page by page, with
a WARNING that its totals were not compared, and pausing replay (`pg_wal_replay_pause()`) makes the
check exact.  With `heapallindexed` it checks that every row the heap shows is
under each of its keys - with its own positions and weights, in an index that stores them - and
evaluates the index's expressions as the table's owner (DESIGN.md §7).

Each of these functions reads one index's storage, and the index of a partitioned table has none:
each partition's index holds the entries of that partition's rows. `lion_index_stats()` returns no
rows for it, so a query over every lion index in `pg_class` goes on to the partitions' own; every
other function is an error, `"..." is a partitioned index`, with a hint to pass a partition's index.
A `lion_index_verify()` over every lion index takes those whose `relkind` is `'i'`.

### Facet counts (DESIGN.md §7, "Facet counts")

    lion_index_facets(idx regclass, k integer DEFAULT NULL, filter text DEFAULT NULL,
                      attno int2 DEFAULT 1, OUT key text, OUT count bigint) RETURNS SETOF record

The k keys of key column `attno` of `idx` with the most rows, largest count first - every key with
a row when `k` is NULL, none when it is 0 - under the rows `filter` keeps. For a multi-key column
(`array_ops`, `tsvector_ops`, `jsonb_contains_ops`) a key is an element, a lexeme or a jsonb path
key, and its count the rows whose value holds it, each row ONCE however many times its value holds
the key. With the filter as a WHERE clause, the answer is exactly

```sql
SELECT e::text, count(DISTINCT t.ctid) FROM t, unnest(t.col) e
 WHERE <filter> AND e IS NOT NULL GROUP BY e ORDER BY 2 DESC, e LIMIT k;
```

for an array, the `ndoc` of `ts_stat('SELECT col FROM t WHERE <filter>')` for a tsvector, and
`SELECT col, count(*) FROM t WHERE <filter> GROUP BY col ORDER BY 2 DESC, col LIMIT k` for a
scalar column, whose NULL group is a row with a NULL key. It is not `count(*)` over `unnest(col)`,
which counts an element twice in an array that repeats it. NULL elements, NULL and empty arrays and
tsvectors are under no key. The counts are of the rows the caller's snapshot sees, exact as the
count functions' are, after deletes and updates VACUUM has not yet cleaned up included.

- **Ties** at the cut are broken by the key, in the order the index keeps the column's keys in:
  the type's btree order under the column's collation (`C` for lexemes), byte order for a type
  with none and for jsonb path keys (their stored form, not the text shown); a NULL scalar key
  last. Every key tied with the k-th is counted, so the cut is deterministic.
- **The key** is the value's text output. A jsonb path key is shown as the condition it stands
  for: `$."a"."b" == "x"` (a scalar at a path; `[*]` marks a step into an array),
  `$."a".type() == "array"` or `"object"` (a container at a path), `? "k"` (what `?` finds: a
  top-level object key, a string element of a top-level array, a raw scalar string),
  `$.type() == "object"` or `"array"` (the document's own kind), and `sha256:<hex>` for a key too
  long to store, which was hashed. A tsvector lexeme or an array element too long to be a key is
  shown as itself, with its own count, as `ts_stat()` and `GROUP BY e` show it: the rows of its
  hashed key are read from the heap to tell it from the others the key may stand for.
- **The filter** is a WHERE condition over the index's table, written as in a query and analysed
  like one, made only of what Lion indexes answer exactly as posting sets: `AND` and `OR` of
  `col = value`, `col IN (...)` or `= ANY (array)`, and `col IS NULL` on a column some valid,
  non-partial Lion index has as a scalar key column, and the operators its keys answer exactly on
  one it has as a multi-key column: an array's `@>` and `&&`; `@@` with a tsquery that has no
  phrase, weight or `!` (prefixes `:*` included); jsonb `?`, `?|`, `?&`, and `@>` unless an array
  element of the query holds two or more keys. Not `<@`, an empty containment, or jsonpath's `@?`
  and `@@`, which the keys only narrow. The values are evaluated once, and may not read the table,
  call a volatile function or hold a subquery. Anything else - a range, `<>`, `NOT`,
  `IS NOT NULL`, a column no Lion index answers - is an ERROR naming the condition, never a scan.
  `NULL` (the default) and `'true'` keep every row.
- **Privileges** are those of the query it stands for: SELECT on the key column and on every column
  the filter reads, EXECUTE on the filter's operators and on the key type's equality, no row-level
  security in force for the caller; an index that is not valid, or that the caller's snapshot may
  not use, is refused, and a SERIALIZABLE caller takes the predicate locks an index scan would.
- **Costs**: with a k, each entry's TID count bounds its key's count under any filter, so the keys
  with the largest bounds are counted first and the rest only while one could still reach the
  k-th; the filter's sets are collected once and ANDed with each key's. When the filter keeps fewer
  rows than counting the keys would cost, the rows are read from the heap instead (a plain column of
  a non-partial index only). The keys counted are held in `work_mem` and sorted in a tuplesort that
  spills to temporary files past it, every key of a large column included; the keys of the filter's
  rows are counted in `hash_mem`, and past it by the keys instead. `SET client_min_messages =
  debug1` shows the entries read, the keys counted, the rows read and the keys sorted past
  `work_mem`.
- A partial index counts the rows it holds. An expression index's keys are the expression's.

### Aggregates from bit slices (DESIGN.md §51)

    lion_index_slice_agg(idx regclass, filter text DEFAULT NULL, attno int2 DEFAULT 1,
                         OUT count bigint, OUT sum numeric, OUT min text, OUT max text)

`count(x)`, `sum(x)`, `min(x)` and `max(x)` of key column `attno` of `idx`, an index built `WITH
(slices = on)` (below, "Reloptions"), over the rows the caller's snapshot sees that `filter` keeps,
counted from the column's bit slices without reading a row:

```sql
SELECT count(x), sum(x), min(x)::text, max(x)::text FROM t WHERE <filter>;
```

`count` is from the column's existence slice (its rows not NULL), `sum` from the rows in each bit's
slice, `min` and `max` by a descent from the top bit, one counted AND a bit. `sum` is a `numeric`
whatever the column's type, computed in 128 bits, so an `int8` column's sum past `int8`'s range is
exact; it is NULL for a `date`, `time`, `timestamp` or `timestamptz` column, whose types have no
`sum`, and NULL with `min` and `max` when no row is counted (`count` 0). `min` and `max` are the
column type's text output, infinities included. The answers are exact under the snapshot, as the
count functions' are: deletes and updates VACUUM has not cleaned up yet, and the caller's own
uncommitted rows, are what the query would see.

- **The filter** is `lion_index_facets()`'s, with the same rules and errors (an AND and OR of
  equalities, `IN` lists, `IS NULL` and the multi-key operators that Lion indexes answer exactly;
  anything else is an ERROR naming the condition, never a scan), collected once and ANDed with each
  slice. `NULL` (the default) and `'true'` keep every row.
- **Privileges** are those of the query it stands for: SELECT on the column and on every column the
  filter reads, EXECUTE on `count`, `sum`, `min` and `max` of the column's type and on the filter's
  operators, no row-level security in force; an index that is not valid, or that the caller's
  snapshot may not use, is refused, and a SERIALIZABLE caller takes the predicate locks an index
  scan would.
- **Refused**: a key column that is not sliced - built without `slices = on`, or of a type that is
  not sliced (the error says which) - and a multi-key column.
- A partial index answers for the rows it holds (`... WHERE <predicate> AND <filter>`). `SET
  client_min_messages = debug1` shows the slices counted and the descent's steps.
- **Its cost** hardly depends on how many rows the filter keeps: 2 · bits + 1 counted ANDs (97 for
  an `int4`, 193 for an `int8`), each over the filter's containers. On 5 million rows it took 26 to
  52 ms, against 30 ms for the query at a 1% filter and 140 to 330 ms at 10% and 50% (DESIGN.md
  §51.11): a win from some 1% of the rows up, sooner for narrower types.
- The planner answers the same aggregates from the slices in its own plans: `SELECT sum(x),
  min(x) ... WHERE ...` and `... GROUP BY g` take the `LionCount` node where the cost model says so
  ("The count pushdown", below; DESIGN.md §52). The function stays for a filter in text, and as
  the reference the tests check the node against.


## Key types

One default operator class per type, reusing the hash access method's hash functions: integers
(`int2/int4/int8`, one family with cross-type equality), `float4/float8`, `numeric`, `bool`, `"char"`,
`name`, `text` (and `varchar` through it), `char(n)`, `bytea`, `uuid`, `date`, `time`, `timetz`,
`timestamp`, `timestamptz`, `interval`, `macaddr`, `inet`, `pg_lsn`, `xid`, `cid`, `tid`,
`oid`, and any enum. `jsonb` has `jsonb_ops` for whole documents too, but it is not the default:
`jsonb_contains_ops` is (see "jsonb containment" below). Case-insensitive text: `CREATE EXTENSION
pg_lion_citext` (requires `citext`) adds `citext_ops`. Domains resolve to their base type.

**Long keys.** A key is at most 2047 bytes as stored, counting a 4-byte length header (2043 bytes of
text). A longer value is stored compressed, as a btree index stores it, so a value of any length
whose compressed form fits is a key - `repeat('abc', 20000)`, a `char(2500)` column's blank-padded
values, a 5000-digit numeric, a `jsonb_ops` document of a few kilobytes - and answers every query
exactly as a short one does. A value that is still over 2047 bytes compressed (incompressible text
of 2044 bytes or more, say) is an error naming the index and the row:

    ERROR:  index key size 2564 exceeds lion maximum 2047 for index "t_v_idx"
    DETAIL:  Index row references tuple (0,4) in relation "t".

where btree's limit, on the compressed size too, is 2704 bytes. tsvector lexemes, text, varchar and
bytea array elements and `jsonb_contains_ops` keys are hashed instead and never fail (below).

## Multi-key columns: arrays and tsvector (DESIGN.md §17)

`array_ops` (any array whose element type has a default hash or btree operator class, as GIN's
needs a btree one) and `tsvector_ops` index one row under many keys, reusing GIN's own
extraction functions (through wrappers that hash elements and lexemes too long to be keys), and
answer

    CREATE INDEX ON doc USING lion (tags);       -- text[], int[], ...
    CREATE INDEX ON doc USING lion (tsv);        -- tsvector, with positions

    tags @> '{a,b}'    the intersection of the elements' posting sets, exact
    tags && '{a,b}'    their union, exact
    tags <@ '{a,b}'    every indexed row, rechecked in the heap
    tags =  '{a,b}'    the rows holding every element, rechecked in the heap
    tsv  @@ 'a & (b | c)'   an AND/OR tree over the lexemes, exact (also as `@@@`)

Several such quals on one column (`tags && '{a}' AND tags && '{b}'`), like two lists on a scalar
column, are ANDed in the index, as quals on several columns are, so the heap gets only the rows they
select together (DESIGN.md §5, SCAN step 5).
`count(*)` over `@>`, `&&` and an AND/OR tsquery is pushed down like any other clause, and can be
combined with a `GROUP BY` on a scalar roaring column. Array `=` and `@@@` are answered by bitmap and
index scans only, as GIN answers them.

Whether an array is empty is answered from an `array_ops` index too, which keeps a column's NULL
arrays, its empty ones and the rest apart (DESIGN.md §17, "Non-empty arrays"). `count(*)` pushes
down, with no heap visit:

    cardinality(tags) > 0           also >= 1, <> 0; array_length(tags, 1) IS NOT NULL, > 0,
                                    >= 1, >= 0; array_ndims(tags) > 0; tags <> '{}'
    tags IS DISTINCT FROM '{}'      also cardinality(tags) IS DISTINCT FROM 0 (NULLs included)
    tags = '{}'                     also cardinality(tags) = 0, < 1
    array_length(tags, 1) IS NULL   also coalesce(cardinality(tags), 0) = 0 (NULLs included)
    tags IS NULL, tags IS NOT NULL  also cardinality(tags) IS NULL, cardinality(tags) >= 0

in any combination on one column (`tags IS NOT NULL AND array_length(tags, 1) > 0`), either way
round, with integer constants of any width, and under `NOT`, `IS TRUE`, `IS NOT FALSE` and the
like; in the `WHERE`, a `FILTER`, an `OR`, beside a `GROUP BY`. An array has an element whatever its
dimensions and lower bounds, and when its elements are all NULL (`'{NULL}'`). Left to the heap, as
a filter: a test some non-empty arrays pass and others fail (`array_length(tags, 1) > 1`,
`array_ndims(tags) = 1`, `array_lower(tags, 1) = 1`), a dimension other than 1, a bound that is a
parameter, and, beside a multi-key query known only at run time, one that NULLs pass. The planner
estimates the rows from the column's statistics, where it would otherwise take these as functions
it knows nothing of. Bitmap and index scans cannot use them: PostgreSQL hands an index no function
of a column, nor `<>`, so only `tags = '{}'` reaches one (answered as every row, rechecked) and
`IS [NOT] NULL` (exact).

A `tsvector` index also stores every lexeme's positions and weights per row, as GIN does, unless it
is built `WITH (store_positions = off)`, so phrases (`a <-> b`, `a <3> b`), weights (`a:A`) and NOTs
(`a & !b`, `a <-> !b`) are answered exactly from the index: bitmap and index scans need no recheck,
and counts skip the heap on all-visible pages, under a `GROUP BY`, `count(DISTINCT)` or the FK-side
join too, under an OR, and for `@@ ANY (array)` in scans. It costs about three bytes per lexeme and
row on top of a lion index without them, more than doubling it; an index that never sees a phrase,
a weight or a NOT, and is not used for ranking, can leave them out:

    CREATE INDEX ON doc USING lion (tsv) WITH (store_positions = off);

An index with positions also ranks. `lion_bm25(index, query, k [, k1, b, col])` returns the `k` rows
with the highest BM25 score for the query's lexemes, best first, scored entirely from the index
(term frequencies, document frequencies and each row's length, which the index keeps under its own
key); the heap is read only to check that the rows returned are visible. Join on `ctid` for the rows:

    SELECT d.*, s.score
      FROM lion_bm25('doc_tsv_idx', to_tsquery('english', 'cat | dog'), 10) s
      JOIN doc d ON d.ctid = s.ctid
     ORDER BY s.score DESC;

The score is Lucene's BM25 (`k1` 1.2 and `b` 0.75 by default; `k1` any finite number from 0, `b`
from 0 to 1) over the query's lexemes, each counted once; lexemes under a NOT do not score, and
operators, phrases and weights do not change it, so add `WHERE d.tsv @@ query` to keep only rows that
match the query as a whole. A prefix lexeme (`foo:*`) cannot be ranked and is an error, except under a
NOT, where it does not score either. As in a search
engine, the statistics count deleted rows until VACUUM removes them, and a backend keeps the mean
row length it read until the row count moves by more than 1/64. Rows that cannot reach the top `k`
are skipped (MaxScore), so a very common lexeme next to rarer ones costs little. Like the counts,
`lion_bm25()` and `lion_bm25_score()` need SELECT on the table and refuse a table where row-level
security applies to the caller: the statistics count every row, so a score would reveal what the
hidden rows hold.

An index on several `tsvector` columns ranks each of them on its own. Every column that stores
positions keeps statistics of its own, so a column ranks exactly as an index on that column alone
would, and `col` says which one; it is needed only when more than one column of the index stores
positions, and leaving it out then is an error that names the candidates:

    CREATE INDEX doc_text_idx ON doc USING lion (title, body, lang);
    SELECT * FROM lion_bm25('doc_text_idx', 'cat | dog', 10, col => 'body');

`col` is the column's name in the index, as `\d doc_text_idx` lists it under Column: for a plain
column the table column's name when the index was built, and for an expression the name CREATE
INDEX gave it, which differs between PostgreSQL versions: `to_tsvector`, then `to_tsvector1` for a
second one, on PostgreSQL 19. A name the index does not have, or a column that does not store
positions, is an error; `col => NULL` is the same as leaving it out. A NULL in any other argument
gives no rows (`lion_bm25_score()`: NULL).

`lion_bm25_score(tsv, query, index [, k1, b, col])` is the same score for one row, from its
tsvector and the statistics of the index column `col` names. Ordered by it, with a `WHERE` that
matches on the same column, the planner returns the rows best first straight from the index (a
`LionBm25` scan) instead of scoring and sorting every match:

    SELECT d.*, lion_bm25_score(d.tsv, 'cat | dog', 'doc_tsv_idx') AS score
      FROM doc d
     WHERE d.tsv @@ 'cat | dog'
     ORDER BY lion_bm25_score(d.tsv, 'cat | dog', 'doc_tsv_idx') DESC
     LIMIT 10;

The scan is offered when the query, index and `col` are constants, the tsvector scored is the
column the function ranks - `d.body` in `lion_bm25_score(d.body, q, 'doc_text_idx', col =>
'body')`, a plain column, not an expression - and every row the `WHERE`'s tsquery can match has
one of the scored lexemes: the same query in both places, or a narrower one such as
`WHERE d.tsv @@ 'cat & black'` under a score of `'cat'`. Other `WHERE` clauses filter its rows, and
a second sort key is an incremental sort above it. Elsewhere - another tsvector scored against a
column's statistics included - the function scores row by row; either way the rows and scores are
the function's. `pg_lion.enable_bm25_scan` turns the scan off.

A lexeme of up to 2043 bytes is a key as it is. A longer one (tsvector allows 2047) is stored as its
first bytes and its SHA-256, and a query that names a lexeme of 2043 bytes or more has every
candidate rechecked in the heap, so the answer stays exact; a prefix `foo:*` longer than about 1975
bytes is not expanded, and every row is rechecked. A `text`, `varchar` or `bytea` array element
(under a deterministic collation) is filed the same way, at any length: one of 2043 bytes or more in
a query is rechecked. An element of another type longer than 2047 bytes is stored compressed, and is
an error only if that does not fit, as a GIN index tuple that does not fit is.

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

`jsonb_contains_ops`, the default for `jsonb` as GIN's containment class is GIN's, indexes a
document under one key per path to a scalar (with the scalar), one per object or array below the
root, the root's type, and one per top-level key, string element or string scalar:

    CREATE INDEX ON docs USING lion (doc);       -- or (doc jsonb_contains_ops)

    doc @> '{"a": {"b": 1}}'   the AND of the query's paths, exact
    doc @> '{"a": [{"b": 1, "c": 2}]}'   the AND, rechecked: an array element with two or more
                               fields or elements, which must be found in one element of the row
    doc ? 'k'                  one key, exact
    doc ?| '{k,l}'             their OR, exact; NULL elements are skipped
    doc ?& '{k,l}'             their AND, exact; `?& '{}'` is every row, rechecked
    doc @@ '$.a[*].b == 1'     the keys under a.b with the value 1, rechecked
    doc @? '$.a ? (@.b > 1)'   the keys under a.b (a range only requires the path), rechecked

The count pushdown also takes `(doc ->> 'k') = 'v'` as `doc @> '{"k": "v"}'`, the same test
when v is a string constant that is not the text of a number, of `true` or `false`, or of an object
or array (it starts with neither `{` nor `[`), compared under a deterministic collation: in a
`WHERE` it is answered from the index instead of the heap, and in a `FILTER` it does not send the
count to the ordinary plan. `->>` compared with a parameter, a number's text and the like stay with
the heap (DESIGN.md §42.6).

Numbers compare by value (`1`, `1.0` and `1e0` are one key) and strings by their bytes, as jsonb
equality does. A key longer than 2047 bytes is stored as its SHA-256, and a query that needs one is
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

Lion's `jsonb_ops`, which is not the default, indexes whole documents instead, under the btree
meaning of `jsonb_ops`: `=`, `<>`, the ranges and `count(DISTINCT doc)`. Name it for those, alone
or beside a `jsonb_contains_ops` index on the same column:

    CREATE INDEX ON docs USING lion (doc jsonb_ops);

## Nearest neighbours: lionvec (DESIGN.md §44)

`CREATE EXTENSION pg_lion_vector` (it needs pgvector 0.7 or later and `pg_lion`) adds the `lionvec`
index access method for pgvector's `vector` and `halfvec` types and three operator classes for
each, one per distance:

    CREATE INDEX ON items USING lionvec (embedding);                     -- vector_l2_ops, <->
    CREATE INDEX ON items USING lionvec (embedding vector_ip_ops);       -- <#>
    CREATE INDEX ON items USING lionvec (embedding vector_cosine_ops);   -- <=>

and, on a `halfvec` column, `halfvec_l2_ops` (the default, `<->`), `halfvec_ip_ops` (`<#>`) and
`halfvec_cosine_ops` (`<=>`), whose query is a `halfvec` too. A `halfvec`'s elements, IEEE 754
half-precision floats, are read as floats, so its index is the same as a `vector` column's, and
its distances are pgvector's `halfvec` ones. An expression index of a `vector` column cast to
`halfvec` serves an `ORDER BY` of the same cast:

    CREATE INDEX ON items USING lionvec ((embedding::halfvec(1536)) halfvec_cosine_ops);
    SELECT id FROM items ORDER BY embedding::halfvec(1536) <=> $1::halfvec(1536) LIMIT 10;

    SELECT id FROM items ORDER BY embedding <=> $1 LIMIT 10;
    SELECT id FROM items WHERE category = 3 AND in_stock ORDER BY embedding <=> $1 LIMIT 10;

The build partitions the rows by k-means and keeps one bit a dimension of every row, rotated so that
its error is known, partition by partition, and the rest of an 8-bit code beside it. A scan reads
the partitions whose centroids are nearest the query, nearest first; it scores their rows from the
bits, refines only the rows their error bound cannot rule out, and computes the distances of the
last few from the heap with pgvector's own operator. It stops once the next centroid, less
`pg_lion.vector_margin`, lies farther than the rows it has, and pauses once
`pg_lion.vector_patience` times `sqrt(n) / 2` rows (n the rows it was built with) have brought
nothing nearer, so it reads a few percent of the rows, and a row in a partition it did not read is
missed: the answer is approximate, as HNSW's and IVFFlat's are (at the defaults, 98% of the true ten
nearest over 975,000 OpenAI embeddings, where HNSW at `ef_search` 100 returns 95%; DESIGN.md §44.9).
Rows come back in the order of their true distances, and the distances are pgvector's; a row found
too late - in a partition read after farther rows were returned, or when a bound of
`pg_lion.vector_epsilon` standard deviations fails - is skipped, never returned out of order. NULL
vectors come last with NULL distances, and in a cosine index zero vectors just before them, with
NaN, as a sort puts them.

    CREATE INDEX ON items USING lionvec (embedding vector_cosine_ops) WITH (partitions = 4000);

`partitions` (0 to 262,144) sets how many partitions the build makes; 0, the default, makes one
for a table of fewer than 10,000 rows and eight times the square root of the rows' count for a
larger (8,000 for a million rows, never more than a sixteenth of the rows). The build chooses them
from a sample of as many rows as half of `maintenance_work_mem` holds (1.5 KB a 1536-dimension
row, at most 16.8 million rows, 25.8 GB), at least 16 sample rows a partition it chooses and one a
partition `partitions` asks for, and runs k-means over at most 128 of them a partition (never
fewer than 65,536 rows): the default 64 MB holds about 21,800 rows of 1536 dimensions, enough for
1,361 partitions, which a table of 28,986 rows outgrows; a million rows need 376 MB for their
8,000, 1 GB gives k-means 44 rows a partition and 3.2 GB the 128, and a billion rows' 252,982 need
12.5 GB. A build whose sample is too small for the partitions its rows want, or for those
`partitions` asks (up to the table's rows), fails as soon as its first pass has read enough rows
to know it, before the rest of the build, naming the `maintenance_work_mem` that would be enough
(for the rows the table holds, reckoned from the part read so far - "About"):

    ERROR:  lionvec index "items_emb" wants 252982 partitions, and its sample has rows for 1361
    DETAIL:  About 1000000000 rows with a vector want 252982 partitions of at least 16 sample rows each, and half of maintenance_work_mem (64MB) holds a sample of 21788 rows of 1536 dimensions.
    HINT:  Set maintenance_work_mem to at least 11890MB, or set pg_lion.vector_build_sample to allow to build 1361 partitions.

`SET pg_lion.vector_build_sample = allow` builds the fewer partitions instead, with a NOTICE
(below). A table of fewer than 10,000 rows, which has one partition, builds at any
`maintenance_work_mem`, and a `partitions` larger than the table's rows wants one a row, which a
sample of every row makes. More partitions make a scan score fewer rows and rank more centroids,
and make the build slower (it grows with the rows times the square root of the partitions); they
leave the pause where it was, as `pg_lion.vector_patience` counts rows, not partitions. The setting
takes effect at the next `REINDEX`.

    CREATE INDEX ON items USING lionvec (embedding vector_cosine_ops) WITH (spill = off);

`spill` (boolean, default `on`) puts each row in a second partition besides its nearest: of the
eight next nearest centroids, the one SOAR's rule picks, off the line from the row's own centroid
through the row, so that a query that passes the row's partition by tends to find it in the
second. A scan then finds more of the nearest rows at the same `pg_lion.vector_margin` (on 115,000
OpenAI embeddings, 0.92 of the ten nearest at the default margin against 0.84 without), or as
many at a smaller one, reading fewer partitions; it returns each row once. The cost is a second
copy of every row's one-bit code: twice the code pages, which are an eighth of a 1536-dimension
index (1.9 GB rather than 1.7 GB for a million rows), each partition twice as long to read, and an
insert written to two partitions. With fewer than two partitions there is nothing to spill to.
The build reads it, so a change takes effect at the next `REINDEX`.

An index whose vectors may lie in TOAST - pgvector's `vector` of more than 506 dimensions, or
`halfvec` of more than 1,012, unless the column is `PLAIN` or `MAIN` or the table's
`toast_tuple_target` keeps its rows whole - keeps where each row's vector lies there (DESIGN.md
§44.13): the TOAST relation's block and line pointer of its chunks, on pages of their own, the
LOCATOR, in heap order (28 bytes a row, 1.3% of a 1536-dimension index), and beside each row's 8-bit
codes. A scan reads such a vector from its chunks' page rather than through the row's table page and
the TOAST index (`pg_lion.vector_toast_direct`, below), checking every chunk and then the row, so
the answer is the same. On DBpedia's 115,186 embeddings in TOAST an exact answer over 1% of the rows
took 68 ms from the disk where it had taken 180, and 8 ms warm where it had taken 14; a probing
scan, which reads only the rows it reranks so, 6 to 7% less from the disk. An index whose vectors
lie in their rows keeps none and costs nothing more.

With a `WHERE` that lion indexes answer, `LionOrdered` builds lion's set of the matching rows and
the scan reads and scores only those (`Custom Scan (LionOrdered)` with `Order By:` in `EXPLAIN`),
so a `LIMIT` is never left short. A set of up to `pg_lion.vector_exact_rows` rows (2,000), or of
up to `pg_lion.vector_exact_per_partition` (1.5) rows for each of the index's partitions if that
is more, is answered exactly, from every member's distance; a larger one by reading partitions as
above, counting its patience in matching rows. The exact answer is priced as the planner prices a
sort of the lion bitmap scan's rows, which is exact too, and planned unless the sort is priced at
less than 1 / 1.2 of it: with the vectors in TOAST, which the node reads by their places, the node
by a wide margin; with them in the rows, where the two read the same pages, the node too (DESIGN.md
§44.7, "Exact path pricing"). A `WHERE` clause no lion index answers - a column without one,
`id > 0` - is never applied to the rows the `lionvec` scan returns: the scan pauses and skips late
rows by the rows it returns, not by those a `Filter` keeps, so the answer would depend on the plan
(DESIGN.md §44.6, "Post-filtering"). By default such a query fails when it is planned
(`pg_lion.vector_postfilter`, below, whose `exact` plans it as a sort of the filter's rows instead);
for an approximate answer, filter the nearest rows of a subquery, which the planner never pushes a
clause into past its `LIMIT` (a clause of the vector column alone, `embedding IS NOT NULL` or a
distance bound such as `embedding <=> $1 < 0.3`, is the exception: it filters the scan's rows, NULL
vectors coming last and the bound rejecting rows by the distance they come in):

    SELECT id FROM (SELECT id, note, embedding <=> $1 AS d FROM items WHERE category = 3
                    ORDER BY d LIMIT 1000) s
     WHERE note LIKE 'a%' ORDER BY d LIMIT 10;

With `pg_lion.vector_parallel_scan` on (below; off by default), an unfiltered order can be scanned in parallel (DESIGN.md §44.4, "A parallel scan"): `Gather Merge`
over `Parallel Index Scan using <lionvec index>`, each process - the workers, and the leader unless
`parallel_leader_participation` is off - probing the partitions it claims first, running the scan's
margin and pause on its own rows and returning them in order of distance, and Gather Merge merging
the orders; each row comes from one process, once, even in an index that spills. Every process
ranks all the centroids, and the planner prices a partial path so: that ranking in full, the rest
of the start-up and every row over core's parallel divisor (DESIGN.md §44.7). The workers are
core's: from the code pages a scan reads (`min_parallel_index_scan_size`) and the table pages its
rows are fetched from (`min_parallel_table_scan_size`), or the table's `parallel_workers`, up to
`max_parallel_workers_per_gather`; at the default `parallel_setup_cost` the serial scan is cheaper,
and on DBpedia's 115,386 rows the serial scan was 4 to 8 times faster than the parallel ones.
Under `pg_lion.vector_postfilter` `error` (the default) or `exact` a partial path that a `Filter`
would follow is removed as the serial one is, and `LionOrdered` over a distance order is never
parallel, so a filtered query runs in one process; under `allow` the parallel scan with a `Filter` is a plan like the
serial one.

## Reloptions

Every option but `max_entries` is read by a build alone: what it decided is on the index's meta
page or in the pages it wrote, and inserts, VACUUM and scans read that, so `ALTER INDEX ... SET`
of one changes the index at its next `REINDEX` (or `REINDEX CONCURRENTLY`). Setting any of them
takes `ShareUpdateExclusiveLock` on the index, as core's `fillfactor` of an index does, and blocks
no scan or insert. `max_entries` applies to the next insert.

`store_positions` (`auto` | `on` | `off`, default `auto`; `true` and `false` are `on` and `off`): on
a `tsvector` column, also store every lexeme's positions and weights, so phrase, weight and NOT
queries are answered from the index and BM25 ranking works (see "Multi-key columns" above), at
about twice the index's size. `auto` stores them when a key column's operator class can
(`tsvector_ops`) and builds an index with no such column without them; `on` on such an index is an
error; `off` leaves them out. Read at build time, like `summaries`: `ALTER INDEX ... SET` changes
nothing until a `REINDEX`. The build sorts the positions with the posting sets, in the same
`maintenance_work_mem`, and writes them to temporary files with them past it, so the index is the
same at any setting; more memory only writes less.
`fillfactor` (10 .. 100, default 90): how full the build packs a directory leaf.
`posting_fillfactor` (10 .. 100, default 90): how full the build packs a posting-tree leaf, the
container pages a key's posting set lives on once it outgrows its entry (DESIGN.md §22). At 100 a
leaf is packed until the next container does not fit, so the first insert after the build that has
to rewrite a container on it - one whose new member lands inside the container, as the new row
version of an UPDATE that stays on the old row's page does - splits the leaf. The default leaves a
tenth of each leaf for that, as a B-tree leaf's fillfactor does, for about a tenth more posting
pages; an index on an append-only or read-only table can be built at 100 and save them, and one
that takes a heavy wave of scattered writes soon after its build may want 80. A leaf always takes
its first container, so a low value never leaves a page empty.
`build_slack` (`auto` | `on` | `off`, default `off`): whether the build writes each entry that
keeps its posting set inline, and each container on a posting-tree leaf, with the growth slack an
insert would give it (DESIGN.md §4), so that the first rows an insert adds to it land inside the
bytes it already has and nothing else on its page moves. That matters with `wal_mode = generic`,
where the WAL record of a change is a diff of the page and moving the entries stored after a grown
one makes it most of the page: on scattered updates soon after a build, `on` cut generic-mode WAL
from 1.1-2.4 KB to 65-435 bytes a row in DESIGN.md's measurement. Under Lion's resource manager the
record is the entry's own change either way and the option buys little. `auto` is `on` for an index
built for generic WAL and `off` for one built for the resource manager. The slack costs space:
about an eighth of each entry or container, 8 to 64 bytes, which came to 4-46% more index
at build in the same measurement. Both options are read at build time; `ALTER INDEX ... SET` takes
effect at the next REINDEX.
`max_entries` (default 0 = unlimited): an advisory cardinality guard.  The index warns once per
backend when it has grown past that many distinct keys - counted exactly by a build, estimated by an
insert that adds a key as the entries on its directory leaf times the number of leaves - and never
rejects a row.  For a multi-key column the keys are the extracted elements or lexemes.
`inline_limit` (64 .. 4096 bytes, default 4096): how large a key's posting set may be before it
moves out of its entry tuple onto container pages of its own. The build records it on the meta
page, and inserts and VACUUM keep to that: `ALTER INDEX ... SET (inline_limit = ...)` takes effect
at the next REINDEX.
`buckets` (0 .. 65536, default 0): accepted and ignored. It sized the hash directory of the
first index formats, which a B-tree keyed by the index key replaced (DESIGN.md §21); that tree
grows by splitting and has nothing to size, so an index built with any value is the index built
without it. A value above 0 is answered with a NOTICE saying so when it is set, by `CREATE INDEX`
or `ALTER INDEX ... SET`; a value outside the range is an error, as for any reloption.
`fillfactor` is what decides how full the build packs the directory's leaves.
`slices` (`on` | `off`, default `off`): bit slices of every integer, date, time and timestamp key
column (DESIGN.md §51). A sliced column keeps, after its keys and their summaries, one posting set
per bit of its values - the rows whose value, as a signed integer of the type's width with the sign
bit flipped, has that bit set: 16 for `int2`, 32 for `int4` and `date`, 64 for `int8`, `time`,
`timestamp` and `timestamptz` - and one of its rows that are not NULL; the count pushdown
(`pg_lion.enable_count_slices`, below) and `lion_index_slice_agg()` count `sum`, `avg`, `min` and
`max` under a filter, and per group, from them. The planner prices the index's scans of its keys
by its pages less the slices', which those scans do not read (DESIGN.md §52). A key column of any other type (`oid`,
`float4`, `float8`, `numeric`, `interval`, text, arrays, ...) is simply not sliced, and
`lion_index_stats().sliced` says which are. Every insert into a sliced column also puts the row
into the slice of each bit its value has set - about half the bits for values spread over the type,
so some 17 posting-set inserts for an `int4`, 33 for an `int8` - which is the price of the option:
an `INSERT` of such values into a lion index of an `int4` and one of an `int8` took about 9 times
as long as without slices (DESIGN.md §51.11). The slices of a dense column are mostly bitsets, which
an insert changes in place; they added 4% to 40% to the size of the indexes measured, the most to a
column of few keys. An index build with slices took 1.2 to 1.5 times as long. Read at build
time, like `summaries`: `ALTER INDEX ... SET (slices = ...)` takes effect at the next REINDEX. An
index with a sliced column is on-disk format 15, which a build of pg_lion before this option
refuses; one without stays format 14, readable by both - so REINDEX with `slices = off` before
going back to an older build.
`summaries` (`off` | `on` | `auto`, default `off`): summary posting sets for ranges (DESIGN.md §32).
With `on` every ordered scalar key column keeps, after its keys, one posting set per bucket of
consecutive keys, and a count over a range sums the buckets it covers whole instead of walking their
keys; `auto` gives them only to the columns whose keys are small next to a bucket (many distinct
values), which is where ranges are slow.  Every insert into a summarized column also updates its
bucket's set.  `summary_tids` (16 .. 16777216, default 4096) is the rows a bucket closes at.  Both
are read at build time: `ALTER INDEX ... SET (summaries = ...)` takes effect at the next REINDEX.
A count over a range beside other conditions counts each bucket's set against them until those
counts have visited twice the containers the conditions' smallest set holds, then collects the
conditions once and steps the remaining buckets at their keys (`Range Walks Probed` in
`EXPLAIN ANALYZE`) - when a copy of that smallest set fits `work_mem`; otherwise it counts every
bucket.  The planner prices whichever the count will do, so a larger `work_mem` can make such a
count the cheaper plan.
A range on a column that does not drive a count (`g, count(*) ... WHERE ts >= $1 GROUP BY
g`, a range in an OR, a join's fact filter) is answered too, collected once from the same walk -
within a hash table's memory (`work_mem` times `hash_mem_multiplier`, shared by the table's ranges:
one that does not fit what the others left sends those to temporary files first) and past it in
windows into a temporary file (`Range Sources Spilled` in `EXPLAIN ANALYZE`), or, where there are
few counts - a few groups, a `count(DISTINCT)`'s tests - and the planner finds it at least half
again cheaper, walked again at each, whether or not it would fit (`Range Sources Walked`; a range
in an OR is always collected). These counters count each range once, however many partitions,
rescans and parallel workers collected it.
An IN list longer than a batch - `work_mem` over 256 bytes a value, and never fewer than 1000
values - is not held whole either: a count of the table as one row locates and counts it a batch at
a time, the list that drives a `GROUP BY` on its own column is located a batch at a time as the
groups are walked (both `List Batches` in `EXPLAIN ANALYZE`), and every other count - other groups,
a `count(DISTINCT)`, a `FILTER`, an OR, a second long list - collects it as a range is, in the same
memory and past it in windows (`Lists Collected`, and of those `Lists Spilled`, each list once).
`wal_mode` (`auto` | `generic` | `rmgr`, default `auto`): which WAL resource manager this index is
logged through (DESIGN.md §25).  Measured on a release build: the 8-client hot-key insert burst goes
from 765 to 1275 tps (p95 14.8 to 9.9 ms, against btree's 1632 / 7.8), 10,000 inserts into the
1M-row eight-index portfolio from 922 ms / 88 MiB to 254 ms / 65 MiB, and an insert into a key whose
posting set is still inside its entry tuple from 2919 to 422 bytes of WAL.  A container or entry
that has to be rewritten is logged as the bytes that changed, or as the bytes an insert put in the
middle of it and where, not as the whole item.  `auto` is `rmgr` when the server registered Lion's own resource
manager and `generic` otherwise, so one CREATE INDEX script works on a cluster that preloads the
library and on one that does not.  The mode is fixed at build time and recorded on the meta page;
`SELECT lion_index_wal_mode(idx)` reports it and REINDEX is what changes it.  An rmgr-mode index can
be READ on any server but can only be WRITTEN where the resource manager is registered (an unlogged
or temporary one writes no WAL and is written anywhere), which needs

    shared_preload_libraries = 'pg_lion'      # and a restart
    # optional: pg_lion.rmgr_id = 170         # the id to register under

So once an index is built in rmgr mode, `pg_lion` has to stay in `shared_preload_libraries`. On a
server started without it, INSERT, UPDATE and COPY into the index's table fail, and so does every
VACUUM that has dead rows to remove from the index - autovacuum included, which reports the error
only in the server log while the table's dead rows (`n_dead_tup`) accumulate. Reads, DELETE and
DROP INDEX still work. The error's hint names both ways out: preload the library again and restart,
or rebuild the index in generic mode. To stop preloading the library, rebuild every rmgr-mode index
first, while it is still preloaded:

    SELECT c.oid::regclass, lion_index_wal_mode(c.oid)
      FROM pg_class c JOIN pg_am a ON a.oid = c.relam
     WHERE a.amname = 'lion' AND c.relkind = 'i';

    ALTER INDEX idx SET (wal_mode = generic);
    REINDEX INDEX idx;

A REINDEX or TRUNCATE on a server without the preload rebuilds an index left at `auto` in generic
mode, and it stays generic when the library is preloaded again, until the next REINDEX there.

`pg_lion.rmgr_id` defaults to 170, the id reserved for pg_lion on the
[list of custom resource managers](https://wiki.postgresql.org/wiki/CustomWALResourceManagers#List_of_Custom_Rmgrs).
Two extensions that take the same id cannot be loaded together, so change it only to move out of
such a collision; `pg_get_wal_resource_managers()` shows what is registered.  Once an index has
been written in rmgr mode the library must stay in `shared_preload_libraries` for as long as WAL
that mentions it may still be replayed - the rule core states for every custom resource manager.
WAL written under one id replays only under that id, so change `pg_lion.rmgr_id` only after a clean
shutdown, once every standby and WAL archive consumer has replayed everything written under the old
id, and restart each server of the cluster with the new one; `pg_upgrade` carries no WAL across, so
an upgrade may change it freely.
`lion_index_stats()` reports ONE ROW PER KEY COLUMN (DESIGN.md §24), with a leading `attno`: the
directory's height, its leaf and internal pages and whether that column is `ordered` (false for a
key type with no btree opclass, whose entries are then in a complete but arbitrary order), the
column's entries, its containers by kind, its sparse segments, its posting trees' internal pages
and tallest height (a set of up to 16 leaves has no internal level, DESIGN.md §22), `null_tids`,
the number of rows whose key in that column is NULL, and `empty_tids`,
the number of rows a multi-key opclass extracted no key from (an array with no elements, a tsvector
with no lexemes), `ndistinct`, the distinct keys
the planner is given for the column (DESIGN.md §33; NULL where none is recorded),
`narrow_containers`, the column's NARROW containers (DESIGN.md §38), `nullelem_tids`,
the number of rows whose array has elements and all of them NULL (DESIGN.md §17),
`position_pages` and `position_internal_pages`, the leaves and the pages above them of the column's
position trees (an index built with `store_positions`, DESIGN.md §17), which `container_pages`,
`posting_internal_pages` and `max_posting_height` leave out, and, last, `emptydim_tids` and
`emptyinner_tids`, the rows whose array has no element but has dimensions, which `empty_tids` leaves
out: an empty array whose first dimension has length 0 (an empty `int2vector` or `oidvector` cast
to `int2[]` or `oid[]`), and one whose first dimension has a length and an inner one none; then
`sliced`, whether the column keeps bit slices (DESIGN.md §51), and `slice_entries`, `slice_tids`,
`slice_bytes` and `slice_pages`, what its slices hold - their entries (a bit's or the existence
slice's), the rows in them counted once a slice, the bytes of their items and their posting pages -
which the other counters leave out, as they leave out the summaries'.  `slack_bytes` and
`inline_slack_bytes` are the growth slack inserts leave inside items and inside INLINE entry
payloads (DESIGN.md §4), which is space a later insert into the same key grows into for free; a
bulk-built index has none of either unless it was built with `build_slack`.  The counters that describe the
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
  a key that fails is skipped unread (`Filter per Value`, DESIGN.md §35), when the filter is
  immutable and equal values of the type are stored identically; any other filter is evaluated row
  by row. When the B-tree holds
  every column the query reads, the rows' values come from its index tuples and the heap is read
  only for pages not all-visible (`Ordered By: <index> (index only)`, DESIGN.md §40); off, no order
  is claimed and the covering walks below are the only ones left.
- `pg_lion.enable_btree_scan`: offer `LionBtreeScan`, a Lion-filtered walk of a covering B-tree
  that claims no order (`SELECT grp, sum(x) FROM t WHERE tags @> '{t}' GROUP BY grp` over a B-tree
  on `(grp) INCLUDE (x, tags)`): the values come from the index tuples, the heap is read only for
  pages not all-visible, and the planner prices the whole walk against the bitmap scan (DESIGN.md
  §40). EXPLAIN names the index it walks `Index: <index> (index only)`. `enable_indexonlyscan =
  off` takes these walks away too, and the index-only mode of the ordered ones. A B-tree that
  holds none of the filter's columns - a primary key under a Lion filter on others - is walked
  whole all the same. The walk's set answers every condition a Lion index answers, one that
  PostgreSQL's bitmap AND leaves to its heap `Filter` because it keeps most rows included (`b IN
  (1, 2)` of three values beside `a IN (...)`): EXPLAIN lists it under `Lion Cond`, and the heap is
  not read for it (DESIGN.md §40.2, "A clause core's AND leaves out"); the planner prices that
  larger set against walks of the smaller sets PostgreSQL chose, which read the heap.
- Nested loop probes: `LionBtreeScan` is also offered as the inner side of a nested loop,
  parameterized by the outer row as PostgreSQL's Index Scan is (`... FROM fact f JOIN dim d ON
  d.id = f.dim_id WHERE <Lion filters on d>`, DESIGN.md §40.10): `Index Cond: (d.id = f.dim_id)`,
  each rescan a descent of the B-tree to the outer row's key, the entry tested against a Lion set
  built once from the relation's own Lion conditions - never from the join - and kept for every
  rescan, a `Memoize` above it or not; the values come from the index tuple and the heap is read
  only for pages not all-visible. `EXPLAIN ANALYZE` counts every rescan's entries and fetches, and
  `Lion Set` shows one build. It needs a B-tree that covers the columns the query reads of the
  relation (and no row lock anywhere in the query, nor an `UPDATE`, `DELETE` or `MERGE`: these give
  every relation a `ctid`, which no index returns) and `enable_indexonlyscan`; the planner
  prices the set's build spread over the loop's rescans - over the outer side's distinct keys when
  `enable_memoize` is on - against PostgreSQL's heap fetch per outer row.
  `pg_lion.enable_btree_scan` turns it off with the other walks.
- `pg_lion.enable_heap_probe` (off): also offer that probe where the B-tree does not cover what
  the query reads of the relation - a primary key under a Lion filter, the query reading other
  columns (DESIGN.md §49). EXPLAIN shows `Index: <index>` without `(index only)`: the entry the
  outer row's key finds is tested against the Lion set, and the heap row is fetched for a member
  alone, where PostgreSQL's Index Scan of the same B-tree fetches every row to test its `Filter`.
  The planner prices both with the same heap-page formula, the probe over its members' fetches
  alone, so it is chosen where the set rejects enough rows to pay for its build. It needs
  `enable_indexscan`, and is offered under a `Memoize`, a left join and `FOR SHARE` of the relation.
- Parallel walks: when the table may be read in parallel, `LionBtreeScan` and the B-tree walks of
  `LionOrdered` are also offered as partial paths (DESIGN.md §40.9): `Parallel Custom Scan
  (LionBtreeScan)` below a `Gather` - under partial aggregates, and on either side of a parallel
  hash join or below an anti join - and `Parallel Custom Scan (LionOrdered)` below a `Gather
  Merge`. The processes share the B-tree's parallel scan, each taking whole leaf pages of it, and
  one Lion set: the first to need it builds it while the others wait (wait event
  `LionOrderedSetBuild` on PostgreSQL 17 and later, `Extension` on 16) and they read it from
  shared memory; a set larger than `work_mem` × `hash_mem_multiplier` for each process planned is
  built by each for itself. Each process tests visibility and rechecks as the serial walk does.
  The price is PostgreSQL's for a parallel index scan: the per-row work divided among the
  processes, the set, the B-tree's own cost and the heap pages not; the workers come from the
  B-tree's pages (`min_parallel_index_scan_size`), and the heap pages a walk in heap mode fetches
  (`min_parallel_table_scan_size`), or the table's `parallel_workers`, up to
  `max_parallel_workers_per_gather`. A parallel walk builds its set before it walks
  (`pg_lion.enable_lazy_set` does not apply) and never switches to fetch and sort. Under `EXPLAIN
  ANALYZE` its counters are summed over the processes, and `Lion Set: ..., shared` says they read
  one copy; a walk that is not parallel itself but ran only in a worker (under
  `debug_parallel_query`, or as the inner side of a join a worker ran) prints no counters. A
  walk of a Lion index's own column and an order by distance stay serial.
  `pg_lion.enable_shared_set` (on): off, each process builds the set for itself, for comparing the
  two.
- The FK-side join (DESIGN.md §27) reads its dimension through whatever plan is cheapest for the
  dimension's rows, so a walk of a B-tree holding the dimension's key (and any column grouped by)
  is its dimension side where it beats a bitmap scan of the dimension's heap, serially and in
  parallel: `Custom Scan (LionCount)` over `Custom Scan (LionOrdered)` or `(LionBtreeScan)`,
  `(index only)` (DESIGN.md §27, "The dimension from a B-tree").
- `pg_lion.enable_ordered_scan` also covers `LionOrdered` over a `lionvec` index's order by
  distance (`ORDER BY embedding <=> $1`), filtered by a Lion set (DESIGN.md §44.6); EXPLAIN shows
  the distance as `Order By`. Off, a filtered nearest-neighbour query fails under
  `pg_lion.vector_postfilter`'s default, `error`, or is sorted exactly under `exact`; only under
  `allow` is it the plain `lionvec` index scan with a `Filter`.
- `pg_lion.enable_lazy_set`: let `LionOrdered` evaluate its Lion set only at the ranges of 64 heap
  blocks its walk reaches, and build it for the whole table only once that has cost what the build
  would (DESIGN.md §30.4, "The set, lazily"). Off, the set is built before the walk starts.
- `pg_lion.enable_decoded_walk`: count a `GROUP BY` of several Lion-indexed columns by decoding,
  at each range of 64 heap blocks, which value of each column every row has (DESIGN.md §34): three
  or more columns, and two where that is cheaper than the nested loop over their entries. Off, a
  `GROUP BY` of three or more columns goes to the ordinary plan and one of two to the nested loop.
- `pg_lion.enable_count_cells`: count a `GROUP BY` of expressions over predicates on Lion-indexed
  columns by CELLS (DESIGN.md §10, "Counts by cells"): `SELECT CASE WHEN score >= 70 THEN 'high'
  ... END AS tier, count(*) FROM t WHERE ... GROUP BY 1`, the score made of `CASE`s over `c = 3`,
  `c IN (4, 5)`, `c IS NULL`, `f`, `tags && '{1,2}'`, `tsv @@ 'a | b'` and the like, with
  arithmetic, `round`, `GREATEST`, `LEAST`, `COALESCE` and nested `CASE`s around them. A predicate
  is an equality, `<>`, `IN`, `NOT IN` or `IS [NOT] DISTINCT FROM` with constants, or a null test,
  of a column with a scalar Lion index, a boolean column and its `IS [NOT] TRUE / FALSE / UNKNOWN`, an `AND`, `OR` or
  `NOT` of those on one column, or `@>`, `&&` or `@@` with a constant on a column with a multi-key
  Lion index (at most 10 such leaves and 63 keys a column, no tsquery `!`). The rows the `WHERE`'s
  Lion clauses select are counted by each predicate's truth - true, false or NULL, by SQL's rules -
  and the expressions computed once for each such cell with rows, so a group's value is the
  expression's own, of its type, and a group no row reaches has no row. A `WHERE` clause over such
  an expression (`count(*) ... WHERE g = 3 AND <the tier> = 'high'`) is tested once a cell the
  same way; a `HAVING` is applied to the groups above the node. The expressions must be immutable
  and read their columns only through predicates; `count(*)` and the `count(col)` forms the
  pushdown answers are counted. The `WHERE`, if there is one, needs a Lion clause that selects rows
  (an equality, `IN`, a range or a multi-key query, not only `<>` or `IS NOT NULL`), as the decoded
  walk of a `GROUP BY` does. A `GROUP BY` that holds a column by itself is the ordinary grouped
  count, never cells. The plan is
  declined when the cells - the product, over the predicates' columns, of the outcomes each column
  can have (each value named, any other value, NULL; for a multi-key column each set of its leaves
  a row can satisfy, or NULL) - exceed `pg_lion.count_cells_max` (134,217,728, at least 2). Runs
  in parallel as a parallel `GROUP BY` does, and beside a `NOT EXISTS` over a small inner side as
  `pg_lion.enable_count_anti` says. Off, such a `GROUP BY` goes to the ordinary plan.
  EXPLAIN shows `Group Strategy: Cells`, the columns of the predicates as `Cell Columns: <index>
  (<column>), ...`, the predicates themselves as `Cell Predicates`, and the bound as `Cells`; a
  `WHERE` clause over the predicates is the node's `Filter`; `EXPLAIN ANALYZE` adds the
  decoded walk's `Decoded Passes`, `Decoded Keys`, `Decoded Rows` and `Decoded Rows Rechecked`,
  `Partial Rows` - the cells with rows - and, when the tally outgrew `work_mem`, `Tally Spills`:

      Finalize HashAggregate
        Group Key: CASE WHEN ((k = ANY ('{1,2,3}'::integer[]))) THEN 'a'::text WHEN (((k = 4) OR (k = 5))) THEN 'b'::text WHEN ((k IS NULL)) THEN 'n'::text ELSE 'c'::text END
        ->  Custom Scan (LionCount)
              Lion Indexes: t_k (k), t_n (n < 50)
              Group Strategy: Cells
              Cell Columns: t_k (k)
              Cell Predicates: (k = ANY ('{1,2,3}'::integer[])), ((k = 4) OR (k = 5)), (k IS NULL)
              Cells: 4
- `pg_lion.enable_count_partial`: let the count pushdown read a partial Lion index whose predicate
  the query's `WHERE` implies - `CREATE INDEX ... USING lion (g, c) WHERE f AND s = 'a'` for a
  query with `WHERE f AND s = 'a' AND c = 3` - leaving out the clauses the predicate implies, as a
  partial index scan does (DESIGN.md §45). It is used only where every row the count can reach is
  one of the index's: a clause read from it that selects rows (`=`, `IN`, `IS NULL`, a range), or
  its entries walked for a `GROUP BY`, a `count(DISTINCT)` or the count of every row. EXPLAIN lists
  the index among the `Lion Indexes` (`Partial Indexes` on a partitioned table) and the clauses
  left out under `Implied by Index Predicate`. Off, the pushdown reads whole Lion indexes only.
- `pg_lion.enable_count_residual`: let the count pushdown take a `WHERE` clause no Lion index
  answers - `payload LIKE 'ab%'`, `id > 1000`, `length(s) = 64` - beside ones that do, testing it on
  the heap row of each row the others select (DESIGN.md §10, "Residual clauses"), over one table or
  each partition of a partitioned one (§16). Off, such a count goes to the ordinary plan.
- `pg_lion.enable_count_anti`: let the count pushdown answer `SELECT count(*) FROM t WHERE ... AND
  NOT EXISTS (SELECT 1 FROM s WHERE s.a = t.k ...)` over a small inner side as the count of the
  `WHERE` less the rows some inner row matches: the node runs the inner side as its child, looks
  each distinct key of its rows up in a B-tree on `t.k` under the query's snapshot, and subtracts
  the rows found that pass the join's clauses and `t`'s own (DESIGN.md §27, "The anti join over a
  small inner side"). Also `count(1)`, a `count(col)` the pushdown answers, `count(DISTINCT)` of a
  unique column, a `GROUP BY` of `t`'s columns the pushdown groups by, a group that comes to
  nothing left out, and a count by cells (`pg_lion.enable_count_cells`), each row found taken from
  its cell by its predicates' truths (DESIGN.md §27, "Counts by cells beside the anti join"); in
  parallel, the ungrouped count and the count by cells. It needs a B-tree whose first column is a key
  of the join; the cost model leaves a large inner side to the ordinary plan. EXPLAIN prints `Join
  Type: Anti`, `Join Rows: the rows counted, less those with a match`, `Join Lookup Index` and
  `Join Cond` above the inner side's plan, and under ANALYZE `Join Child Rows`, `Join Keys Looked
  Up`, `Join Rows Fetched` and `Join Rows Subtracted` (with `Join Child Time` and `Join Lookup Time`
  under TIMING), and `Join Spilled` where part of the run went past its memory: `keys`, `inner rows`
  and `ranges` where the inner side's keys, a key's inner rows or, in parallel, the rows taken away
  by range went to disk past `work_mem`; `losses` where what the groups or cells lose went past
  `hash_mem` (`work_mem` times `hash_mem_multiplier`) into a sort that the count's rows are then held
  back into, and `held rows` where that sort went to disk past `work_mem` (DESIGN.md §27, "The anti
  join within work_mem"). In a parallel plan each counter is every participant's, summed, and `Join
  Spilled` what went past its memory in any of them: beside a parallel count by cells, where every
  participant reads the inner side and looks each key up, `Join Child Rows`, `Join Keys Looked Up`,
  `Join Rows Fetched` and the times add up to a run per participant, while `Join Rows Subtracted` is
  what the serial plan subtracts, each row taken away by the one participant that counts its range.
  `LEFT JOIN ... IS NULL` is left alone, and so is `NOT IN` but on PostgreSQL 19 where neither side
  can be NULL (a NOT NULL column, or a filter that rules NULL out): 19 plans that one as the same
  anti join as `NOT EXISTS`, and it is counted as that is. Off, the pushdown does not count an anti
  join this way.
- `pg_lion.enable_probe_join`: group a join of a table to a dimension by the dimension's primary
  key with `LionProbeJoin` (DESIGN.md §46) - `SELECT CASE WHEN d.region IN ('north', 'east') THEN
  'ne' ELSE 'other' END, f.channel, count(*) FROM fact f JOIN dim d ON f.dim_id = d.id WHERE
  d.active AND d.tier < 4 GROUP BY 1, 2`, a `NOT EXISTS (SELECT 1 FROM s WHERE s.dim_id =
  f.dim_id)` over a small table beside it or not. The fact's rows come from PostgreSQL's own plan
  of the fact; each row's key is looked up in the dimension's unique B-tree, and the entry tested
  against Lion sets of the dimension built once - its `WHERE`'s, and one for each outcome of each
  column the `GROUP BY`'s predicates read - and against the `NOT EXISTS`'s set of the B-tree
  entries of its keys; the heap is read for pages the visibility map does not call all-visible,
  and for every row found when the `WHERE`'s set is inexact, to recheck the `WHERE` on the heap
  tuple (`Rows Removed by Lion Recheck`). Each row found goes up as the fact's columns and the
  predicates' truths, and PostgreSQL's aggregate computes the groups above it - in parallel as
  `Finalize HashAggregate`, `Gather` and `Partial HashAggregate` over the node over the fact's
  parallel scan, each process building the sets for itself. It needs an inner join of two tables on
  one equality whose dimension side has a unique one-column B-tree; every `WHERE` clause on the
  dimension answered by its Lion indexes; the dimension's columns read only inside predicates
  `pg_lion.enable_count_cells` takes (an equality, `IN`, a null test, a boolean, a multi-key query,
  with constants) - a dimension column in the output by itself, under a `THEN` or in an aggregate
  keeps it out; and `enable_indexonlyscan`. The fact needs no Lion index; a run of sets past
  `work_mem` × `hash_mem_multiplier` spills to a temporary file. The planner prices a descent of the
  B-tree for each fact row and the sets' builds against PostgreSQL's plan, the nested loop's cold
  heap reads included (DESIGN.md §39). EXPLAIN prints `Probe Index`, `Probe Cond`, `Lion Cond` and
  `Lion Indexes` for the `WHERE`, `Cell Predicates` and `Cell Indexes` for the labels, and `Anti
  Join Cond`; `EXPLAIN ANALYZE` adds `Probes`, `Null Keys`, `Rows Removed by Lion Cond`, `Rows
  Removed by Anti Join`, `Heap TIDs Skipped via VM`, `Heap TIDs Rechecked`, `Rows Removed by
  Visibility`, `Rows Removed by Lion Recheck` (a set built inexact, checked on the heap tuple),
  `Lion Sets: <sets>, <members> members in <containers> containers[, rechecked][, spilled N
  kB][, N builds]` and `Anti Join Keys: <rows> rows, <TIDs> TIDs` - the leader's alone in
  parallel:

      HashAggregate
        Group Key: CASE WHEN ((d.region = ANY ('{north,east}'::text[]))) THEN 'ne'::text ELSE 'other'::text END, f.channel
        ->  Custom Scan (LionProbeJoin) on dim d
              Probe Index: dim_pkey
              Probe Cond: (d.id = f.dim_id)
              Lion Cond: (d.active AND (d.tier < 4))
              Lion Indexes: dim_active, dim_tier
              Cell Predicates: (d.region = ANY ('{north,east}'::text[]))
              Cell Indexes: dim_region
              ->  Seq Scan on fact f

  Off, or with `pg_lion.enable_count_pushdown` off, such a join is left to PostgreSQL's join
  methods. `enable_hashjoin`, `enable_mergejoin` and `enable_nestloop` do not disable it.
- `pg_lion.enable_ordered_join`: return the first rows of a join of a table to a dimension by
  the dimension's key, in the order of a B-tree of the table, with `LionOrderedJoin` (DESIGN.md
  §47) - `SELECT f.id, f.score, d.name FROM fact f JOIN dim d ON f.dim_id = d.id WHERE f.tags @>
  '{a1}' AND d.region = 'north' ORDER BY f.score DESC LIMIT 20`. The node walks the B-tree in
  order over a Lion set of the fact's `WHERE`, as `LionOrdered` does (above: its lazy set, early
  stop and fetch-and-sort switch, `pg_lion.ordered_switch_ratio`); looks each row's key up in the
  dimension's unique B-tree; tests the entry against a Lion set of the dimension's `WHERE` before
  it reads the dimension's heap; and returns the two rows joined, in the walk's order. Either table
  may lack a Lion clause - its rows are then fetched and filtered - as long as the other has one or
  a key set filters. A table that only filters the join's key - `f.dim_id IN (SELECT ...)`,
  `EXISTS (SELECT 1 FROM s WHERE s.id = d.id ...)`, or an inner join on a unique column of it with
  none of its columns in the output - is read once, before the walk, into a hash set of its keys,
  and each fact row's key is tested against it before the lookup; its key must be of the join
  key's type with a hashable equality, and its expected keys must fit in `work_mem` (a set is not
  spilled). Where the fact's key column has a Lion index of its own (scalar, of the key's type,
  not partial unless the query implies its predicate), the key set's keys are also looked up in it
  and their rows ANDed into the walk's set, so that a row whose key the table lacks is never read
  (`pg_lion.enable_key_set_fold`, below; `Key Set Index: <index>` in `EXPLAIN`). Where the fact's
  ordering B-tree holds the key column, as a key or an `INCLUDE` column, each entry's key is tested
  on its index tuple before the row is read, and a row the table lacks is not read either (`Key
  Sets Tested On: <index>` in `EXPLAIN`). With neither, the walk reads each row to test its key,
  and the planner charges it the pages of those reads.
  It needs an inner join of two tables on one equality whose dimension side has a unique
  one-column B-tree and no other clause between them; heap tables or materialized views, not
  partitioned, without row-level security or `TABLESAMPLE`; a `SELECT` without `FOR UPDATE`/`FOR
  SHARE`; and a B-tree of the fact that gives the `ORDER BY` at least its first column. It is
  never parallel and never the inner side of a parameterized nested loop. The fact's walk is
  index-only where its B-tree holds every column of the fact the query reads (`Ordered By: <index>
  (index only)`); the dimension's row is read from its heap once its entry has passed the
  dimension's set, never from an index. The planner prices the walk, a B-tree descent per key and
  the dimension rows read, divided by `pg_lion.pushdown_margin`. Where PostgreSQL's cheapest plan
  under the `LIMIT` is a nested loop in the walk's order (over the fact's B-tree or over
  `LionOrdered`), that loop's Memoize is taken at the cache hits the `LIMIT` lets it meet
  (`pg_lion.enable_memoize_limit`); and where the tables do not fit in `effective_cache_size`, the
  pages each plan reads outside the cache are charged `pg_lion.cold_page_cost`, which takes the
  node's price down by at most `pg_lion.cold_max_discount`. EXPLAIN prints `Ordered By`, `Index
  Cond`, `Filter`, `Lion Cond` and `Lion Indexes` for the fact, `Dimension: <table> via <index>`
  with `Dimension Filter`, `Dimension Lion Cond` and `Dimension Lion Indexes`, and
  `Key Sets: <tables>`, each key set's table's plan below the node; `EXPLAIN ANALYZE` adds the
  fact's `Index Entries Walked`, `Lion Set Hits`, `Heap Fetches`, `Rows Removed by Lion Recheck`,
  `Rows Removed by Filter`, `Lion Set`, `Switched to Fetch and Sort` and `Scans Stopped Early`; `Key
  Set Keys`, `Key Set Builds` and `Keys Rejected by Key Sets`; and the dimension's `Dimension
  Descents`, `Dimension Lion Set Hits`, `Dimension Heap Fetches`, `Dimension Rows Removed by Lion
  Recheck`, `Dimension Rows Removed by Filter` and `Dimension Lion Set`:

      Limit
        ->  Custom Scan (LionOrderedJoin)
              Ordered By: fact_grp_score
              Index Cond: (f.grp = 2)
              Lion Cond: (f.tags @> '{a1}'::text[])
              Lion Indexes: fact_tags
              Dimension: dim d via dim_pkey
              Dimension Filter: d.flag
              Dimension Lion Cond: (d.lvl = 3)
              Dimension Lion Indexes: dim_lvl
              Key Sets: flt
              ->  Seq Scan on flt
                    Filter: (x < 3)

  Where the `ORDER BY` leads with a column of the dimension instead - `ORDER BY d.flag DESC,
  f.score DESC LIMIT 20` - and goes on with the order of a B-tree of the fact, `LionOrderedSplit`
  (DESIGN.md §48) runs the join an arm at a time: it walks the column's entries in its Lion index
  in the `ORDER BY`'s direction, its NULLs first or last as the `ORDER BY` puts them, and for each
  value runs one `LionOrderedJoin` with the dimension narrowed to that value's rows, the fact's
  B-tree walked from its start each time, until the `LIMIT` has its rows. The column must be a plain
  column of the dimension, a key column of a Lion index that is not multi-key, sorted in its type's
  default B-tree order under its own collation (not `USING` another operator, not `COLLATE` another
  collation); the planner offers the split only where it expects at most
  `pg_lion.ordered_split_max` values (below), and prices the values in order, from the column's
  statistics, so that the `LIMIT` is met in the right one. At run time it runs every value it meets
  in the index, whatever the statistics say, and passes over a value none of whose rows pass the
  dimension's Lion filter. While a value runs, the set of its rows that pass that filter - all of
  them without one - is held with the filter's own set within `work_mem` × `hash_mem_multiplier`,
  and past it in a temporary file. What follows the fact's order in the `ORDER BY` is sorted by an
  Incremental Sort above it. EXPLAIN prints `Split By: <index> (<column>[, backward][, nulls
  first|last])` and `Arms Max`, and the `LionOrderedJoin` beneath it once; `EXPLAIN ANALYZE` adds
  `Arms Run` and `Entries Skipped`, and `Arm Sets Spilled: <values>, <N> kB` where a value's set
  spilled - the values whose sets did, and the most of one in the file:

      Limit
        ->  Incremental Sort
              Sort Key: d.flag DESC, f.score DESC NULLS LAST, f.id
              Presorted Key: d.flag, f.score
              ->  Custom Scan (LionOrderedSplit)
                    Split By: dim_flag (flag, backward, nulls first)
                    Arms Max: 8
                    ->  Custom Scan (LionOrderedJoin)
                          Ordered By: fact_grp_score
                          Index Cond: (f.grp = 2)
                          Lion Cond: (f.tags @> '{a1}'::text[])
                          Lion Indexes: fact_tags
                          Dimension: dim d via dim_pkey
                          Dimension Lion Cond: (d.lvl < 30)
                          Dimension Lion Indexes: dim_lvl

  Off, such a join is left to PostgreSQL's join methods, with `LionOrdered` or a `LionBtreeScan`
  probe on either side where they apply; the split goes with it. `enable_hashjoin`,
  `enable_mergejoin` and `enable_nestloop` do not disable either.
- `pg_lion.enable_key_set_fold` (on by default): offer `LionOrderedJoin` and `LionOrderedSplit`
  with their key sets folded into the fact's walk where the fact's key has a Lion index (DESIGN.md
  §47.6): the walk's set is then the rows of the key sets' keys, and a row a key set rejects is
  never fetched; `EXPLAIN` prints `Key Set Index: <index>`. Each join is offered both ways and the
  planner keeps the cheaper: the fold looks every key up before the walk starts. Off, the key sets
  are only tested after each row is read.
- `pg_lion.enable_count_slices`: let the count pushdown answer `count(x)`, `sum(x)`, `avg(x)`,
  `min(x)` and `max(x)` of a column a Lion index slices (`slices = on`) from its bit slices
  (DESIGN.md §52): under any `WHERE` the pushdown takes - equalities, `IN`, `IS [NOT] NULL`, `<>`,
  `OR`s, multi-key operators, ranges on other columns, parameters and stable expressions - beside
  `count(*)` and filtered counts, and per group of a `GROUP BY` of one or two Lion-indexed columns
  (with a `HAVING` on them). `sum` and `avg` of `int2`, `int4` and `int8`, `min` and `max` of those
  and of `date`, `time`, `timestamp` and `timestamptz`, of the bare column; the results are core's
  to the bit (an `int8` sum in `numeric`, `sum(int4)` wrapping in `int8` as core's does, `avg` as
  core's `numeric`). EXPLAIN prints `Aggregates From Slices: <index> (<column>: every bit counted |
  existence counted[, min descent][, max descent])`, a column a time, and for a `GROUP BY` `Slice
  Groups: one at a time` or `decoded by batch` (`pg_lion.enable_slice_decode`, below); `EXPLAIN
  ANALYZE` adds `Slices Located`, `Slices Absent` (a bit no row has), `Slice Counts`, `Slice
  Containers Counted`, `Slice Containers by Kernel`, `Slice Kernel Keys` and `Slice Kernel` (the
  plane kernel's instruction set, below), `Slice Descent Steps`, `Slice Candidate Sets`, `Slice
  WHERE Copies`, `Slice Batches Decoded`, `Slice Keys Decoded`, `Slice Rows Decoded` and the heap
  rechecks of pages not all-visible. With no `WHERE` and no `GROUP BY` it competes with the
  aggregates over keys (`Aggregates Over Keys`, DESIGN.md §37) on price. Not taken: a join, a
  partitioned table, a residual clause, `DISTINCT`, `FILTER` or `ORDER BY` in the aggregate, an
  expression of the column, a filtered count beside a `GROUP BY`, three grouping columns, a float
  column, and a hot standby, where the pushdown collects nothing. Off, these aggregates go to the
  aggregates over keys or to the ordinary plan.
- `pg_lion.enable_slice_decode`: answer those aggregates of a `GROUP BY` of one column a batch of
  groups at a time (DESIGN.md §52, "The decode route"): one walk of the batch's groups and the
  `WHERE` reads each slice once and every row's value off them - 64 rows at a time by a transpose
  of the slices' words - where a group at a time counts every slice against a copy of each
  group's rows. The planner prices both and keeps the cheaper: a group at a time for a few
  groups, by batch from some ten (on 5M rows, 20 groups under a 10% `WHERE` in 41 ms against 220
  a group at a time, 200 in 41 ms against 1,734, and the ordinary plan's 181 to 186). Two grouping columns, and groups an `IN` list on
  the grouping column drives, are answered a group at a time. Off, every group is.
- `pg_lion.enable_slice_kernel`: count a key's slices that are bitmaps of one width, at a container
  key the visibility map vouches for, in one pass of the bit-plane kernel (`lion_planes_count_strided()`,
  DESIGN.md §50, §52). Off, each slice's container is ANDed with the `WHERE` and counted on its own;
  the answers are the same.
- `pg_lion.enable_topk`: for `GROUP BY g ORDER BY count(*) DESC LIMIT k`, count only the groups
  that can be among the first `k`: each key's entry records how many rows it holds, which bounds
  its count, so the largest of those are counted first and the rest are never read once they cannot
  catch up (DESIGN.md §36). Off, every group is counted.
- `pg_lion.enable_bm25_scan`: offer `LionBm25` for `ORDER BY lion_bm25_score(tsv, query, index
  [, ...]) DESC` with a `WHERE tsv @@ ...` it covers: the rows best first from the index's BM25
  walk (DESIGN.md §17, "Ranking"). Off, every match is scored and sorted.
- `pg_lion.enable_plain_scan`: let the planner use plain and index-only scans of Lion indexes
  (`amgettuple`, DESIGN.md §29). Off, Lion indexes are planned for bitmap scans only, as GIN
  indexes are, and every other index's scans are unaffected - where `enable_indexscan = off` would
  also take away B-tree index scans and every Lion path below. The count pushdown and `LionOrdered`
  still use Lion indexes.
- `pg_lion.enable_intersection_probe`: let Lion measure, at plan time, how many rows the AND of
  two or more equality, `IN` or multi-key clauses on one Lion index really selects, when the
  planner's product of their selectivities may be far off - correlated filters - and price its
  own paths (its index scans and the count pushdown) from that (DESIGN.md §29.11, "Correlated
  sets"). A bounded sample of the index, 1,024 buffer accesses a planner run and 64 more for each
  posting set of the widest filter it measures - an `IN` list's values each a set - up to 3,072
  in all: the containers of the clause with the fewest rows, chosen among its own - every one,
  where they are few, which makes the measure an exact count - so that a table loaded in batches
  is measured as well as one whose rows are mixed ("A sample of the driver's own rows"). A sample
  that meets none of the AND's rows lowers the estimate only as far as it can rule out, not to one
  row ("What a sample can rule out"); a filter of more than 32 sets is measured without its widest
  lists, which are taken as independent of the rest ("Wide lists"). Off, Lion prices its paths
  from the planner's estimate.
- `pg_lion.enable_rows_correction`: let Lion's estimate of a table's own filters correct the
  planner's row estimate for that table as well, so that joins above it are sized for the rows its
  filters really leave (DESIGN.md §29.11, "The relation's rows"): what the intersection probe
  measures of them, and the forms Lion estimates them in where the planner's estimate is off
  whatever the data - an OR of equalities on one column as the `IN` list it spells, and a run of
  exclusions on one column, `k <> 'a' AND k <> 'b' ...` or `k NOT IN ('a', 'b', ...)`, as the one
  exclusion it is: the rows that are not NULL and hold none of the values, where the planner takes
  the NULLs away once for each value and comes out hundreds of times short on a column with many
  NULLs ("A run of `<>` is one exclusion"). It applies to a table with a Lion index, with the probe
  on or off. Off, those correct Lion's own estimates only and the planner keeps its own row count.
- `pg_lion.enable_index_ndistinct`: give the planner a column's number of distinct values from a
  Lion index on it - the index's count of its keys, taken at build, by every VACUUM that deletes
  rows (in the walk it makes anyway) and by an ANALYZE when the index's directory is no larger than
  ANALYZE's own sample - in place of the number ANALYZE estimates from its sample, which a column
  with many rare values gets far too low (DESIGN.md §33). It applies to a column with statistics, a
  non-partial index with a scalar opclass on exactly that column (or an expression index on the
  expression), while the table holds within a factor of 2 of the rows the count was taken over, and
  never where the column's `n_distinct` has been set by hand. `pg_stats` still shows ANALYZE's
  number.
- `pg_lion.enable_index_mcv`: give the planner the exact frequency of each value a query compares a
  column with from a Lion index on it. The values are the constants of every `col = c`, `col IN
  (...)` and `col = ANY (array)` on the column (of an `ARRAY[...]`, its constant elements) in the
  table's `WHERE` conditions and in the join conditions that mention the table, whether such a
  comparison stands alone or inside `AND`s and `OR`s nested to any depth. Each gets the rows of
  its entry in the index, or none for a value the index does not have,
  added to ANALYZE's most common values (or put in place of the sampled frequency of one it has)
  for that query's plan (DESIGN.md §33, "The frequencies of the listed values"). Without it, the
  planner estimates a value ANALYZE did not list as an equal share of the rest of the rows, which
  on a skewed column takes a value of thousands of rows for a handful, the more so with the exact
  number of distinct values above. It applies to the columns and indexes that setting applies to,
  but not to an expression; to constants of the index's key type compared by its equality (a
  generic plan's parameter is estimated as before); to at most 1000 values of a column in one table
  of a query, past which, or where another comparison of the column for equality cannot be
  taken, the column is estimated as without it; and only for a user who may read every row of the
  table. Each value is looked up once per planning, one descent of the index's directory, and an
  `IN` list of 1000 values took about 5 ms to plan where it took 0.5 without. `pg_stats` still
  shows ANALYZE's list.
- `pg_lion.enable_skew_estimate` (on by default): price Lion's probes of a key the plan does not
  know (a nested loop's outer row, a correlated subquery's, a generic plan's parameter) at the rows
  such a key really meets on a skewed column, and price the core scan they compete with at the same
  rows (DESIGN.md §33, "The rows of a probed key", and §39, "A skewed key's probes"). The planner
  prices such a probe at the column's rows per key. But the keys a query probes are drawn as rows
  are, so on a column where a few keys have thousands of rows and most have a handful, a probe
  meets many times that. The same pass that counts a Lion index's keys sums the squares of their
  rows, and with ANALYZE's most common values that gives the rows of a probed key outside them.
  Lion's price is then put back into the planner's units, so row estimates and every other plan
  comparison are unchanged. The same conditions as `pg_lion.enable_index_ndistinct` apply. Off,
  every probe is priced at the planner's rows per key.
- `pg_lion.enable_memoize_limit` (on by default): price a nested loop that Lion's paths compete
  with at the cache hits its Memoize meets under the query's `LIMIT` (DESIGN.md §39, "A Memoize
  under a LIMIT"). The planner prices a Memoize at one hit ratio for every call of the loop's whole
  walk. A `LIMIT` that stops the loop after a few thousand calls meets few repeated keys, and the
  loop's price of those rows counts hits it does not get: 70% where it gets under 1% in DESIGN.md
  §47.9's measurement. Lion's price is put into the loop's units by the loop's price over its
  price with those hits taken out, so row estimates and every other plan comparison are unchanged.
  It applies where the query has a `LIMIT` and no grouping, aggregate or window, the loop's order
  gives the query at least its first sort key, and the `LIMIT` takes a share of the loop's rows.
  Off, the loop is taken at the planner's price.

PostgreSQL's own scan settings disable Lion's custom paths as they disable the index scans each
stands for: the path is kept but marked disabled (`disabled_nodes` on PostgreSQL 18, `disable_cost`
before), so that it is chosen only where nothing enabled is left, and `EXPLAIN` on 18 shows it
`Disabled: true` when it is. Lion's own switches above still take a path out altogether.

| PostgreSQL setting | the Lion paths it disables |
|---|---|
| `enable_indexscan = off` | every one: `LionCount` (with its FK-side joins and grouped forms), `LionSemiJoin`, `LionAntiJoin`, `LionProbeJoin`, `LionOrdered` (a B-tree's, a Lion column's and a `lionvec` index's walks), `LionOrderedJoin`, `LionOrderedSplit`, `LionBtreeScan` and `LionBm25`, as it disables index-only scans with plain ones |
| `enable_indexonlyscan = off` | the ones that answer from the index alone: `LionCount` and its forms, `LionSemiJoin`, `LionAntiJoin` and `LionProbeJoin`, which have no form that reads the rows to fall back to, as an index-only scan has its plain scan. It also takes away the index-only mode of `LionOrdered`, `LionBtreeScan` and `LionOrderedJoin`'s walk (above), and the plain walks stay enabled; a nested loop's `LionBtreeScan` probe, which is index-only or nothing, goes with it |
| `enable_bitmapscan`, `enable_seqscan`, joins and sorts | none |

`LionOrdered`, `LionOrderedJoin`, `LionOrderedSplit`, `LionBtreeScan` and `LionBm25` read the rows,
as an index scan does, and so answer to `enable_indexscan` alone. To have the planner prefer Lion's paths without
disabling PostgreSQL's, set `pg_lion.pushdown_margin` above 1 (below).

Testing knobs rather than tuning ones: `pg_lion.scan_window_floor` (4 MB), the least memory a plain
scan's window of container keys takes (DESIGN.md §29.3), `pg_lion.parallel_range_keys` (16), the
fewest container keys - of 64 heap blocks each - a range of a parallel count or `GROUP BY`
covers when it runs (DESIGN.md §10, "A GROUP BY in parallel" and "A count in parallel"; the
planner prices the default),
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
| `and_member_cost` | 0.8 | `cpu_operator_cost` | a member of an intersection ANDed with what a seek found, both arrays, or looked up in a container of a union's |
| `bitmap_word_cost` | 0.07 | `cpu_operator_cost` | a word of a bitmap container passed over to AND, unite, subtract or count it |
| `bitmap_member_cost` | 0.2 | `cpu_operator_cost` | a member of an array container looked up in a bitmap container, to AND or subtract the two |
| `union_key_cost` | 30 | `cpu_operator_cost` | the union of an `IN` list's or a multi-key query's containers built at a container key |
| `union_member_cost` | 0.25 | `cpu_operator_cost` | a member of such a union, or of the intersection ANDed with it |
| `descent_cost` | 120 | `cpu_operator_cost` | a level of an entry directory descended |
| `union_set_cost` | 100 | `cpu_tuple_cost` | a set of an `IN` list or `OR` rebuilt by each count of a GROUP BY |
| `recheck_tid_cost` | 1.5 | `cpu_tuple_cost` | a candidate row of a count's heap recheck |
| `recheck_group_tid_cost` | 6.0 | `cpu_tuple_cost` | the same in a grouped count |
| `position_member_cost` | 2.4 | `cpu_operator_cost` | a member of a lexeme's stored positions decoded by a position filter, seeking a candidate row (DESIGN.md §17, "Stored positions") |
| `position_check_cost` | 4.3 | `cpu_operator_cost` | a candidate row decided from stored positions, per lexeme of the query |
| `resident_page_cost` | 120 | `cpu_operator_cost` | a page of a Lion index a count reads while the index fits in `effective_cache_size` with the query's tables (DESIGN.md §39); a page that does not is priced as I/O |
| `cold_page_cost` | 50 | `seq_page_cost` | a page read from the device, not the cache, one synchronous read at a time: a nested loop's inner index scan (a `Memoize`'s misses where one sits over it; in a grouped query, the loop under a semi or anti join above it) competing with an FK-side join or a `LionProbeJoin`, and that join's own reads, in the share of the tables `effective_cache_size` cannot hold (DESIGN.md §39, "A nested loop's cold reads"); 0 turns the charge off |
| `cold_stream_page_cost` | 6.25 | `seq_page_cost` | a page read from the device, not the cache, asked for ahead of the reader: a bitmap heap scan's heap and index pages competing with a covering walk (`LionOrdered`, `LionBtreeScan` index-only), a count or an FK-side join, and those paths' own reads, in the share of the tables `effective_cache_size` cannot hold (DESIGN.md §39, "A bitmap heap scan's cold reads"); 0 turns the charge off |
| `probe_entry_cost` | 4.5 | `cpu_operator_cost` | an entry a nested loop's B-tree probe (`LionBtreeScan` parameterized by the outer row, and `LionProbeJoin`'s descents) meets, beyond core's Index Scan of the same B-tree: its test against the set, measured at that Index Scan's own rate (DESIGN.md §49, "The probe's own work") |
| `probe_rescan_cost` | 16 | `cpu_operator_cost` | a scan of `LionBtreeScan`'s probe beyond core's Index Scan's: its rescan, and its code run beside core's, measured so |
| `entry_count_cost` | 50 | `cpu_tuple_cost` | a count of a GROUP BY: an entry, or a pair of two |
| `list_group_cost` | 18 | `cpu_tuple_cost` | a count of a group an `IN` list drives |
| `distinct_test_cost` | 50 | `cpu_tuple_cost` | a test of a `count(DISTINCT)` walk |
| `range_entry_cost` | 40 | `cpu_tuple_cost` | an entry of a range walk counted on its own |
| `range_union_entry_cost` | 12 | `cpu_tuple_cost` | a small entry of a summed range, counted with its leaf |
| `probe_step_cost` | 10 | `cpu_operator_cost` | a container of a set a summed range probes |
| `range_fold_cost` | 420 | `cpu_operator_cost` | a fold into a container of a range's union, collected as a source, that is not a bitset |
| `spill_read_cost` | 27 | `cpu_operator_cost` | a container a count reads back from a set spilled to a temporary file: a range collected as a source, the FK-side join's copy of its fact filters |
| `slice_key_cost` | 120 | `cpu_operator_cost` | a container key of a `WHERE`'s copy at which a count of a sliced column's slices meets it, a batch of up to 64 slices (DESIGN.md §52) |
| `slice_container_cost` | 15 | `cpu_operator_cost` | a container of such a slice read and counted there |
| `slice_page_cost` | 200 | `cpu_operator_cost` | a posting page of such a slice copied and walked, beside its read |
| `slice_step_cost` | 1000 | `cpu_operator_cost` | a step of a `min` or `max` descent over the slices: the slice located and the count set up |
| `slice_step_key_cost` | 50 | `cpu_operator_cost` | a container key of the candidates such a step counts |
| `slice_step_row_cost` | 0.9 | `cpu_operator_cost` | a candidate row such a step counts, up to 1,024 a key |
| `slice_gather_cost` | 12 | `cpu_operator_cost` | a `GROUP BY` decoding a batch of groups (`pg_lion.enable_slice_decode`): a word of 64 rows' values gathered from 32 slices by a transpose |
| `slice_decode_bit_cost` | 0.3 | `cpu_operator_cost` | ... a bit of a row's value read on its own, where a word holds too few rows to transpose |
| `slice_decode_row_cost` | 1.0 | `cpu_operator_cost` | ... a row's value added to its group's aggregates |
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
millisecond when calibrating the cost settings above, force its plans that way - `enable_seqscan`
and `enable_bitmapscan` off, and `pg_lion.enable_plain_scan`, which leave Lion's paths enabled, on
a table with no other index - or set every rate below to 1. `bench/calib/matrix.py` measures each
kind of plan's units a millisecond, and the planner's mispicks, on synthetic tables (DESIGN.md §39,
"The matrix").

| `pg_lion.` | default | the PostgreSQL plans it is the rate of, as a multiple of 500 units a millisecond |
|---|---|---|
| `hashagg_rate` | 0.1 | an aggregate that hashes: its own cost, less its input's; the rest of its plan is priced at 1 |
| `agg_rate` | 1.0 | a plain or sorted aggregate over a scan |
| `hashjoin_rate` | 0.5 | a hash join |
| `mergejoin_rate` | 1.0 | a merge join |
| `bitmap_rate` | 1.0 | a bitmap heap scan |

`pg_lion.pushdown_margin` (1, no margin; 0.01 to 1000): the share of the cheapest competing plan's
cost a Lion custom path (`LionCount`, `LionSemiJoin`, `LionAntiJoin`, `LionOrdered`,
`LionBtreeScan`) must be priced at to be chosen. Its own price is divided by it: set below 1, a near
tie goes to PostgreSQL's plan, and its cost in `EXPLAIN` is marked up by it; set above 1, a Lion
path is chosen up to that many times dearer than PostgreSQL's plan, and at 1000 wherever it is
offered, which forces Lion's paths without disabling PostgreSQL's (above), and marks its cost in
`EXPLAIN` down. It tips `LionBtreeScan` against
the sequential scan the same way: the covering walk's marked-up price must beat the sequential
scan's for the walk to be kept. It is not applied where it hedges nothing: to a plan
forced with nothing of PostgreSQL's left enabled, to a count whose cheapest competitor is the
access method's own scan of a Lion index (both prices Lion's), and to a count whose multi-key query
is a generic plan's parameter (priced at its dearest already). A plain count of one table is
always priced below an aggregate over the access method's own plain or bitmap scan that reads
every set the count reads - every condition of the `WHERE` an index condition of the index the
count reads it from - since that scan does the count's work and fetches every row besides: past
the planner's 1% tie, whatever either is estimated at (DESIGN.md §39, "The AM's own scan of the
same sets"). A path disabled by PostgreSQL's `enable_*` settings (above) loses to an enabled one,
and Lion's switches leave theirs out, whatever the margin or price.

`pg_lion.competitor_pricing` (on): whether a Lion custom path's price is corrected by what Lion
reads of the PostgreSQL plan it competes with - the rates above, that plan's uncached reads
(`cold_page_cost`, `cold_stream_page_cost`, `cold_max_discount`), the rows a skewed key meets
(`pg_lion.enable_skew_estimate`) and a `Memoize`'s hits under a `LIMIT`
(`pg_lion.enable_memoize_limit`). Off, a Lion path is priced by its own work alone, in
PostgreSQL's units, and PostgreSQL's plans by PostgreSQL (DESIGN.md §49); the margin still
applies.

`pg_lion.cold_max_discount` (2; 1 to 10): the most the uncached reads of a competing plan may
divide a Lion custom path's price by, where the query's tables do not fit in
`effective_cache_size` - a nested loop's against an FK-side join or a `LionProbeJoin`
(`cold_page_cost`), a bitmap heap scan's against a covering walk, a count or an FK-side join
(`cold_stream_page_cost`), each with the Lion path's own uncached reads added. 1 takes nothing
off. It bounds what a misjudged cache costs: where the tables are cached after all, a Lion path
chosen for those reads can take up to this many times its competitor's time (DESIGN.md §39, "A
bitmap heap scan's cold reads").

`pg_lion.vector_epsilon` (4, 1 to 10): how wide a `lionvec` scan's error bounds are, in
standard deviations (DESIGN.md §44.8). A row of the partitions a scan reads is missed only when its
bound fails and it belongs in the answer; at 4, no bound failed in 900 benchmark queries over
975,000 OpenAI embeddings, where 3 lost a row in one query of 100 under a 0.1% filter
(DESIGN.md §44.9). A wider bound refines and fetches more rows.

`pg_lion.vector_margin` (3, -10 to 1000): how much nearer than its centroid a `lionvec` scan
takes a partition's rows to come, in standard deviations of the rows about their centroids along
one dimension (DESIGN.md §44.4). A scan reads partitions until the next one's centroid, less this,
lies farther than the rows it has: larger reads more partitions and misses fewer of the nearest
rows, smaller the reverse (on 975,000 OpenAI embeddings, 2, 3 and 4 returned 96%, 98% and 98.3% of
the true ten nearest in 6.6, 7.9 and 9.1 ms, from an index built before `spill`, which finds more
at each margin). The recall a margin gives depends on the embeddings: on 115,186 of DBpedia's
text-embedding-3-large ones, 3 returned 84% and 95% took about 7, 98% about 11 (DESIGN.md §44.9),
so measure it on your own data. A filter through lion uses the same margin.

`pg_lion.vector_patience` (30, 0 to 1,000,000): how many units of `sqrt(n) / 2` rows - n the rows
the index was built with, so the rows an average partition would hold in `2 sqrt(n)` partitions,
however many it has; under a lion filter that share of the matching rows - a `lionvec` scan scores
without finding one nearer than the rows it is after before it pauses: the nearest 10, 20, 40 and
so on up to `pg_lion.vector_batch`, the least past the rows it has returned, and under a lion
filter none fewer than the query's `LIMIT` (and `OFFSET`) when the planner knows it. Paused, it
stops reading partitions until it has returned those rows, and resumes if the query wants more
(DESIGN.md §44.4).
A query whose nearest rows lie far from every centroid would otherwise read many partitions for
nothing; on 975,000 OpenAI embeddings the pause cut the partitions a query read from 254 to 77 on
average and the 95th percentile latency from 68 to 16 ms, for half a point of recall (98.4% of the
true ten nearest to 97.9%), and under a 1% filter from 1,511 partitions to 244 and 109 ms to 24, for
two points (100% to 98.1%). Smaller pauses sooner and misses more; 0 never pauses.

`pg_lion.vector_batch` (100, 1 to 100,000): how many rows a `lionvec` scan gathers candidates for
from the partitions it reads; under a lion filter, the query's `LIMIT` (and `OFFSET`) instead when
the planner knows it and it is more, up to an eighth of the candidates `work_mem` holds. A query
that wants more (a `LIMIT` past that, one the planner does not know, or none) reads those
partitions again, gathering four times as many each time. `work_mem` bounds how many candidates a
scan keeps (DESIGN.md §44.4).

`pg_lion.vector_exact_rows` (2,000, 0 to 100,000,000): how many rows a lion filter may admit for a
`lionvec` scan under it to compute every matching row's distance from the table, exactly, instead
of reading partitions, whatever the index's partitions; 0 leaves it to
`pg_lion.vector_exact_per_partition`. The exact path reads each matching row's vector (6 KB at
1536 dimensions, 3 KB as a `halfvec`, so 2,000 rows are 12 MB or 6 MB, a page each when the
vectors are in TOAST or alone on their heap page) where a probe reads a few hundred to a thousand
8 KB code pages, but under a filter that leaves most partitions empty it probes many for nothing.
Raise it for exact answers under wider filters, at a table read a row; the planner prices both
paths and may sort the filter's rows itself above it, which is exact too (DESIGN.md §44.6).

`pg_lion.vector_exact_per_partition` (1.5, 0 to 1,000,000): the same, counted in rows for each of
the index's partitions: a filter of no more than this many rows a partition, on average, is
answered exactly too, however many rows that is - 12,000 at a million rows in the 8,000 partitions
the build makes, 4,072 at 115,000 in 2,715. Where a filter's rows are this thin, each partition
read finds about one of them, and reading them all from the table is faster, warm, than reading
partitions for 0.98 of the ten nearest; from about three rows a partition reading partitions is
faster (DESIGN.md §44.9). Cold, the exact path is a random read a row, and the planner prices it
as it prices a sort of the filter's rows, which reads the same rows: where the exact path would take
a filter, `LionOrdered` may lose to that sort, or under `pg_lion.vector_postfilter = allow` to the
plain index scan with a `Filter` (DESIGN.md §44.7, "Exact path pricing"). 0 leaves the choice to
`pg_lion.vector_exact_rows`; the larger of the two counts.

`pg_lion.vector_overflow` (`error`, or `stop`): what a `lionvec` scan does when what it knows of
its rows outgrows `hash_mem` (`work_mem` times `hash_mem_multiplier`), which only a scan with no
`LIMIT`, or a large one, over rows its codes cannot tell apart reaches. `error` fails the query;
`stop` ends the scan with a NOTICE, the rows it returned being in order, each once, and the
nearest it found (DESIGN.md §44.4).

`pg_lion.vector_postfilter` (`error`, `exact` or `allow`): whether a plan may apply a `WHERE` clause
to the rows a `lionvec` scan returns, after the scan (DESIGN.md §44.6, "Post-filtering"). Such a
scan stops and skips rows by the rows it returns, most of which the filter then rejects, so it finds
other rows than `LionOrdered`'s scan of the matching rows alone at the same `pg_lion.vector_margin`
and `pg_lion.vector_patience`: the plan would decide the answer. `error`, the default: no plan does;
a filter lion indexes answer goes through `LionOrdered`, and a query whose `lionvec` order would be
filtered and that `LionOrdered` cannot answer fails when it is planned, `EXPLAIN` included, naming
the clauses no lion index answers and suggesting the subquery form above, or this setting. `exact`:
the same, but such a query is planned exactly, as a sort of the
filter's rows, however many rows that reads (`WHERE id = 5 ORDER BY embedding <=> $1 LIMIT 1` reads
the one row by the primary key and sorts it). `allow`: the plain scan with a `Filter`, and
`LionOrdered` with one, are plans like any other, chosen by cost.
`pg_lion.enable_ordered_scan = off` does not re-admit the plain scan: under `error` the query then
fails, and under `exact` it is sorted. A clause the planner tests once (`WHERE $1`,
`WHERE current_user = 'app'`), and one a partial `lionvec` index's predicate implies, is no filter
of the scan's rows, and one of the vector column alone (`embedding IS NOT NULL`, a distance bound)
filters them under every value; any other is a filter, a key's `id = 5` too. A table with
row-level security `lionvec` refuses under every value (below).

`pg_lion.vector_build_sample` (`error`, or `allow`): what a `lionvec` build does when half of
`maintenance_work_mem` holds too small a sample of the rows for the partitions it wants - fewer
than a sample of every row would give (above, and DESIGN.md §44.8). `error`, the default, fails the
build in its first pass, as soon as the rows it has read want more partitions than the sample has
rows for - before k-means and the second pass, and for a parallel build before its workers start -
naming the partitions wanted, those the sample has rows for, and the `maintenance_work_mem` that
would hold enough, rounded up to a megabyte. `allow` builds the partitions the sample has rows for,
with a NOTICE that names the same. The partitions decide which rows a scan reads, so its answers,
which is why a memory setting does not change them unseen; a `REINDEX` reads the setting again.

`pg_lion.vector_parallel_scan` (`off`): whether the planner may scan a `lionvec` index in parallel
(above, and DESIGN.md §44.4, "A parallel scan"). Off, a `lionvec` order gets no partial path at all,
whatever core's parallel settings say: on every query measured the parallel scan was slower than
the serial one.

`pg_lion.vector_prefetch` (`off`, `fadvise` or `stream`; `stream` from PostgreSQL 17, `fadvise` on
16): how a `lionvec` scan reads ahead the pages it will read next (DESIGN.md §44.4, "Prefetch" and
"Read streams"): the rest of a partition's code pages, laid one after another by the build, the
partitions it expects to probe next, the refine pages and table rows at the front of its queues, and
an exact answer's matching rows, as many ahead as the tablespace's `effective_io_concurrency`, up to
64 (0 reads none ahead). `stream` reads them into shared buffers through PostgreSQL's read streams,
several pages a read, on 18 as `io_method` says; `fadvise` asks the kernel for them
(`PrefetchBuffer()`), the only way on 16, where `stream` means `fadvise`; `off` reads each when it
comes. The table's rows are read ahead only when its access method is heap's and its vectors lie in
its pages (`STORAGE PLAIN` or `MAIN`, or at most 506 dimensions, 1,012 as `halfvec`, or within the
table's `toast_tuple_target` where that is larger) - an exact answer's through a stream under
`stream`, the rows whose exact distances a scan computes by asking the kernel under either. A vector
in TOAST is read ahead only where its index knows its place (`pg_lion.vector_toast_direct`, below):
an exact answer's chunks through a stream under `stream`, a reranked row's first chunk page by
asking the kernel under either. The rows returned, and their order, are the same under all three.
`on` and `off`, from when it was a boolean, still work, `on` being the default. On 115,186
embeddings (DESIGN.md §44.9), each query alone with its pages on the disk, `stream` took a scan at a
margin of 8 from 48 ms asking the kernel to 43 (67 reading as it went), a 1% filter's from 83 to 62,
and an exact answer's from 59 to 53. With the index's pages in the operating system's cache and not
in shared buffers, though, `stream` costs up to 40% more than `fadvise` under 18's `io_method`
`worker` or `io_uring`, which hand each read to another process or thread; `fadvise` suits an index
that lives there. With everything in shared buffers `stream` costs 2 to 11% and `fadvise` under 5%:
after 32 reads in a row found their page there, a scan stops reading ahead for the next 256, then
starts again. Each scan's `DEBUG1` line names what its streams read (`N pages streamed (M read in),
A read apart, W streamed unread, R restarts`), or the pages it asked for and how many were not in
shared buffers.

`pg_lion.vector_toast_direct` (boolean, `on`): whether a `lionvec` scan reads a vector stored in
TOAST from its chunks' pages, by the places its index keeps (above, and DESIGN.md §44.13), rather
than through the row's table page and the TOAST index. An exact answer (`pg_lion.vector_exact_rows`)
then reads its matching rows' vectors through one read stream over their chunks' pages, and a
probing scan reads each row it computes the exact distance of from its table page and its chunks'
page, asked for together, with no TOAST index descent between. The rows returned, their order and
their distances are the same either way: each chunk read by place is checked to be the one its
value's id and sequence call for, and falls back to the table when it is not; a row read so is
returned only once its table row, fetched under the query's snapshot, stores the same value, and an
index that names another value for a live row is an `ERRCODE_INDEX_CORRUPTED` error, never an
answer. A scan whose reads by place mostly fail - its TOAST table rewritten on its own (`VACUUM FULL
pg_toast.pg_toast_N`) since the index was built - stops trying after 64; a `REINDEX` makes every
place right again. Only an index that keeps places is read so, over a heap table, under an MVCC
snapshot, and on 16 not under `old_snapshot_threshold`. Inserts keep the places up whatever the
setting. Each scan's `DEBUG1` line adds `N read by place (L locator pages, F not), C checked` - the
vectors read by place, the locator pages read for them, the reads that fell back, the rows checked
against the table - and the TOAST pages it asked the kernel for ahead. Off, every vector is read
through its table row and the TOAST index, as before; it is there to compare.

`pg_lion.ordered_switch_ratio` (32, 1 to 1,000,000): how many B-tree entries `LionOrdered` walks per
member it has not met yet before it stops walking, fetches the members left in TID order and sorts
them (DESIGN.md §30.4, "When the walk is not paying"; `Switched to Fetch and Sort` in `EXPLAIN
ANALYZE`). The sort is a Sort node's: past `work_mem` it goes to disk, and under a constant `LIMIT`
that nothing between the node and it can drop a row from, it keeps only the rows the `LIMIT` takes
(`Fetch and Sort Method`, `Fetch and Sort Space Used` and `Fetch and Sort Space Type` in `EXPLAIN
ANALYZE`), so whether and where a walk switches does not depend on `work_mem`. In index-only mode
the switch is also held back while the walk meets members at the rate a uniform spread predicts,
since there it would read heap pages the walk never touches (DESIGN.md §40.4). A large value keeps
a walk from switching at all.

`pg_lion.ordered_split_max` (8, 0 to 1024): the most values of a dimension's column - its NULLs one
more where it has them - that the planner offers a `LionOrderedSplit` for (DESIGN.md §48), from the
column's number of distinct values; a column whose count is a default is not split. 0 offers none.
It bounds the plan only: at run time the split runs every value it meets in the index, however
many, and `EXPLAIN` prints the value it was planned under as `Arms Max`.

## Monitoring

Lion's custom scans and SQL functions keep PostgreSQL's index usage statistics, `pg_stat_user_indexes`
and `pg_stat_user_tables`, as its own index scans keep them (DESIGN.md §29.7, "Statistics"):

- `idx_scan` and `last_idx_scan`: a scan of each index a node or function reads, each time it reads
  it, a rescan with another parameter included: `LionCount` in all its forms, `LionSemiJoin`,
  `LionAntiJoin`, `LionOrdered`, `LionBtreeScan`, `LionBm25`, `lion_index_count()`,
  `lion_index_count_any()`, `lion_index_count_group_stats()`, `lion_index_facets()`,
  `lion_index_slice_agg()`, `lion_bm25()`,
  and `lion_bm25_score()`, which reads the index's statistics once a query. The B-tree that
  `LionOrdered` or `LionBtreeScan` walks is counted by PostgreSQL, as its own scans are. A
  partitioned table's partitions count their own indexes.
- `idx_tup_read`: the rows an index gave. A count credits each index it read with the rows it
  counted (a `count(DISTINCT)` with the distinct values, `lion_index_facets()` with the rows of the
  keys it counted), a semi or anti join with the rows it returns; `LionOrdered` and `LionBtreeScan` credit the
  members of the set they built from each Lion index and the entries a walk of a Lion column read,
  `LionBm25` and `lion_bm25()` the entries of the query's words they took.
- `idx_tup_fetch`: the table rows an index scan fetched itself - `LionOrdered`'s walk of a Lion
  column, the rows `LionBm25` and `lion_bm25()` return - on the index and the table; and, on the
  table only, as a bitmap heap scan's are, the rows a count rechecked on pages not all-visible or
  for a residual clause, and those `LionOrdered` fetched by its set's TIDs.
- In `pg_stat_user_tables`, a table's `idx_scan` is the sum of its indexes', and its
  `idx_tup_fetch` adds its own fetches to theirs; no Lion node counts a `seq_scan`.

## Known limitations

Equality, `IN` lists, scalar ranges and the multi-key operators above are supported, through bitmap
scans and plain index scans (`amgettuple`, DESIGN.md §29). `col IS NULL` and `col IS NOT NULL` are
answered exactly beside any of them, in these scans and in the sets of `LionOrdered` and
`LionBtreeScan`: `IS NOT NULL` takes the column's NULL rows out of what the other conditions select
(an empty array, tsvector or jsonb is not NULL), and leaves nothing to the heap recheck (DESIGN.md
§14). There are no ordered scans of the
access method itself (an `ORDER BY` is `LionOrdered`'s: a B-tree walked with a lion filter, or a
lion index's own ordered column walked, §30), no index-only scans of a lion index
that return a column (only those that need none, like `count(*)`; the rows of a Lion-filtered
query come from a covering B-tree instead, §40), no INCLUDE columns, no
parallel scan (a parallel build is supported from PostgreSQL 17). VACUUM reclaims the pages it
empties, as a B-tree does: directory leaves, and the leaves of a key's posting tree while the key
still has rows; the internal pages of a posting tree go with its key. Inserts serialise on the directory
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
`GROUP BY` or a sum-over-all-entries count (its entries are keys, not row values; `USING lion (doc)`
on `jsonb` is one, and `doc jsonb_ops` the index for `=` and `count(DISTINCT doc)` on whole
documents), and the cost model inherits the
stale `relallvisible` blind spot of index-only scans. A partial Lion index is read by the count
pushdown only when the query's `WHERE` implies its predicate, one per table or partition in a plan,
and never for a uniqueness proof, the facet counts or the planner's estimates (DESIGN.md §45).
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
Bit slices (`slices = on`, DESIGN.md §51) are kept for `int2`, `int4`, `int8`, `date`, `time`,
`timestamp` and `timestamptz` key columns only - not `oid`, whose order is unsigned, nor floats,
`numeric` or `interval` - for every such column of the index or none (the option is the index's,
not a column's). The count pushdown answers `count`, `sum`, `avg`, `min` and `max` of the column
from them (DESIGN.md §52), but not a range on the sliced column itself, a percentile, a join's or
a partitioned table's aggregates. An insert into a sliced column costs a posting-set insert per set
bit of its value.
On PostgreSQL 16 a server with `old_snapshot_threshold` set (it is -1, off, by default; 17 removed
the setting) does not read lion or `lionvec` indexes at all: neither detects "snapshot too old", so
the planner prices them out and declines the count pushdown and `LionOrdered` - a filtered
nearest-neighbour query is then planned as a sort, under every `pg_lion.vector_postfilter` - and a
scan or SQL count that reaches one anyway fails with an error rather than return a different answer
than the snapshot's (DESIGN.md §9).  Inserts and VACUUM work as usual.

Under SERIALIZABLE a lion index takes page-level predicate locks, as GIN does (DESIGN.md §9,
"SERIALIZABLE"): a read of a key locks the root of its posting tree when its rows have one, and
otherwise the directory leaf that holds the key, or would hold it; a range, a `GROUP BY` or a
walk of a column locks every leaf it reads and those roots of the keys on it. An
insert checks the page that stands for its key. So SERIALIZABLE transactions that read some keys
and insert others do not fail each other: only keys without a posting tree of their own (a set
still under `inline_limit`) that share a directory leaf conflict, as keys on one B-tree page do. A
lookup that has to search beyond one leaf for an absent key (its hash collides with keys on the
next) locks the whole index. The aggregates over keys (DESIGN.md §37) read every row, so they lock
the table, as a sequential scan does. Rows the heap visits are locked as for any index scan; a read
of more rows than `max_pred_locks_per_relation` allows locks the whole table instead, as it would
through a B-tree. A `lionvec` index locks the whole index: any insert into it conflicts with every
SERIALIZABLE read of it.

A `lionvec` index (DESIGN.md §44) answers only an `ORDER BY` of its own distance operator, forward.
It has no search operators, no bitmap or index-only scans and no INCLUDE columns, scans in
parallel only without a filter (above), and indexes a single column of pgvector's `vector` or
`halfvec` (not `sparsevec` or `bit`) of at most 8128 dimensions. Its results are approximate (`pg_lion.vector_margin`), except under a
lion filter of up to `pg_lion.vector_exact_rows` rows, or `pg_lion.vector_exact_per_partition` a
partition. A filter no lion index answers fails by default, and under
`pg_lion.vector_postfilter = exact` is a sort of the filter's rows: exact, and as slow as reading
every matching row. It does not support row-level security: no `lionvec` index is built on a table
that has it enabled, or on a partition of one, and a query such an index could order fails when it
is planned, whoever runs it, until row-level security is disabled. Inserts
serialize on the index's meta page and read every partition's centroid, or with 64 partitions or
more the centroids of the partitions' regions and of the partitions in the regions nearest the row
(16 of them, 5 with `spill = off`). VACUUM removes dead rows' codes but reuses their space only for new rows on
a chain's last page, so a table with heavy churn grows its index until `REINDEX`, which also
recomputes the partitions and the mean the codes are centred on (rows that have drifted from them
make scans read more or miss more, never answer out of order). Building a large index runs k-means
over the sampled rows, and then finds every row's partition; both compare a row with the centroids
of the partitions' regions, about the square root of the partitions' number, and then with those
of the partitions in the regions nearest it (5 in k-means, 16 for a row and its spill copy), so
the build grows with the rows times the square root of the partitions (DESIGN.md §44.3). Both
steps are shared among `max_parallel_maintenance_workers` workers and the build's own process,
from PostgreSQL 16, for a table of 10,000 rows or more or one whose `parallel_workers` is set
(DESIGN.md §44.12): 115,386 rows of 1536 dimensions in 2,717 partitions built in 108 s serially
and 36 s with three workers on four cores, where comparing each row with every centroid took
292 s and 85 s. A parallel build holds its sample, up to half of `maintenance_work_mem`,
and the centroids in dynamic shared memory, which Docker limits to 64 MB unless given
`--shm-size`. A build whose `maintenance_work_mem` holds too small a sample for its partitions
fails (`pg_lion.vector_build_sample`, above), `pg_restore`'s and `reindexdb`'s too, which run at
the server's setting unless told otherwise (`PGOPTIONS='-c maintenance_work_mem=12GB'`).
Each returned row is fetched from the heap twice, once for its distance and once by the executor.
A small `work_mem` with a large `LIMIT` makes a scan read its partitions several times (`SET
client_min_messages = debug1` shows the passes). What a scan remembers of the rows it has read is
held to `hash_mem` (`work_mem` times `hash_mem_multiplier`), as a hash join's table is: past it,
the scan forgets first the rows it can tell from their codes alone, then rows it can work out
again. A scan with no `LIMIT` (or a large one) over rows whose codes cannot tell them apart from
those it returned may still outgrow it; it then fails the query (`ERROR: lionvec scan of
"items_embedding_idx" outgrew hash_mem after 52000 rows`), or, with `pg_lion.vector_overflow` set
to `stop`, returns no more rows, with a NOTICE; what it returned is then in order, each row once.
Raise `work_mem` or `hash_mem_multiplier`, add a `LIMIT`, or let such a query sort (`SET
enable_indexscan = off`), which spills to disk.
pgvector prices its distance functions as nearly free (`COST 1`) and its vectors are stored out of
line, so under a filter that keeps a large share of the rows the planner may sort them by distance
where the `lionvec` scan is faster: on a million rows it did, for filters of 1% and 10%, 3 to 17
times slower, until scans paused (`pg_lion.vector_patience`). `lionvec` assumes the three
functions its operator classes use cost 100 (`ALTER FUNCTION l2_distance(vector, vector) COST
100`, and the same for `vector_negative_inner_product(vector, vector)` and `cosine_distance(vector,
vector)`, and for a `halfvec` column `l2_distance(halfvec, halfvec)`,
`halfvec_negative_inner_product(halfvec, halfvec)` and `cosine_distance(halfvec, halfvec)`), which
`pg_dump` does not keep and an `ALTER EXTENSION vector UPDATE` may reset; `SET
enable_sort = off` steers a single query the same way. An HNSW index on the same column is chosen
over `lionvec` for nearly every query, filtered or not, since pgvector prices its scan as nearly
free.

With Citus, list `citus` before `pg_lion` in `shared_preload_libraries`: Citus installs
`set_rel_pathlist_hook` and `set_join_pathlist_hook` without chaining the previous hook, so loaded
after lion it drops `LionOrdered`, `LionBtreeScan`, `LionBm25`, `LionSemiJoin`, `LionAntiJoin` and
lion's plain-scan repricing (the count pushdown, on `create_upper_paths_hook`, survives, and answers
are right either way).  The SQL functions that take an index (`lion_index_count()`,
`lion_index_facets()`, `lion_bm25()`, `lion_bm25_score()`) read the one index they are given: on
the coordinator, and on every worker inside a distributed query, that is the empty shell of the
distributed table's index, so `lion_index_count()` returns 0, `lion_index_facets()` no rows and
`lion_bm25_score()` 0 for every row; run them on the shards through `run_command_on_shards()`.
Citus rewrites `avg()` into `sum()` and `count(col)` per shard, a pair the aggregates over keys do
not answer (DESIGN.md §37).  A shard a move or rebalance has copied has no visibility-map bits
until it is vacuumed, and counts recheck every row there until then.  With TimescaleDB, a
hypertable is planned as an inheritance parent, which the count pushdown declines: a count over
the hypertable uses lion scans per chunk under TimescaleDB's chunk-wise aggregation, and a count
over one chunk by name uses the pushdown.  A compressed chunk keeps its lion indexes, and lion
plans nothing on it; a chunk in a table access method other than heap makes `CREATE INDEX ...
USING lion` fail for the whole hypertable (DESIGN.md §2) rather than skip it, and a build that
fails under `timescaledb.transaction_per_chunk` leaves an invalid parent index to drop.  A table
queried without its inheritance children - `FROM ONLY parent`, or a hypertable TimescaleDB plans
that way for a `LIMIT` query - gets no lion custom scan of its own, only core's scans of its
indexes.
