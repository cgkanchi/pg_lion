-- lionvec, the nearest-neighbour index of pgvector's `vector` (DESIGN.md §44).
--
-- Every ORDER BY below is run through the index (the sequential scan and
-- the sort disabled, so that nothing else can be chosen) and through a
-- sequential scan and sort (the index disabled), and the two answers are
-- compared: the distances in order, and the rows.  pg_lion.vector_epsilon
-- is raised for the comparisons, so that no bound failure can make a row
-- come back late (§44.4), and pg_lion.vector_margin, so that every
-- partition that could hold a nearer row is read; the default epsilon's own
-- rate is the unit tests', and the default margin's recall is measured at
-- the end.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- every operator class validates
SELECT opcname, amvalidate(c.oid)
  FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod
 WHERE a.amname = 'lionvec' ORDER BY opcname;

/*
 * vt_vec() is a pseudo-random vector of n coordinates in (-scale, scale),
 * the same on every platform: from hashint8extended(), not random().
 */
CREATE FUNCTION vt_vec(seed bigint, n int, scale float8 DEFAULT 1.0)
RETURNS vector LANGUAGE sql IMMUTABLE
-- an index expression below calls it, and CREATE INDEX runs it under a safe
-- search_path from PostgreSQL 17 on, where vector would not be found
SET search_path FROM CURRENT AS $$
	SELECT array_agg(((hashint8extended(seed * 1000 + j, 42) % 100000)::float8
					  / 100000.0 * scale)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

/*
 * vec_cmp() runs q - which returns columns id and d, ORDER BY d - through
 * the index and through a sequential scan and sort, and compares the
 * distances in order (to 4 places), then the rows with a distance, sorted
 * (rows of equal distance may come in either order), then the count of rows
 * without one (NULL vectors: any of them may come).  It says whether the
 * first plan really was an index scan.
 */
CREATE FUNCTION vec_cmp(q text) RETURNS text
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
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Index Scan using' THEN
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
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	FOR r IN EXECUTE q LOOP
		bd := bd || coalesce(round(r.d::numeric, 4)::text, 'NULL');
		IF r.d IS NOT NULL THEN
			bi := bi || r.id;
		END IF;
	END LOOP;
	PERFORM set_config('enable_indexscan', 'on', true);

	IF ad IS DISTINCT FROM bd THEN
		RETURN format('MISMATCH: %s rows through the index, %s by the sort; distances %s / %s',
					  coalesce(array_length(ad, 1), 0), coalesce(array_length(bd, 1), 0),
					  ad[1:3], bd[1:3]);
	END IF;
	IF (SELECT array_agg(x ORDER BY x) FROM unnest(ai) x) IS DISTINCT FROM
	   (SELECT array_agg(x ORDER BY x) FROM unnest(bi) x) THEN
		RETURN format('MISMATCH: other rows at the same distances');
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'index' ELSE 'no index' END,
				  coalesce(array_length(ad, 1), 0));
END $$;

-- 3000 rows in ten clusters, five NULL vectors and one of zeros; indexed in
-- 20 partitions, which a table this small would not be by itself
CREATE TABLE vt (id int PRIMARY KEY, cat int, v vector(40))
	WITH (autovacuum_enabled = off);
INSERT INTO vt
SELECT i, i % 10, vt_vec(-(i % 10) - 1, 40) + vt_vec(i, 40, 0.3)
  FROM generate_series(1, 3000) i;
INSERT INTO vt SELECT i, NULL, NULL FROM generate_series(3001, 3005) i;
INSERT INTO vt VALUES (3006, NULL, array_fill(0, ARRAY[40])::real[]::vector);

CREATE INDEX vt_l2 ON vt USING lionvec (v) WITH (partitions = 20);
CREATE INDEX vt_ip ON vt USING lionvec (v vector_ip_ops) WITH (partitions = 20);
CREATE INDEX vt_cos ON vt USING lionvec (v vector_cosine_ops) WITH (partitions = 20);
VACUUM (ANALYZE) vt;

SET pg_lion.vector_epsilon = 6;
SET pg_lion.vector_margin = 100;

-- every metric, a query vector in a cluster and between them, any LIMIT
CREATE TABLE vt_cases AS
SELECT op, q, k
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (VALUES ('(SELECT v FROM vt WHERE id = 7)'),
			   ('(SELECT v FROM vt WHERE id = 2999)'),
			   ('vt_vec(77, 40, 0.5)')) q(q),
	   (VALUES (1), (10), (100), (3006)) k(k);
SELECT op, k, vec_cmp(format('SELECT id, v %s %s AS d FROM vt ORDER BY v %s %s LIMIT %s',
							 op, q, op, q, k)) AS result
  FROM vt_cases ORDER BY op, q, k;

-- no LIMIT: every row, the NULL vectors last
SELECT vec_cmp('SELECT id, v <-> vt_vec(5, 40) AS d FROM vt ORDER BY v <-> vt_vec(5, 40)');

-- a NULL query (a NULL row's: a constant NULL is folded away): every row,
-- each at a NULL distance
SELECT vec_cmp('SELECT id, v <-> (SELECT v FROM vt WHERE id = 3001) AS d FROM vt
				ORDER BY v <-> (SELECT v FROM vt WHERE id = 3001) LIMIT 10');
SELECT vec_cmp('SELECT id, v <=> (SELECT v FROM vt WHERE id = 3001) AS d FROM vt
				ORDER BY v <=> (SELECT v FROM vt WHERE id = 3001)');

-- the cosine of the zero vector is NaN, after every number and before NULL
SELECT vec_cmp('SELECT id, v <=> vt_vec(9, 40) AS d FROM vt ORDER BY v <=> vt_vec(9, 40) OFFSET 2995');
SELECT vec_cmp(format('SELECT id, v <=> %L::vector AS d FROM vt ORDER BY v <=> %L::vector',
					  array_fill(0, ARRAY[40])::real[]::vector,
					  array_fill(0, ARRAY[40])::real[]::vector));

-- a query of other dimensions is pgvector's error
SET enable_seqscan = off;
SELECT id FROM vt ORDER BY v <-> '[1,2,3]' LIMIT 1;
RESET enable_seqscan;

-- the scan as it goes on past a pass: a batch of one, and passes cut to work_mem
SET pg_lion.vector_batch = 1;
SELECT vec_cmp('SELECT id, v <-> vt_vec(11, 40) AS d FROM vt ORDER BY v <-> vt_vec(11, 40) LIMIT 300');
SET work_mem = '64kB';
SELECT vec_cmp('SELECT id, v <#> vt_vec(12, 40) AS d FROM vt ORDER BY v <#> vt_vec(12, 40) LIMIT 2000');
RESET work_mem;
RESET pg_lion.vector_batch;

-- inserts after the build: a new cluster, far from the mean, which goes to
-- the partitions nearest it
INSERT INTO vt
SELECT i, 10, vt_vec(-11, 40, 4.0) + vt_vec(i, 40, 0.3)
  FROM generate_series(3007, 3600) i;
SELECT op, k, vec_cmp(format('SELECT id, v %s %s AS d FROM vt ORDER BY v %s %s LIMIT %s',
							 op, q, op, q, k)) AS result
  FROM vt_cases WHERE k IN (10, 3006) ORDER BY op, q, k;
SELECT vec_cmp('SELECT id, v <-> vt_vec(-11, 40, 4.0) AS d FROM vt ORDER BY v <-> vt_vec(-11, 40, 4.0) LIMIT 50');

-- updates, deletes and VACUUM; then more inserts into what it freed
UPDATE vt SET v = vt_vec(id + 100000, 40) WHERE id % 7 = 0;
DELETE FROM vt WHERE id % 3 = 0;
VACUUM vt;
SELECT op, k, vec_cmp(format('SELECT id, v %s %s AS d FROM vt ORDER BY v %s %s LIMIT %s',
							 op, q, op, q, k)) AS result
  FROM vt_cases WHERE k IN (10, 3006) ORDER BY op, q, k;
INSERT INTO vt
SELECT i, i % 10, vt_vec(-(i % 10) - 1, 40) + vt_vec(i, 40, 0.3)
  FROM generate_series(4000, 4500) i;
SELECT vec_cmp('SELECT id, v <-> vt_vec(13, 40) AS d FROM vt ORDER BY v <-> vt_vec(13, 40) LIMIT 200');

-- every row deleted: nothing comes back, NULLs included
CREATE TABLE vt2 AS SELECT * FROM vt WHERE id < 100 OR v IS NULL;
CREATE INDEX vt2_l2 ON vt2 USING lionvec (v);
DELETE FROM vt2;
VACUUM vt2;
SELECT vec_cmp('SELECT id, v <-> vt_vec(1, 40) AS d FROM vt2 ORDER BY v <-> vt_vec(1, 40)');

-- REINDEX, and a concurrent build
REINDEX INDEX vt_l2;
SELECT vec_cmp('SELECT id, v <-> vt_vec(14, 40) AS d FROM vt ORDER BY v <-> vt_vec(14, 40) LIMIT 100');
DROP INDEX vt_l2;
CREATE INDEX CONCURRENTLY vt_l2 ON vt USING lionvec (v) WITH (partitions = 20);
SELECT vec_cmp('SELECT id, v <-> vt_vec(15, 40) AS d FROM vt ORDER BY v <-> vt_vec(15, 40) LIMIT 100');

-- an expression index, and a partial one
CREATE INDEX vt_half ON vt USING lionvec ((v * vt_vec(16, 40)));
SELECT vec_cmp('SELECT id, (v * vt_vec(16, 40)) <-> vt_vec(17, 40) AS d FROM vt
				ORDER BY (v * vt_vec(16, 40)) <-> vt_vec(17, 40) LIMIT 50');
DROP INDEX vt_half;
CREATE INDEX vt_cat3 ON vt USING lionvec (v) WHERE cat = 3;
SELECT vec_cmp('SELECT id, v <-> vt_vec(18, 40) AS d FROM vt WHERE cat = 3
				ORDER BY v <-> vt_vec(18, 40) LIMIT 50');
DROP INDEX vt_cat3;

-- a scrollable cursor goes backward over a Material: the scan never does
SET enable_seqscan = off;
SET enable_sort = off;
BEGIN;
DECLARE vc SCROLL CURSOR FOR SELECT id FROM vt ORDER BY v <-> vt_vec(19, 40) LIMIT 5;
FETCH 3 FROM vc;
FETCH BACKWARD 2 FROM vc;
COMMIT;
RESET enable_sort;
RESET enable_seqscan;

-- a table empty at the build, whose column has no dimensions: the first row
-- sets them, and a row of others is pgvector's error
CREATE TABLE ve (id int, v vector);
CREATE INDEX ve_l2 ON ve USING lionvec (v);
CREATE INDEX ve_ip ON ve USING lionvec (v vector_ip_ops);
INSERT INTO ve VALUES (1, '[1,2,3]'), (2, '[3,2,1]'), (3, NULL), (4, '[0,0,1]');
INSERT INTO ve VALUES (5, '[1,2,3,4]');
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM ve ORDER BY v <-> '[1,1,2]' LIMIT 2;
SELECT id, v <-> '[1,1,2]' AS d FROM ve ORDER BY v <-> '[1,1,2]';
SELECT id, v <#> '[1,1,2]' AS d FROM ve ORDER BY v <#> '[1,1,2]';
SELECT id FROM ve ORDER BY v <-> '[1,1]';
RESET enable_seqscan;

-- unlogged: the init fork (ambuildempty)
CREATE UNLOGGED TABLE vu (id int, v vector(3));
CREATE INDEX vu_l2 ON vu USING lionvec (v);
INSERT INTO vu SELECT i, vt_vec(i, 3) FROM generate_series(1, 200) i;
SELECT vec_cmp('SELECT id, v <-> ''[0.1,0.2,0.3]'' AS d FROM vu ORDER BY v <-> ''[0.1,0.2,0.3]'' LIMIT 20');

-- more partitions than rows: as many as the rows, one each; and none, of
-- a table empty at the build, whose rows all go to the chain of rows in no
-- partition
DROP INDEX vt2_l2;
CREATE INDEX vt2_many ON vt2 USING lionvec (v) WITH (partitions = 500);
INSERT INTO vt2 SELECT * FROM vt WHERE id < 300 OR v IS NULL;
SELECT vec_cmp('SELECT id, v <-> vt_vec(20, 40) AS d FROM vt2 ORDER BY v <-> vt_vec(20, 40) LIMIT 20');
REINDEX INDEX vt2_many;
SELECT vec_cmp('SELECT id, v <-> vt_vec(20, 40) AS d FROM vt2 ORDER BY v <-> vt_vec(20, 40) LIMIT 20');
SELECT vec_cmp('SELECT id, v <-> vt_vec(20, 40) AS d FROM vt2 ORDER BY v <-> vt_vec(20, 40)');
DROP INDEX vt2_many;

/*
 * vec_recall() runs q - which returns a column id - through the index and
 * through a sequential scan and sort, and returns the share of the second's
 * rows the first has.
 */
CREATE FUNCTION vec_recall(q text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	a int[];
	b int[];
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	EXECUTE 'SELECT array_agg(id) FROM (' || q || ') s' INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'SELECT array_agg(id) FROM (' || q || ') s' INTO b;
	PERFORM set_config('enable_indexscan', 'on', true);
	RETURN (SELECT count(*) FROM unnest(a) x WHERE x = ANY (b))::float8 /
		greatest(cardinality(b), 1);
END $$;

-- the default margin reads the partitions near the query only, and misses
-- few of its nearest rows; a margin below zero reads fewer, and misses more
RESET pg_lion.vector_margin;
SELECT op, avg(vec_recall(format('SELECT id FROM vt ORDER BY v %s %s LIMIT 10',
								 op, q))) >= 0.9 AS "most of the nearest"
  FROM (VALUES ('<->'), ('<#>'), ('<=>')) o(op),
	   (SELECT format('(vt_vec(-%s, 40) + vt_vec(%s, 40, 0.3))', s % 10 + 1, s + 9000) AS q
		  FROM generate_series(1, 20) s) q
 GROUP BY op ORDER BY op;
SET pg_lion.vector_margin = -10;
SELECT vec_recall('SELECT id FROM vt ORDER BY v <-> vt_vec(77, 40, 0.5) LIMIT 100') < 1 AS "fewer";
RESET pg_lion.vector_margin;

-- what lionvec refuses
CREATE TABLE vbad (i int, v vector(9000), w vector(3)[]);
CREATE INDEX ON vbad USING lionvec (i);
CREATE INDEX ON vbad USING lionvec (v);
CREATE INDEX ON vbad USING lionvec (w);
CREATE INDEX ON vbad USING lionvec (v, v);
CREATE INDEX ON vt USING lionvec (v) WITH (partitions = -1);
CREATE INDEX ON vt USING lionvec (v) WITH (partitions = 1000001);
SET pg_lion.vector_epsilon = 0.5;
SET pg_lion.vector_batch = 0;
SET pg_lion.vector_margin = 1001;

DROP TABLE vt, vt2, vt_cases, ve, vu, vbad;
DROP FUNCTION vec_cmp(text);
DROP FUNCTION vec_recall(text);
DROP FUNCTION vt_vec(bigint, int, float8);
