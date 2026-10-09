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
and 179 ms for 100,000, where a HashAggregate over the table took 900 ms and 1.9 s.

`GROUP BY g ORDER BY count(*) DESC LIMIT k` counts only the groups that can be among the first `k`
(`Top K` in EXPLAIN; DESIGN.md §36): each key's entry records how many rows it holds, which bounds
its group's count, so the largest entries are counted first and the rest never once they cannot
catch up - ten groups of 3.4 million on ClickBench's `UserID`. Expressions of the grouping column
(`GROUP BY ip, ip - 1`) split no group and are computed from it.

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
```

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
