# pg_lion

pg_lion is a new inverted index for PostgreSQL that accelerates counting, filtering and GROUP BY queries significantly. pg_lion uses a roaring bitmap-inspired compressed bitmap data structure to instantly identify which rows hold which values, allowing most count queries to be answered straight from the index.

As an inverted index, lion can also answer these queries on document types (arrays, `jsonb` and `tsvector`), and it can do so significantly faster than GIN. In addition, when combined with an appropriate BTree index, lion can augment it with its fast filtering, while letting BTree do the sort.

```sql
CREATE EXTENSION pg_lion;
CREATE INDEX ON events USING lion (country);
CREATE INDEX ON events USING lion (event_type);

SELECT country, count(*) FROM events WHERE event_type = 'purchase' GROUP BY country;
```

A lion index stores, for every distinct value, the set of rows that have it, as a compressed bitmap. Many queries can then be answered by combining bitmaps instead of visiting rows:

- `count(*)` with equality, `IN`, range, `<>` and `IS NULL` filters, ANDed and ORed;
- `GROUP BY`, `count(DISTINCT ...)`, top-k groups;
- counts over a fact table joined to a filtered dimension;
- the first rows in a B-tree's order (`ORDER BY ... LIMIT`) under lion filters, also across a join to a filtered dimension by its key;
- array membership (`tags @> ...`) and full-text search (`tsv @@ ...`), with phrase search and BM25 ranking;
- jsonb containment, key existence and jsonpath (`doc @> ...`, `doc ? ...`, `doc @@ ...`).

`pg_lion_vector` adds `lionvec`, a nearest-neighbour index for pgvector's `vector` and `halfvec` that works under any lion filter.

On a vacuumed table most of these counts never touch the heap, so they are often orders of magnitude faster than a B-tree or GIN index. They show up in `EXPLAIN` as `Custom Scan (LionCount)`.

## When to use it

| Your workload                                                                         | Use                                                                 |
| ------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| Dashboards, facet counts, `GROUP BY` over large tables                                | **Lion.** This is what it is for.                                   |
| Range counts on a column with many distinct values (timestamps, ids)                  | Lion `WITH (summaries = auto)`                                      |
| Sums, averages and extremes of an integer column under filters                        | Lion `WITH (slices = on)` on that column (see below)                |
| Array membership and full-text search, especially counts                              | Lion. Phrases and ranking come from the index too.                  |
| jsonb `@>`, `?` and jsonpath filters, especially counts                               | Lion                                                                |
| Unique constraints, or `ORDER BY col LIMIT n` on a single column                      | B-tree                                                              |
| Returning many filtered rows in order                                                 | A covering B-tree, which lion can filter as it walks it (see below) |
| Write-heavy tables                                                                    | Measure first. Lion inserts are slower than B-tree.                 |
| Nearest neighbours of pgvector embeddings, especially under a `WHERE`                 | `lionvec`, from `pg_lion_vector`                                    |

Lion, B-tree and GIN indexes coexist happily. A typical setup keeps B-tree for primary keys and ordering and adds lion indexes on the columns your counts and filters use.

Counts are fastest right after `VACUUM`. Rows on pages the visibility map doesn't mark all-visible still have to be checked in the heap, so a frequently updated, rarely vacuumed table sees much less benefit.

## Installing

pg_lion needs PostgreSQL 16 or later. Build against your PostgreSQL's `pg_config`:

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

An index built while the library is preloaded needs the preload until it is rebuilt: without it, writes and VACUUM on its table fail. See [`wal_mode`](docs/REFERENCE.md#reloptions) before you turn it off, and [Upgrading](docs/REFERENCE.md#upgrading) when you install a newer build.

## A quick tour

The [usage guide](docs/GUIDE.md) covers each of these in detail.

### Counts and groups

```sql
CREATE INDEX events_country ON events USING lion (country);
CREATE INDEX events_type    ON events USING lion (event_type);
VACUUM ANALYZE events;

SELECT count(*) FROM events WHERE country = 'JP' AND event_type = 'purchase';
SELECT country, count(*) FROM events GROUP BY country;
SELECT country, count(*) FROM events GROUP BY country ORDER BY count(*) DESC LIMIT 10;
```

All three are answered from the indexes. Grouping by several columns, `count(DISTINCT col)` and partitioned tables work too.

### Ranges

```sql
CREATE INDEX events_ts ON events USING lion (created_at) WITH (summaries = auto);

SELECT count(*) FROM events WHERE created_at >= now() - interval '7 days';
```

With `summaries`, lion reads pre-merged bitmaps instead of every distinct value in the range, which matters on a column with many of them, such as a timestamp.

### Sums and extremes from bit slices

```sql
CREATE INDEX events_amount ON events USING lion (amount) WITH (slices = on);

SELECT sum(amount), avg(amount), max(amount) FROM events WHERE country = 'JP';
```

A sliced integer, date, time or timestamp column keeps one bitmap per bit of its values, so these are answered without reading rows. The price is on writes: inserts become several times slower.

### Joins

```sql
-- fact table orders, dimension customers; lion indexes on orders(customer_id) and orders(status)
SELECT count(*)
  FROM orders o
 WHERE o.status = 'open'
   AND EXISTS (SELECT 1 FROM customers c WHERE c.id = o.customer_id AND c.country = 'NZ');
```

Lion looks up each matching customer in the `customer_id` index and counts without visiting `orders`. JOIN, `IN` and `NOT EXISTS` forms work too.

### Returning rows

Lion tells you *which* rows match; to *return* them quickly, pair it with a covering B-tree:

```sql
CREATE INDEX docs_recent ON docs (created_at) INCLUDE (id, title);

SELECT id, title FROM docs
 WHERE tags @> ARRAY['postgres', 'index']
 ORDER BY created_at DESC LIMIT 10;
```

Lion walks the B-tree newest first, keeping only the entries whose rows are in the lion bitmap, and stops at the tenth (`Custom Scan (LionOrdered)`).

### Arrays and full-text search

```sql
CREATE INDEX docs_tags ON docs USING lion (tags);    -- any array
CREATE INDEX docs_tsv  ON docs USING lion (tsv);     -- tsvector

SELECT count(*) FROM docs WHERE tags @> '{postgres,index}';
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english', 'roaring & bitmap');
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english', 'roaring <-> bitmap & !btree');
```

Lion uses PostgreSQL's own operators and types, and returns exactly the rows a sequential scan or a GIN index would. A `tsvector` index stores word positions by default, so phrases and weights are answered from the index too.

### jsonb

```sql
CREATE INDEX docs_doc ON docs USING lion (doc);

SELECT count(*) FROM docs WHERE doc @> '{"status": "active", "tags": ["sale"]}';
SELECT count(*) FROM docs WHERE doc ? 'discount' AND doc ?| '{eu,uk}';
SELECT count(*) FROM docs WHERE doc @@ '$.items[*].sku == "A1"';
```

`@>`, `?`, `?|` and `?&` are generally answered from the index alone. jsonpath uses the same index to narrow the rows, and rechecks them in the heap, as GIN does. To filter or group on a known field, an expression index such as `USING lion ((doc->>'status'))` is smaller.

### Ranking with BM25

```sql
SELECT id, title, lion_bm25_score(tsv, q, 'docs_tsv') AS score
  FROM docs, to_tsquery('english', 'roaring | bitmap') q
 WHERE tsv @@ q
 ORDER BY lion_bm25_score(tsv, q, 'docs_tsv') DESC
 LIMIT 10;
```

A `tsvector` index can rank from the positions it stores, with Lucene's BM25. When enough rows hold the query's words, the planner finds the top rows from the index instead of scoring and sorting every match (`Custom Scan (LionBm25)`).

### Nearest neighbours

With [pgvector](https://github.com/pgvector/pgvector) 0.7 or later:

```sql
CREATE EXTENSION vector;
CREATE EXTENSION pg_lion_vector;

CREATE INDEX items_embedding ON items USING lionvec (embedding vector_cosine_ops);
CREATE INDEX items_category  ON items USING lion (category);

SELECT id FROM items ORDER BY embedding <=> $1 LIMIT 10;
SELECT id FROM items WHERE category = 3 ORDER BY embedding <=> $1 LIMIT 10;
```

Like HNSW, `lionvec` is approximate. Under a `WHERE` that lion indexes answer, it scores only the matching rows, so a filter of any selectivity still fills its `LIMIT`. The [guide](docs/GUIDE.md) has the distance function costs it assumes and what happens under a `WHERE` that no lion index answers.

## Supported types

Integers, floats, `numeric`, `bool`, `text`/`varchar`, `char(n)`, `bytea`, `uuid`, dates, times, timestamps, `interval`, `inet`, `macaddr`, `jsonb`, `pg_lsn`, `oid`, enums, any array and `tsvector`, with `citext` through `pg_lion_citext`. Indexes can have several columns. `lionvec` indexes pgvector's `vector` and `halfvec`.

## Limitations

- No unique indexes or `INCLUDE` columns. Use a B-tree for uniqueness, a plain `ORDER BY col LIMIT n`, and returning many rows in order.
- Heap tables only.
- With `pg_lion` preloaded, inserts are about 1.4× slower than B-tree, and more without it. Slices add much more.
- An index on an array, `tsvector` or default `jsonb` column can't drive a `GROUP BY`.
- Rows on pages that aren't all-visible are checked in the heap, and so is every row a jsonpath query finds.
- `lionvec` needs its distance function costs set (see the [guide](docs/GUIDE.md)), and a table with heavy churn needs an occasional `REINDEX`.
- Loading the library can change the plans of queries that have nothing to do with lion, through its estimates. Results don't change, and settings turn this off.

The [full list](docs/REFERENCE.md#known-limitations) has the details.

## Performance

| Query                                                       | Lion     | B-tree                          | GIN                            |
| ----------------------------------------------------------- | -------- | ------------------------------- | ------------------------------ |
| Count a value in 50% of rows                                | 0.21 ms  | 168 ms (lion 787× faster)       | 515 ms (lion 2,419× faster)    |
| Count a value in 0.5% of rows                               | 0.13 ms  | 1.67 ms (lion 13× faster)       | 22.6 ms (lion 174× faster)     |
| Count a value in 0.005% of rows                             | 0.025 ms | 0.035 ms (lion 1.4× faster)     | 0.27 ms (lion 11× faster)      |
| Two equality filters ANDed                                  | 0.43 ms  | 19.8 ms (lion 46× faster)       | 205 ms (lion 477× faster)      |
| Count per group, 200 groups                                 | 21.6 ms  | 443 ms (lion 21× faster)        | 843 ms (lion 39× faster)       |
| Count a small range                                         | 0.88 ms  | 1.70 ms (lion 1.9× faster)      | 288 ms (lion 328× faster)      |
| Count a value in 50% of rows, 1% of rows updated, no VACUUM | 86.2 ms  | 372 ms (lion 4.3× faster)       | 525 ms (lion 6.1× faster)      |
| Count per group, 200 groups, 1% of rows updated, no VACUUM  | 334 ms   | 854 ms (lion 2.6× faster)       | 876 ms (lion 2.6× faster)      |
| Fetch the rows matching two filters                         | 11.9 ms  | 13.8 ms (lion 1.2× faster)      | 30.8 ms (lion 2.6× faster)     |
| Two filters, `ORDER BY` another column, `LIMIT 10`          | 1.31 ms  | 4.99 ms (lion 3.8× faster)      |                                |
| `WHERE c >= .. ORDER BY c LIMIT 100`                        | 0.073 ms | 0.020 ms (lion **3.6× slower**) |                                |
| Full-text count, `'w1 & w17'` (200k docs)                   | 0.075 ms |                                 | 1.14 ms (lion 15× faster)      |
| Full-text count, phrase `'common <-> w1'`                   | 2.52 ms  |                                 | 7.60 ms (lion 3.0× faster)     |
| Full-text count, prefix `'rare12:*'`                        | 0.63 ms  |                                 | 1.72 ms (lion 2.7× faster)     |
| Top 10 by BM25 vs `ts_rank`, 33k matches (500k docs)        | 6.88 ms  |                                 | 97.6 ms (lion 14× faster)      |
| Top 10 by BM25 vs `ts_rank`, 584 matches                    | 3.74 ms  |                                 | 3.25 ms (lion **1.2× slower**) |

On the 5M-row table the lion indexes are smaller than both the B-tree and the GIN ones and build in under half B-tree's time. A `tsvector` index with positions is larger than GIN's. The [benchmark page](docs/BENCHMARKS.md) has every query, the setup and how to reproduce it.

## Documentation

- [Usage guide](docs/GUIDE.md): every query shape in detail, and what to look for in `EXPLAIN`.
- [Reference](docs/REFERENCE.md): SQL functions, operators, index options, settings, limitations.
- [Benchmarks](docs/BENCHMARKS.md): measurements and how to reproduce them.
- [Developing](docs/DEVELOPING.md): building, the test suites, source layout.
- [DESIGN.md](DESIGN.md): the on-disk format, locking, crash safety and planner design. It's long.

## License

Copyright (c) 2026, Chinmay Kanchi. Licensed under the [PostgreSQL License](LICENSE).
