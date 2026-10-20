-- The intersection probe on wide filters (DESIGN.md §29.11, "Wide
-- filters"): equalities and IN lists on columns that all follow one hidden
-- attribute, each list value a posting set of its own, so that the probe
-- seeks many sets at every container it samples.
--
-- A family of rows - one in sixteen, spread over the whole heap - shares the
-- values of six columns' clauses; the part of the family loaded in the first
-- quarter of the heap shares the values of three more.  The filter of the
-- nine is that part: its rows lie in a quarter of the heap's containers, and
-- core's product of the nine clauses is a fraction of a row.  The probe
-- samples LION_ISECT_KEYS of the driver's containers however many sets the
-- filter seeks, and its budget grows with them: a sample of
-- LION_ISECT_MIN_KEYS would meet the part's quarter in two containers or so,
-- too few to raise the estimate further than the rows they hold.  A filter of
-- more sets than a probe seeks is measured without its widest list.  A sample
-- that meets the whole AND in too few containers raises it as far as the
-- longest part of the AND that it met often says, and as the rows of the
-- whole it met allow.
--
-- ANALYZE samples the whole table, so its statistics - and the plans - are
-- the same on every run.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 160,000 rows, about 22 to a page: 119 container keys on a 64-bit build.
-- u[1 .. 13] are the row's draws, each 0 to 999.  fam: one row in sixteen;
-- sub: half of the family's rows in the first quarter of the heap; mid: the
-- family's rows in a stretch of under a twentieth of it.  a, b, d, e, f, g are
-- the family's values for the family, and any of their domain's for the
-- rest; c, h and k are sub's for sub, values outside sub's for the rest of
-- the family, and any for the rest; m is 1 for mid and for most rows of no
-- family, 2 for the rest of the family; j follows nothing.
CREATE TABLE iw (id int NOT NULL, a int NOT NULL, b int NOT NULL,
				 c int NOT NULL, d int NOT NULL, e int NOT NULL,
				 f int NOT NULL, g int NOT NULL, h int NOT NULL,
				 k int NOT NULL, m int NOT NULL, j int NOT NULL)
	WITH (fillfactor = 20, autovacuum_enabled = off);
INSERT INTO iw
SELECT i,
	   CASE WHEN fam THEN 1 ELSE 1 + u[3] * 5 / 1000 END,
	   CASE WHEN fam THEN 1 + u[4] * 2 / 1000 ELSE 1 + u[4] * 8 / 1000 END,
	   CASE WHEN sub THEN 1 WHEN fam THEN 2 + u[5] * 9 / 1000
			ELSE 1 + u[5] * 10 / 1000 END,
	   CASE WHEN fam THEN 1 + u[6] * 2 / 1000 ELSE 1 + u[6] * 20 / 1000 END,
	   CASE WHEN fam THEN 1 + u[7] * 3 / 1000 ELSE 1 + u[7] * 24 / 1000 END,
	   CASE WHEN fam THEN 1 + u[8] * 5 / 1000 ELSE 1 + u[8] * 30 / 1000 END,
	   CASE WHEN fam THEN 1 + u[9] * 7 / 1000 ELSE 1 + u[9] * 32 / 1000 END,
	   CASE WHEN sub THEN 1 + u[10] * 7 / 1000 WHEN fam THEN 8 + u[10] * 30 / 1000
			ELSE 1 + u[10] * 36 / 1000 END,
	   CASE WHEN sub THEN 1 + u[11] * 3 / 1000 WHEN fam THEN 4 + u[11] * 32 / 1000
			ELSE 1 + u[11] * 40 / 1000 END,
	   CASE WHEN mid THEN 1 WHEN fam THEN 2 WHEN u[13] < 600 THEN 1 ELSE 2 END,
	   1 + u[12] * 20 / 1000
  FROM (SELECT i, u, u[1] < 1000 / 16 AS fam,
			   u[1] < 1000 / 16 AND i < 40000 AND u[2] < 500 AS sub,
			   u[1] < 1000 / 16 AND i >= 80000 AND i < 86000 AS mid
		  FROM (SELECT i, ARRAY(SELECT (hashint8extended(i, s) & 2147483647) % 1000
								  FROM generate_series(1, 13) s) AS u
				  FROM generate_series(0, 159999) i) s0) s1
 ORDER BY i;
CREATE INDEX iw_i ON iw USING lion (a, b, c, d, e, f, g, h, k, m, j);
VACUUM (FREEZE, ANALYZE) iw;
SELECT pg_relation_size('iw') / 8192 / 64 BETWEEN 100 AND 140 AS container_keys;

/*
 * iw_rows() is the row estimate of the Bitmap Index Scan of iw_i that answers
 * q - amcostestimate()'s selectivity, lion's own estimate - with the probe on
 * or off; iw_relrows() the relation's (a sequential scan's rows, which the
 * rows correction sets from the probe).
 */
CREATE FUNCTION iw_rows(q text, probe boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('pg_lion.enable_intersection_probe',
					   CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_intersection_probe', 'on', true);
	RETURN jsonb_path_query_first(j::jsonb,
		'$.** ? (@."Node Type" == "Bitmap Index Scan" && @."Index Name" == "iw_i")."Plan Rows"')::text::bigint;
END $$;

CREATE FUNCTION iw_relrows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN (j->0->'Plan'->>'Plan Rows')::bigint;
END $$;

/* The rows q returns, read by a sequential scan: the reference answer. */
CREATE FUNCTION iw_actual(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'SELECT count(*) FROM (' || q || ') s' INTO n;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN n;
END $$;

/* The buffers planning q accessed, with the probe on or off. */
CREATE FUNCTION iw_planbuf(q text, probe boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('pg_lion.enable_intersection_probe',
					   CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	EXECUTE 'EXPLAIN (FORMAT JSON, BUFFERS) ' || q INTO j;
	PERFORM set_config('pg_lion.enable_intersection_probe', 'on', true);
	RETURN coalesce((j->0->'Planning'->>'Shared Hit Blocks')::bigint, 0) +
		coalesce((j->0->'Planning'->>'Shared Read Blocks')::bigint, 0) +
		coalesce((j->0->'Planning'->>'Local Hit Blocks')::bigint, 0) +
		coalesce((j->0->'Planning'->>'Local Read Blocks')::bigint, 0);
END $$;

-- ---------- 1. the filter widened, column by column ----------
-- sets: the posting sets the conjunction seeks, a list value each.  20: the
-- family, a sixteenth of the rows; 28 and 31: the part of it in the first
-- quarter of the heap, whose AND lies in a quarter of the driver's
-- containers.  Core's product is far short of every one; the probe's
-- estimate, and the relation's rows with it, are within a factor of three of
-- the rows.
CREATE TABLE iw_q (sets int, q text);
INSERT INTO iw_q VALUES
	(20, $$SELECT id FROM iw WHERE a = 1 AND b IN (1, 2) AND d IN (1, 2) AND e IN (1, 2, 3)
		   AND f IN (1, 2, 3, 4, 5) AND g IN (1, 2, 3, 4, 5, 6, 7)$$),
	(28, $$SELECT id FROM iw WHERE a = 1 AND b IN (1, 2) AND c = 1 AND d IN (1, 2) AND e IN (1, 2, 3)
		   AND f IN (1, 2, 3, 4, 5) AND g IN (1, 2, 3, 4, 5, 6, 7) AND h IN (1, 2, 3, 4, 5, 6, 7)$$),
	(31, $$SELECT id FROM iw WHERE a = 1 AND b IN (1, 2) AND c = 1 AND d IN (1, 2) AND e IN (1, 2, 3)
		   AND f IN (1, 2, 3, 4, 5) AND g IN (1, 2, 3, 4, 5, 6, 7) AND h IN (1, 2, 3, 4, 5, 6, 7)
		   AND k IN (1, 2, 3)$$);
SELECT sets, iw_actual(q) AS actual,
	   iw_rows(q, true) BETWEEN iw_actual(q) / 3 AND iw_actual(q) * 3 AS probed_within_3x,
	   iw_rows(q, false) * 10 < iw_actual(q) AS core_far_short,
	   iw_relrows(q) = iw_rows(q, true) AS relation_agrees
  FROM iw_q ORDER BY sets;

-- ---------- 2. past the bounds ----------
-- 41 sets, past LION_ISECT_MAX_SETS: measured without the widest list, j's
-- ten values, which is taken as independent of the rest, as core takes it -
-- and it is: half of the part's rows.
INSERT INTO iw_q VALUES
	(41, $$SELECT id FROM iw WHERE a = 1 AND b IN (1, 2) AND c = 1 AND d IN (1, 2) AND e IN (1, 2, 3)
		   AND f IN (1, 2, 3, 4, 5) AND g IN (1, 2, 3, 4, 5, 6, 7) AND h IN (1, 2, 3, 4, 5, 6, 7)
		   AND k IN (1, 2, 3) AND j IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)$$);
-- 21 sets whose AND - the family's rows in under a twentieth of the heap -
-- the sample meets in two containers: too few to say how many rows it holds.
-- m, of the most rows, is ANDed last, and the AND of the others, the
-- family's, met in every container, measures them; that m keeps under a
-- twentieth of them where core says more than half is what the rows of the
-- whole AND the sample met allow, and no more, so the estimate is within a
-- factor of three of the rows, not the rows the two containers held.
INSERT INTO iw_q VALUES
	(21, $$SELECT id FROM iw WHERE a = 1 AND b IN (1, 2) AND d IN (1, 2) AND e IN (1, 2, 3)
		   AND f IN (1, 2, 3, 4, 5) AND g IN (1, 2, 3, 4, 5, 6, 7) AND m = 1$$);
SELECT sets, iw_actual(q) AS actual,
	   iw_rows(q, true) BETWEEN iw_actual(q) / 3 AND iw_actual(q) * 3 AS probed_within_3x,
	   iw_rows(q, false) * 10 < iw_actual(q) AS core_far_short,
	   iw_relrows(q) = iw_rows(q, true) AS relation_agrees
  FROM iw_q WHERE sets IN (21, 41) ORDER BY sets;

-- ---------- 3. what it costs ----------
-- A planner run's probes access at most LION_ISECT_RUN_BUFFERS beside what
-- LION_ISECT_KEYS samples of its widest conjunction cost at two buffers a
-- seek of a set: 3,072 for LION_ISECT_MAX_SETS sets.  Planning the 31 sets
-- reads more with the probe than without it, and stays within that.  So does
-- planning twelve such conjunctions of 32 sets in one statement, each its own
-- probe: the run makes LION_ISECT_RUN_PROBES of them at most.
SELECT iw_planbuf(q, true) > iw_planbuf(q, false) AS probed,
	   iw_planbuf(q, true) < 3072 AS bounded
  FROM iw_q WHERE sets = 31;
SELECT iw_planbuf(string_agg(format('(%s)', replace(q, 'k IN (1, 2, 3)',
												   format('k IN (1, 2, 3, %s)', 4 + n))),
						 ' UNION ALL '), true) < 3072 AS bounded
  FROM iw_q, generate_series(0, 11) n WHERE sets = 31;

-- and the answers are the sequential scan's
SELECT sets, (SELECT count(*) FROM iw WHERE a = 1 AND b IN (1, 2) AND c = 1 AND d IN (1, 2)
			  AND e IN (1, 2, 3) AND f IN (1, 2, 3, 4, 5) AND g IN (1, 2, 3, 4, 5, 6, 7)
			  AND h IN (1, 2, 3, 4, 5, 6, 7) AND k IN (1, 2, 3)) AS counted,
	   iw_actual(q) AS actual
  FROM iw_q WHERE sets = 31;

-- ---------- 4. one table, several lion indexes ----------
-- A conjunction is measured for its table, not for an index: every lion
-- index of iw that answers it takes the one measurement, whichever asked
-- first.  Three indexes of iw's columns, and three of the 32-set
-- conjunctions in one statement: measured once an index, the run's eight
-- probes would run out at the third conjunction's third index, whose path,
-- priced for core's product of a fraction of a row, would be the cheapest,
-- under a relation of the probe's rows.  Each conjunction's Bitmap Index
-- Scan, whichever index it takes, is estimated at its Bitmap Heap Scan's
-- rows, and both within a factor of three of the rows.
CREATE INDEX iw_i2 ON iw USING lion (k, h, g, f, e, d, c, b, a, m, j);
CREATE INDEX iw_i3 ON iw USING lion (j, m, a, b, c, d, e, f, g, h, k);
CREATE FUNCTION iw_scans(q text, OUT heap_rows bigint, OUT index_rows bigint)
RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN QUERY
		SELECT (s->>'Plan Rows')::bigint, (s->'Plans'->0->>'Plan Rows')::bigint
		  FROM jsonb_path_query(j::jsonb,
				'$.** ? (@."Node Type" == "Bitmap Heap Scan")') s;
END $$;
CREATE TABLE iw_k AS
SELECT n, replace(q, 'k IN (1, 2, 3)', format('k IN (1, 2, 3, %s)', 4 + n)) AS q
  FROM iw_q, generate_series(0, 2) n WHERE sets = 31;
SELECT b.n + 1 AS conjunction, iw_actual(b.q) AS actual,
	   s.index_rows = s.heap_rows AS index_agrees,
	   s.heap_rows BETWEEN iw_actual(b.q) / 3 AND iw_actual(b.q) * 3 AS within_3x
  FROM iw_k b
  JOIN iw_scans((SELECT string_agg(format('(%s)', q), ' UNION ALL ' ORDER BY n)
				   FROM iw_k)) WITH ORDINALITY s ON s.ordinality = b.n + 1
 ORDER BY b.n;

DROP TABLE iw, iw_q, iw_k;
DROP FUNCTION iw_rows(text, boolean), iw_relrows(text), iw_actual(text),
	iw_planbuf(text, boolean), iw_scans(text);
RESET default_statistics_target;
