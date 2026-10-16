-- Filtered nearest neighbours: LionOrdered over a lionvec index (DESIGN.md
-- §44.6).
--
-- SELECT ... WHERE <lion-answerable clauses> ORDER BY v <op> q [LIMIT n]:
-- the node builds lion's TID set for the WHERE and hands it to the vector
-- scan, which reads and scores only its members.  Every query below is run
-- through the node (every core scan disabled, so that nothing else can be
-- chosen) and through the ordinary plan (the node off, everything else
-- enabled), and the answers are compared: the distances in order, and the
-- rows.  pg_lion.vector_epsilon and pg_lion.vector_margin are raised, and
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

CREATE FUNCTION vf_vec(seed bigint, n int, scale float8 DEFAULT 1.0)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
	SELECT array_agg(((hashint8extended(seed * 1000 + j, 43) % 100000)::float8
					  / 100000.0 * scale)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

/*
 * vf_ord() runs q - which returns columns id and d, ORDER BY d - through the
 * node and through the ordinary plan, and compares them as vector.sql's
 * vec_cmp() does; and says whether the first plan really was the node.  A
 * prepared statement's cached plan does not change with the settings, so
 * for one the ordinary answer comes from the query it stands for.
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
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		bd := bd || coalesce(round(r.d::numeric, 4)::text, 'NULL');
		IF r.d IS NOT NULL THEN
			bi := bi || r.id;
		END IF;
	END LOOP;
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
-- pg_lion.vector_exact_rows (2,000) of them fetches each one's distance from
-- the heap, and reads no CODE page (DESIGN.md §44.6); one restricted to more
-- probes partitions, as an unrestricted scan does.
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
-- the same either way
SET pg_lion.vector_exact_rows = 0;
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
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
SELECT :b1 - :b0 > 0 AS "read some", 4 * (:b1 - :b0) < :b2 - :b1 AS "read few";
SELECT 4 * (:b1 - :b0) < :b4 - :b3 AS "probed at 0", 4 * (:b6 - :b5) < :b2 - :b1 AS "fetched at 5000";
DROP FUNCTION vfb_blks();

/*
 * vf_recall() runs q - which returns a column id - through the node and
 * through a sequential scan and sort, and returns the share of the second's
 * rows the first has.
 */
CREATE FUNCTION vf_recall(q text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	a int[];
	b int[];
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
	EXECUTE 'SELECT array_agg(id) FROM (' || q || ') s' INTO b;
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
-- margin and patience and at the tests'
SELECT op, f, vf_unique(format('SELECT id FROM vf WHERE %s ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							   f, op)) AS result
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('cat <> 5'), ('tag < 25')) f(f)
 ORDER BY op, f;
-- (at the defaults a pause skips the few rows it finds too late, §44.4)
RESET pg_lion.vector_margin;
RESET pg_lion.vector_patience;
SELECT op, vf_unique(format('SELECT id FROM vf WHERE cat <> 5 ORDER BY v %s vf_vec(4, 24) LIMIT 20000',
							op)) ~ '^LionOrdered: (\d+) rows, \1 distinct; ordinary: (\d+) rows, \2 distinct$'
	   AS "each once"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op) ORDER BY op;
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
-- either (pg_lion.vector_postfilter, DESIGN.md §44.6, "Post-filtering"): the
-- filter's rows are sorted, exactly.  Under allow the vector scan filters
-- instead (sorts are off, or sorting the lion scan's rows would be cheaper).
SET pg_lion.enable_ordered_scan = off;
SET enable_seqscan = off; SET enable_bitmapscan = off;
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
