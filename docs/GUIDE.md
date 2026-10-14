# pg_lion usage guide

The detailed version of the README's feature tour: what each query shape does, how to recognise it in `EXPLAIN`, and where the design notes are. Section numbers (§) refer to [DESIGN.md](../DESIGN.md).

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

A `GROUP BY` of three or more indexed columns (`SELECT country, event_type, device, count(*)
FROM events GROUP BY country, event_type, device`), or of two with more than a few combinations,
is counted in one walk of the index whatever the number of combinations: at each range of 64 heap blocks it reads which value of each column
every row has, and counts each row under its combination (`Group Strategy: Decoded` in EXPLAIN,
under a `Finalize HashAggregate`; DESIGN.md §34). On 5M rows it took 77 ms for 1,000 combinations
and 179 ms for 100,000, where a HashAggregate over the table took 900 ms and 1.9 s. A range in the
`WHERE` (`... WHERE amount < 500 GROUP BY a, b, c`) is collected with the rest of the filter, into
a temporary file when it is larger than the memory: a quarter of 5M rows grouped by three columns
took 153 ms against 361.

`GROUP BY g ORDER BY count(*) DESC LIMIT k` counts only the groups that can be among the first `k`
(`Top K` in EXPLAIN; DESIGN.md §36): each key's entry records how many rows it holds, which bounds
its group's count, so the largest entries are counted first and the rest never once they cannot
catch up - ten groups of 3.4 million on ClickBench's `UserID`. Expressions of the grouping column
(`GROUP BY ip, ip - 1`) split no group and are computed from it.

These forms are answered as the counts they stand for (DESIGN.md §10, "Shapes rewritten to the
node's counts"):

- expressions over the counts and the grouping columns, such as `count(*)::int` or `g,
  round(100.0 * count(*) / 5000000, 2) ... GROUP BY g`, computed once per output row.
- `count(1)`, or `count` of any other non-NULL constant, which is `count(*)`.
- `count(col)` of a nullable indexed column, without a `GROUP BY` and when every aggregate is a
  count of that column: counted as `count(*) ... AND col IS NOT NULL`.
- `col NOT IN (a, b, ...)` (or `col <> ALL (...)`) of a literal list without NULLs, counted as
  `col <> a AND col <> b ...`.
- filtered and conditional counts: `count(*) FILTER (WHERE c)`, `count(CASE WHEN c THEN 1 END)`,
  `sum(CASE WHEN c THEN 1 ELSE 0 END)` and `sum(CASE WHEN c THEN 1 END)`, where `c` is a condition
  the `WHERE` could have (`SELECT count(*) FILTER (WHERE event_type = 'purchase'), count(*) FROM
  events WHERE country = 'Japan'`): each condition is one more count of the `WHERE` with `c`
  added (`Count Filter 1: ...` in EXPLAIN), and each form gives the answer it gives over a table
  scan, NULL included. With a `GROUP BY` the counts come out as partial rows a `Finalize
  HashAggregate` adds up. A condition no Lion index answers leaves the whole query to the ordinary
  plan (DESIGN.md §10, "Filtered counts").
- window functions over the counts, such as a share of the total, `g, round(100.0 * count(*) /
  sum(count(*)) OVER (), 2) ... GROUP BY g`: the counts are pushed down and the window runs above
  them.
- `GROUP BY` with no aggregate and `SELECT DISTINCT` of indexed columns over one table (`SELECT
  DISTINCT country FROM events`): the keys that have a visible row, each once.
- `GROUP BY ROLLUP (...)`, `CUBE (...)` and `GROUPING SETS (...)` of indexed columns over one
  table, `GROUPING()` included: the node counts the plain `GROUP BY` of every column the sets
  name, and PostgreSQL's own grouping sets aggregate adds those counts up into each set's
  groups (`LionJoinAgg` above a `GroupAggregate` in EXPLAIN; DESIGN.md §10, "Grouping sets").
  The aggregates may be `count(*)`, `count` and `sum` (of `smallint` or `integer`) of grouping
  columns, `min`, `max`, `bool_and` and `bool_or` of them, and any `DISTINCT` aggregate of them,
  with `FILTER` clauses over the grouping columns; any other aggregate leaves the query to the
  ordinary plan. On 5M rows a `ROLLUP` of two columns of 20 and 2 values took 46 ms where the
  ordinary plan took 885 ms.
- `count(DISTINCT col)` (or another `DISTINCT` aggregate of indexed columns) beside a `GROUP BY`
  of two or more columns, `SELECT country, device, count(DISTINCT event_type) ... GROUP BY
  country, device`: the node counts the `GROUP BY` with `event_type` added, and PostgreSQL's
  aggregate counts each group's distinct values over those combinations (DESIGN.md §10,
  "DISTINCT aggregates over a finer grouping"). On 5M rows, two grouping columns of 20 and 2
  values and a distinct column of 200 took 92 ms where the ordinary plan took 3.3 s.

Clauses that exclude rows are counted by subtracting what they reject: `col <> v`, `col IS NOT
NULL`, `col IS DISTINCT FROM v` (which, unlike `<>`, keeps the rows where `col` is NULL), and `NOT
(tags @> '{x}')` or `NOT (doc @@ 'a & b')` of a literal multi-key query (which drops the NULL
arrays or documents, as the query does). With nothing else in the `WHERE`, the count of every row
is taken from one of the table's Lion columns (`(all rows)` in EXPLAIN). They may also stand alone
in an arm of an `OR` - `n <> 17 OR n IS NULL`, `status = 'open' OR region <> 'eu'` - which is
counted as two counts by De Morgan's rule (DESIGN.md §35, "Complements in an OR"); an arm that
mixes them with an equality, such as `(a = 1 AND n <> 3) OR ...`, is left to the ordinary plan, as
is a `NOT` of a query whose index sets only bound its rows (an empty array, a NULL element, a
phrase). On five million rows `count(*) WHERE n IS DISTINCT FROM 17` took 1.6 ms against 380 for
the sequential scan.

A `WHERE` clause no Lion index answers - `payload LIKE 'ab%'`, `id > 1000` on a column without
one, `length(name) = 3` - no longer sends the count to the ordinary plan when the other clauses do
select rows: the node counts those rows and tests the clause on each of them in the heap
(`Residual Filter` in EXPLAIN; DESIGN.md §10, "Residual clauses"). It does the work of a bitmap
heap scan over the same rows, so it is taken when the other clauses select at most half the table,
and the planner compares its price with the ordinary plan's; a volatile function, a subquery, a
partitioned table or a join's count keeps the ordinary plan. `pg_lion.enable_count_residual = off`
turns it off.

An expression index counts as a column of its expression (DESIGN.md §41). With `CREATE INDEX ON
events USING lion ((payload->>'status'))`, `WHERE payload->>'status' = 'paid'`, `IN`, ranges,
`<>`, `IS NULL`, `GROUP BY payload->>'status'` and `count(DISTINCT payload->>'status')` are
pushed down like the same queries on a column; so is a cast of a field, such as an index on
`((payload->>'qty')::int)` and `WHERE (payload->>'qty')::int > 10`. The query has to write the
expression as the index does. EXPLAIN prints the expression where it would print a column name.

With no `WHERE` and no `GROUP BY`, `sum`, `avg` (of integers), `min`, `max`, `bool_and` and
`bool_or` of an expression of one Lion-indexed column are computed from the column's keys, each
weighted by its rows (`Aggregates Over Keys` in EXPLAIN; DESIGN.md §37): `SELECT sum(width),
avg(width + 1) FROM t` walks `width`'s distinct values, not the table. On a table the visibility
map calls all-visible - as after a `VACUUM` - the rows of each key are the count its entry keeps,
and nothing else is read; a `VACUUM` that removes rows from the index while the walk runs, or a
table that is not all-visible, has each key's rows counted instead.

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

A `LEFT JOIN` from the dimension keeps every customer, one with no order counted 0: `SELECT
cu.country, count(o.id) FROM customers cu LEFT JOIN orders o ON o.customer_id = cu.id AND o.status =
'open' GROUP BY cu.country` is pushed down as the JOIN is, each customer emitted with its count
whether or not it has orders (`Join Type: Left` in EXPLAIN; DESIGN.md §27, "The dimension LEFT JOIN
the fact"). Conditions on the orders go in the `ON` clause; one on the customers goes in `WHERE`.
`count(*)` counts a customer without orders once, as the join does, and `count(o.col)` counts the
orders when `o.col` is the foreign key or declared `NOT NULL`.

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

A second table joined to the fact table only to filter it is folded into a fact filter (DESIGN.md
§27, "A second dimension that only filters"). In `SELECT cu.country, count(*) FROM orders o JOIN
customers cu ON o.customer_id = cu.id JOIN products p ON o.product_id = p.id WHERE p.discontinued
GROUP BY cu.country` nothing of `products` is wanted above the join and its key is unique, so the
join keeps the orders whose `product_id` is the id of a discontinued product, each once. The node
runs the plan of `products` itself, as a second child, at the start of each scan, and counts with
`o.product_id = ANY (<those ids>)` among the fact filters, which needs a Lion index on
`o.product_id`: EXPLAIN lists it among the `Lion Indexes` as `(product_id = p.id)`, and EXPLAIN
ANALYZE counts its keys (`Join Filter Keys`). Up to eight such tables are folded, each inner-joined
to the fact table alone by one equality on a unique key of its own and expected to return no more
rows than the node locates of one `IN` list at once (`work_mem` of them at 256 bytes each, at least
1,000). A table whose column is selected or grouped by, that is joined to another table too, or
that is outer-joined keeps the ordinary plan; with a folded table the node does not run in
parallel.

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
```

The facet counts beside a search's results - the ten tags with the most matching documents, the
lexemes of the matches, the documents in each group - come from `lion_index_facets(idx, k, filter)`
(docs/REFERENCE.md, "Facet counts"). It counts the rows under each key of one Lion index column,
exactly and under the caller's snapshot, and with a `k` it counts only the keys that can be among the
k largest; `filter` is the search's `WHERE` clause, answered from the Lion indexes alone:

```sql
-- beside docs_tags_lion above, and a grp int column
CREATE INDEX docs_tsv_lion ON docs USING lion (tsv);
CREATE INDEX docs_grp_lion ON docs USING lion (grp);

-- the search: documents matching 'w7 | w42' in group 3
SELECT id, title FROM docs WHERE tsv @@ 'w7 | w42' AND grp = 3 ORDER BY id LIMIT 20;

-- its facets: the ten most frequent tags of every match, and the matches in each group
SELECT * FROM lion_index_facets('docs_tags_lion', 10, $$tsv @@ 'w7 | w42' AND grp = 3$$);
SELECT * FROM lion_index_facets('docs_grp_lion', NULL, $$tsv @@ 'w7 | w42'$$);

-- the twenty most frequent lexemes of group 3, the ndoc of
-- ts_stat('SELECT tsv FROM docs WHERE grp = 3')
SELECT * FROM lion_index_facets('docs_tsv_lion', 20, 'grp = 3');
```

A tag's count is the number of documents whose `tags` hold it, however often - `count(DISTINCT id)`
over `unnest(tags)`, not `count(*)`, which counts a tag twice in a document that repeats it. The
filter is an `AND`/`OR` of what the indexes answer as sets - `=`, `IN`, `IS NULL` on a column a Lion
index keys, and `@>`, `&&`, `@@`, `?` on a multi-key one; a condition they cannot answer (`grp > 2`,
a phrase query) is an error that names it, never a scan. Each call checks what the query would:
SELECT on the columns, EXECUTE on the operators, no row-level security for the caller.

To return ROWS rather than counts under a Lion filter, give the query a covering B-tree, as you
would for an Index Only Scan, and let Lion filter its walk (DESIGN.md §30 and §40): the node walks
the B-tree in its order, tests each entry's TID against the Lion indexes' posting sets before it
touches the heap, and returns the members' values from the index tuples, reading the heap only for
pages the visibility map does not call all-visible (`Heap Fetches` in `EXPLAIN ANALYZE`, 0 after a
`VACUUM`). Lion's part is the membership test, which answers what the B-tree cannot hold: a
multi-key column (`tags @> ...`), a column behind a high-cardinality key, ANDs and ORs across
independent indexes. The B-tree must hold, as key or `INCLUDE` columns, every column the query
reads - its target and the `WHERE` clauses the node still tests itself - but not the Lion-filtered
columns: a set that turns out inexact at run time is rechecked on the heap tuple, fetched for that
alone (`Heap Fetches` counts those too).

```sql
CREATE INDEX docs_created_cov ON docs (created_at) INCLUDE (id, title);

-- the latest matching rows: LionOrdered walks the B-tree backward and stops at the tenth member
SELECT id, title FROM docs WHERE tags @> ARRAY['t1', 't17'] ORDER BY created_at DESC LIMIT 10;

-- no ORDER BY: LionBtreeScan walks the whole covering B-tree, never the heap
CREATE INDEX events_country_cov ON events (country) INCLUDE (amount);
SELECT country, sum(amount) FROM events WHERE event_type = 'purchase' GROUP BY country;
```

`EXPLAIN` shows `Custom Scan (LionOrdered)` with `Ordered By: <index> (index only)` for a walk that
gives the query its order, and `Custom Scan (LionBtreeScan)` with `Index: <index> (index only)` for
one that does not; `Lion Cond` is what Lion answered. A full covering walk reads the B-tree in place
of the heap but spends about 170 ns an entry on the membership probe, so with a warm cache it loses
to the Lion index's own scan for a filter of a few percent and to a sequential scan for a wide one;
it pays when the heap would be read cold or the filter is far wider than the B-tree's share of the
heap, and the planner's I/O constants (`random_page_cost`, `seq_page_cost`) decide (DESIGN.md
§40.3). A query that reads a column the B-tree lacks
walks in heap mode under an `ORDER BY` and gets no unordered walk; `SELECT ... FOR
UPDATE` needs the row's `ctid` and is heap mode too. The cost model chooses between the node, the
B-tree's own scans and a bitmap scan of the Lion indexes; `pg_lion.enable_ordered_scan` and
`pg_lion.enable_btree_scan` turn the two forms off, and `enable_indexonlyscan = off` turns the
index-only mode off.

## Nearest neighbours: lionvec

`CREATE EXTENSION pg_lion_vector` (with pgvector installed) adds `lionvec`, an index that answers
`ORDER BY embedding <op> $1 LIMIT k` for pgvector's `vector`, with `vector_l2_ops` (`<->`, the
default), `vector_ip_ops` (`<#>`) or `vector_cosine_ops` (`<=>`), and that a Lion filter restricts
(DESIGN.md §44):

```sql
CREATE EXTENSION pg_lion_vector;
-- once per database: pgvector's own cost of 1 makes the planner sort a filter's
-- rows by distance where the lionvec scan is faster
ALTER FUNCTION l2_distance(vector, vector) COST 100;
ALTER FUNCTION vector_negative_inner_product(vector, vector) COST 100;
ALTER FUNCTION cosine_distance(vector, vector) COST 100;

CREATE INDEX items_emb ON items USING lionvec (embedding vector_cosine_ops);
CREATE INDEX items_cat ON items USING lion (category);

-- the ten nearest: an Index Scan of items_emb with Order By
SELECT id FROM items ORDER BY embedding <=> $1 LIMIT 10;

-- the ten nearest in category 3: LionOrdered scores only the category's rows
SELECT id FROM items WHERE category = 3 ORDER BY embedding <=> $1 LIMIT 10;
```

The second plan reads

```
 Limit
   ->  Custom Scan (LionOrdered) on items
         Ordered By: items_emb
         Order By: (embedding <=> $1)
         Lion Cond: (category = 3)
         Lion Indexes: items_cat
```

The node builds the Lion set of the `WHERE`, and the vector scan reads the codes of only the
pages that hold a member and scores only the members, so the `LIMIT` is filled from the matching
rows whatever their share of the table; `Heap Fetches` in `EXPLAIN ANALYZE` counts the rows the node
fetched, about `k`. Where nearly every row passes the filter the planner prefers the plain index
scan with a `Filter`, and for a filter of a few hundred rows a sort of the Lion bitmap scan's rows.
Between the two it chooses that sort too, where the node is two to four times faster, unless the
distance functions cost 100 as above (pgvector's vectors are stored out of line, so the planner
sees a small column and, at a cost of 1, a nearly free function; `pg_dump` does not keep the
`ALTER FUNCTION`s, so run them again after a restore). `SET enable_sort = off` steers a single
query the same way. `pg_lion.enable_ordered_scan = off` takes the node away. An HNSW index on the
same column is chosen over `lionvec` for nearly every query, filtered or not: pgvector prices its
scan as nearly free.

What a scan did is in its `DEBUG1` line (`SET client_min_messages = debug1`). On 975,000 OpenAI
embeddings, without a filter and then under one keeping 0.1% of the rows:

```
lionvec scan of "items_lv": 1 passes, 11472 candidates, 35 refined, 10 fetched, 10 returned, 0 skipped out of order, 0 bounds missed, 27858 pages read
lionvec scan of "items_lv": 1 passes, 947 candidates, 76 refined, 10 fetched, 10 returned, 0 skipped out of order, 0 bounds missed, 965 pages read
```

A pass reads the one-bit codes of every row (of every matching row, under a filter, and only the
pages that hold one); candidates are the rows its bounds could not rule out, refined the ones
whose 8-bit code was read, fetched the ones whose exact distance was computed from the table;
bounds missed counts fetched rows that turned out nearer than their lower bound said, and pages
read the code pages the passes read. A query that wants more rows than `pg_lion.vector_batch` makes more passes; a row "skipped out of
order" was found after rows farther than it had been returned, which costs that row and nothing
else. `pg_lion.vector_epsilon` widens or narrows the bounds (DESIGN.md §44.8).
