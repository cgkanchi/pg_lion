# pg_lion

A PostgreSQL index for counting, filtering and searching, built on compressed roaring bitmaps.

```sql
CREATE EXTENSION pg_lion;
CREATE INDEX ON events USING lion (country);
CREATE INDEX ON events USING lion (event_type);

SELECT country, count(*) FROM events WHERE event_type = 'purchase' GROUP BY country;
```

A lion index stores, for every distinct value, the set of rows that have it, as a compressed
bitmap. Many queries can then be answered by combining bitmaps instead of visiting rows:

- `count(*)` with equality, `IN`, range, `<>` and `IS NULL` filters, ANDed and ORed;
- `GROUP BY`, `count(DISTINCT ...)`, top-k groups;
- counts over a fact table joined to a filtered dimension;
- the first rows in a B-tree's order (`ORDER BY ... LIMIT`) under lion filters, also across a join
  to a filtered dimension by its key;
- array membership (`tags @> ...`) and full-text search (`tsv @@ ...`), with phrase search and BM25
  ranking;
- jsonb containment, key existence and jsonpath (`doc @> ...`, `doc ? ...`, `doc @@ ...`).

`pg_lion_vector` adds `lionvec`, a nearest-neighbour index for pgvector's `vector` and `halfvec`
that reads a few percent of itself a query and fills its `LIMIT` under any lion filter (see
[Nearest neighbours](#nearest-neighbours)).

On a vacuumed table these counts never touch the heap, so they can be one or two orders of magnitude
faster than a B-tree or GIN index. They show up in `EXPLAIN` as `Custom Scan (LionCount)`.

**Status: beta.** It works on PostgreSQL 16 through 19 and master, and CI tests every one of them
in both WAL modes, with crash recovery and standby replay. The on-disk format may still change
before 1.0; a release that changes it will say so, and the indexes will need a `REINDEX`. Until
then queries run without them (each backend warns once), and inserts into their tables fail.

## When to use it

| Your workload | Use |
| --- | --- |
| Dashboards, facet counts, `GROUP BY` over large tables | **Lion.** This is what it is for. |
| Range counts on a column with many distinct values (timestamps, ids) | Lion `WITH (summaries = auto)` |
| Sums, averages and extremes of an integer column under filters, and wide ranges on it | Lion `WITH (slices = on)` on that column (see below) |
| Array membership and full-text search, especially counts | Lion. Phrases and ranking come from the index too. |
| jsonb `@>`, `?` and jsonpath filters, especially counts | Lion |
| Unique constraints, or `ORDER BY col LIMIT n` on a single column | B-tree |
| Returning many filtered rows in order | A covering B-tree, which lion can filter as it walks it (see below) |
| Write-heavy tables | Measure first. Lion inserts are slower than B-tree. |
| Nearest neighbours of pgvector embeddings, especially under a `WHERE` | `lionvec`, from `pg_lion_vector` |

Lion, B-tree and GIN indexes coexist happily. A typical setup keeps B-tree for primary keys and
ordering and adds lion indexes on the columns your counts and filters use.

Counts are fastest right after `VACUUM`. Rows on pages the visibility map doesn't mark all-visible
still have to be checked in the heap, so a frequently updated, rarely vacuumed table sees much less
benefit.

## Installing

Build against your PostgreSQL's `pg_config`:

```sh
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
```

```sql
CREATE EXTENSION pg_lion;
```

Optionally, preload the library to get lion's own WAL format, which makes inserts noticeably cheaper:

```
shared_preload_libraries = 'pg_lion'
```

Indexes work either way, but an index built while the library is preloaded is written in Lion's
format from then on, and needs the preload for as long as it exists: without it, inserts and
updates into its table fail, and so does VACUUM (autovacuum too), so dead rows pile up. To stop
preloading it, first REINDEX each such index with `wal_mode = generic`. See
[Reloptions → `wal_mode`](docs/REFERENCE.md#reloptions) for the details.

## A quick tour

### Counts and groups

```sql
CREATE INDEX events_country ON events USING lion (country);
CREATE INDEX events_type    ON events USING lion (event_type);
VACUUM ANALYZE events;

SELECT count(*) FROM events WHERE country = 'JP' AND event_type = 'purchase';
SELECT country, count(*) FROM events GROUP BY country;
SELECT country, count(*) FROM events GROUP BY country ORDER BY count(*) DESC LIMIT 10;
```

All three are answered from the indexes. Grouping by several lion-indexed columns works too, as do
`count(DISTINCT col)` and `sum`/`min`/`max` of an indexed column over the whole table, or under any
filter and per group when the column is sliced (below). Partitioned tables are counted partition
by partition, and filters that a partition's bounds already imply are skipped.

### Ranges

```sql
CREATE INDEX events_ts ON events USING lion (created_at) WITH (summaries = auto);

SELECT count(*) FROM events WHERE created_at >= now() - interval '7 days';
```

Without `summaries`, a range reads every distinct value inside it, which is slow for a timestamp
column. With them, lion keeps a pre-merged bitmap per block of about 4,096 rows and reads those
instead.

### Sums, extremes and wide ranges from bit slices

```sql
CREATE INDEX events_amount ON events USING lion (amount) WITH (slices = on);

SELECT sum(amount), avg(amount), max(amount) FROM events WHERE country = 'JP';
SELECT country, sum(amount), min(amount) FROM events WHERE device = 'ios' GROUP BY country;
SELECT count(*) FROM events WHERE amount > 100;
```

A sliced integer, date, time or timestamp column keeps one bitmap per bit of its values beside its
ordinary entries. A sum under a filter is then the sum over bits of 2^i times the rows of bit i's
bitmap that pass the filter, a minimum or maximum a descent from the top bit, and a range on the
column a comparison from the top bit down at each block of heap pages, so the work grows with the
column's width and the filter's bitmaps, not with the rows or the distinct values the query covers
(`Aggregates From Slices` and `Range From Slices` in `EXPLAIN`). On 5M rows a sum under a 10%
filter took 7 ms where the ordinary plan took 166, the sum, minimum and maximum of each of 200
groups 41 ms against 181, and a count over half the rows of a column of distinct values 7 ms
against 32 from summaries and 362 for the ordinary plan; a selective range stays with the
summaries or the values, and the planner prices each. The price is on writes: every set bit of a
value is one more posting-set insert, so an `INSERT` into a table with two sliced columns took 4.6
times as long as without slices with `pg_lion` preloaded. Floats, `numeric`, joins and partitioned
tables are not answered from slices yet ([GUIDE.md](docs/GUIDE.md), DESIGN.md §51 to §53).

### Joins

```sql
-- fact table orders, dimension customers; lion indexes on orders(customer_id) and orders(status)
SELECT count(*)
  FROM orders o
 WHERE o.status = 'open'
   AND EXISTS (SELECT 1 FROM customers c WHERE c.id = o.customer_id AND c.country = 'NZ');
```

Lion looks up each matching customer in the `customer_id` index and counts against `status = 'open'`
without visiting `orders`. JOIN, `IN` and `NOT EXISTS` forms work too, as do grouping by dimension
columns and per-customer counts.

### Returning rows

Lion tells you *which* rows match; to *return* them quickly, pair it with a covering B-tree:

```sql
CREATE INDEX docs_recent ON docs (created_at) INCLUDE (id, title);

SELECT id, title FROM docs
 WHERE tags @> ARRAY['postgres', 'index']
 ORDER BY created_at DESC LIMIT 10;
```

Lion walks the B-tree newest first and keeps only the entries whose rows are in the lion bitmap,
stopping at the tenth (`Custom Scan (LionOrdered)` in `EXPLAIN`). The values come straight from the
B-tree, so the heap is read only for pages that aren't all-visible.

The walk also crosses a join to a dimension by its primary key: for `SELECT ... FROM fact f JOIN
dim d ON f.dim_id = d.id WHERE <lion filters on both> ORDER BY f.score DESC LIMIT 20`, each fact
row the walk keeps looks its key up in the dimension's primary key, and the entry is tested against
the dimension's lion bitmap before the dimension row is read (`Custom Scan (LionOrderedJoin)`).
When the `ORDER BY` leads with a lion-indexed column of the dimension that has a few values -
`ORDER BY d.verified DESC, f.score DESC` - the join runs a value at a time, in that order, the
dimension narrowed to the value's rows, until the page is full (`Custom Scan (LionOrderedSplit)`).

### Arrays and full-text search

```sql
CREATE INDEX docs_tags ON docs USING lion (tags);    -- any array
CREATE INDEX docs_tsv  ON docs USING lion (tsv);     -- tsvector

SELECT count(*) FROM docs WHERE tags @> '{postgres,index}';
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english', 'roaring & bitmap');
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english', 'roaring <-> bitmap & !btree');
```

Lion uses PostgreSQL's own operators and types, and returns exactly the rows a sequential scan or a
GIN index would. A fuzz test in CI checks this against both.

AND/OR queries are answered from the index alone, and so are phrases (`<->`), weights (`:A`) and
NOT, because a `tsvector` index stores every word's positions by default. Storing positions roughly
doubles the index size; `WITH (store_positions = off)` leaves them out, and then phrases, weights
and NOT still work, but each candidate row is rechecked in the heap, as GIN does. A prefix
(`foo:*`) is answered from the index too, as the OR of the indexed words that start with `foo`,
unless it matches more than 1000 words; then lion rechecks rows, and GIN is faster.

### jsonb

```sql
CREATE INDEX docs_doc ON docs USING lion (doc);

SELECT count(*) FROM docs WHERE doc @> '{"status": "active", "tags": ["sale"]}';
SELECT count(*) FROM docs WHERE doc ? 'discount' AND doc ?| '{eu,uk}';
SELECT count(*) FROM docs WHERE doc @@ '$.items[*].sku == "A1"';
```

The default operator class for `jsonb`, `jsonb_contains_ops`, indexes every path to a value in the
document, with the value, so `@>`, `?`, `?|` and `?&` are answered from the index alone, with the
same rows as a sequential scan or GIN.
The one exception is a query whose array element has two or more fields
(`{"items": [{"sku": "A", "qty": 2}]}`): the index finds the documents with both, and each one is
rechecked in the heap to see that they sit in the same element, as GIN does for every `@>`.
The index is about the size of GIN's `jsonb_ops`. To filter or group on a known field, an
expression index such as `USING lion ((doc->>'status'))` is smaller. To compare whole documents
with `=` or count the distinct ones, name lion's `jsonb_ops`: `USING lion (doc jsonb_ops)`.

jsonpath `@?` and `@@` use the same index, and like GIN, lion rechecks every row it finds in the
heap. The index narrows the rows on each `path == value` and on each path that must exist. A path
stops narrowing at `.*`, `.**` or an item method such as `.size()`. A range (`$.price > 10`) only
requires the path to exist. A path with more than 1000 distinct values under it narrows nothing.

### Ranking with BM25

A `tsvector` index can also rank, from the positions it stores:

```sql
SELECT id, title, lion_bm25_score(tsv, q, 'docs_tsv') AS score
  FROM docs, to_tsquery('english', 'roaring | bitmap') q
 WHERE tsv @@ q
 ORDER BY lion_bm25_score(tsv, q, 'docs_tsv') DESC
 LIMIT 10;
```

The planner turns this into a `LionBm25` scan, which finds the top rows from the index and skips
rows that can't make the cut, instead of scoring and sorting every match. `lion_bm25(index, query, k)`
returns the same top `k` as `(ctid, score)` pairs, if you'd rather join yourself. Scores are Lucene's
BM25 (`k1 = 1.2`, `b = 0.75`, both adjustable). An index on several tsvector columns ranks each one
by its own statistics; name it with `col => 'body'`.

### Nearest neighbours

With [pgvector](https://github.com/pgvector/pgvector) 0.7 or later installed:

```sql
CREATE EXTENSION pg_lion_vector;
-- pgvector prices its distances as nearly free, which can make the planner sort a
-- filter's rows rather than use lionvec; price them closer to what they cost
ALTER FUNCTION l2_distance(vector, vector) COST 100;
ALTER FUNCTION vector_negative_inner_product(vector, vector) COST 100;
ALTER FUNCTION cosine_distance(vector, vector) COST 100;
-- and halfvec's, if you index halfvec columns
ALTER FUNCTION l2_distance(halfvec, halfvec) COST 100;
ALTER FUNCTION halfvec_negative_inner_product(halfvec, halfvec) COST 100;
ALTER FUNCTION cosine_distance(halfvec, halfvec) COST 100;

CREATE INDEX items_embedding ON items USING lionvec (embedding vector_cosine_ops);
CREATE INDEX items_category  ON items USING lion (category);

SELECT id FROM items ORDER BY embedding <=> $1 LIMIT 10;
SELECT id FROM items WHERE category = 3 ORDER BY embedding <=> $1 LIMIT 10;
```

`lionvec` partitions the rows by k-means when it builds, and keeps one bit per dimension of every
row, rotated so that the error of a distance worked out from those bits is known, plus an 8-bit
code read only when the bits can't decide. A query ranks the partitions by their centroids, reads
the bits of the rows in the nearest few, refines the rows that might make the cut, and computes the
last few distances exactly from the table, pausing once a few dozen partitions have brought
nothing nearer. Like HNSW's, the answer is approximate: on 975,000 1536-dimension OpenAI
embeddings a query reads 4% of the rows (9 MB) and returns 98% of the true ten nearest in 7.9 ms,
about the time pgvector's HNSW takes to return 95% (8.1 ms at `ef_search` 100);
`pg_lion.vector_margin` and `pg_lion.vector_patience` trade one for the other. Under a `WHERE` that lion indexes answer, the scan scores only the
matching rows, so a filter of any selectivity gets its ten nearest: 24 ms at 1% of the rows (98%
recall), where HNSW returns fewer than one row, or 95% of the right ones in 37 to 41 ms with
iterative scans, and a filter of up to 2,000 rows (`pg_lion.vector_exact_rows`), or of up to 1.5
rows a partition (`pg_lion.vector_exact_per_partition`), is answered exactly.
A `WHERE` that no lion index answers - a column without one, `id = 5` - is an error by default,
rather than a filter of the rows the scan returns, which would let the plan decide which rows come
back: the error suggests filtering the nearest rows of a subquery instead,
and `SET pg_lion.vector_postfilter = exact` sorts the matching rows by distance, exactly, which
reads every one of them ([REFERENCE.md](docs/REFERENCE.md#settings)). A clause of the vector
column alone, `embedding IS NOT NULL` or a distance bound, is no such filter: it filters the scan's
rows.
A range over a column of many distinct values, a timestamp or a price, is a walk of every key it
covers unless the column's lion index keeps summaries (`WITH (summaries = auto)`): at 10% of a
million unique values, 45 ms before the first row without them and 3 ms with. The index is 1.7 GB to
HNSW's 3.8 GB (1.9 GB as built now, with every row's one-bit code in two partitions, which finds
more of the nearest rows at each margin: the figures here are from before), and since a query
reads only the partitions it needs, it holds up when memory is short: with PostgreSQL held to
1 GB, 19 ms and 100 pages read from disk a query, where HNSW took 51 ms and 767 pages (and 691 ms
and 9,229 pages under the 1% filter, where lionvec read none).
`vector_l2_ops` (`<->`, the default), `vector_ip_ops` (`<#>`) and `vector_cosine_ops` (`<=>`) are
supported, and for a `halfvec` column `halfvec_l2_ops` (the default), `halfvec_ip_ops` and
`halfvec_cosine_ops`, up to 8128 dimensions. See
[BENCHMARKS.md](docs/BENCHMARKS.md#nearest-neighbours-lionvec-against-hnsw).

## Supported types

Integers, floats, `numeric`, `bool`, `text`/`varchar`, `char(n)`, `bytea`, `uuid`, dates, times,
timestamps, `interval`, `inet`, `macaddr`, `jsonb` (paths, or whole values with `jsonb_ops`), `pg_lsn`, `oid`, enums, any array, and `tsvector`. Install `pg_lion_citext` for case-insensitive `citext`. Indexes can have several
columns and can be built in parallel on PostgreSQL 17 and later. `pg_lion_vector` indexes pgvector's
`vector` and `halfvec` with `lionvec` (one column, built in parallel from PostgreSQL 16; its scans
can run in parallel too, and ask for the pages they will read before reading them, which cut a
cold scan's wait on the disk by a third to a half on 115,000 embeddings).

## Limitations

- No `ORDER BY` from the index itself, no unique indexes, no `INCLUDE` columns, no parallel scans
  (of lion's own indexes: a `lionvec` scan can run in parallel).
  Use a covering B-tree for ordered or row-returning queries, as shown above.
- Inserts are about 1.5× slower than B-tree. A sliced column (`WITH (slices = on)`) pays a
  posting-set insert per set bit of each value on top: an `INSERT` into a table with a sliced
  `int4` and a sliced `int8` column took 4.6× as long as without slices with `pg_lion` preloaded,
  and about 11× without it. Slices serve int2, int4, int8, date, time and timestamp columns on one
  table; not floats, `numeric`, joins or partitioned tables.
- A plain `ORDER BY col LIMIT n` on one column is faster with a B-tree.
- A full-text prefix (`foo:*`) matching more than 1000 distinct words is rechecked row by row, and
  is slower than GIN.
- BM25 top-k is slower than GIN + `ts_rank` when a query matches only a few hundred rows, and slows
  down when every query term is very common.
- The count pushdown uses an expression index (for example, lion on `(data->>'key')`) only on a
  table that is not partitioned, and not in its joins.
- `lionvec` assumes pgvector's distance functions are costed at 100 (the `ALTER FUNCTION`s under
  [Nearest neighbours](#nearest-neighbours)). At pgvector's own cost of 1 the planner may sort a
  filter's rows by distance instead: before scans paused, it did for filters of 1% and 10% of a
  million rows, 3 to 17 times slower. The setting is not kept by `pg_dump`, and an `ALTER
  EXTENSION vector UPDATE` may reset it, so run them again after either.
- If the column also has an HNSW index, the planner chooses HNSW for nearly every query, filtered
  or not, because pgvector prices its scan as nearly free; under a filter it then returns fewer
  rows than the `LIMIT` unless `hnsw.iterative_scan` is on.
- A `lionvec` query under a `WHERE` that no lion index answers fails by default
  (`pg_lion.vector_postfilter`); `SET pg_lion.vector_postfilter = exact` plans it as an exact sort
  instead.
- A `lionvec` build chooses its partitions from a sample of what half of `maintenance_work_mem`
  holds, so a large table needs it large (376 MB for a million rows of 1536 dimensions, 12 GB for a
  billion); given too little, the build fails early, naming the setting it needs
  (`pg_lion.vector_build_sample`; `allow` builds fewer partitions instead).
- `lionvec` does not support row-level security: it builds no index on a table that has it
  enabled, or on a partition of one, and a query such an index could order fails.
- jsonb `<@` isn't indexed, and jsonpath (`@?`, `@@`) is always rechecked in the heap. Updating
  an indexed jsonb document writes one entry per path, so it costs more than with GIN's pending
  list.
- `lionvec` answers only an `ORDER BY` of its distance, approximately, as HNSW and IVFFlat do. Its
  build is arithmetic: k-means, then every row compared with the centroids of the groups of
  partitions (regions) and of the partitions in the regions nearest it, so it grows with the rows
  times the square root of the partitions: 115,386 rows of 1536 dimensions in 2,717 partitions
  took 108 s alone and 36 s with three parallel workers (292 s and 85 s comparing every row with
  every centroid). It reuses the space of deleted rows only slowly and
  keeps the partitions it was built with, so a table with heavy churn or drift needs an occasional
  `REINDEX`. Each row it returns is read from the table twice. Its scan runs in parallel only with
  `pg_lion.vector_parallel_scan` on (off by default) and `parallel_setup_cost` lowered, and on
  115,000 rows it was then 4 to 8 times slower than alone, each process probing about as much as one scan does; a filtered query stays serial.
- On a hot standby, indexes using the default (generic) WAL mode recheck every row in counts.
  Preloading the library avoids this.
- Loading the library changes estimates, and so plans, that have nothing to do with lion: a column
  with a lion index takes its number of distinct values, and the frequencies of the values a query
  compares it with, from the index, and a table's row estimate can be corrected from one, even with
  `pg_lion.enable_count_pushdown` off. Results never change. Set `pg_lion.enable_index_ndistinct`,
  `pg_lion.enable_index_mcv` and `pg_lion.enable_rows_correction` off to keep the planner's own
  estimates. On a skewed key column, Lion also prices a probe of a key the plan does not know, and
  the core scan it competes with, at the rows such a key really meets. That can move nested-loop
  and correlated-subquery plans. `pg_lion.enable_skew_estimate` off prices them at the planner's
  rows per key.

The [full list](docs/REFERENCE.md#known-limitations) has the details.

## Citus and TimescaleDB

Both were run through a compatibility matrix with pg_lion on PostgreSQL 18 (Citus 14.2, TimescaleDB
2.30): every answer matched the ordinary plans, index DDL reaches every shard and chunk, and shard
moves, rebalances, chunk compression, continuous aggregates and retention leave valid lion indexes.
What to know:

- **Citus:** list `citus` before `pg_lion` in `shared_preload_libraries`. Loaded after lion, Citus
  replaces two planner hooks without chaining them, and lion's ordered, BM25 and join nodes vanish
  (counts stay, and answers are right either way). The count pushdown runs inside each shard query
  on the workers. The SQL functions that take an index name (`lion_index_count`,
  `lion_index_facets`, `lion_bm25`, `lion_bm25_score`) read the one index they are given, which on
  the coordinator and inside a distributed query is the empty shell of the distributed table's
  index: `lion_bm25_score` then scores every row 0. Call them on the shards through
  `run_command_on_shards`. After a shard move or rebalance, `VACUUM` the table so counts can use
  the visibility map again.
- **TimescaleDB:** `CREATE INDEX ... USING lion` on a hypertable creates one lion index per
  chunk, new chunks included. The count pushdown does not run on a hypertable as a whole (it is an
  inheritance parent, which the pushdown declines): a count uses lion scans per chunk under
  TimescaleDB's chunk-wise aggregation, and a count on one chunk by name uses `LionCount`.
  Compressed chunks keep their lion indexes, which lion then plans nothing on. An index on
  `time_bucket('1 hour', ts)` is not used for counts; a stored bucket column is.

## Performance

Query execution times on a synthetic benchmark (5M rows unless noted, vacuumed, warm cache, 4 vCPUs,
PostgreSQL 18), with how lion compares. "lion 10× faster" means the other index took ten times as long.

| Query | Lion | B-tree | GIN |
| --- | --- | --- | --- |
| Count a value in 50% of rows | 0.34 ms | 133 ms (lion 392× faster) | 703 ms (lion 2,073× faster) |
| Count a value in 0.5% of rows | 0.14 ms | 1.35 ms (lion 9.7× faster) | 62.9 ms (lion 453× faster) |
| Count a value in 0.005% of rows | 0.043 ms | 0.044 ms (same) | 0.90 ms (lion 21× faster) |
| Two equality filters ANDed | 0.87 ms | 27.6 ms (lion 32× faster) | 195 ms (lion 224× faster) |
| Count per group, 200 groups | 25.1 ms | 330 ms (lion 13× faster) | 940 ms (lion 37× faster) |
| Count a small range | 1.05 ms | 1.46 ms (lion 1.4× faster) | 512 ms (lion 486× faster) |
| Count a value in 50% of rows, 1% of rows updated, no VACUUM | 116 ms | 616 ms (lion 5.3× faster) | 820 ms (lion 7.1× faster) |
| Fetch the rows matching two filters | 16.4 ms | 17.7 ms (lion 1.1× faster) | 29.3 ms (lion 1.8× faster) |
| Two filters, `ORDER BY` another column, `LIMIT 10` | 1.67 ms | 15.4 ms (lion 9.2× faster) |  |
| `WHERE c >= .. ORDER BY c LIMIT 100` | 0.42 ms | 0.028 ms (lion **15× slower**) |  |
| Full-text count, `'w1 & w17'` (200k docs) | 0.11 ms |  | 1.37 ms (lion 12× faster) |
| Full-text count, phrase `'common <-> w1'` | 4.58 ms |  | 16.1 ms (lion 3.5× faster) |
| Full-text count, prefix `'rare12:*'` | 0.97 ms |  | 3.23 ms (lion 3.3× faster) |
| Top 10 by BM25 vs `ts_rank`, 33k matches (500k docs) | 6.82 ms | | 112 ms (lion 16× faster) |
| Top 10 by BM25 vs `ts_rank`, 584 matches | 4.83 ms | | 2.06 ms (lion **2.3× slower**) |

The lion indexes are smaller than both the B-tree and the GIN ones, build in a little over half
B-tree's time, and inserts are about 1.5× slower than B-tree while writing a little less WAL. The
[benchmark page](docs/BENCHMARKS.md) has every query, the setup and how to reproduce it.

## Documentation

- [Usage guide](docs/GUIDE.md): every query shape in detail, and what to look for in `EXPLAIN`.
- [Reference](docs/REFERENCE.md): SQL functions, operators, index options, settings, limitations.
- [Benchmarks](docs/BENCHMARKS.md): measurements and how to reproduce them.
- [Developing](docs/DEVELOPING.md): building, the test suites, source layout.
- [DESIGN.md](DESIGN.md): the on-disk format, locking, crash safety and planner design. It's long.

## License

Copyright (c) 2026, Chinmay Kanchi. Licensed under the [PostgreSQL License](LICENSE).
