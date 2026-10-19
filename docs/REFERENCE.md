# pg_lion reference

SQL functions, key types, multi-key operators, index options, settings and known limitations. Section numbers (§) refer to [DESIGN.md](../DESIGN.md).

## SQL functions

Ordinary SQL is the interface - the planner uses the index and the `LionCount` pushdown on its own -
and these functions are for testing, diagnostics and the occasional direct count:

    lion_bm25(idx, query, k [, k1, b])              the k best rows by BM25, best first (an index with positions)
    lion_bm25_score(tsv, query, idx [, k1, b])      one row's BM25 score; ORDER BY it for the LionBm25 scan
    lion_index_count(idx, key)                      count(*) WHERE col = key, through one index
    lion_index_count(idx1, key1, idx2, key2)        ... AND col2 = key2, two indexes on one table
    lion_index_count_any(idx, keys)                 count(*) WHERE col = ANY (keys)
    lion_index_count_stats(idx, key)                lion_index_count() plus the heap it visited
    lion_index_count_group_stats(idx [, use_cache, attno])   every group's count, as GROUP BY does
    lion_index_facets(idx [, k, filter, attno])     the k keys with the most rows under a filter (below)
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
  long to store, which was hashed.
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
  a non-partial index only). `SET client_min_messages = debug1` shows the entries read, the keys
  counted and the rows read.
- A partial index counts the rows it holds. An expression index's keys are the expression's.


## Key types

One default operator class per type, reusing the hash access method's hash functions: integers
(`int2/int4/int8`, one family with cross-type equality), `float4/float8`, `numeric`, `bool`, `"char"`,
`name`, `text` (and `varchar` through it), `char(n)`, `bytea`, `uuid`, `date`, `time`, `timetz`,
`timestamp`, `timestamptz`, `interval`, `macaddr`, `inet`, `pg_lsn`, `xid`, `cid`, `tid`,
`oid`, and any enum. `jsonb` has `jsonb_ops` for whole documents too, but it is not the default:
`jsonb_contains_ops` is (see "jsonb containment" below). Case-insensitive text: `CREATE EXTENSION
pg_lion_citext` (requires `citext`) adds `citext_ops`. Domains resolve to their base type. Keys over 2047 bytes, counting a 4-byte length header (2043 bytes
of text), are rejected, except tsvector lexemes and jsonb keys, which are hashed (below).

## Multi-key columns: arrays and tsvector (DESIGN.md §17)

`array_ops` (any array type) and `tsvector_ops` index one row under many keys, reusing GIN's own
extraction functions (for tsvector through wrappers that hash lexemes too long to be keys), and
answer

    CREATE INDEX ON doc USING lion (tags);       -- text[], int[], ...
    CREATE INDEX ON doc USING lion (tsv);        -- tsvector, with positions

    tags @> '{a,b}'    the intersection of the elements' posting sets, exact
    tags && '{a,b}'    their union, exact
    tags <@ '{a,b}'    every indexed row, rechecked in the heap
    tsv  @@ 'a & (b | c)'   an AND/OR tree over the lexemes, exact

Several such quals on one column (`tags && '{a}' AND tags && '{b}'`), like two lists on a scalar
column, are ANDed in the index, as quals on several columns are, so the heap gets only the rows they
select together (DESIGN.md §5, SCAN step 5).
`count(*)` over `@>`, `&&` and an AND/OR tsquery is pushed down like any other clause, and can be
combined with a `GROUP BY` on a scalar roaring column.

A `tsvector` index also stores every lexeme's positions and weights per row, as GIN does, unless it
is built `WITH (store_positions = off)`, so phrases (`a <-> b`, `a <3> b`), weights (`a:A`) and NOTs
(`a & !b`, `a <-> !b`) are answered exactly from the index: bitmap and index scans need no recheck,
and counts skip the heap on all-visible pages, under a `GROUP BY`, `count(DISTINCT)` or the FK-side
join too, under an OR, and for `@@ ANY (array)` in scans. It costs about three bytes per lexeme and
row on top of a lion index without them, more than doubling it; an index that never sees a phrase,
a weight or a NOT, and is not used for ranking, can leave them out:

    CREATE INDEX ON doc USING lion (tsv) WITH (store_positions = off);

An index with positions also ranks. `lion_bm25(index, query, k [, k1, b])` returns the `k` rows
with the highest BM25 score for the query's lexemes, best first, scored entirely from the index
(term frequencies, document frequencies and each row's length, which the index keeps under its own
key); the heap is read only to check that the rows returned are visible. Join on `ctid` for the rows:

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

A lexeme of up to 2043 bytes is a key as it is. A longer one (tsvector allows 2047) is stored as its
first bytes and its SHA-256, and a query that names a lexeme of 2043 bytes or more has every
candidate rechecked in the heap, so the answer stays exact; a prefix `foo:*` longer than about 1975
bytes is not expanded, and every row is rechecked.

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
larger (8,000 for a million rows), but never more than a sixteenth of the rows the build samples.
The build samples as many rows as half of `maintenance_work_mem` holds (1.5 KB a 1536-dimension
row, at most 16.8 million rows, 25.8 GB) and runs k-means over at most 128 of them a partition
(never fewer than 65,536 rows): the
default 64 MB holds about 21,800 rows of 1536 dimensions, so at most 1,361 partitions; a million
rows need 376 MB for their 8,000, 1 GB gives k-means 44 rows a partition and 3.2 GB the 128, and a
billion rows' 252,982 need 12.5 GB. More partitions make a scan score fewer rows and rank more
centroids, and make the build slower (it grows with the rows times the square root of the
partitions); they leave the
pause where it was, as `pg_lion.vector_patience` counts rows, not partitions. The setting takes
effect at the next `REINDEX`.

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
would follow is removed as the serial one is, and `LionOrdered` is never parallel, so a filtered
query runs in one process; under `allow` the parallel scan with a `Filter` is a plan like the
serial one.

## Reloptions

`store_positions` (`auto` | `on` | `off`, default `auto`; `true` and `false` are `on` and `off`): on
a `tsvector` column, also store every lexeme's positions and weights, so phrase, weight and NOT
queries are answered from the index and BM25 ranking works (see "Multi-key columns" above), at
about twice the index's size. `auto` stores them when a key column's operator class can
(`tsvector_ops`) and builds an index with no such column without them; `on` on such an index is an
error; `off` leaves them out. Read at build time, like `summaries`: `ALTER INDEX ... SET` changes
nothing until a `REINDEX`.
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
moves out of its entry tuple onto container pages of its own.
`buckets` (0 .. 65536, default 0): accepted and ignored. It sized the hash directory of the
first index formats, which a B-tree keyed by the index key replaced (DESIGN.md §21); that tree
grows by splitting and has nothing to size, so an index built with any value is the index built
without it. A value above 0 is answered with a NOTICE saying so when it is set, by `CREATE INDEX`
or `ALTER INDEX ... SET`; a value outside the range is an error, as for any reloption.
`fillfactor` is what decides how full the build packs the directory's leaves.
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
the number of rows a multi-key opclass extracted no key from, `ndistinct`, the distinct keys
the planner is given for the column (DESIGN.md §33; NULL where none is recorded), and, last,
`narrow_containers`, the column's NARROW containers (DESIGN.md §38).  `slack_bytes` and
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
  off` takes these walks away too, and the index-only mode of the ordered ones.
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
- `pg_lion.enable_count_residual`: let the count pushdown take a `WHERE` clause no Lion index
  answers - `payload LIKE 'ab%'`, `id > 1000`, `length(s) = 64` - beside ones that do, testing it on
  the heap row of each row the others select (DESIGN.md §10, "Residual clauses"). Off, such a count
  goes to the ordinary plan.
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
the clauses no lion index answers and suggesting the subquery form above, or this setting. `exact`
(the default before 2026-10-17): the same, but such a query is planned exactly, as a sort of the
filter's rows, however many rows that reads (`WHERE id = 5 ORDER BY embedding <=> $1 LIMIT 1` reads
the one row by the primary key and sorts it). `allow`: the plain scan with a `Filter`, and
`LionOrdered` with one, are plans like any other, chosen by cost, as before 2026-10-16.
`pg_lion.enable_ordered_scan = off` does not re-admit the plain scan: under `error` the query then
fails, and under `exact` it is sorted. A clause the planner tests once (`WHERE $1`,
`WHERE current_user = 'app'`), and one a partial `lionvec` index's predicate implies, is no filter
of the scan's rows, and one of the vector column alone (`embedding IS NOT NULL`, a distance bound)
filters them under every value; any other is a filter, a key's `id = 5` too. A table with
row-level security `lionvec` refuses under every value (below).

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
`GROUP BY` or a sum-over-all-entries count (its entries are keys, not row values; `USING lion (doc)`
on `jsonb` is one, and `doc jsonb_ops` the index for `=` and `count(DISTINCT doc)` on whole
documents), and the cost model inherits the
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
the setting) does not read lion or `lionvec` indexes at all: neither detects "snapshot too old", so
the planner prices them out and declines the count pushdown and `LionOrdered` - a filtered
nearest-neighbour query is then planned as a sort, under every `pg_lion.vector_postfilter` - and a
scan or SQL count that reaches one anyway fails with an error rather than return a different answer
than the snapshot's (DESIGN.md §9).  Inserts and VACUUM work as usual.

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
`--shm-size`.
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
