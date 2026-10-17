-- lionvec's parallel scan and its prefetch (DESIGN.md §44.4, "A parallel
-- scan" and "Prefetch").
--
-- In a parallel scan every participant - each worker, and the leader unless
-- parallel_leader_participation is off - runs the whole scan over the
-- partitions it is the first to claim, and Gather Merge merges their orders.
-- The parallel plans are forced (no setup or tuple cost, no least index or
-- table size, the table's parallel_workers) and shown with the sequential
-- and bitmap scans and the sort disabled.  How many workers start is the
-- server's, and nothing below depends on it: with none the leader runs the
-- plan alone.  At pg_lion.vector_margin = 100, with the pause off and the
-- epsilon raised as in vector.sql, some participant probes every partition
-- that could hold a nearer row, so the rows and their order are the sort's;
-- at the defaults each participant stops by its own rules, and the share of
-- the nearest rows found is compared with the serial scan's.
-- pg_lion.vector_prefetch, and effective_io_concurrency = 0, change no row
-- a scan returns.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- parallel safe: a query that calls a function that is not plans no Gather
CREATE FUNCTION ps_vec(seed bigint, n int, scale float8 DEFAULT 1.0)
RETURNS vector LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT array_agg(((hashint8extended(seed * 1000 + j, 44) % 100000)::float8
					  / 100000.0 * scale)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

/*
 * ps_plan() prints q's plan, each vector literal as '...', and without the
 * line PostgreSQL 18 adds under a node it had to take though disabled.
 */
CREATE FUNCTION ps_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln !~ '^\s*Disabled' THEN
			RETURN NEXT regexp_replace(ln, '''\[[^]]*\]''::(vector|halfvec)', '''...''', 'g');
		END IF;
	END LOOP;
END $$;

/*
 * ps_how() says how q's plan scans the vectors: a Gather Merge over a
 * Parallel Index Scan, the lionvec index serially, LionOrdered, or a sort.
 */
CREATE FUNCTION ps_how(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	gm boolean := false;
	how text := 'a sort';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Gather Merge' THEN
			gm := true;
		END IF;
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			how := 'LionOrdered';
		ELSIF ln ~ 'Parallel Index Scan using psh?_(l2|ip|cos)' AND how = 'a sort' THEN
			how := CASE WHEN gm THEN 'Gather Merge, Parallel Index Scan'
					ELSE 'Parallel Index Scan' END;
		ELSIF ln ~ 'Index Scan using psh?_(l2|ip|cos)' AND how = 'a sort' THEN
			how := 'the index, serially';
		END IF;
	END LOOP;
	RETURN how;
END $$;

/*
 * ps_rows() wraps q - which returns columns id and d, ORDER BY d - in the
 * query ps_cmp() runs: its distances in the order q returns them (to 4
 * places), and its rows with a distance, sorted.  The order is taken by a
 * window over q's rows, above any Gather, and the query is run whole (EXECUTE
 * ... INTO), as a PL/pgSQL loop over rows, which might stop early, never
 * runs a parallel plan.
 */
CREATE FUNCTION ps_rows(q text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT format('SELECT array_agg(coalesce(round(d::numeric, 4)::text, ''NULL'') ORDER BY n),
						  array_agg(id ORDER BY id) FILTER (WHERE d IS NOT NULL)
					 FROM (SELECT id, d, row_number() OVER () AS n FROM (%s) q) s', q)
$$;

/*
 * ps_cmp() runs q as planned under the settings in force, and as a sort (no
 * index scan, no parallel plan, and pg_lion.vector_postfilter exact, which
 * sorts a filter LionOrdered does not answer where the default, error, would
 * fail the query), and compares the distances in order, then the rows with
 * a distance (rows of equal distance may come in either order, and from
 * either participant), then the count of the rest (NULL vectors); and says
 * how the first plan scanned the vectors (ps_how()).  A row two
 * participants both returned would be a mismatch.
 */
CREATE FUNCTION ps_cmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	w text := ps_rows(q);
	how text := ps_how(w);
	ad text[];
	bd text[];
	ai int[];
	bi int[];
	saved text[] := ARRAY[current_setting('enable_seqscan'),
						  current_setting('enable_bitmapscan'),
						  current_setting('enable_indexscan'),
						  current_setting('enable_sort'),
						  current_setting('max_parallel_workers_per_gather'),
						  current_setting('pg_lion.vector_postfilter')];
BEGIN
	EXECUTE w INTO ad, ai;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('pg_lion.vector_postfilter', 'exact', true);
	EXECUTE w INTO bd, bi;
	PERFORM set_config('enable_seqscan', saved[1], true);
	PERFORM set_config('enable_bitmapscan', saved[2], true);
	PERFORM set_config('enable_indexscan', saved[3], true);
	PERFORM set_config('enable_sort', saved[4], true);
	PERFORM set_config('max_parallel_workers_per_gather', saved[5], true);
	PERFORM set_config('pg_lion.vector_postfilter', saved[6], true);

	IF ad IS DISTINCT FROM bd THEN
		RETURN format('MISMATCH: %s rows, %s by the sort; distances %s / %s',
					  coalesce(array_length(ad, 1), 0), coalesce(array_length(bd, 1), 0),
					  ad[1:3], bd[1:3]);
	END IF;
	IF ai IS DISTINCT FROM bi THEN
		RETURN 'MISMATCH: other rows at the same distances';
	END IF;
	RETURN format('%s: the sort''s %s rows', how, coalesce(array_length(ad, 1), 0));
END $$;

/*
 * ps_recall() runs q - which returns a column id - with w workers a Gather
 * (0: the serial scan) and as a sort, and returns the share of the second's
 * rows the first has.
 */
CREATE FUNCTION ps_recall(q text, w int) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	a int[];
	b int[];
	saved text := current_setting('max_parallel_workers_per_gather');
BEGIN
	PERFORM set_config('max_parallel_workers_per_gather', w::text, true);
	EXECUTE 'SELECT array_agg(id) FROM (' || q || ') s' INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE 'SELECT array_agg(id) FROM (' || q || ') s' INTO b;
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', saved, true);
	RETURN (SELECT count(*) FROM unnest(a) x WHERE x = ANY (b))::float8 /
		greatest(cardinality(b), 1);
END $$;

/* ps_ids() is q's ids - a column id - in the order q returns them. */
CREATE FUNCTION ps_ids(q text) RETURNS int[]
LANGUAGE plpgsql AS $$
DECLARE
	a int[];
BEGIN
	EXECUTE format('SELECT array_agg(id ORDER BY n)
					  FROM (SELECT id, row_number() OVER () AS n FROM (%s) q) s', q) INTO a;
	RETURN a;
END $$;

-- 3,000 rows in ten clusters, some NULL vectors and one zero vector, indexed
-- in 20 partitions for each operator; cat has a lion index
CREATE TABLE ps (
	id int PRIMARY KEY,
	cat int,			-- 10 values, the cluster
	v vector(24)
) WITH (autovacuum_enabled = off);
INSERT INTO ps
SELECT i, i % 10, ps_vec(-(i % 10) - 1, 24) + ps_vec(i, 24, 0.3)
  FROM generate_series(1, 3000) i;
UPDATE ps SET v = NULL WHERE id % 211 = 0;
UPDATE ps SET v = array_fill(0, ARRAY[24])::real[]::vector WHERE id = 7;
CREATE INDEX ps_l2 ON ps USING lionvec (v vector_l2_ops) WITH (partitions = 20);
CREATE INDEX ps_ip ON ps USING lionvec (v vector_ip_ops) WITH (partitions = 20);
CREATE INDEX ps_cos ON ps USING lionvec (v vector_cosine_ops) WITH (partitions = 20);
CREATE INDEX ps_cat ON ps USING lion (cat);
ALTER TABLE ps SET (parallel_workers = 2);
VACUUM (FREEZE, ANALYZE) ps;

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SET min_parallel_table_scan_size = 0;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;
SET pg_lion.vector_epsilon = 6;
SET pg_lion.vector_margin = 100;
SET pg_lion.vector_patience = 0;

-- the plan: a Gather Merge over a Parallel Index Scan of the operator's index
SELECT * FROM ps_plan('SELECT id FROM ps ORDER BY v <-> ps_vec(4, 24) LIMIT 10');
SELECT * FROM ps_plan('SELECT id, v <#> ps_vec(4, 24) AS d FROM ps ORDER BY v <#> ps_vec(4, 24) LIMIT 10');
SELECT * FROM ps_plan('SELECT id FROM ps ORDER BY v <=> ps_vec(4, 24)');

-- ... whose rows are the sort's: near a cluster and far from all, LIMITs
-- that stop in the partitions and none, where the zero vector (cosine: NaN)
-- and the NULL vectors come last, each row once though the index spills
CREATE TABLE ps_cases (op text, q text, k text);
INSERT INTO ps_cases
SELECT op, q, k
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('ps_vec(4, 24)'), ('ps_vec(-3, 24, 3.0)')) c(q),
	   (VALUES ('LIMIT 1'), ('LIMIT 10'), ('LIMIT 300'), ('')) l(k);
SELECT op, q, k, ps_cmp(format('SELECT id, v %s %s AS d FROM ps ORDER BY v %s %s %s',
							   op, q, op, q, k)) AS result
  FROM ps_cases ORDER BY op, q, k;

-- the workers alone, the leader only gathering
SET parallel_leader_participation = off;
SELECT op, q, k, ps_cmp(format('SELECT id, v %s %s AS d FROM ps ORDER BY v %s %s %s',
							   op, q, op, q, k)) AS result
  FROM ps_cases WHERE q = 'ps_vec(4, 24)' ORDER BY op, q, k;
RESET parallel_leader_participation;

-- the walks: a NULL query's, of every chain, and a cosine zero query's, of
-- the zero vectors and then the NULLs - each row once, by whichever
-- participant claimed its chain
SELECT ps_how('SELECT count(*), count(DISTINCT id)
				FROM (SELECT id FROM ps ORDER BY v <-> (SELECT v FROM ps WHERE id = 211)) s') AS plan;
SELECT count(*), count(DISTINCT id)
  FROM (SELECT id FROM ps ORDER BY v <-> (SELECT v FROM ps WHERE id = 211)) s;
SELECT ps_how('SELECT count(*), count(DISTINCT id)
				FROM (SELECT id FROM ps ORDER BY v <=> array_fill(0, ARRAY[24])::real[]::vector) s') AS plan;
SELECT count(*), count(DISTINCT id)
  FROM (SELECT id FROM ps ORDER BY v <=> array_fill(0, ARRAY[24])::real[]::vector) s;
SELECT ps_cmp('SELECT id, v <=> (SELECT v FROM ps WHERE id = 7) AS d FROM ps
			   ORDER BY v <=> (SELECT v FROM ps WHERE id = 7)') AS result;
-- ... and the order's tail: the farthest rows, the zero vector, the NULLs
SELECT ps_cmp('SELECT id, v <=> ps_vec(9, 24) AS d FROM ps
			   ORDER BY v <=> ps_vec(9, 24) OFFSET 2980') AS result;
-- with no ORDER BY the index is never scanned, in parallel or not
SELECT count(*) FROM ps;

-- a rescan: the Gather Merge on a nested loop's inner side runs twice, its
-- scan's claims cleared in between (amparallelrescan), and gives the same
-- rows each time
SET enable_material = off;
SELECT * FROM ps_plan('SELECT s.pass, s.id FROM generate_series(1, 2) g,
					   LATERAL (SELECT id, g AS pass FROM ps
								ORDER BY v <-> ps_vec(4, 24) LIMIT 3) s');
SELECT array_agg(id ORDER BY id) AS exact3
  FROM (SELECT id FROM ps ORDER BY v <-> ps_vec(4, 24) LIMIT 3) s \gset
SELECT s.pass, array_agg(s.id ORDER BY s.id) = :'exact3'::int[] AS "the nearest three"
  FROM generate_series(1, 2) g,
	   LATERAL (SELECT id, g AS pass FROM ps ORDER BY v <-> ps_vec(4, 24) LIMIT 3) s
 GROUP BY s.pass ORDER BY s.pass;
RESET enable_material;

-- an index that does not spill: one item a row
DROP INDEX ps_l2;
CREATE INDEX ps_l2n ON ps USING lionvec (v vector_l2_ops) WITH (partitions = 20, spill = false);
SELECT op, q, k, ps_cmp(format('SELECT id, v %s %s AS d FROM ps ORDER BY v %s %s %s',
							   op, q, op, q, k)) AS result
  FROM ps_cases WHERE op = '<->' ORDER BY op, q, k;
DROP INDEX ps_l2n;
CREATE INDEX ps_l2 ON ps USING lionvec (v vector_l2_ops) WITH (partitions = 20);

-- halfvec
CREATE TABLE psh (id int PRIMARY KEY, v halfvec(24)) WITH (autovacuum_enabled = off);
INSERT INTO psh SELECT id, v::halfvec FROM ps;
CREATE INDEX psh_l2 ON psh USING lionvec (v halfvec_l2_ops) WITH (partitions = 20);
CREATE INDEX psh_cos ON psh USING lionvec (v halfvec_cosine_ops) WITH (partitions = 20);
ALTER TABLE psh SET (parallel_workers = 2);
VACUUM (FREEZE, ANALYZE) psh;
SELECT op, k, ps_cmp(format('SELECT id, v %s ps_vec(4, 24)::halfvec AS d FROM psh
							 ORDER BY v %s ps_vec(4, 24)::halfvec %s', op, op, k)) AS result
  FROM (VALUES ('<->'), ('<=>')) o(op), (VALUES ('LIMIT 10'), ('')) l(k)
 ORDER BY op, k;
DROP TABLE psh;

-- a filter.  Under pg_lion.vector_postfilter error, the default, and exact
-- no plan filters the parallel scan's rows any more than the serial scan's:
-- a filter lion answers goes through LionOrdered, which is serial, and any
-- other fails, the parallel scan being no way round it, or under exact gets
-- a sort of its rows; their answers are the sort's
SELECT * FROM ps_plan('SELECT id FROM ps WHERE cat = 3 ORDER BY v <-> ps_vec(4, 24) LIMIT 10');
SELECT ps_how('SELECT id FROM ps WHERE id % 7 = 1 ORDER BY v <-> ps_vec(4, 24) LIMIT 10') AS plan;
SET pg_lion.vector_postfilter = exact;
SELECT ps_how('SELECT id FROM ps WHERE id % 7 = 1 ORDER BY v <-> ps_vec(4, 24) LIMIT 10') AS plan;
SELECT f, ps_cmp(format('SELECT id, v <-> ps_vec(4, 24) AS d FROM ps WHERE %s
						 ORDER BY v <-> ps_vec(4, 24) LIMIT 10', f)) AS result
  FROM (VALUES ('cat = 3'), ('id % 7 = 1')) f(f) ORDER BY f;
-- ... under allow the parallel scan with a Filter may be chosen, and at
-- this margin finds the same rows
SET pg_lion.vector_postfilter = allow;
SET pg_lion.enable_ordered_scan = off;
SELECT * FROM ps_plan('SELECT id FROM ps WHERE id % 7 = 1 ORDER BY v <-> ps_vec(4, 24) LIMIT 10');
SELECT ps_cmp('SELECT id, v <-> ps_vec(4, 24) AS d FROM ps WHERE id % 7 = 1
			   ORDER BY v <-> ps_vec(4, 24) LIMIT 10') AS result;
RESET pg_lion.enable_ordered_scan;
RESET pg_lion.vector_postfilter;

-- prefetch, and none: the same rows, in parallel at this margin ...
SET pg_lion.vector_prefetch = off;
SELECT op, ps_cmp(format('SELECT id, v %s ps_vec(4, 24) AS d FROM ps
						  ORDER BY v %s ps_vec(4, 24) LIMIT 100', op, op)) AS result
  FROM (VALUES ('<->'), ('<=>')) o(op) ORDER BY op;
RESET pg_lion.vector_prefetch;
SET effective_io_concurrency = 0;
SELECT op, ps_cmp(format('SELECT id, v %s ps_vec(4, 24) AS d FROM ps
						  ORDER BY v %s ps_vec(4, 24) LIMIT 100', op, op)) AS result
  FROM (VALUES ('<->'), ('<=>')) o(op) ORDER BY op;
RESET effective_io_concurrency;

-- ... and serially at the defaults, where the scan stops early and pauses,
-- through passes (a LIMIT past L), and through LionOrdered's restricted
-- scans, exact (lv_exact()) and, with no members exact, probed
RESET pg_lion.vector_epsilon;
RESET pg_lion.vector_margin;
RESET pg_lion.vector_patience;
SET max_parallel_workers_per_gather = 0;
CREATE FUNCTION ps_same(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a int[] := ps_ids(q);
	b int[];
	c int[];
	saved text[] := ARRAY[current_setting('pg_lion.vector_prefetch'),
						  current_setting('effective_io_concurrency')];
BEGIN
	PERFORM set_config('pg_lion.vector_prefetch', 'off', true);
	b := ps_ids(q);
	PERFORM set_config('pg_lion.vector_prefetch', saved[1], true);
	PERFORM set_config('effective_io_concurrency', '0', true);
	c := ps_ids(q);
	PERFORM set_config('effective_io_concurrency', saved[2], true);
	RETURN format('%s rows, %s', cardinality(a),
				  CASE WHEN a = b AND a = c THEN 'the same with no prefetch'
				  ELSE 'MISMATCH' END);
END $$;
CREATE TABLE ps_serial (q text);
INSERT INTO ps_serial VALUES
	('SELECT id FROM ps ORDER BY v <-> ps_vec(4, 24) LIMIT 10'),
	('SELECT id FROM ps ORDER BY v <=> ps_vec(-3, 24, 3.0) LIMIT 10'),
	('SELECT id FROM ps ORDER BY v <#> ps_vec(5, 24) LIMIT 700'),
	('SELECT id FROM ps WHERE cat = 3 ORDER BY v <-> ps_vec(4, 24) LIMIT 10'),
	('SELECT id FROM ps WHERE cat IN (1, 2, 3, 4, 5, 6) ORDER BY v <=> ps_vec(4, 24) LIMIT 10');
SELECT q, ps_same(q) AS result FROM ps_serial ORDER BY q;
SET pg_lion.vector_exact_rows = 0;
SET pg_lion.vector_exact_per_partition = 0;
SELECT q, ps_same(q) AS result FROM ps_serial WHERE q ~ 'WHERE' ORDER BY q;
RESET pg_lion.vector_exact_rows;
RESET pg_lion.vector_exact_per_partition;
DROP TABLE ps_serial;
DROP FUNCTION ps_same(text);

-- at the defaults in parallel, each participant stops by its own rules:
-- about the serial scan's share of the ten nearest
SET max_parallel_workers_per_gather = 2;
SELECT op, avg(ps_recall(format('SELECT id FROM ps ORDER BY v %s %s LIMIT 10', op, q), 2)) >= 0.9
		   AS "most of the nearest",
	   avg(ps_recall(format('SELECT id FROM ps ORDER BY v %s %s LIMIT 10', op, q), 2)) >=
	   avg(ps_recall(format('SELECT id FROM ps ORDER BY v %s %s LIMIT 10', op, q), 0)) - 0.1
		   AS "about the serial scan's"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (SELECT format('(ps_vec(-%s, 24) + ps_vec(%s, 24, 0.3))', s % 10 + 1, s + 9000) AS q
		  FROM generate_series(1, 20) s) c
 GROUP BY op ORDER BY op;

RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_index_scan_size;
RESET min_parallel_table_scan_size;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;
DROP TABLE ps, ps_cases;
DROP FUNCTION ps_vec(bigint, int, float8), ps_plan(text), ps_how(text), ps_rows(text),
	ps_cmp(text), ps_recall(text, int), ps_ids(text);
