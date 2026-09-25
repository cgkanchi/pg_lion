-- Two million rows: multi-page container chains, bitset containers for the
-- low-cardinality columns and long runs for the clustered one.  Takes a few
-- seconds.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

SELECT setseed(0.42);

CREATE TABLE lion_large AS
SELECT i,
       (i / 20000)::int4                     AS clustered,   -- 100 long runs
       ((i::bigint * 2654435761) % 4)::int4          AS c4,          -- 4 dense values
       ((i::bigint * 2654435761) % 977)::int4        AS c977,
       'k' || ((i::bigint * 48271) % 20000)          AS c20k,
       (random() * 7)::int4                  AS r8
  FROM generate_series(1, 2000000) i;

CREATE INDEX lion_large_clustered ON lion_large USING lion (clustered);
CREATE INDEX lion_large_c4 ON lion_large USING lion (c4);
CREATE INDEX lion_large_c977 ON lion_large USING lion (c977);
CREATE INDEX lion_large_c20k ON lion_large USING lion (c20k);
CREATE INDEX lion_large_r8 ON lion_large USING lion (r8) WITH (inline_limit = 64);
ANALYZE lion_large;

CREATE OR REPLACE FUNCTION lion_cmp(tbl text, pred text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT count(*) AS c, coalesce(sum(i), 0) AS s FROM %s WHERE %s', tbl, pred);
	a record;
	b record;
	ln text;
	used boolean := false;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', false);
	PERFORM set_config('enable_bitmapscan', 'on', false);
	PERFORM set_config('enable_indexscan', 'off', false);	-- the bitmap path (§29: a plain scan could be chosen)
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Bitmap Index Scan%' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE q INTO a;

	PERFORM set_config('enable_seqscan', 'on', false);
	PERFORM set_config('enable_bitmapscan', 'off', false);
	EXECUTE q INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', false);
	PERFORM set_config('enable_indexscan', 'on', false);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH index=%s seqscan=%s', a::text, b::text);
	END IF;
	IF NOT used THEN
		RETURN format('NO BITMAP INDEX SCAN (%s rows)', a.c);
	END IF;
	RETURN format('ok %s rows', a.c);
END $$;

-- clustered: every key is one long run, so its posting set is tiny
SELECT lion_cmp('lion_large', 'clustered = 0');
SELECT lion_cmp('lion_large', 'clustered = 50');
SELECT lion_cmp('lion_large', 'clustered = 100');
SELECT lion_cmp('lion_large', 'clustered = 101');

-- four dense values: bitset containers, long chains
SELECT lion_cmp('lion_large', 'c4 = 0');
SELECT lion_cmp('lion_large', 'c4 = 3');

-- medium and high cardinality
SELECT lion_cmp('lion_large', 'c977 = 1');
SELECT lion_cmp('lion_large', 'c977 = 976');
SELECT lion_cmp('lion_large', 'c20k = ''k0''');
SELECT lion_cmp('lion_large', 'c20k = ''k19999''');
SELECT lion_cmp('lion_large', 'c20k = ''k4242''');
SELECT lion_cmp('lion_large', 'r8 = 5');

-- combinations
SELECT lion_cmp('lion_large', 'c4 = 1 AND c977 = 5');
SELECT lion_cmp('lion_large', 'c977 IN (1, 2, 3)');
SELECT lion_cmp('lion_large', 'clustered = 7 AND c4 = 2');

-- A run-encoded posting set for 20000 rows must be far smaller than a btree.
SELECT pg_relation_size('lion_large_clustered') < pg_relation_size('lion_large_c4')
	AS clustered_index_is_smallest;
SELECT pg_relation_size('lion_large_clustered') < 1024 * 1024 AS clustered_under_1mb;

DROP TABLE lion_large;
