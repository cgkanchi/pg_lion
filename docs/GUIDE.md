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
to a temporary file past it. A range on another column (`... WHERE created_at >= $1 GROUP BY
country`, with `summaries = on` on its index) is one such filter, collected from the summaries and
past the memory into a temporary file too - a table's ranges share the memory, and one that does
not fit what the others left sends those to files first; where there are only a few counts to
make - a few groups, or a `count(DISTINCT)`'s tests, which stop at a row - the planner may walk it
at each instead, whether or not it fits, when that is clearly the cheaper (`Range Sources Walked`
in `EXPLAIN ANALYZE`). The groups are then
counted against it up to a few hundred at a time, in one pass over its containers for all of them
(`Group Batches` in `EXPLAIN ANALYZE`). With parallel query enabled
(`max_parallel_workers_per_gather`) such a `GROUP BY` over one table can
run in parallel: the workers divide the table's blocks into ranges, each collects the filters of
its ranges and counts every group there, and a `Finalize HashAggregate` above the `Gather` adds
the groups' partial counts up (`Parallel Custom Scan (LionCount)`, `Key Ranges` in `EXPLAIN
ANALYZE`; DESIGN.md §10, "A GROUP BY in parallel"). A `GROUP BY coalesce(col, c)` divides the
same way.

A count without a `GROUP BY` (`SELECT count(*) FROM events WHERE event_type = 'purchase' AND
...`, with `count(col)`s, filtered counts, OR lists, `IN` lists, ranges or a text search) runs in
parallel the same way when the planner finds it cheaper: each process counts the ranges it
claims, as the serial count counts the whole table, and a `Finalize Aggregate` adds the partial
counts and applies any `HAVING` (DESIGN.md §10, "A count in parallel"). The workers are what a
parallel scan of the pages the count reads would get, up to `max_parallel_workers_per_gather`,
or the table's `parallel_workers`; a table under about 16 MB is one range, and counted serially.
A `count(DISTINCT)`, an aggregate of a column's values such as
`sum(col)`, a partitioned table and a join stay serial. Starting the workers takes a few
milliseconds, which `parallel_setup_cost` (1000) may price low: a count the planner overprices can
go parallel and take longer, a count of 2 ms taking 8 in the measurements of that section, and a
higher `parallel_setup_cost` keeps such counts serial.

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
one, `length(name) = 3` - does not send the count to the ordinary plan when the other clauses do
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
a partition that also takes NULLs, a default partition, a generic plan's parameter or a stable
expression (`current_setting(...)`, whose value a cached plan does not keep) implies only what that
proof can show. An `OR` with an arm per kind - `(kind = 'a' AND tags && '{x}') OR
(kind = 'b' AND tags && '{y}')` - is narrowed the same way: each partition leaves out the arms its
bounds rule out and, in the arm it keeps, the `kind = ...` they imply, so the partition of `'a'`
counts `tags && '{x}'` alone (`Refuted by Partition Bounds` in `EXPLAIN`).

When every query of a workload carries the same few filters that keep most of the table - `WHERE
active AND state = 'published' AND ...` - a partial index on them holds only those rows, and the
count pushdown can read it (DESIGN.md §45):

```sql
CREATE INDEX events_current ON events USING lion (country, event_type, device)
    WHERE active AND state = 'published';

SELECT country, count(*) FROM events
 WHERE active AND state = 'published' AND event_type = 'purchase' GROUP BY country;
```

```
 Custom Scan (LionCount)
   Lion Indexes: events_current.country (country), events_current.event_type (event_type = 'purchase'::text)
   Implied by Index Predicate: active, (state = 'published'::text)
   Group Key: country
```

The filters the predicate implies are left out of the count, as a partial index scan leaves them
out (`Implied by Index Predicate`), and the other columns' sets are those of the predicate's rows
alone, so there is less to read and nothing dense to intersect: on four million rows whose three
default filters kept 77% of them, a count with one more filter took 0.2 ms against 0.47, a range
2.4 ms against 28, and a `GROUP BY` 3.0 against 5.8. The query has to imply the predicate - a
stronger condition does (`x > 20` under `WHERE x > 10`), a parameter of a generic plan usually
does not - and the partial index is read only where every row the count reaches is one of its
own: through a clause it answers that selects rows (`=`, `IN`, `IS NULL`, a range) or a walk of
its entries (a `GROUP BY`, a `count(DISTINCT)`, the count of every row). A count that would have
to take the rest from another index - `active AND state = 'published' AND region = 'eu'` with no
`region` in the partial index - reads the whole index instead, and so does a query that leaves
the predicate's filters out. A partial index made on a partitioned table is one per partition,
and each partition reads its own (`Partial Indexes` in EXPLAIN). `pg_lion.enable_count_partial =
off` turns this off.

A count over a fact table joined to a filtered dimension (DESIGN.md §27) is pushed down too, when the
fact's foreign-key column has a Lion index and its own filters are ones Lion answers: the dimension
side runs as an ordinary plan (its own Lion index serves its filters through a bitmap scan, or,
where a B-tree holds the dimension's key, a Lion-filtered walk of that B-tree that reads no heap,
below), and for
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
and collects the filters part way through once that reaches what collecting them costs - a copy
expected not to fit that memory priced as spilled, its pages written once and read back by the
counts after it (DESIGN.md §27, "Probed, then collected"; `EXPLAIN ANALYZE` prints
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
column. `NOT IN` keeps the ordinary plan, but on PostgreSQL 19 where neither side can be NULL (a
NOT NULL column, or a filter that rules NULL out): 19 plans that one as the same anti join as `NOT
EXISTS`, and it is offered for it as for that. The cost model chooses between it and PostgreSQL's
own joins; `pg_lion.enable_semijoin` turns it off.

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
-- beside docs_tags_lion above, and a grp int column; a tsvector index stores
-- its words' positions by default, for phrases and ranking, at about twice
-- the size (WITH (store_positions = off) leaves them out)
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
alone (`Heap Fetches` counts those too). A set is inexact when the query has a condition Lion can
only bound (`tags <@ ...`, a phrase query): `Lion Set: ..., rechecked` in `EXPLAIN ANALYZE`.
`col IS NULL` and `col IS NOT NULL` never make it so - beside any other condition the column's
NULL rows are taken out of the set, or kept, exactly (DESIGN.md §14).

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
index-only mode off. With parallel query enabled (`max_parallel_workers_per_gather`) the walk can
be divided among the workers, each walking its share of the B-tree's leaf pages against one set
they share: `Parallel Custom Scan (LionBtreeScan)` below a `Gather`, `Parallel Custom Scan
(LionOrdered)` below a `Gather Merge` (DESIGN.md §40.9). The B-tree may be one that holds none of
the filtered columns, such as the primary key of `SELECT sum(id) FROM t WHERE <Lion filter>`; it
walks the whole index.

The same walk is the inner side of a nested loop when the other table's filter leaves few rows:

```sql
-- the dimension rows that pass Lion filters, counted by the fact rows that match a search
SELECT d.id, count(*) AS n FROM dim d JOIN fact f ON f.dim_id = d.id
 WHERE d.tags && ARRAY['t1', 't3'] AND d.verified AND f.tsv @@ 'w7 & x3'::tsquery
 GROUP BY d.id ORDER BY n DESC LIMIT 100;
```

PostgreSQL's plan looks each fact row's `dim_id` up with an Index Scan of `dim_pkey` and reads the
dimension row from the heap to test the filter, rejecting most of them. `Custom Scan (LionBtreeScan)`
with `Index Cond: (d.id = f.dim_id)` looks the key up in the same B-tree, tests the entry against
a Lion set of the dimension's filter, built once for the whole loop, and takes `id` from the index
tuple: the heap is read only for pages the visibility map does not call all-visible, so keep the
dimension vacuumed (DESIGN.md §40.10). The B-tree must hold every column the query reads of the
dimension; a `Memoize` above the probe and a parallel nested loop work as with PostgreSQL's scan.

When the query groups the join by expressions over the dimension's Lion-indexed columns themselves,
which no B-tree on the key holds, and the fact table has no Lion index, the join and the group keys'
predicates are one node:

```sql
-- the fact's rows grouped by a CASE over the dimension's Lion columns and a fact column
SELECT CASE WHEN d.region IN ('north', 'east') THEN 'ne'
            WHEN d.tier = 1 THEN 't1' ELSE 'other' END AS k,
       f.channel, count(*)
  FROM fact f JOIN dim d ON f.dim_id = d.id
 WHERE d.active AND d.tier < 4
   AND NOT EXISTS (SELECT 1 FROM blocked b WHERE b.dim_id = f.dim_id)
 GROUP BY 1, 2;
```

`Custom Scan (LionProbeJoin)` reads the fact's rows from PostgreSQL's own plan of the fact, looks
each `dim_id` up in `dim_pkey` (`Probe Index`, `Probe Cond`), and tests the entry against Lion sets
of the dimension built once: the `WHERE`'s (`Lion Cond`), one for each outcome of the columns the
`CASE`'s predicates read (`Cell Predicates`), and the `NOT EXISTS`'s keys looked up in the same
B-tree (`Anti Join Cond`). The heap is read for pages the visibility map does not call all-visible,
and for every row found when the `WHERE`'s set is inexact, to recheck the `WHERE` on the heap tuple
(`Heap TIDs Rechecked` and `Rows Removed by Lion Recheck` in `EXPLAIN ANALYZE`). PostgreSQL's
`HashAggregate` computes the groups from the predicates' truths - in parallel over the fact's
parallel scan. The predicates are the ones `pg_lion.enable_count_cells` counts (equalities, `IN`,
null tests, booleans and multi-key queries with constants); a dimension column used any other way -
grouped by itself, under a `THEN`, in an aggregate - keeps the node out, and so does a `WHERE`
clause on the dimension no Lion index answers. The planner prices a B-tree descent per fact row and
the sets' builds against PostgreSQL's plan: the node pays where the fact's rows are few against the
dimension's `WHERE`, or the dimension's heap would be read cold; `pg_lion.enable_probe_join` turns
it off (DESIGN.md §46).

When the query returns the joined rows themselves, a page of them in the order of a B-tree of the
fact table, the ordered walk crosses the join:

```sql
CREATE INDEX fact_grp_score ON fact (grp, score DESC NULLS LAST);

-- the best-scored fact rows of a group whose tags match, with their dimension rows that pass
-- the dimension's own Lion filter, and only for the keys a small table lists
SELECT f.id, f.score, d.id, d.name FROM fact f JOIN dim d ON f.dim_id = d.id
 WHERE f.grp = 2 AND f.tags @> ARRAY['a1'] AND d.region = 'north' AND d.verified
   AND f.dim_id IN (SELECT dim_id FROM watchlist WHERE active)
 ORDER BY f.score DESC NULLS LAST LIMIT 20;
```

PostgreSQL's plan walks `fact_grp_score` with the tags as a `Filter`, reading each fact row to
test them, and looks each kept row's `dim_id` up in `dim_pkey`, reading the dimension row to test
`region` and `verified` and rejecting most of them; or it reads every row of both filters, joins
them and sorts. `Custom Scan (LionOrderedJoin)` walks the B-tree over a Lion set of the fact's
filter (`Ordered By`, `Lion Cond`), as `LionOrdered` does, looks each member's key up in the
dimension's primary key (`Dimension: dim d via dim_pkey`) and tests the entry against a Lion set of
the dimension's filter (`Dimension Lion Cond`) before reading the dimension row, and returns the
rows joined in the walk's order until the `LIMIT` has its twenty. A table that only filters the key,
as `watchlist` does here, is read once into a hash set of its keys (`Key Sets: watchlist`), and a
fact row whose key is not in it is skipped before the lookup. Either table may have no Lion
filter, as long as the other has one or a key set filters. The dimension needs a unique B-tree on
its key, the join that one equality and nothing else; the fact's B-tree must give the `ORDER BY`
at least its first column, and where it holds every fact column the query reads the walk is
index-only (`Ordered By: fact_grp_score (index only)`). `EXPLAIN ANALYZE` shows each side's
counters, the dimension's behind `Dimension` (`Dimension Descents`, `Dimension Heap Fetches`).
The planner prices it against PostgreSQL's plans. A nested loop's Memoize is taken at the cache hits
the `LIMIT` lets it meet, not at the hit ratio of the loop's whole walk. Where the tables outgrow
`effective_cache_size`, the pages PostgreSQL's nested loop reads for each fact row and key it keeps
are charged as cold reads, as are the node's own, which are fewer. `pg_lion.enable_ordered_join`
turns it off (DESIGN.md §47).

When the page is ordered first by a column of the dimension with few values, and then by the
fact's B-tree, no B-tree gives the order, and PostgreSQL joins every row of both filters and sorts:

```sql
-- the verified dimension rows' best-scored fact rows first, then the rest's
SELECT f.id, f.score, d.id, d.name FROM fact f JOIN dim d ON f.dim_id = d.id
 WHERE f.grp = 2 AND f.tags @> ARRAY['a1'] AND d.region = 'north'
 ORDER BY d.verified DESC, f.score DESC NULLS LAST LIMIT 20;
```

With a Lion index on `d.verified`, `Custom Scan (LionOrderedSplit)` runs the join a value of
the column at a time, in the `ORDER BY`'s order (`Split By: dim_verified (verified, backward, nulls
first)`): a `LionOrderedJoin` beneath it with the dimension narrowed to the rows of that value,
which walks the fact's B-tree from the top until the page is full, and moves on to the next value
only if it is not. A column can be split when it is a plain column with a Lion index, sorted in its
type's own order and collation, and has at most `pg_lion.ordered_split_max` values (8) by its
statistics; values added since `ANALYZE` are still returned in their place. `EXPLAIN ANALYZE`
shows how many values were run (`Arms Run`) and how many were passed over because none of their
rows pass the dimension's filter (`Entries Skipped`). An `ORDER BY` that goes on past the fact's
B-tree is finished by an Incremental Sort above (DESIGN.md §48).

## Nearest neighbours: lionvec

`CREATE EXTENSION pg_lion_vector` (with pgvector installed) adds `lionvec`, an index that answers
`ORDER BY embedding <op> $1 LIMIT k` for pgvector's `vector`, with `vector_l2_ops` (`<->`, the
default), `vector_ip_ops` (`<#>`) or `vector_cosine_ops` (`<=>`), and for its `halfvec`, with
`halfvec_l2_ops` (the default), `halfvec_ip_ops` or `halfvec_cosine_ops`, and that a Lion filter
restricts (DESIGN.md §44). A `halfvec` column's index is the same as a `vector` column's - its
halves are read as floats and coded as a vector's - so it is no smaller, while the column itself,
and what an exact distance reads of it from the table, takes half the space.

```sql
CREATE EXTENSION pg_lion_vector;
-- once per database: at pgvector's own cost of 1 the planner may sort a filter's
-- rows by distance where the lionvec scan is faster
ALTER FUNCTION l2_distance(vector, vector) COST 100;
ALTER FUNCTION vector_negative_inner_product(vector, vector) COST 100;
ALTER FUNCTION cosine_distance(vector, vector) COST 100;
-- and halfvec's, if you index halfvec columns
ALTER FUNCTION l2_distance(halfvec, halfvec) COST 100;
ALTER FUNCTION halfvec_negative_inner_product(halfvec, halfvec) COST 100;
ALTER FUNCTION cosine_distance(halfvec, halfvec) COST 100;

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

Both answers are approximate, as HNSW's are: the index partitions the rows by k-means when it is
built, and a scan reads the partitions whose centroids are nearest the query, nearest first, until
the next centroid, less `pg_lion.vector_margin`, lies farther than the rows it has, or until
`pg_lion.vector_patience` times `sqrt(n) / 2` rows (n the rows at the build) have brought nothing
nearer. A row in a partition it did not read is missed. On 975,000 OpenAI embeddings the defaults
return 98% of the true ten nearest, reading 4% of the rows; `SET pg_lion.vector_margin = 2` returns
96% in five sixths of the time, and `SET pg_lion.vector_patience = 0`, which reads on to the margin
however long nothing nearer turns up, 98.4% in 1.8 times the time and a 95th percentile four times
as long. The recall a margin gives is the data's: on 115,186 of DBpedia's text-embedding-3-large
embeddings the same defaults return 84%, and 95% takes `SET pg_lion.vector_margin = 7` (DESIGN.md
§44.9), so measure it on your own rows against an exact search.

By default the build also puts each row in a second partition, chosen among its eight next nearest
so that a query that passes the row's own partition by is likely to read it (`spill`, after SOAR),
so a scan finds more of the nearest rows at the same margin; it returns each once. That doubles the
one-bit code pages, an eighth of a 1536-dimension index: those 975,000 embeddings make an index of
1.9 GB rather than 1.7 GB, and every insert writes to two partitions. On 115,000 OpenAI embeddings
the default margin returned 92% of the true ten nearest with it and 84% without, and a margin of 3
with it as many as one of 5 without, reading less than half the partitions. `WITH (spill = off)`
and a `REINDEX` keep one copy (the figures above and below are from an index without).

Building the index runs k-means over a sample of the rows and then finds every row's partition,
comparing each row with the centroids of the groups of partitions (regions) and then of the
partitions in the regions nearest it, so it takes time in proportion to the rows times the square
root of the partitions. Both steps run in parallel for a table of 10,000 rows or more, or one
with a `parallel_workers` setting, in up to
`max_parallel_maintenance_workers` workers (2 by default) besides the building backend; for a
large table raise it, and `maintenance_work_mem`, which holds the sample k-means runs on:

```sql
SET maintenance_work_mem = '4GB';
SET max_parallel_maintenance_workers = 7;  -- eight processes, within max_parallel_workers
CREATE INDEX items_emb ON items USING lionvec (embedding vector_cosine_ops);
```

115,386 rows of 1536 dimensions in 2,717 partitions took 108 s alone and 36 s with three workers
on four cores (292 s and 85 s before the regions), and a scan answers the same through either
index (DESIGN.md §44.12). A parallel build keeps
its sample of the rows, up to half of `maintenance_work_mem`, in shared memory: in Docker, whose
`/dev/shm` is 64 MB unless `--shm-size` says otherwise, give it at least that much, or the build
stops with "could not resize shared memory segment"; `SET max_parallel_maintenance_workers = 0`
builds alone.

The partitions are chosen from that sample, half of `maintenance_work_mem`, at least 16 of its
rows each, so a large table needs it large: 376 MB for a million rows of 1536 dimensions, 12 GB
for a billion, where the default 64 MB is enough for 28,985 such rows. Given too little, the build
stops early - as soon as it has read enough rows to know - with an error that says how many
partitions it wants, how many the sample has rows for, and the setting that would be enough:

```
ERROR:  lionvec index "items_emb" wants 8000 partitions, and its sample has rows for 1361
DETAIL:  About 1000000 rows with a vector want 8000 partitions of at least 16 sample rows each, and half of maintenance_work_mem (64MB) holds a sample of 21788 rows of 1536 dimensions.
HINT:  Set maintenance_work_mem to at least 376MB, or set pg_lion.vector_build_sample to allow to build 1361 partitions.
```

Set it and build again, or `SET pg_lion.vector_build_sample = allow` to build the fewer, larger
partitions, which make a scan read more rows for the same recall. A `pg_restore` or `reindexdb`
builds at the server's `maintenance_work_mem` unless told otherwise:
`PGOPTIONS='-c maintenance_work_mem=4GB' pg_restore ...`.

The node builds the Lion set of the `WHERE`, and the vector scan scores only the members in the
partitions it reads, so the `LIMIT` is filled from the matching rows whatever their share of the
table; `Heap Fetches` in `EXPLAIN ANALYZE` counts the rows the node fetched, about `k`. A set of up
to `pg_lion.vector_exact_rows` rows (2,000), or to `pg_lion.vector_exact_per_partition` (1.5) rows
for each of the index's partitions if that is more, is answered exactly, from every member's
distance; a larger one by reading partitions as above, its patience counted in matching rows. For
a filter of a few thousand rows the planner weighs that exact answer against a sort of the Lion
bitmap scan's rows, exact too, priced alike and the node preferred up to 1.2 times the sort's
price: with the vectors in TOAST, which the node reads by their places, it takes the node by a wide
margin, and with them in the rows, where the two read the same pages, the node too. With the
distance functions left at pgvector's cost of 1 it may choose that sort for larger
filters as well, where the node is several times faster (pgvector's vectors are stored out of line,
so the planner sees a small column and a nearly free function); it did on the benchmark until
scans paused, and the `ALTER FUNCTION`s above keep it from doing so. `pg_dump` does not keep them,
so run them again after a restore; `SET enable_sort = off` steers a single query the same way.

A `WHERE` clause no Lion index answers is never applied to the rows the `lionvec` scan returns: the
scan pauses, and skips rows it finds too late, by the rows it has returned, so filtering them
afterwards would find other rows than the node does at the same settings, and the plan would decide
the answer (DESIGN.md §44.6, "Post-filtering"). By default such a query fails when it is planned,
`EXPLAIN` included, with an error that names the clauses no Lion index answers and suggests asking
for the approximate answer in so many words: filter the nearest rows of a subquery (the planner
does not push the outer `WHERE` past its `LIMIT`):

```sql
SELECT id FROM (SELECT id, title, embedding <=> $1 AS d FROM items WHERE category = 3
                ORDER BY d LIMIT 1000) s
 WHERE title LIKE 'A%' ORDER BY d LIMIT 10;
```

Every clause counts, `id = 5` as much as `title LIKE 'A%'`; only one the planner tests once
(`WHERE $1`, `WHERE current_user = 'app'`), one a partial `lionvec` index's predicate implies, and
one of the vector column alone (`embedding IS NOT NULL`, a distance bound
`embedding <=> $1 < 0.3`), is no filter. The last filters the scan's rows, harmlessly: the NULL
vectors come last in its order, and a bound rejects rows by the distance it orders them by, so the
rows before them are the same. A bound few rows pass leaves the `LIMIT` unfilled, and the scan then
reads to its end. `SET pg_lion.vector_postfilter = exact` plans a filtered query exactly instead, as a sort of the rows that pass the filter, which reads every one of them
(`WHERE id = 5 ORDER BY embedding <=> $1 LIMIT 1` sorts the one row the primary key finds), and
`allow` plans as before this rule, the scan with a `Filter` among the plans. `lionvec` does not
support row-level security: it builds no index on a table that has it enabled, or on a partition of
one, and a query such an index could order fails, whoever runs it. `pg_lion.enable_ordered_scan = off`
takes the node away, and a filtered query then fails, or under `exact` is sorted. An HNSW index on
the same column is chosen over `lionvec` for nearly every query, filtered or not: pgvector prices
its scan as nearly free.

What a scan did is in its `DEBUG1` line (`SET client_min_messages = debug1`). On those 975,000
embeddings, in 1,974 partitions, one query without a filter and then under one keeping 1% of the
rows:

```
lionvec scan of "items_lv": 110 partitions probed, 1 passes, 15345 candidates, 631 refined, 10 fetched, 10 returned, 0 skipped out of order, 0 bounds missed, 1745 pages read, 1 pauses, 0 copies skipped
lionvec scan of "items_lv": 306 partitions probed, 1 passes, 1230 candidates, 46 refined, 11 fetched, 10 returned, 0 skipped out of order, 0 bounds missed, 4843 pages read, 1 pauses, 0 copies skipped
```

Partitions probed are the partitions whose rows the scan read; candidates the rows of those (the
members, under a filter) that plane 1's bounds could not rule out; refined the ones whose 8-bit
code was read, fetched the ones whose exact distance was computed from the table; bounds missed
counts fetched rows that turned out nearer than their lower bound said, and pages read the code
pages the probes read; pauses counts the times the scan stopped probing because
`pg_lion.vector_patience` times `sqrt(n) / 2` rows had brought nothing nearer; copies skipped
counts the rows met a second time in a pass, in their other partition, in an index that spills
(none here, the index being built without). Without pauses a
query whose nearest rows lie far from every centroid reads many partitions for nothing: unfiltered,
the partitions a query probed had a median of 68, a 95th percentile of 175 and a maximum of 256 with
pauses, and 150, 837 and 1,664 without. A query that wants more rows than `pg_lion.vector_batch`
makes more passes, each probing again the partitions probed so far; a row "skipped out of order"
turned up after rows farther than it had been returned (it lay in a partition read later, or its
bound failed), which costs that row and nothing else. A filter answered exactly
(`pg_lion.vector_exact_rows`, `pg_lion.vector_exact_per_partition`) logs `N members fetched, N
returned` instead. `pg_lion.vector_margin` and `pg_lion.vector_patience` read more partitions or fewer, and
`pg_lion.vector_epsilon` widens or narrows the bounds (DESIGN.md §44.8).

A scan reads ahead the pages it will read next - the rest of a partition's code pages, which the
build wrote one after another, the partitions it expects to probe next, the refine pages and table
rows at the front of its queues, and under a small filter every matching row - as many ahead as the
tablespace's `effective_io_concurrency`, up to 64 (`pg_lion.vector_prefetch`; DESIGN.md §44.4,
"Prefetch" and "Read streams"). From PostgreSQL 17 it reads them through read streams into shared
buffers (`stream`, the default there); on 16, or under `fadvise`, it asks the kernel for them. It
never changes an answer. It matters when the index is not in memory: on DBpedia's 115,186
embeddings under PostgreSQL 18, each query alone after a restart with the page cache dropped, a
scan at `pg_lion.vector_margin = 8` took 43 ms streaming, 48 asking the kernel and 67 with neither.
pgvector stores a vector of more than 506 dimensions (a `halfvec` of more than 1,012) in TOAST
unless the column or the table says otherwise, and a vector there is read through its row's table
page and the TOAST index, a read each that only the one before it can name. So an index of such
vectors keeps where each one's chunks lie (DESIGN.md §44.13) and reads them there, every chunk
checked, through a read stream for an exact answer (`pg_lion.vector_toast_direct`, on by default;
the answer is the same either way): on DBpedia's embeddings in TOAST a 1% filter answered exactly
took 68 ms cold, where reading each vector through its table page and the TOAST index took
180. The vectors can also
be kept in the table, which an exact answer reads ahead through its rows' pages:

```sql
ALTER TABLE items ALTER embedding SET STORAGE PLAIN;  -- new rows; existing ones once rewritten
ALTER TABLE items SET (toast_tuple_target = 8160);    -- the same: rows up to 8160 bytes stay whole
```

With the vectors in the table so, a 1% filter answered exactly, from every matching row's vector,
took 53 ms cold streaming, 59 asking the kernel and 108 with neither. Streaming costs more where the
pages come from the operating system's cache rather than the disk: PostgreSQL 18 hands each streamed
read to an I/O worker (`io_method = worker`, the default) or a kernel thread (`io_uring`), where
reading one page at a time copies it in the backend, and a scan whose index the operating system
holds but shared buffers do not took up to 40% longer streaming than asking the kernel, and an exact
answer under `io_uring` up to twice as long. Set `pg_lion.vector_prefetch = fadvise` for an index
that lives there. In shared buffers streaming costs 2 to 11% and asking the kernel under 5%: a scan
whose reads keep finding their pages there mostly stops reading ahead.
`SET pg_lion.vector_prefetch = off` turns it off altogether.

With `pg_lion.vector_parallel_scan` on (it is off by default), an unfiltered `ORDER BY embedding
<=> $1 LIMIT k` can run in parallel: a Gather Merge over a Parallel Index Scan, each process probing
the partitions it reaches first and returning their rows nearest first, merged into one order
(DESIGN.md §44.4, "A parallel scan"). Even then, at the planner's default costs it stays serial - `parallel_setup_cost` (1000) is more than a serial scan's start-up - and a
table whose heap is under `min_parallel_table_scan_size` (8 MB, as it is when the vectors are in
TOAST) is planned no workers unless its `parallel_workers` is set:

```sql
SET pg_lion.vector_parallel_scan = on;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 10;
ALTER TABLE items SET (parallel_workers = 2);   -- or lower min_parallel_table_scan_size

EXPLAIN (COSTS OFF) SELECT id FROM items ORDER BY embedding <=> $1 LIMIT 10;
--  Limit
--    ->  Gather Merge
--          Workers Planned: 2
--          ->  Parallel Index Scan using items_emb on items
--                Order By: (embedding <=> $1)
```

It was not faster on any query measured. Each process ranks every centroid and probes until its
own margin and patience stop it, so two processes probe about 1.6 times the partitions one does and
four 3.7 times, finding a little more of the nearest rows for it (on DBpedia at the default margin,
93.5% and 95% of the true ten against 92%); Gather Merge waits for every process's first row, the
workers take about 10 ms to start, and they run on into their queues after the `LIMIT` has its
rows. On DBpedia in memory the serial scan took 2.4 ms and the parallel ones 12 and 16; each
query alone cold, 17 ms and 28 with two processes. A filtered query stays serial: `LionOrdered`
over the distance order, which a filter that lion indexes answer goes through, has no parallel
plan, and under
`pg_lion.vector_postfilter` `error`, the default, or `exact` no plan filters a parallel scan's rows
either; under `allow` a parallel scan with a `Filter` is among the plans. Each process sends its
own `DEBUG1` line, ending with the partitions another process probed and, in an index that spills,
the rows another returned; reading ahead, what its streams read, or the pages it asked for and how
many of them were not in shared buffers.
