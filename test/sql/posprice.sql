-- The price of a position filter's candidates (DESIGN.md §17, "Stored
-- positions", "The price"): the rows of the clause's superset, which the
-- filter decides from stored positions, estimated over the rows whose column
-- is not NULL, with the AND of its lexemes as the intersection probe
-- measures it however near core's product, and in a partition's own
-- columns.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * The same 10,000 rows three times: alone, with 30,000 rows of a NULL
 * column between them, and in two partitions.  `foo` is in every row and
 * `bar` in seven of eight, after `foo`; `new york` in a tenth.
 */
CREATE TABLE pq_plain (id int, d tsvector) WITH (autovacuum_enabled = off);
INSERT INTO pq_plain
SELECT i, to_tsvector('simple',
	'foo ' || CASE WHEN i % 8 = 0 THEN 'baz' ELSE 'bar' END || ' w' || (i % 97) ||
	CASE WHEN i % 10 = 0 THEN ' new york' ELSE '' END)
FROM generate_series(1, 10000) i;
CREATE TABLE pq_null (id int, d tsvector) WITH (autovacuum_enabled = off);
INSERT INTO pq_null
SELECT i, CASE WHEN i % 4 = 0 THEN to_tsvector('simple',
	'foo ' || CASE WHEN i / 4 % 8 = 0 THEN 'baz' ELSE 'bar' END || ' w' || (i / 4 % 97) ||
	CASE WHEN i / 4 % 10 = 0 THEN ' new york' ELSE '' END) END
FROM generate_series(1, 40000) i;
CREATE TABLE pq_part (id int, d tsvector) PARTITION BY RANGE (id);
CREATE TABLE pq_part1 PARTITION OF pq_part FOR VALUES FROM (0) TO (5000)
	WITH (autovacuum_enabled = off);
CREATE TABLE pq_part2 PARTITION OF pq_part FOR VALUES FROM (5000) TO (20000)
	WITH (autovacuum_enabled = off);
INSERT INTO pq_part SELECT * FROM pq_plain;
CREATE INDEX pq_plain_d ON pq_plain USING lion (d) WITH (store_positions = on);
CREATE INDEX pq_null_d ON pq_null USING lion (d) WITH (store_positions = on);
CREATE INDEX pq_part_d ON pq_part USING lion (d) WITH (store_positions = on);
-- every row in the sample
ALTER TABLE pq_null ALTER d SET STATISTICS 1000;
VACUUM (FREEZE, ANALYZE) pq_plain, pq_null, pq_part1, pq_part2;
ANALYZE pq_part;

-- what a bitmap scan of `d @@ q` is priced at, its position filter with it
CREATE FUNCTION pq_cost(t text, q text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	e jsonb;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('EXPLAIN (FORMAT JSON) SELECT * FROM %s WHERE d @@ %L::tsquery',
				   t, q) INTO e;
	RETURN (SELECT sum((x->>'Total Cost')::float8)
			FROM jsonb_path_query(e,
				'strict $.** ? (@."Node Type" == "Bitmap Index Scan")') x);
END $$;

/*
 * A phrase's filter - its price beyond the AND of its lexemes, which the
 * posting sets answer alone - decides the same candidates with the NULLs as
 * without them.  Each operand's share of the rows, as core estimates it,
 * carries the NULLs once: combined as they were, a phrase of two lexemes
 * came out at three times the price on the table with the NULLs.
 */
SELECT q,
	   round(((pq_cost('pq_null', q) - pq_cost('pq_null', a)) /
			  (pq_cost('pq_plain', q) - pq_cost('pq_plain', a)))::numeric, 2) AS nulls
FROM (VALUES ('new <-> york', 'new & york'), ('foo <-> bar', 'foo & bar')) v(q, a);

/*
 * ... and under a NOT, where the superset is `foo` for `foo <-> !bar` and
 * `bar` for `!foo <-> bar`.  A NOT took the NULLs the other way: the first
 * came out at two fifths of the price and the second at a tenth.  Over the
 * rows that are not NULL the second, of `foo` in every row, is estimated at
 * next to nothing, and its candidates are no more than its superset's rows,
 * the rows with `bar`: widened from next to nothing, they were every row of
 * the table, the NULL ones too.
 */
SELECT q, round((pq_cost('pq_null', q) / pq_cost('pq_plain', q))::numeric, 1) AS nulls
FROM unnest(ARRAY['foo <-> !bar', '!foo <-> bar']) q;

/*
 * A count of the partitioned table prices each partition's filter with the
 * partitioned table's clause, which the planner hands it in the partition's
 * own columns: the intersection probe looks its lexemes up in the
 * partition's index, whose keys a clause in the partitioned table's columns
 * matched none of, and the correlated phrase came out at a third of the
 * price, its candidates core's product of its lexemes, a tenth of them.
 * Every core plan off, the count is priced in lion's own units.
 */
CREATE FUNCTION pq_count(t text, q text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	e jsonb;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('EXPLAIN (FORMAT JSON) SELECT count(*) FROM %s WHERE d @@ %L::tsquery',
				   t, q) INTO e;
	RETURN (SELECT sum((x->>'Total Cost')::float8)
			FROM jsonb_path_query(e,
				'strict $.** ? (@."Custom Plan Provider" == "LionCount")') x);
END $$;
SELECT round(((pq_count('pq_part', 'new <-> york') - pq_count('pq_part', 'new & york')) /
			  (pq_count('pq_plain', 'new <-> york') -
			   pq_count('pq_plain', 'new & york')))::numeric, 2) AS partitioned;

/*
 * The plan that makes: 20,000 rows over 2,000 keys, ten a key spread over
 * the heap, `alpha beta` in two of five and a btree on the key.  The AND of
 * `alpha` and `beta` holds 2.5 times core's product of them, less than the
 * three times a rows estimate takes from the intersection probe (DESIGN.md
 * §29.11): priced over that product, the FK-side join's counts at three
 * keys, each deciding its key's four candidates from both lexemes' 8,000
 * members, would be taken over a nested loop that fetches the keys' 30 rows and
 * tests them, ten times as fast.  At a hundred keys the join collects the
 * filter once and is the plan.
 */
CREATE TABLE pq_fact (id int, fk int, d tsvector) WITH (autovacuum_enabled = off);
INSERT INTO pq_fact
SELECT i, 1 + (i * 7919) % 2000,
	   to_tsvector('simple', CASE WHEN (i / 2000) % 5 < 2 THEN 'alpha beta w' || (i % 97)
								  ELSE 'gamma w' || (i % 97) END)
FROM generate_series(1, 20000) i;
CREATE INDEX pq_fact_dfk ON pq_fact USING lion (d, fk) WITH (store_positions = on);
CREATE INDEX pq_fact_fk ON pq_fact (fk);
CREATE TABLE pq_dim (pk int PRIMARY KEY, name text);
INSERT INTO pq_dim SELECT g, 'n' || (g % 4) FROM generate_series(1, 2000) g;
VACUUM (FREEZE, ANALYZE) pq_fact, pq_dim;
-- PostgreSQL 19 may aggregate below the join instead (eager aggregation)
DO $$BEGIN IF EXISTS (SELECT 1 FROM pg_settings WHERE name = 'enable_eager_aggregate') THEN PERFORM set_config('enable_eager_aggregate', 'off', false); END IF; END$$;
CREATE FUNCTION pq_plan(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	e jsonb;
	j text;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || q INTO e;
	IF jsonb_path_exists(e, '$.** ? (@."Custom Plan Provider" == "LionCount")') THEN
		RETURN 'LionCount';
	END IF;
	j := jsonb_path_query_first(e,
		'$.** ? (@."Node Type" == "Nested Loop" || @."Node Type" == "Hash Join" || @."Node Type" == "Merge Join")."Node Type"') #>> '{}';
	RETURN coalesce(j, 'no join');
END $$;
SELECT k, pq_plan(format($q$SELECT count(*) FROM pq_fact f JOIN pq_dim d
	ON f.fk = d.pk WHERE d.pk <= %s AND f.d @@ 'alpha <-> beta'$q$, k)) AS plan
FROM (VALUES (3), (100)) v(k);
SELECT count(*) FROM pq_fact f JOIN pq_dim d ON f.fk = d.pk
	WHERE d.pk <= 3 AND f.d @@ 'alpha <-> beta';
DO $$BEGIN IF EXISTS (SELECT 1 FROM pg_settings WHERE name = 'enable_eager_aggregate') THEN EXECUTE 'RESET enable_eager_aggregate'; END IF; END$$;

DROP FUNCTION pq_plan(text);
DROP FUNCTION pq_cost(text, text);
DROP FUNCTION pq_count(text, text);
DROP TABLE pq_plain, pq_null, pq_part, pq_fact, pq_dim;
