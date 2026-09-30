-- A list on the column LionOrdered walks (DESIGN.md §30.11, "Lists").
--
-- `g IN (...) ... ORDER BY g LIMIT n` over a lion column's own order: the
-- walk descends to each of the list's values in turn and walks only their
-- entries, where it used to leave the list to the set and walk the column
-- from its first entry.  Every answer is compared, IN ORDER, with the plan
-- the planner makes with pg_lion.enable_ordered_scan off; every ORDER BY ends
-- in the unique id, which core sorts incrementally above the walk.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- g: 50 values, a: 20, b: 30, none of them in heap order; n: NULL on every
-- 11th row; ts: nearly unique; k: a permutation of the ids, with a btree,
-- in no heap order either; tags: a multi-key column.  One lion index over
-- the filter columns, one over ts, one over tags.
CREATE TABLE lol (id int PRIMARY KEY, g text, a text, b text, n int,
				  ts timestamptz, k int, tags text[], pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO lol
SELECT i,
	   'g' || lpad(((hashint8extended(i::bigint, 1) & 9223372036854775807) % 50)::text, 2, '0'),
	   'a' || ((hashint8extended(i::bigint, 2) & 9223372036854775807) % 20),
	   'b' || ((hashint8extended(i::bigint, 3) & 9223372036854775807) % 30),
	   CASE WHEN i % 11 = 0 THEN NULL ELSE i % 13 END,
	   timestamptz '2026-01-01 00:00+00' + ((i::bigint * 7919) % 60013) * interval '1 minute',
	   (i::bigint * 7919 % 60013)::int,
	   ARRAY['t' || (i % 7)],
	   repeat('x', 200)
  FROM generate_series(1, 60000) i;
CREATE INDEX lol_l ON lol USING lion (a, g, b, n);
CREATE INDEX lol_ts ON lol USING lion (ts);
CREATE INDEX lol_tags ON lol USING lion (tags);
CREATE INDEX lol_k ON lol (k, id);
VACUUM (FREEZE, ANALYZE) lol;

/*
 * ol_cmp() runs q without core's Sort, so that an ordered path is what is
 * left, and says whether its plan has a LionOrdered node; runs it again with
 * the node off; and compares the two answers in order.
 */
CREATE FUNCTION ol_cmp(q text, ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	used boolean := false;
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);

	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  cardinality(a), cardinality(b), a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  cardinality(a));
END $$;

-- ol_plan() prints the node's lines of the plan without core's Sort.
CREATE FUNCTION ol_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(LionOrdered|Ordered By|Index Cond|Lion Cond|Filter)' THEN
			RETURN NEXT btrim(ln);
		END IF;
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);
END $$;

-- ol_run() prints the node's counters under EXPLAIN ANALYZE, without Sort.
CREATE FUNCTION ol_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ '(Index Entries Walked|Lion Keys|Lion Set|Heap Fetches|Rows Removed by|Switched)' THEN
			RETURN NEXT btrim(ln);
		END IF;
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);
END $$;

-- 1. The plan: the list is the walk's (Index Cond), and the set is the other
--    clauses' alone.
SELECT * FROM ol_plan($$SELECT id FROM lol
	WHERE a IN ('a3', 'a7') AND g IN ('g10', 'g11') AND b IN ('b4', 'b9')
	ORDER BY g, id LIMIT 20$$);
SELECT * FROM ol_plan($$SELECT id FROM lol
	WHERE a IN ('a3', 'a7') AND g IN ('g10', 'g11') ORDER BY g DESC, id LIMIT 20$$);

-- 2. What the walk did: only the list's entries, and a fetch a row.  Before,
--    this walk read every entry below 'g10' and gave up on it.
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE a IN ('a3', 'a7') AND g IN ('g10', 'g11') AND b IN ('b4', 'b9')
	ORDER BY g LIMIT 5$$);
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE a IN ('a3', 'a7') AND g IN ('g10', 'g11') ORDER BY g DESC LIMIT 5$$);

-- 3. The answers.
-- a list in the middle of the column, either way, with and without a LIMIT
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a IN ('a3', 'a7') AND g IN ('g10', 'g11') AND b IN ('b4', 'b9')
	ORDER BY g, id LIMIT 20$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a IN ('a3', 'a7') AND g IN ('g11', 'g10') AND b IN ('b4', 'b9')
	ORDER BY g DESC, id LIMIT 20$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a3' AND g IN ('g48', 'g02', 'g25') ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a3' AND g IN ('g48', 'g02', 'g25') ORDER BY g DESC, id$$);
-- a list and a range of the same column: only the values inside the range
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a5' AND g IN ('g01', 'g20', 'g30', 'g45') AND g > 'g10' AND g <= 'g30'
	ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a5' AND g IN ('g01', 'g20', 'g30', 'g45') AND g < 'g45'
	ORDER BY g DESC, id$$);
-- NULLs and duplicates in the list, values no row has, and a list of none
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a1' AND g = ANY (ARRAY['g12', NULL, 'g12', 'g05', 'g05'])
	ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a1' AND g IN ('zz', 'g99', 'a') ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a1' AND g = ANY ('{}'::text[]) ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a1' AND g = ANY (ARRAY[NULL]::text[]) ORDER BY g, id$$);
-- two lists on the column: the walk takes one, the set the other
SELECT * FROM ol_plan($$SELECT id FROM lol
	WHERE g IN ('g01', 'g02', 'g03') AND g IN ('g02', 'g03', 'g04') AND a = 'a2'
	ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE g IN ('g01', 'g02', 'g03') AND g IN ('g02', 'g03', 'g04') AND a = 'a2'
	ORDER BY g, id$$);
-- a list and nothing else: the walk alone, no set
SELECT ol_cmp($$SELECT id, g FROM lol WHERE g IN ('g33', 'g07') ORDER BY g, id LIMIT 30$$);
-- the NULL group of a column is no member of any list
SELECT ol_cmp($$SELECT id, n FROM lol
	WHERE a = 'a9' AND n IN (3, 5, 8) ORDER BY n NULLS FIRST, id$$);
-- a list of another type is no list of the walk: it stays the set's
SELECT * FROM ol_plan($$SELECT id FROM lol
	WHERE a = 'a9' AND n = ANY ('{3, 5}'::int8[]) ORDER BY n, id$$);
SELECT ol_cmp($$SELECT id, n FROM lol
	WHERE a = 'a9' AND n = ANY ('{3, 5}'::int8[]) ORDER BY n, id$$);
-- a nearly unique column, walked backwards over a long list
SELECT ol_cmp($$SELECT id, ts FROM lol
	WHERE ts = ANY (ARRAY(SELECT timestamptz '2026-01-01 00:00+00' + i * interval '7 minutes'
						   FROM generate_series(1, 400) i))
	  AND a IN ('a1', 'a2', 'a3', 'a4', 'a5')
	ORDER BY ts DESC, id LIMIT 25$$);

-- 4. Params: a generic plan, and a LATERAL list per outer row.
SET plan_cache_mode = force_generic_plan;
PREPARE ol_p(text[], text) AS
	SELECT id, g FROM lol WHERE g = ANY ($1) AND a = $2 ORDER BY g, id LIMIT 12;
SELECT ol_cmp($$EXECUTE ol_p(ARRAY['g40', 'g04'], 'a6')$$,
			  $$SELECT id, g FROM lol WHERE g = ANY (ARRAY['g40', 'g04']) AND a = 'a6'
				ORDER BY g, id LIMIT 12$$);
SELECT ol_cmp($$EXECUTE ol_p(NULL, 'a6')$$,
			  $$SELECT id, g FROM lol WHERE g = ANY (NULL::text[]) AND a = 'a6'
				ORDER BY g, id LIMIT 12$$);
SELECT ol_cmp($$EXECUTE ol_p(ARRAY['g17'], 'a0')$$,
			  $$SELECT id, g FROM lol WHERE g = ANY (ARRAY['g17']) AND a = 'a0'
				ORDER BY g, id LIMIT 12$$);
DEALLOCATE ol_p;
RESET plan_cache_mode;
SELECT ol_cmp($$
	SELECT o.v, x.id, x.g FROM (VALUES (ARRAY['g01', 'g03']), (ARRAY['g49']),
										(ARRAY['g20', 'g21', 'g22'])) o(v),
	  LATERAL (SELECT l.id, l.g FROM lol l WHERE l.g = ANY (o.v) AND l.a = 'a4'
				ORDER BY l.g DESC, l.id LIMIT 4) x$$);

-- 5. The set, lazily (DESIGN.md §30.4, "The set, lazily"): evaluated only at
--    the container keys the walk meets, when every key of it is a set tree
--    (the counters of 2. above say so too).
-- an OR of two leaves, and a leaf of two keys: ANDed and ORed at each key
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE (a = 'a3' OR b = 'b2') AND g IN ('g10', 'g11', 'g40') ORDER BY g LIMIT 5$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE (a = 'a3' OR b = 'b2') AND g IN ('g10', 'g11', 'g40') ORDER BY g, id LIMIT 40$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a IN ('a3', 'a4') AND b IN ('b2', 'b7', 'b9') ORDER BY g DESC, id LIMIT 40$$);
-- a key of no entry makes its AND empty: no walk at all
SELECT * FROM ol_run($$SELECT id FROM lol WHERE a = 'zz' AND b = 'b1' ORDER BY g LIMIT 5$$);
SELECT ol_cmp($$SELECT id, g FROM lol WHERE a = 'zz' AND b = 'b1' ORDER BY g, id LIMIT 5$$);
SELECT ol_cmp($$SELECT id, g FROM lol WHERE (a = 'zz' OR b = 'b1') AND g < 'g03'
	ORDER BY g, id LIMIT 5$$);
-- a range, or a multi-key key, in the set: built, as before
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE a = 'a3' AND b > 'b25' AND g IN ('g10', 'g11') ORDER BY g LIMIT 5$$);
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE tags @> ARRAY['t2'] AND g IN ('g10', 'g11') ORDER BY g LIMIT 5$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a3' AND b > 'b25' AND g IN ('g10', 'g11') ORDER BY g, id LIMIT 20$$);
-- a list longer than a plain scan opens at once: built
SET work_mem = '64kB';
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE ts = ANY (ARRAY(SELECT timestamptz '2026-01-01 00:00+00' + i * interval '1 minute'
						   FROM generate_series(1, 40) i))
	  AND g IN ('g10', 'g11') ORDER BY g LIMIT 3$$);
RESET work_mem;

-- 6. Built after all: a btree walk in random heap order starts the streams
--    again at nearly every key, until the probes have cost what the build
--    reads - here at once, the sets being small.
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE a = 'a3' AND b = 'b4' ORDER BY k LIMIT 5$$);
SELECT ol_cmp($$SELECT id, k FROM lol WHERE a = 'a3' AND b = 'b4' ORDER BY k, id LIMIT 10$$);
-- and a long walk: built once it has met 40,000 entries, the members it met
-- lazily counted as met, so the early stop ends the walk and the switch,
-- should it come, fetches none of them again
SELECT * FROM ol_run($$SELECT id FROM lol
	WHERE a IN ('a1', 'a2') AND b IN ('b1', 'b2', 'b3') ORDER BY g$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a IN ('a1', 'a2') AND b IN ('b1', 'b2', 'b3') ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a = 'a1' AND b = 'b1' AND n = 3 ORDER BY g, id$$);
SELECT ol_cmp($$SELECT id, ts FROM lol
	WHERE a = 'a1' AND b IN ('b1', 'b2') ORDER BY ts DESC, id$$);

-- 7. Rescans: a LATERAL filter per outer row starts the set again; one whose
--    Param is only the walk's keeps it; a cursor fetched in pieces.
SELECT ol_cmp($$
	SELECT o.v, x.id, x.g FROM (VALUES ('a1'), ('a7'), ('zz'), ('a19')) o(v),
	  LATERAL (SELECT l.id, l.g FROM lol l WHERE l.a = o.v AND l.b IN ('b1', 'b5')
				ORDER BY l.g, l.id LIMIT 3) x$$);
SELECT ol_cmp($$
	SELECT o.v, x.id, x.g FROM (VALUES (ARRAY['g01', 'g03']), (ARRAY['g49']),
										(ARRAY['g20', 'g21', 'g22'])) o(v),
	  LATERAL (SELECT l.id, l.g FROM lol l WHERE l.g = ANY (o.v) AND l.a = 'a4' AND l.b <> 'b0'
				ORDER BY l.g, l.id LIMIT 4) x$$);
SET enable_sort = off;
BEGIN;
DECLARE ol_c CURSOR FOR
	SELECT id, g FROM lol WHERE a IN ('a3', 'a7') AND b = 'b4' ORDER BY g, id LIMIT 30;
FETCH 7 FROM ol_c;
FETCH 11 FROM ol_c;
FETCH ALL FROM ol_c;
ROLLBACK;
RESET enable_sort;
SET pg_lion.enable_ordered_scan = off;
SELECT id, g FROM lol WHERE a IN ('a3', 'a7') AND b = 'b4' ORDER BY g, id LIMIT 30;
RESET pg_lion.enable_ordered_scan;

-- a list Param of the set in a generic plan: NULL selects nothing, and no
-- walk is made; a NULL element is no key
SET plan_cache_mode = force_generic_plan;
PREPARE ol_s(text[]) AS
	SELECT id, g FROM lol WHERE a = ANY ($1) AND b = 'b1' AND g IN ('g01', 'g11', 'g21')
	ORDER BY g, id LIMIT 5;
SELECT * FROM ol_run('EXECUTE ol_s(NULL)');
SELECT ol_cmp('EXECUTE ol_s(NULL)',
			  $$SELECT id, g FROM lol WHERE a = ANY (NULL::text[]) AND b = 'b1'
				  AND g IN ('g01', 'g11', 'g21') ORDER BY g, id LIMIT 5$$);
SELECT * FROM ol_run($$EXECUTE ol_s(ARRAY['a1', NULL, 'a11'])$$);
SELECT ol_cmp($$EXECUTE ol_s(ARRAY['a1', NULL, 'a11'])$$,
			  $$SELECT id, g FROM lol WHERE a = ANY (ARRAY['a1', NULL, 'a11']) AND b = 'b1'
				  AND g IN ('g01', 'g11', 'g21') ORDER BY g, id LIMIT 5$$);
DEALLOCATE ol_s;
RESET plan_cache_mode;

-- 8. A heap that is not all-visible: rows deleted and updated after the
--    index was built, and a row inserted - the walk meets TIDs whose rows
--    the snapshot does not see, and the set knows no better.
DELETE FROM lol WHERE id % 17 = 0;
UPDATE lol SET a = 'a3' WHERE id % 23 = 0;
INSERT INTO lol VALUES (60001, 'g10', 'a3', 'b4', 1, now(), 60001, '{t1}', 'y');
SELECT ol_cmp($$SELECT id, g FROM lol
	WHERE a IN ('a3', 'a7') AND g IN ('g10', 'g11') AND b IN ('b4', 'b9')
	ORDER BY g, id LIMIT 20$$);
SELECT ol_cmp($$SELECT id, g FROM lol WHERE a = 'a3' AND b IN ('b4', 'b5') ORDER BY g DESC, id$$);

DROP FUNCTION ol_cmp(text, text);
DROP FUNCTION ol_plan(text);
DROP FUNCTION ol_run(text);
DROP TABLE lol;
