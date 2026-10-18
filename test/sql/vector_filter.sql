-- Filtered nearest neighbours: LionOrdered over a lionvec index (DESIGN.md
-- §44.6).
--
-- SELECT ... WHERE <lion-answerable clauses> ORDER BY v <op> q [LIMIT n]:
-- the node builds lion's TID set for the WHERE and hands it to the vector
-- scan, which reads and scores only its members.  Every query below is run
-- through the node (every core scan disabled, so that nothing else can be
-- chosen) and through the ordinary plan (the node off, everything else
-- enabled, and pg_lion.vector_postfilter exact, under which that plan is a
-- sort of the filter's rows where the default, error, fails the query), and
-- the answers are compared: the distances in order, and the rows.
-- pg_lion.vector_epsilon and pg_lion.vector_margin are raised, and
-- pg_lion.vector_patience is off, as in vector.sql.  The indexes spill (each
-- row in two partitions, DESIGN.md §44.3), so every comparison is also of a
-- restricted scan that must return each member once.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET pg_lion.vector_epsilon = 6;
SET pg_lion.vector_margin = 100;
SET pg_lion.vector_patience = 0;
-- The node restricts the scan by lion's set unless said otherwise below: the
-- tests from before the filter at the fetch (2026-10-18) are the set's, and
-- pinned to it so that the flavour pg_lion.vector_restrict = auto would price
-- lower does not change what they test.  The filter's own tests below set it,
-- compare the two, and ask auto which it plans.
SET pg_lion.vector_restrict = set;

CREATE FUNCTION vf_vec(seed bigint, n int, scale float8 DEFAULT 1.0)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
	SELECT array_agg(((hashint8extended(seed * 1000 + j, 43) % 100000)::float8
					  / 100000.0 * scale)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

/*
 * vf_ord() runs q - which returns columns id and d, ORDER BY d - through the
 * node and through the ordinary plan (the node off, and
 * pg_lion.vector_postfilter exact: a sort of the filter's rows), and
 * compares them as vector.sql's vec_cmp() does; and says whether the first
 * plan really was the node.  A prepared statement's cached plan does not
 * change with the settings, so for one the ordinary answer comes from the
 * query it stands for.
 */
CREATE FUNCTION vf_ord(q text, ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	used boolean := false;
	ad text[] := '{}';
	bd text[] := '{}';
	ai int[] := '{}';
	bi int[] := '{}';
	postfilter text := current_setting('pg_lion.vector_postfilter');
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		ad := ad || coalesce(round(r.d::numeric, 4)::text, 'NULL');
		IF r.d IS NOT NULL THEN
			ai := ai || r.id;
		END IF;
	END LOOP;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.vector_postfilter', 'exact', true);
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		bd := bd || coalesce(round(r.d::numeric, 4)::text, 'NULL');
		IF r.d IS NOT NULL THEN
			bi := bi || r.id;
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.vector_postfilter', postfilter, true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);

	IF ad IS DISTINCT FROM bd THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; distances %s / %s',
					  coalesce(array_length(ad, 1), 0), coalesce(array_length(bd, 1), 0),
					  ad[1:3], bd[1:3]);
	END IF;
	IF (SELECT array_agg(x ORDER BY x) FROM unnest(ai) x) IS DISTINCT FROM
	   (SELECT array_agg(x ORDER BY x) FROM unnest(bi) x) THEN
		RETURN 'MISMATCH: other rows at the same distances';
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  coalesce(array_length(ad, 1), 0));
END $$;

/*
 * vf_run() runs q through the node under EXPLAIN ANALYZE and prints the
 * node's lines, without the ones whose contents vary between runs.
 */
CREATE FUNCTION vf_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers)' THEN
			-- PostgreSQL 18 prints rows with two decimals
			RETURN NEXT regexp_replace(ln, 'rows=(\d+)\.00 ', 'rows=\1 ', 'g');
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);
END $$;

-- 10,000 rows in ten clusters, indexed in 50 partitions; grp follows the
-- heap, so that a filter on it leaves whole CODE pages without a member
CREATE TABLE vf (
	id int PRIMARY KEY,
	cat int,			-- 10 values, the cluster
	tag int,			-- 100 values, independent of cat
	grp int,			-- id / 1000
	flag boolean,		-- every 37th row
	v vector(24),
	pad text
) WITH (autovacuum_enabled = off);
INSERT INTO vf
SELECT i, i % 10,
	   ((hashint8extended(i::bigint, 7) & 9223372036854775807) % 100)::int,
	   i / 1000, i % 37 = 0,
	   vf_vec(-(i % 10) - 1, 24) + vf_vec(i, 24, 0.3),
	   repeat('x', 200)
  FROM generate_series(1, 10000) i;
-- NULL vectors and zero vectors, in the filters' way
UPDATE vf SET v = NULL WHERE id % 211 = 0;
UPDATE vf SET v = array_fill(0, ARRAY[24])::real[]::vector WHERE id % 503 = 0;

CREATE INDEX vf_l2 ON vf USING lionvec (v) WITH (partitions = 50);
CREATE INDEX vf_ip ON vf USING lionvec (v vector_ip_ops) WITH (partitions = 50);
CREATE INDEX vf_cos ON vf USING lionvec (v vector_cosine_ops) WITH (partitions = 50);
CREATE INDEX vf_cat ON vf USING lion (cat);
CREATE INDEX vf_tag ON vf USING lion (tag);
CREATE INDEX vf_grp ON vf USING lion (grp);
CREATE INDEX vf_flag ON vf USING lion (flag);
VACUUM (FREEZE, ANALYZE) vf;

-- every metric under each kind of filter, at any LIMIT
CREATE TABLE vf_cases AS
SELECT op, f, k
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('tag = 3'), ('tag < 30'), ('cat IN (1, 2) AND tag < 50'),
			   ('tag = 7 OR cat = 4'), ('cat <> 5'), ('grp = 3'), ('flag'),
			   ('tag = 1000')) f(f),
	   (VALUES ('LIMIT 1'), ('LIMIT 10'), ('LIMIT 100'), ('')) k(k);
SELECT op, f, k,
	   vf_ord(format('SELECT id, v %s (SELECT v FROM vf WHERE id = 17) AS d FROM vf
					  WHERE %s ORDER BY v %s (SELECT v FROM vf WHERE id = 17) %s',
					 op, f, op, k)) AS result
  FROM vf_cases ORDER BY op, f, k;

-- a query vector away from every cluster, and the zero vector (cosine: NaN)
SELECT op, vf_ord(format('SELECT id, v %s vf_vec(5, 24, 3.0) AS d FROM vf
						  WHERE tag < 20 ORDER BY v %s vf_vec(5, 24, 3.0) LIMIT 50',
						 op, op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op) ORDER BY op;
SELECT vf_ord(format('SELECT id, v <=> %L::vector AS d FROM vf WHERE grp = 5
					  ORDER BY v <=> %L::vector',
					 array_fill(0, ARRAY[24])::real[]::vector,
					 array_fill(0, ARRAY[24])::real[]::vector));
-- a NULL query vector (a NULL row's): every member, at a NULL distance
SELECT vf_ord('SELECT id, v <-> (SELECT v FROM vf WHERE id = 211) AS d FROM vf
			   WHERE grp = 5 ORDER BY v <-> (SELECT v FROM vf WHERE id = 211)');

-- a prepared statement's generic plan: the filter and the vector are Params
PREPARE vfp(int, vector) AS
	SELECT id, v <-> $2 AS d FROM vf WHERE tag = $1 ORDER BY v <-> $2 LIMIT 10;
SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) EXECUTE vfp(3, vf_vec(1, 24));
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
-- what the node did: the vector scan returns members only
SELECT * FROM vf_run('EXECUTE vfp(3, vf_vec(1, 24))');
SELECT vf_ord('EXECUTE vfp(3, vf_vec(1, 24))',
			  'SELECT id, v <-> vf_vec(1, 24) AS d FROM vf WHERE tag = 3
			   ORDER BY v <-> vf_vec(1, 24) LIMIT 10');
SELECT vf_ord('EXECUTE vfp(42, vf_vec(2, 24))',
			  'SELECT id, v <-> vf_vec(2, 24) AS d FROM vf WHERE tag = 42
			   ORDER BY v <-> vf_vec(2, 24) LIMIT 10');
RESET plan_cache_mode;

-- rescans: per outer row, a filter and a vector of its own (the set is
-- rebuilt for every one)
SELECT vf_ord('SELECT q.id, (SELECT sum(id) FROM (SELECT id FROM vf
									WHERE cat = q.cat AND tag <> q.tag
									ORDER BY v <-> q.v LIMIT 5) s)::float8 AS d
				 FROM vf q WHERE q.id BETWEEN 100 AND 140 ORDER BY q.id');

-- Filter at the fetch (DESIGN.md §44.6, 2026-10-18): the node's other flavour
-- builds no set, and hands the vector scan the lion clauses instead, which it
-- tests on the heap tuple of each row it fetches for its exact distance,
-- dropping a row that fails as the set drops it before its estimate.
-- pg_lion.vector_restrict pins either flavour; under auto, the default, both
-- are offered and the cheaper planned.

/*
 * vf_both() runs q - which returns columns id and d, ORDER BY d - through the
 * node under each flavour, the set's and then the filter's, compares the two
 * answers as vf_ord() compares the node's with the ordinary plan's, and says
 * which flavour each plan was: EXPLAIN's "Lion Cond" (the set) or "Lion
 * Filter".
 */
CREATE FUNCTION vf_both(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	m text;
	kind text;
	kinds text[] := '{}';
	d text[];
	ids int[];
	ad text[];
	ai int[];
	restrict text := current_setting('pg_lion.vector_restrict');
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	FOREACH m IN ARRAY ARRAY['set', 'filter'] LOOP
		PERFORM set_config('pg_lion.vector_restrict', m, true);
		kind := 'no LionOrdered';
		FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
			IF ln ~ '^\s*Lion Cond:' THEN
				kind := 'set';
			ELSIF ln ~ '^\s*Lion Filter:' THEN
				kind := 'filter';
			END IF;
		END LOOP;
		kinds := kinds || kind;
		d := '{}';
		ids := '{}';
		FOR r IN EXECUTE q LOOP
			d := d || coalesce(round(r.d::numeric, 4)::text, 'NULL');
			IF r.d IS NOT NULL THEN
				ids := ids || r.id;
			END IF;
		END LOOP;
		IF m = 'set' THEN
			ad := d;
			ai := ids;
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.vector_restrict', restrict, true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);

	IF ad IS DISTINCT FROM d THEN
		RETURN format('MISMATCH: %s rows through the set, %s through the filter; distances %s / %s',
					  coalesce(array_length(ad, 1), 0), coalesce(array_length(d, 1), 0),
					  ad[1:3], d[1:3]);
	END IF;
	IF (SELECT array_agg(x ORDER BY x) FROM unnest(ai) x) IS DISTINCT FROM
	   (SELECT array_agg(x ORDER BY x) FROM unnest(ids) x) THEN
		RETURN 'MISMATCH: other rows at the same distances';
	END IF;
	RETURN format('%s / %s, %s rows', kinds[1], kinds[2],
				  coalesce(array_length(d, 1), 0));
END $$;

-- the same rows from both, every metric under each kind of filter, at any
-- LIMIT: the zero vectors (cosine: NaN) and the NULL rows after the rest,
-- each walk's rows tested by the filter too
SELECT op, f, k,
	   vf_both(format('SELECT id, v %s (SELECT v FROM vf WHERE id = 17) AS d FROM vf
					   WHERE %s ORDER BY v %s (SELECT v FROM vf WHERE id = 17) %s',
					  op, f, op, k)) AS result
  FROM vf_cases ORDER BY op, f, k;
-- the zero vector (cosine: NaN for every row) and a NULL query vector (NULL
-- for every row): only the walks, which return a member only
SELECT vf_both(format('SELECT id, v <=> %L::vector AS d FROM vf WHERE grp = 5
					   ORDER BY v <=> %L::vector',
					  array_fill(0, ARRAY[24])::real[]::vector,
					  array_fill(0, ARRAY[24])::real[]::vector));
SELECT vf_both('SELECT id, v <-> (SELECT v FROM vf WHERE id = 211) AS d FROM vf
				WHERE tag < 10 ORDER BY v <-> (SELECT v FROM vf WHERE id = 211) LIMIT 50');

-- the two plans: the filter's names no lion index, and reads none
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
SET pg_lion.vector_restrict = set;
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE tag = 7 OR cat = 4 ORDER BY v <-> vf_vec(1, 24) LIMIT 5;
SET pg_lion.vector_restrict = filter;
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE tag = 7 OR cat = 4 ORDER BY v <-> vf_vec(1, 24) LIMIT 5;
-- a clause the vector index's own column answers stays the node's Filter
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE tag < 30 AND v IS NOT NULL ORDER BY v <-> vf_vec(1, 24) LIMIT 5;
-- auto prices both and plans the cheaper: on these 10,000 rows the set,
-- whose members cost little to collect, even under the filter that keeps nine
-- rows in ten (a set that would not fit hash_mem leaves auto the filter:
-- vfn's, below)
SET pg_lion.vector_restrict = auto;
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE tag = 3 ORDER BY v <-> vf_vec(1, 24) LIMIT 10;
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE cat <> 5 ORDER BY v <-> vf_vec(1, 24) LIMIT 10;
SET pg_lion.vector_restrict = filter;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
-- and what it did: the rows the filter removed, at the fetch of their exact
-- distances or as a walk met them, which the node never saw
SELECT * FROM vf_run('SELECT id FROM vf WHERE tag < 30 ORDER BY v <-> vf_vec(1, 24) LIMIT 5');
SELECT * FROM vf_run('SELECT id FROM vf WHERE grp = 5 ORDER BY v <=> vf_vec(0, 24, 0.0)');

-- a generic plan's Params, the filter's and the vector, read where the scan
-- evaluates the filter
PREPARE vff(int, vector) AS
	SELECT id, v <-> $2 AS d FROM vf WHERE tag = $1 ORDER BY v <-> $2 LIMIT 10;
SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) EXECUTE vff(3, vf_vec(1, 24));
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
SELECT vf_ord('EXECUTE vff(3, vf_vec(1, 24))',
			  'SELECT id, v <-> vf_vec(1, 24) AS d FROM vf WHERE tag = 3
			   ORDER BY v <-> vf_vec(1, 24) LIMIT 10');
SELECT vf_ord('EXECUTE vff(42, vf_vec(2, 24))',
			  'SELECT id, v <-> vf_vec(2, 24) AS d FROM vf WHERE tag = 42
			   ORDER BY v <-> vf_vec(2, 24) LIMIT 10');
RESET plan_cache_mode;
DEALLOCATE vff;

-- rescans, the filter handed to the scan again after each: a correlated
-- subquery, and a LATERAL subquery under a nested loop, each outer row with a
-- filter and a vector of its own
SELECT vf_ord('SELECT q.id, (SELECT sum(id) FROM (SELECT id FROM vf
									WHERE cat = q.cat AND tag <> q.tag
									ORDER BY v <-> q.v LIMIT 5) s)::float8 AS d
				 FROM vf q WHERE q.id BETWEEN 100 AND 140 ORDER BY q.id');
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT q.id, s.id FROM vf q,
	   LATERAL (SELECT id FROM vf WHERE cat = q.cat AND tag < 50
				ORDER BY v <-> q.v LIMIT 3) s
 WHERE q.id IN (101, 102, 103);
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
SELECT vf_ord('SELECT q.id * 100000 + s.id AS id, s.d FROM vf q,
					  LATERAL (SELECT id, v <-> q.v AS d FROM vf
								WHERE cat = q.cat AND tag < 50
								ORDER BY v <-> q.v LIMIT 3) s
				WHERE q.id BETWEEN 100 AND 130 ORDER BY q.id, s.d');
SELECT * FROM vf_run('SELECT q.id, s.id FROM vf q,
						  LATERAL (SELECT id FROM vf WHERE cat = q.cat AND tag < 50
								   ORDER BY v <-> q.v LIMIT 3) s
					 WHERE q.id IN (101, 102, 103)');
SET pg_lion.vector_restrict = set;

-- a set that outgrows hash_mem at run time degrades to its container keys,
-- and every member is rechecked (DESIGN.md §30.4): planned at 64MB (a
-- generic plan keeps it), run at 64kB.  Half of vfn's 400,000 rows, 65 to a
-- page, is 97 containers of about 1 kB each, as ordered.sql's lon is.  (20
-- partitions: the 1,264 a table this size gets take minutes to build in
-- three dimensions, where plane 1 rules out few centroids.)
CREATE TABLE vfn (id int PRIMARY KEY, c2 int, v vector(3), pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO vfn
SELECT i, ((hashint8extended(i::bigint, 11) & 9223372036854775807) % 2)::int,
	   ARRAY[(hashint8extended(i::bigint, 1) % 1000000) / 1e6,
			 (hashint8extended(i::bigint, 2) % 1000000) / 1e6,
			 (hashint8extended(i::bigint, 3) % 1000000) / 1e6]::real[]::vector,
	   repeat('x', 64)
  FROM generate_series(1, 400000) i;
CREATE INDEX vfn_l2 ON vfn USING lionvec (v) WITH (partitions = 20);
CREATE INDEX vfn_c2 ON vfn USING lion (c2);
VACUUM (FREEZE, ANALYZE) vfn;
-- planned at 64kB, the set would not fit, and is no plan: under set the
-- query fails (pg_lion.vector_postfilter's default), and auto plans the
-- filter at the fetch, which needs no hash_mem (DESIGN.md §44.6, "Filter at
-- the fetch")
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM vfn WHERE c2 = 1 ORDER BY v <-> '[0.5,-0.25,0.125]' LIMIT 10;
SET pg_lion.vector_restrict = auto;
EXPLAIN (COSTS OFF)
SELECT id FROM vfn WHERE c2 = 1 ORDER BY v <-> '[0.5,-0.25,0.125]' LIMIT 10;
SET pg_lion.vector_restrict = set;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
RESET hash_mem_multiplier;
RESET work_mem;
SET work_mem = '64MB';
SET plan_cache_mode = force_generic_plan;
PREPARE vfd(int) AS
	SELECT id, v <-> '[0.5,-0.25,0.125]' AS d FROM vfn WHERE c2 = $1
	 ORDER BY v <-> '[0.5,-0.25,0.125]' LIMIT 10;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) EXECUTE vfd(1);
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
-- (three dimensions are plane 1's worst case: it rules out few rows, and a
-- pass the size of work_mem would be one of many)
SET pg_lion.vector_batch = 20000;
SELECT * FROM vf_run('EXECUTE vfd(1)') WHERE vf_run ~ 'Lion Set:|Recheck';
SELECT vf_ord('EXECUTE vfd(1)',
			  'SELECT id, v <-> ''[0.5,-0.25,0.125]'' AS d FROM vfn WHERE c2 = 1
			   ORDER BY v <-> ''[0.5,-0.25,0.125]'' LIMIT 10');
RESET pg_lion.vector_batch;
RESET hash_mem_multiplier;
RESET work_mem;
RESET plan_cache_mode;

-- a few members in every container key.  A scan restricted to no more than
-- pg_lion.vector_exact_rows (2,000) of them, or than
-- pg_lion.vector_exact_per_partition (1.5) a partition if that is more,
-- fetches each one's distance from the heap, and reads no CODE page
-- (DESIGN.md §44.6); one restricted to more probes partitions, as an
-- unrestricted scan does.  Here the 2,000 decide: 1.5 of the 400 partitions
-- is 600.
CREATE TABLE vfb (id int, s int, v vector(64)) WITH (autovacuum_enabled = off);
INSERT INTO vfb SELECT i, i % 2000, vf_vec(i, 64) FROM generate_series(1, 40000) i;
-- pins the old default's count, 2 sqrt(n) = 400, for test time (1,600: 42 s)
CREATE INDEX vfb_l2 ON vfb USING lionvec (v) WITH (partitions = 400);
CREATE INDEX vfb_s ON vfb USING lion (s);
VACUUM (FREEZE, ANALYZE) vfb;
SELECT vf_ord('SELECT id, v <-> vf_vec(7, 64) AS d FROM vfb WHERE s < 10
			   ORDER BY v <-> vf_vec(7, 64) LIMIT 10');
SELECT vf_ord('SELECT id, v <-> vf_vec(7, 64) AS d FROM vfb WHERE s < 200
			   ORDER BY v <-> vf_vec(7, 64) LIMIT 10');
SELECT vf_ord('SELECT id, v <-> (SELECT NULL::vector) AS d FROM vfb WHERE s < 10
			   ORDER BY v <-> (SELECT NULL::vector)');
-- (pg_statio_user_indexes, flushed on demand, as in indexscan.sql: the
-- transaction's own counts are not kept on every version)
CREATE FUNCTION vfb_blks() RETURNS bigint LANGUAGE sql AS
$$ SELECT idx_blks_hit + idx_blks_read FROM pg_statio_user_indexes
	WHERE indexrelname = 'vfb_l2' $$;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b0 \gset
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 10
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b1 \gset
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 200
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b2 \gset
-- the threshold moved: at 0 the 200 members are probed for, as the 4,000
-- were, and at 5,000 the 4,000 are fetched, as the 200 were; the order is
-- the same either way.  (pg_lion.vector_exact_per_partition is 0 as well,
-- or its 1.5 a partition would keep the 200, half a partition, exact.)
SET pg_lion.vector_exact_rows = 0;
SET pg_lion.vector_exact_per_partition = 0;
SELECT vf_ord('SELECT id, v <-> vf_vec(7, 64) AS d FROM vfb WHERE s < 10
			   ORDER BY v <-> vf_vec(7, 64) LIMIT 10');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b3 \gset
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 10
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b4 \gset
SET pg_lion.vector_exact_rows = 5000;
SELECT vf_ord('SELECT id, v <-> vf_vec(7, 64) AS d FROM vfb WHERE s < 200
			   ORDER BY v <-> vf_vec(7, 64) LIMIT 10');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b5 \gset
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 200
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b6 \gset
RESET pg_lion.vector_exact_rows;
RESET pg_lion.vector_exact_per_partition;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
SELECT :b1 - :b0 > 0 AS "read some", 4 * (:b1 - :b0) < :b2 - :b1 AS "read few";
SELECT 4 * (:b1 - :b0) < :b4 - :b3 AS "probed at 0", 4 * (:b6 - :b5) < :b2 - :b1 AS "fetched at 5000";

-- pg_lion.vector_exact_per_partition alone (pg_lion.vector_exact_rows 0):
-- what decides is the members a partition, not their number (DESIGN.md
-- §44.6).  At the default, 1.5, the 200 members, half a partition, are
-- fetched; at 0 they are probed for.  The 4,000, ten a partition, are
-- fetched at 10, the bound itself, and probed for at 9.99.  The scan's
-- DEBUG1 line says when it fetched every member, and the index blocks a
-- scan read when it probed; the order is the same either way.
SET pg_lion.vector_exact_rows = 0;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
-- (with no prefetch, whose counts the line would add: vector_scan_parallel)
SET pg_lion.vector_prefetch = off;
SET client_min_messages = debug1;
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 10
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
-- under its filter the scan never computes every member's distance, which
-- needs their list: it probes, and its line counts the rows the filter
-- removed (DESIGN.md §44.6, "Filter at the fetch")
SET pg_lion.vector_restrict = filter;
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 10
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
SET pg_lion.vector_restrict = set;
SET pg_lion.vector_exact_per_partition = 10;
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 200
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
RESET client_min_messages;
RESET pg_lion.vector_prefetch;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b7 \gset
SET pg_lion.vector_exact_per_partition = 9.99;
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 200
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b8 \gset
SET pg_lion.vector_exact_per_partition = 0;
SELECT count(*) FROM (SELECT id FROM vfb WHERE s < 10
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT vfb_blks() AS b9 \gset
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
SELECT 4 * (:b1 - :b0) < :b8 - :b7 AS "probed at 9.99", 4 * (:b1 - :b0) < :b9 - :b8 AS "probed at 0";
SET pg_lion.vector_exact_per_partition = 10;
SELECT vf_ord('SELECT id, v <-> vf_vec(7, 64) AS d FROM vfb WHERE s < 200
			   ORDER BY v <-> vf_vec(7, 64) LIMIT 10');
SET pg_lion.vector_exact_per_partition = 9.99;
SELECT vf_ord('SELECT id, v <-> vf_vec(7, 64) AS d FROM vfb WHERE s < 200
			   ORDER BY v <-> vf_vec(7, 64) LIMIT 10');
-- and the planner prices what the scan will do, from the same partition
-- count: the node's start-up at 20 a partition (8,000, past the 4,000 it
-- expects) is the exact path's, as pg_lion.vector_exact_rows = 5,000 prices
-- it, not the probes' (both thresholds 0)
CREATE FUNCTION vfb_startup() RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN SELECT id FROM vfb WHERE s < 200
					   ORDER BY v <-> vf_vec(7, 64) LIMIT 10' LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			RETURN substring(ln from 'cost=([0-9]+\.[0-9]+)\.\.')::float8;
		END IF;
	END LOOP;
	RETURN NULL;
END $$;
SET pg_lion.vector_exact_per_partition = 20;
SELECT vfb_startup() AS c0 \gset
SET pg_lion.vector_exact_per_partition = 0;
SET pg_lion.vector_exact_rows = 5000;
SELECT vfb_startup() AS c1 \gset
SET pg_lion.vector_exact_rows = 0;
SELECT vfb_startup() AS c2 \gset
SELECT :c0 = :c1 AS "priced exact", :c0 <> :c2 AS "not as probes";
RESET pg_lion.vector_exact_rows;
RESET pg_lion.vector_exact_per_partition;
-- the setting's range
SET pg_lion.vector_exact_per_partition = -1;
SET pg_lion.vector_exact_per_partition = 1000001;
SHOW pg_lion.vector_exact_per_partition;
DROP FUNCTION vfb_startup();
DROP FUNCTION vfb_blks();

/*
 * vf_recall() runs q - which returns a column id - through the node and
 * through a sequential scan and sort (the node off, and
 * pg_lion.vector_postfilter exact), and returns the share of the second's
 * rows the first has.
 */
CREATE FUNCTION vf_recall(q text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	a int[];
	b int[];
	postfilter text := current_setting('pg_lion.vector_postfilter');
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	EXECUTE 'SELECT array_agg(id) FROM (' || q || ') s' INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.vector_postfilter', 'exact', true);
	EXECUTE 'SELECT array_agg(id) FROM (' || q || ') s' INTO b;
	PERFORM set_config('pg_lion.vector_postfilter', postfilter, true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	RETURN (SELECT count(*) FROM unnest(a) x WHERE x = ANY (b))::float8 /
		greatest(cardinality(b), 1);
END $$;

-- the default margin and patience, over a filter that leaves too many
-- members to fetch one by one: the scan probes the partitions near the
-- query, and misses few of its nearest members
RESET pg_lion.vector_margin;
RESET pg_lion.vector_patience;
SELECT op, avg(vf_recall(format('SELECT id FROM vf WHERE cat <> 5
								  ORDER BY v %s vf_vec(-%s, 24) + vf_vec(%s, 24, 0.3)
								  LIMIT 10', op, s % 10 + 1, s + 20000))) >= 0.9
	   AS "most of the nearest"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op), generate_series(1, 20) s
 GROUP BY op ORDER BY op;
-- and under the filter at the fetch, which probes as the set's scan does but
-- tells the members only as it fetches them, its tiers and patience counted
-- in rows (DESIGN.md §44.6, "Filter at the fetch")
SET pg_lion.vector_restrict = filter;
SELECT op, avg(vf_recall(format('SELECT id FROM vf WHERE cat <> 5
								  ORDER BY v %s vf_vec(-%s, 24) + vf_vec(%s, 24, 0.3)
								  LIMIT 10', op, s % 10 + 1, s + 20000))) >= 0.9
	   AS "most of the nearest"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op), generate_series(1, 20) s
 GROUP BY op ORDER BY op;
SELECT op, avg(vf_recall(format('SELECT id FROM vf WHERE tag < 30
								  ORDER BY v %s vf_vec(-%s, 24) + vf_vec(%s, 24, 0.3)
								  LIMIT 10', op, s % 10 + 1, s + 20000))) >= 0.9
	   AS "most of the nearest"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op), generate_series(1, 20) s
 GROUP BY op ORDER BY op;
SET pg_lion.vector_restrict = set;

-- a restricted scan that pauses at once, every partition within its margin
-- and its patience one partition's members (a unit is the members of one
-- of 2 sqrt(9000) partitions, so four of them in 50): it still fills its
-- LIMIT, in order, from the members it holds and then the partitions after
-- the pause
SET pg_lion.vector_margin = 100;
SET pg_lion.vector_patience = 4;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_sort = off;
SELECT count(*) AS "through the node"
  FROM vf_run('SELECT id FROM vf WHERE cat <> 5 ORDER BY v <-> vf_vec(4, 24) LIMIT 2000') ln
 WHERE ln ~ 'LionOrdered';
SELECT cardinality(a) AS n, a = (SELECT array_agg(x ORDER BY x) FROM unnest(a) x) AS "in order"
  FROM (SELECT array_agg(d) AS a
		  FROM (SELECT v <-> vf_vec(4, 24) AS d FROM vf WHERE cat <> 5
				 ORDER BY v <-> vf_vec(4, 24) LIMIT 2000) l) s;
-- the same under the filter, whose pause comes on rows, members or not
SET pg_lion.vector_restrict = filter;
SELECT count(*) FILTER (WHERE ln ~ 'LionOrdered') AS "through the node",
	   count(*) FILTER (WHERE ln ~ '^\s*Lion Filter:') AS "its filter"
  FROM vf_run('SELECT id FROM vf WHERE cat <> 5 ORDER BY v <-> vf_vec(4, 24) LIMIT 2000') ln;
SELECT cardinality(a) AS n, a = (SELECT array_agg(x ORDER BY x) FROM unnest(a) x) AS "in order"
  FROM (SELECT array_agg(d) AS a
		  FROM (SELECT v <-> vf_vec(4, 24) AS d FROM vf WHERE cat <> 5
				 ORDER BY v <-> vf_vec(4, 24) LIMIT 2000) l) s;
SET pg_lion.vector_restrict = set;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_sort;
SET pg_lion.vector_patience = 0;

/*
 * vf_unique() runs q - which returns a column id - through the node, and
 * through the vector scan with a Filter (the node off, and
 * pg_lion.vector_postfilter = allow, without which no plan filters a lionvec
 * scan's rows: DESIGN.md §44.6, "Post-filtering"), and counts the rows and
 * the distinct ones of each.
 */
CREATE FUNCTION vf_unique(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
	used boolean := false;
	ln text;
	postfilter text := current_setting('pg_lion.vector_postfilter');
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		used := used OR ln ~ 'Custom Scan \(LionOrdered\)';
	END LOOP;
	EXECUTE 'SELECT format(''%s rows, %s distinct'', count(*), count(DISTINCT id)) FROM (' ||
		q || ') s' INTO a;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.vector_postfilter', 'allow', true);
	EXECUTE 'SELECT format(''%s rows, %s distinct'', count(*), count(DISTINCT id)) FROM (' ||
		q || ') s' INTO b;
	PERFORM set_config('pg_lion.vector_postfilter', postfilter, true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_sort', 'on', true);
	RETURN format('%s: %s; ordinary: %s',
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END, a, b);
END $$;

-- each member once, though it is in two partitions: under filters that
-- probe (9,000 and 2,500 members), past the members' count, at the default
-- margin and patience and at the tests'; by the set, and by the filter
SELECT op, f, vf_unique(format('SELECT id FROM vf WHERE %s ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							   f, op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('cat <> 5'), ('tag < 25')) f(f)
 ORDER BY op, f;
SET pg_lion.vector_restrict = filter;
SELECT op, f, vf_unique(format('SELECT id FROM vf WHERE %s ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							   f, op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('cat <> 5'), ('tag < 25')) f(f)
 ORDER BY op, f;
SET pg_lion.vector_restrict = set;
-- (at the defaults a pause skips the few rows it finds too late, §44.4)
RESET pg_lion.vector_margin;
RESET pg_lion.vector_patience;
SELECT op, vf_unique(format('SELECT id FROM vf WHERE cat <> 5 ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							op)) ~ '^LionOrdered: (\d+) rows, \1 distinct; ordinary: (\d+) rows, \2 distinct$'
	   AS "each once"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op) ORDER BY op;
SET pg_lion.vector_restrict = filter;
SELECT op, vf_unique(format('SELECT id FROM vf WHERE cat <> 5 ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							op)) ~ '^LionOrdered: (\d+) rows, \1 distinct; ordinary: (\d+) rows, \2 distinct$'
	   AS "each once"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op) ORDER BY op;
SET pg_lion.vector_restrict = set;
SET pg_lion.vector_margin = 100;
SET pg_lion.vector_patience = 0;

-- inserts after the builds, then deletes and VACUUM
INSERT INTO vf
SELECT i, i % 10,
	   ((hashint8extended(i::bigint, 7) & 9223372036854775807) % 100)::int,
	   i / 1000, i % 37 = 0,
	   vf_vec(-(i % 10) - 1, 24) + vf_vec(i, 24, 0.3), 'y'
  FROM generate_series(10001, 12000) i;
SELECT op, f, vf_ord(format('SELECT id, v %s vf_vec(4, 24) AS d FROM vf
							WHERE %s ORDER BY v %s vf_vec(4, 24) LIMIT 30',
							op, f, op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('tag = 3'), ('grp IN (3, 11)'), ('cat <> 5')) f(f)
 ORDER BY op, f;
SELECT op, vf_unique(format('SELECT id FROM vf WHERE cat <> 5 ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op) ORDER BY op;
DELETE FROM vf WHERE id % 4 = 0;
VACUUM vf;
SELECT op, f, vf_ord(format('SELECT id, v %s vf_vec(4, 24) AS d FROM vf
							WHERE %s ORDER BY v %s vf_vec(4, 24) LIMIT 30',
							op, f, op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('tag = 3'), ('grp IN (3, 11)'), ('cat <> 5')) f(f)
 ORDER BY op, f;
SELECT op, vf_unique(format('SELECT id FROM vf WHERE cat <> 5 ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op) ORDER BY op;

-- off, the node is not offered, and no plan filters the vector scan's rows
-- either (pg_lion.vector_postfilter, DESIGN.md §44.6, "Post-filtering"): by
-- default the query fails, and under exact the filter's rows are sorted,
-- exactly.  Under allow the vector scan filters instead (sorts are off, or
-- sorting the lion scan's rows would be cheaper).
SET pg_lion.enable_ordered_scan = off;
SET enable_seqscan = off; SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE tag = 3 ORDER BY v <-> '[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]' LIMIT 5;
SET pg_lion.vector_postfilter = exact;
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE tag = 3 ORDER BY v <-> '[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]' LIMIT 5;
SET pg_lion.vector_postfilter = allow; SET enable_sort = off;
EXPLAIN (COSTS OFF)
SELECT id FROM vf WHERE tag = 3 ORDER BY v <-> '[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]' LIMIT 5;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_sort;
RESET pg_lion.vector_postfilter; RESET pg_lion.enable_ordered_scan;

DEALLOCATE vfp;
DEALLOCATE vfd;
DROP TABLE vf, vf_cases, vfn, vfb;
DROP FUNCTION vf_recall(text);
DROP FUNCTION vf_unique(text);
DROP FUNCTION vf_ord(text, text);
DROP FUNCTION vf_run(text);
DROP FUNCTION vf_vec(bigint, int, float8);
