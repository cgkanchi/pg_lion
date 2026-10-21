-- `(doc ->> 'k') = 'v'` as the `doc @> '{"k": "v"}'` it is the same test as,
-- whenever v is a string no other JSON value prints as (DESIGN.md §42.6).
-- Every query runs through the count pushdown with every other scan
-- disabled and through a sequential scan with the pushdown off, and the two
-- results are compared as multisets both ways round.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_jf() says whether the count was pushed down, whether a clause was left
 * to the heap (a residual filter), and the result: the row when there is
 * one, or how many.  With args, q is PREPAREd and EXECUTEd under a generic
 * plan.
 */
CREATE FUNCTION lion_jf(q text, args text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	ex text := q;
	pushed boolean := false;
	residual boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	IF args IS NOT NULL THEN
		EXECUTE 'PREPARE lion_jf_q AS ' || q;
		ex := 'EXECUTE lion_jf_q(' || args || ')';
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || ex LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		ELSIF ln LIKE '%Residual Filter%' THEN
			residual := true;
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lion_jf_on AS ' || ex;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'CREATE TEMP TABLE lion_jf_off AS ' || ex;
	IF args IS NOT NULL THEN
		EXECUTE 'DEALLOCATE lion_jf_q';
	END IF;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('plan_cache_mode', 'auto', true);

	EXECUTE 'SELECT count(*) FROM lion_jf_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_jf_on EXCEPT ALL SELECT * FROM lion_jf_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_jf_off EXCEPT ALL SELECT * FROM lion_jf_on) b)'
		INTO ndiff;
	IF nrows = 1 THEN
		EXECUTE 'SELECT s::text FROM lion_jf_off s' INTO ln;
	ELSE
		ln := nrows || ' rows';
	END IF;
	EXECUTE 'DROP TABLE lion_jf_on, lion_jf_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	IF NOT pushed THEN
		RETURN 'not pushed: ' || ln;
	END IF;
	IF residual THEN
		RETURN 'LionCount with a residual filter: ' || ln;
	END IF;
	RETURN 'LionCount: ' || ln;
END $$;

/* The plan lion_jf() runs, without 18's "Disabled: true" lines. */
CREATE FUNCTION lion_jf_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

-- ---- the data ----------------------------------------------------------
-- Field s is, by id % 23: the string 'v1' and other strings, among them
-- strings spelled like a number, a boolean, an object and an array, 'v1'
-- padded and in capitals; the number 1, true, null, an array and an object
-- holding 'v1'; a document without s, one with s only below another key, a
-- top-level array, a raw scalar string, and NULL.  The key `q"k` needs an
-- escape, and so do the values under it.
CREATE TABLE lion_jf_t (id int NOT NULL, k int NOT NULL, doc jsonb);
INSERT INTO lion_jf_t
SELECT i, i % 5,
	   CASE i % 23
			WHEN 0 THEN NULL
			WHEN 1 THEN '{"s": "v1", "o": 1}'
			WHEN 2 THEN '{"s": "v2"}'
			WHEN 3 THEN '{"s": "1"}'
			WHEN 4 THEN '{"s": 1}'
			WHEN 5 THEN '{"s": 1.0}'
			WHEN 6 THEN '{"s": true}'
			WHEN 7 THEN '{"s": "true"}'
			WHEN 8 THEN '{"s": null}'
			WHEN 9 THEN '{"s": ["v1"]}'
			WHEN 10 THEN '{"s": {"v1": 1}}'
			WHEN 11 THEN '{"s": "{\"v1\": 1}"}'
			WHEN 12 THEN '{"s": "[1]"}'
			WHEN 13 THEN '{"s": " v1"}'
			WHEN 14 THEN '{"s": "V1"}'
			WHEN 15 THEN '{"a": {"s": "v1"}}'
			WHEN 16 THEN '[{"s": "v1"}]'
			WHEN 17 THEN '"v1"'
			WHEN 18 THEN '{}'
			WHEN 19 THEN '{"q\"k": "x\"y\\z", "s": "v1"}'
			WHEN 20 THEN '{"q\"k": "tab\there", "s": "NaN"}'
			WHEN 21 THEN '{"s": "é ü"}'
			ELSE '{"s": "v1", "s": "v2"}'
	   END::jsonb
  FROM generate_series(1, 4600) i;
CREATE INDEX lion_jf_k ON lion_jf_t USING lion (k);
CREATE INDEX lion_jf_doc ON lion_jf_t USING lion (doc);
VACUUM (FREEZE, ANALYZE) lion_jf_t;

-- ---- taken: the containment ----------------------------------------------
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'v1'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND 'v1' = (doc ->> 's')$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'v2'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = ' v1'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'V1'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'é ü'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'v1'::varchar$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'v1' COLLATE "C"$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 'q"k') = 'x"y\z'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 'q"k') = E'tab\there'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'nothing'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE (doc ->> 's') = 'v1'$$);
-- strings that only look like other values are still strings
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = '1x'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'True'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = ' {"v1": 1}'$$);
-- in a filtered count, beside the document's own tests
SELECT lion_jf($$SELECT count(*) AS n,
						count(*) FILTER (WHERE doc IS NOT NULL AND doc ? 's' AND (doc ->> 's') = 'v1') AS f
				   FROM lion_jf_t WHERE k = 2$$);
SELECT lion_jf($$SELECT k, count(*) FILTER (WHERE (doc ->> 's') = 'v2') FROM lion_jf_t GROUP BY k$$);
SELECT lion_jf($$SELECT count(*) AS n,
						count(*) FILTER (WHERE doc IS NOT NULL AND doc ? 's' AND (doc ->> 's') = 'v1') AS f
				   FROM lion_jf_t WHERE k = $1$$, '2');
-- an OR, a GROUP BY
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE (doc ->> 's') = 'v1' OR (doc ->> 's') = 'v2'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 1 OR (doc ->> 's') = 'v2'$$);
SELECT lion_jf($$SELECT k, count(*) FROM lion_jf_t WHERE (doc ->> 's') = 'v1' GROUP BY k$$);
SELECT lion_jf_plan($$SELECT count(*), count(*) FILTER (WHERE doc ? 's' AND (doc ->> 's') = 'v1')
						FROM lion_jf_t WHERE k = 2$$);

-- ---- declined: left to the heap in a WHERE, the count in a FILTER -------
-- a value a number, a boolean or a container prints as
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = '1'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = '1.0'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'true'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = '{"v1": 1}'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = '[1]'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'NaN'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = ' 1'$$);
-- a value known only at run time, `<>`, NOT, a list, `->`
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = $1$$, $$'v1'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') <> 'v1'$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND NOT ((doc ->> 's') = 'v1')$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') IN ('v1', 'v2')$$);
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc -> 's') = '"v1"'$$);
SELECT lion_jf($$SELECT count(*) AS n, count(*) FILTER (WHERE (doc ->> 's') = '1') AS f
				   FROM lion_jf_t WHERE k = 2$$);
SELECT lion_jf_plan($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'true'$$);

-- ---- after writes ---------------------------------------------------------
UPDATE lion_jf_t SET doc = '{"s": "v1"}' WHERE id % 23 = 4 AND id % 2 = 0;
UPDATE lion_jf_t SET doc = '{"s": 1}' WHERE id % 23 = 1 AND id % 3 = 0;
DELETE FROM lion_jf_t WHERE id % 23 = 22 AND id % 4 = 0;
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'v1'$$);
SELECT lion_jf($$SELECT k, count(*) FILTER (WHERE (doc ->> 's') = 'v1') FROM lion_jf_t GROUP BY k$$);
VACUUM (FREEZE, ANALYZE) lion_jf_t;
SELECT lion_jf($$SELECT count(*) FROM lion_jf_t WHERE k = 2 AND (doc ->> 's') = 'v1'$$);

DROP TABLE lion_jf_t;
DROP FUNCTION lion_jf(text, text);
DROP FUNCTION lion_jf_plan(text);
