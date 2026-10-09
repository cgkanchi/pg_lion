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
- array membership (`tags @> ...`) and full-text search (`tsv @@ ...`), with phrase search and BM25
  ranking;
- jsonb containment, key existence and jsonpath (`doc @> ...`, `doc ? ...`, `doc @@ ...`).

On a vacuumed table these counts never touch the heap, so they can be one or two orders of magnitude
faster than a B-tree or GIN index. They show up in `EXPLAIN` as `Custom Scan (LionCount)`.

**Status: prototype.** It works on PostgreSQL 16 through 19 and master, and CI tests every one of
them. The on-disk format may still change. Don't put data you can't rebuild behind it yet.

## When to use it

| Your workload | Use |
| --- | --- |
| Dashboards, facet counts, `GROUP BY` over large tables | **Lion.** This is what it is for. |
| Range counts on a column with many distinct values (timestamps, ids) | Lion `WITH (summaries = auto)` |
| Array membership and full-text search, especially counts | Lion. Add `store_positions = true` for phrases and ranking. |
| jsonb `@>`, `?` and jsonpath filters, especially counts | Lion with `jsonb_contains_ops` |
| Unique constraints, or `ORDER BY col LIMIT n` on a single column | B-tree |
| Returning many filtered rows in order | A covering B-tree, which lion can filter as it walks it (see below) |
| Write-heavy tables | Measure first. Lion inserts are slower and write more WAL than B-tree. |

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

Indexes work either way. See [Reloptions → `wal_mode`](docs/REFERENCE.md#reloptions) for the
details and the one thing to know before you turn it on.

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
`count(DISTINCT col)` and `sum`/`min`/`max` of an indexed column over the whole table. Partitioned
tables are counted partition by partition, and filters that a partition's bounds already imply are
skipped.

### Ranges

```sql
CREATE INDEX events_ts ON events USING lion (created_at) WITH (summaries = auto);

SELECT count(*) FROM events WHERE created_at >= now() - interval '7 days';
```

Without `summaries`, a range reads every distinct value inside it, which is slow for a timestamp
column. With them, lion keeps a pre-merged bitmap per block of about 4,096 rows and reads those
instead.

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

### Arrays and full-text search

```sql
CREATE INDEX docs_tags ON docs USING lion (tags);                                   -- any array
CREATE INDEX docs_tsv  ON docs USING lion (tsv) WITH (store_positions = true);      -- tsvector

SELECT count(*) FROM docs WHERE tags @> '{postgres,index}';
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english', 'roaring & bitmap');
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english', 'roaring <-> bitmap & !btree');
```

Lion uses PostgreSQL's own operators and types, and returns exactly the rows a sequential scan or a
GIN index would. A fuzz test in CI checks this against both.

AND/OR queries are answered from the index alone. Phrases (`<->`), weights (`:A`) and NOT need
`store_positions = true`. Without it they still work, but each candidate row is rechecked in the
heap, as GIN does. Storing positions roughly doubles the index size. A prefix (`foo:*`) is answered
from the index too, as the OR of the indexed words that start with `foo`, unless it matches more than
1000 words; then lion rechecks rows, and GIN is faster.

### jsonb

```sql
CREATE INDEX docs_doc ON docs USING lion (doc jsonb_contains_ops);

SELECT count(*) FROM docs WHERE doc @> '{"status": "active", "tags": ["sale"]}';
SELECT count(*) FROM docs WHERE doc ? 'discount' AND doc ?| '{eu,uk}';
SELECT count(*) FROM docs WHERE doc @@ '$.items[*].sku == "A1"';
```

`jsonb_contains_ops` indexes every path to a value in the document, with the value, so `@>`, `?`,
`?|` and `?&` are answered from the index alone, with the same rows as a sequential scan or GIN.
The one exception is a query whose array element has two or more fields
(`{"items": [{"sku": "A", "qty": 2}]}`): the index finds the documents with both, and each one is
rechecked in the heap to see that they sit in the same element, as GIN does for every `@>`.
The index is about the size of GIN's `jsonb_ops`. To filter or group on a known field, an
expression index such as `USING lion ((doc->>'status'))` is smaller.

jsonpath `@?` and `@@` use the same index, and like GIN, lion rechecks every row it finds in the
heap. The index narrows the rows on each `path == value` and on each path that must exist. A path
stops narrowing at `.*`, `.**` or an item method such as `.size()`. A range (`$.price > 10`) only
requires the path to exist. A path with more than 1000 distinct values under it narrows nothing.

### Ranking with BM25

An index with `store_positions = true` can also rank:

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
BM25 (`k1 = 1.2`, `b = 0.75`, both adjustable).

## Supported types

Integers, floats, `numeric`, `bool`, `text`/`varchar`, `char(n)`, `bytea`, `uuid`, dates, times,
timestamps, `interval`, `inet`, `macaddr`, `jsonb` (whole values, or paths with
`jsonb_contains_ops`), `pg_lsn`, `oid`, enums, any array, and `tsvector`. Install `pg_lion_citext` for case-insensitive `citext`. Indexes can have several
columns and can be built in parallel on PostgreSQL 17 and later.

## Limitations

- No `ORDER BY` from the index itself, no unique indexes, no `INCLUDE` columns, no parallel scans.
  Use a covering B-tree for ordered or row-returning queries, as shown above.
- Inserts are about 1.3× slower than B-tree and write about 1.5× the WAL.
- A plain `ORDER BY col LIMIT n` on one column is faster with a B-tree.
- A full-text prefix (`foo:*`) matching more than 1000 distinct words is rechecked row by row, and
  is slower than GIN.
- BM25 top-k is slower than GIN + `ts_rank` when a query matches only a few hundred rows, and slows
  down when every query term is very common.
- The count pushdown uses an expression index (for example, lion on `(data->>'key')`) only on a
  table that is not partitioned, and not in its joins.
- jsonb `<@` isn't indexed, and jsonpath (`@?`, `@@`) is always rechecked in the heap. Updating
  an indexed jsonb document writes one entry per path, so it costs more than with GIN's pending
  list.
- On a hot standby, indexes using the default (generic) WAL mode recheck every row in counts.
  Preloading the library avoids this.
- Loading the library changes estimates, and so plans, that have nothing to do with lion: a column
  with a lion index takes its number of distinct values from the index, and a table's row estimate
  can be corrected from one, even with `pg_lion.enable_count_pushdown` off. Results never change.
  Set `pg_lion.enable_index_ndistinct` and `pg_lion.enable_rows_correction` off to keep the
  planner's own estimates.

The [full list](docs/REFERENCE.md#known-limitations) has the details.

## Performance

Query execution times on a synthetic benchmark (5M rows unless noted, vacuumed, warm cache, 4 vCPUs,
PostgreSQL 18), with how lion compares. "lion 10× faster" means the other index took ten times as long.

| Query | Lion | B-tree | GIN |
| --- | --- | --- | --- |
| Count a value in 50% of rows | 0.41 ms | 189 ms (lion 464× faster) | 933 ms (lion 2,287× faster) |
| Count a value in 0.5% of rows | 0.18 ms | 1.86 ms (lion 10× faster) | 89.2 ms (lion 485× faster) |
| Count a value in 0.005% of rows | 0.039 ms | 0.069 ms (lion 1.8× faster) | 1.24 ms (lion 32× faster) |
| Two equality filters ANDed | 1.21 ms | 34.9 ms (lion 29× faster) | 296 ms (lion 245× faster) |
| Count per group, 200 groups | 31.8 ms | 493 ms (lion 15× faster) | 1,275 ms (lion 40× faster) |
| Count a small range | 1.02 ms | 1.91 ms (lion 1.9× faster) | 666 ms (lion 653× faster) |
| Count a value in 50% of rows, 1% of rows updated, no VACUUM | 183 ms | 882 ms (lion 4.8× faster) | 1,021 ms (lion 5.6× faster) |
| Fetch the rows matching two filters | 20.8 ms | 22.1 ms (lion 1.1× faster) | 39.9 ms (lion 1.9× faster) |
| Two filters, `ORDER BY` another column, `LIMIT 10` | 1.88 ms | 20.7 ms (lion 11× faster) |  |
| `WHERE c >= .. ORDER BY c LIMIT 100` | 0.45 ms | 0.030 ms (lion **15× slower**) |  |
| Full-text count, `'w1 & w17'` (200k docs) | 0.16 ms |  | 1.33 ms (lion 8.4× faster) |
| Full-text count, phrase `'common <-> w1'` | 3.90 ms |  | 14.7 ms (lion 3.8× faster) |
| Full-text count, prefix `'rare12:*'` | 1.20 ms |  | 2.57 ms (lion 2.1× faster) |
| Top 10 by BM25 vs `ts_rank`, 33k matches (500k docs) | 9.9 ms | | 121 ms (lion 12× faster) |
| Top 10 by BM25 vs `ts_rank`, 584 matches | 7.1 ms | | 3.7 ms (lion **1.9× slower**) |

Index size sits between GIN and B-tree, builds are about twice as fast as B-tree (lion builds in
parallel), and inserts are about 1.3× slower than B-tree with 1.5× the WAL. The [benchmark page](docs/BENCHMARKS.md) has every query, the
setup and how to reproduce it.

## Documentation

- [Usage guide](docs/GUIDE.md): every query shape in detail, and what to look for in `EXPLAIN`.
- [Reference](docs/REFERENCE.md): SQL functions, operators, index options, settings, limitations.
- [Benchmarks](docs/BENCHMARKS.md): measurements and how to reproduce them.
- [Developing](docs/DEVELOPING.md): building, the test suites, source layout.
- [DESIGN.md](DESIGN.md): the on-disk format, locking, crash safety and planner design. It's long.

## License

Copyright (c) 2026, Chinmay Kanchi. Licensed under the [PostgreSQL License](LICENSE).
