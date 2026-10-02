-- A range on a column that does not drive the count, taken as a source
-- (DESIGN.md §32, "A range as a source").
--
-- `g, count(*) ... WHERE <range on k> GROUP BY g`, a range beside a sum over
-- another range, a range as an OR's arm and a range among the fact filters of
-- an FK-side join: the rows whose key lies in the range are one source of the
-- count, collected into memory once - summaries and all - or, when they do
-- not fit in a hash table's memory, summed over the walk of the range at
-- every count.  Every answer is checked against the same query with the
-- pushdown off.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_src() runs q with the pushdown forced (every other scan and join
 * disabled) and again with the pushdown off and the planner left alone, and
 * compares the answers as multisets.  It reports how the node took its ranges.
 */
CREATE FUNCTION lion_src(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	node jsonb;
	how text;
	got text;
	want text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	node := jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	IF node IS NULL THEN
		how := 'not pushed down';
	ELSE
		how := 'collected ' || coalesce(node ->> 'Range Sources Collected', '0') ||
			', walked ' || coalesce(node ->> 'Range Sources Walked', '0');
		IF node ->> 'Range Evaluation' IS NOT NULL THEN
			how := how || ', ' || (node ->> 'Range Evaluation');
		END IF;
	END IF;
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO got;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO want;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	IF got IS DISTINCT FROM want THEN
		RETURN format('MISMATCH: %s, pushed %s, seqscan %s', how, got, want);
	END IF;
	RETURN how || ': ' || left(got, 160);
END $$;

/* The same through a generic prepared plan, $1 and $2 kept Params. */
CREATE FUNCTION lion_src_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	got text;
	want text;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_srcp AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_srcp(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM lion_srcp_run(%L) s', args)
		INTO got;
	EXECUTE 'DEALLOCATE lion_srcp';

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE 'PREPARE lion_srcp AS ' || q;
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM lion_srcp_run(%L) s', args)
		INTO want;
	EXECUTE 'DEALLOCATE lion_srcp';
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('plan_cache_mode', 'auto', true);

	IF got IS DISTINCT FROM want THEN
		RETURN format('MISMATCH: pushed %s, seqscan %s', got, want);
	END IF;
	RETURN CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END ||
		': ' || left(got, 160);
END $$;

/* The rows of EXECUTE lion_srcp(args), as text. */
CREATE FUNCTION lion_srcp_run(args text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	FOR r IN EXECUTE 'EXECUTE lion_srcp(' || args || ')' LOOP
		RETURN NEXT r::text;
	END LOOP;
END $$;

-- ---------- the table ----------
-- u unique and in heap order, r unique in no order, g 7 values, k 800
-- values with NULLs, a 30 values, tags a small array
CREATE TABLE lion_src_t (id int NOT NULL, u int NOT NULL, r int NOT NULL,
						 g int NOT NULL, k int, a int NOT NULL, tags int[]);
INSERT INTO lion_src_t
SELECT i, i, (i * 7919) % 20011, i % 7,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 17) % 800 END,
	   i % 30, ARRAY[i % 5, 10 + i % 4]
  FROM generate_series(1, 20000) i;
CREATE INDEX lion_src_ur ON lion_src_t USING lion (u, r)
	WITH (summaries = on, summary_tids = 64);
CREATE INDEX lion_src_g ON lion_src_t USING lion (g);
CREATE INDEX lion_src_k ON lion_src_t USING lion (k);
CREATE INDEX lion_src_a ON lion_src_t USING lion (a);
CREATE INDEX lion_src_tags ON lion_src_t USING lion (tags);
VACUUM (FREEZE, ANALYZE) lion_src_t;

-- ---------- 1. a range on a column that does not drive a GROUP BY ----------
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u >= 5000 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u BETWEEN 3000 AND 11000 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE r < 900 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE k > 700 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE k > 700 AND k <= 790 AND a = 3 GROUP BY g');
SELECT lion_src('SELECT g, count(k) FROM lion_src_t WHERE k < 20 GROUP BY g');
-- two ranges beside a GROUP BY, and two bounds on one side of one
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u > 100 AND r < 15000 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u < 9000 AND u <= 8000::int8 AND r > 50 GROUP BY g');
-- an empty range, a range outside the keys
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u BETWEEN 900 AND 300 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u > 5000000000::int8 GROUP BY g');
-- a range and an equality on one column: two sources
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE k > 100 AND k = 257 GROUP BY g');
-- a two-column GROUP BY, and count(DISTINCT) beside a range on another column
SELECT lion_src('SELECT g, a, count(*) FROM lion_src_t WHERE u < 700 GROUP BY g, a');
SELECT lion_src('SELECT count(DISTINCT a) FROM lion_src_t WHERE u BETWEEN 100 AND 130');
SELECT lion_src('SELECT g, count(DISTINCT a) FROM lion_src_t WHERE r < 300 GROUP BY g');

-- ---------- 2. two ranges and no GROUP BY: the wider drives, the other is a source ----------
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE u >= 100 AND r < 5000');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE u BETWEEN 100 AND 300 AND r > 50');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE k >= 3 AND k < 790 AND u > 1000');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE k >= 3 AND u > 1000 AND r < 19000 AND g = 2');

-- ---------- 3. a range as an OR's arm ----------
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE (u < 100 OR a = 3) GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE (u < 100 OR u > 19900) GROUP BY g');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE ((k >= 5 AND k < 9 AND a = 1) OR a = 2) AND g = 1');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE ((r BETWEEN 10 AND 900) OR a = 7) AND g = 4');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE (r < 100 OR k > 790) AND u > 17000');
-- with nothing to carry the §9 interlock beside the collected range, left to
-- the ordinary plan
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE u < 100 OR a = 3');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE (k >= 5 AND k < 9 AND a = 1) OR a = 2');

-- ---------- 4. too large for a hash table's memory: walked at every count ----------
-- A range over half of the rows of a column in no order is a NARROW of
-- width 2 (DESIGN.md §38), a kilobyte, at each container key of the heap,
-- and 360,000 rows 65 to a page are some ninety of them: more than the 64 kB
-- of work_mem's floor.  (The pad puts the rows just past the 63 offsets of
-- a NARROW of width 1, which makes a NARROW the largest for the rows it
-- holds.)  a takes each value for two rows in turn, so that a range of it
-- is RUNs of a few hundred bytes a key, which are collected beside a walked
-- range of r.
CREATE TABLE lion_src_w (u int NOT NULL, r int NOT NULL, g int NOT NULL,
						 a int NOT NULL, pad text);
INSERT INTO lion_src_w
SELECT i, hashint4(i) & 1048575, i % 7, i / 2 % 30, repeat('x', 79)
  FROM generate_series(1, 360000) i;
CREATE INDEX lion_src_w_ur ON lion_src_w USING lion (u, r)
	WITH (summaries = on, summary_tids = 1024);
CREATE INDEX lion_src_w_g ON lion_src_w USING lion (g);
CREATE INDEX lion_src_w_a ON lion_src_w USING lion (a);
VACUUM (FREEZE, ANALYZE) lion_src_w;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT lion_src('SELECT g, count(*) FROM lion_src_w WHERE r < 524288 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_w WHERE r >= 300000 AND r < 800000 AND a < 20 GROUP BY g');
-- the complement of a walked range, and its inside
SELECT lion_src('SELECT count(*) FROM lion_src_w WHERE u >= 100 AND r BETWEEN 1000 AND 1040000');
SELECT lion_src('SELECT count(*) FROM lion_src_w WHERE u BETWEEN 100 AND 340000 AND r > 524288 AND g = 3');
-- existence tests over a walked range
SELECT lion_src('SELECT count(DISTINCT a) FROM lion_src_w WHERE r < 524288');
SELECT lion_src('SELECT g, count(DISTINCT a) FROM lion_src_w WHERE r > 500000 GROUP BY g');
-- dirty
UPDATE lion_src_w SET a = a + 1 WHERE u % 50 = 0;
DELETE FROM lion_src_w WHERE u % 70 = 0;
SELECT lion_src('SELECT g, count(*) FROM lion_src_w WHERE r < 524288 GROUP BY g');
SELECT lion_src('SELECT count(*) FROM lion_src_w WHERE u >= 100 AND r BETWEEN 1000 AND 1040000');
RESET work_mem;
RESET hash_mem_multiplier;
-- and the same with the memory to collect it
SELECT lion_src('SELECT g, count(*) FROM lion_src_w WHERE r < 524288 GROUP BY g');
DROP TABLE lion_src_w;

-- ---------- 5. generic plans: bounds as Params, a NULL one, a multi-key Param ----------
SELECT lion_src_prep('SELECT g, count(*) FROM lion_src_t WHERE u >= $1 AND u < $2 GROUP BY g', '1000, 9000');
SELECT lion_src_prep('SELECT g, count(*) FROM lion_src_t WHERE u >= $1 AND u < $2 GROUP BY g', 'NULL, 9000');
SELECT lion_src_prep('SELECT count(*) FROM lion_src_t WHERE u >= $1 AND r < $2', '10, 7000');
-- beside a multi-key clause whose value is known only at run time, and one
-- that the posting sets answer only as a superset (a NULL element)
SELECT lion_src_prep('SELECT g, count(*) FROM lion_src_t WHERE tags @> $1 AND u < $2 GROUP BY g', '''{2}'', 12000');
SELECT lion_src_prep('SELECT g, count(*) FROM lion_src_t WHERE tags @> $1 AND u < $2 GROUP BY g', '''{2,NULL}'', 12000');
SELECT lion_src_prep('SELECT count(*) FROM lion_src_t WHERE tags && $1 AND u < $2 AND r > 100', '''{1,11}'', 15000');

-- ---------- 6. a dirty heap, and VACUUM ----------
UPDATE lion_src_t SET a = a + 1 WHERE id % 9 = 0;
DELETE FROM lion_src_t WHERE id % 11 = 0;
INSERT INTO lion_src_t SELECT 30000 + i, 30000 + i, 20011 + i, i % 7, i % 800, i % 30, ARRAY[i % 5]
  FROM generate_series(1, 500) i;
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u >= 5000 GROUP BY g');
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE r < 900 OR a = 3 GROUP BY g');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE u >= 100 AND r < 5000');
VACUUM (FREEZE) lion_src_t;
SELECT lion_src('SELECT g, count(*) FROM lion_src_t WHERE u >= 5000 GROUP BY g');
SELECT lion_src('SELECT count(*) FROM lion_src_t WHERE u >= 100 AND r < 5000');
SELECT lion_index_verify('lion_src_ur', true);

-- ---------- 7. a partitioned table ----------
CREATE TABLE lion_src_p (u int NOT NULL, g int NOT NULL, ts timestamptz NOT NULL)
	PARTITION BY RANGE (u);
CREATE TABLE lion_src_p1 PARTITION OF lion_src_p FOR VALUES FROM (0) TO (6000);
CREATE TABLE lion_src_p2 PARTITION OF lion_src_p FOR VALUES FROM (6000) TO (20000);
INSERT INTO lion_src_p SELECT i, i % 5, '2025-01-01'::timestamptz + (i * 37 % 20000) * interval '1 minute'
  FROM generate_series(0, 19999) i;
CREATE INDEX lion_src_p1_i ON lion_src_p1 USING lion (u, g, ts) WITH (summaries = on, summary_tids = 64);
CREATE INDEX lion_src_p2_i ON lion_src_p2 USING lion (u, g, ts);
VACUUM (FREEZE, ANALYZE) lion_src_p;
SELECT lion_src('SELECT g, count(*) FROM lion_src_p WHERE ts >= ''2025-01-05'' GROUP BY g');
SELECT lion_src('SELECT count(*) FROM lion_src_p WHERE u > 300 AND ts < ''2025-01-09''');

-- ---------- 8. the fact filters of an FK-side join ----------
CREATE TABLE lion_src_dim (pk int PRIMARY KEY, attr text NOT NULL);
INSERT INTO lion_src_dim SELECT i, 'attr' || (i % 4) FROM generate_series(0, 29) i;
CREATE TABLE lion_src_fact (fk int NOT NULL, ts int NOT NULL, v int NOT NULL);
INSERT INTO lion_src_fact SELECT i % 30, i, i % 11 FROM generate_series(1, 30000) i;
CREATE INDEX lion_src_fact_fk ON lion_src_fact USING lion (fk);
CREATE INDEX lion_src_fact_tsv ON lion_src_fact USING lion (ts, v)
	WITH (summaries = on, summary_tids = 128);
VACUUM (FREEZE, ANALYZE) lion_src_fact;
ANALYZE lion_src_dim;
SELECT lion_src('SELECT d.attr, count(*) FROM lion_src_fact f JOIN lion_src_dim d ON f.fk = d.pk WHERE f.ts >= 12000 GROUP BY d.attr');
SELECT lion_src('SELECT d.attr, count(*) FROM lion_src_fact f JOIN lion_src_dim d ON f.fk = d.pk WHERE f.ts BETWEEN 100 AND 25000 AND f.v = 3 GROUP BY d.attr');
SELECT lion_src('SELECT count(*) FROM lion_src_fact f JOIN lion_src_dim d ON f.fk = d.pk WHERE f.ts < 9000 AND d.attr = ''attr1''');

-- EXPLAIN names the range with every bound
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lion_src_t WHERE u > 100 AND u <= 900 AND r < 15000 GROUP BY g;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_src_t WHERE (u < 100 OR a = 3) AND g = 2;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- ---------- 9. what collecting a range costs (DESIGN.md §32, 2026-09-29) ----------
-- The FK-side join collects a range among its fact filters once, and its
-- union is most of what that costs: a fold of every container of it that is
-- not a bitset yet, each time a set of the range brings it members.  Over a
-- few dimension rows that is more than a nested loop's handful of index
-- probes, and the planner now says so; over more keys and a narrower range
-- the node wins.  200,000 narrow rows lie on some seventeen container keys,
-- and a summary of 1024 scattered rows puts sixty members at each: the
-- union of a range over nearly all of them is merged into, summary by
-- summary, until it is a bitset.  The statistics read every row, so the
-- plans do not depend on a sample.
SET default_statistics_target = 1000;
CREATE TABLE lion_src_cd (pk int PRIMARY KEY, sel int NOT NULL, attr int NOT NULL);
INSERT INTO lion_src_cd
SELECT i, (hashint4(i) & 2147483647) % 1000000, i % 5 FROM generate_series(0, 4999) i;
CREATE INDEX lion_src_cd_sel ON lion_src_cd (sel) INCLUDE (pk, attr);
CREATE TABLE lion_src_cf (fk int NOT NULL, ts int NOT NULL, v int NOT NULL);
INSERT INTO lion_src_cf
SELECT (hashint4(i + 7) & 2147483647) % 5000, hashint4(i) & 1048575, i % 11
  FROM generate_series(1, 200000) i;
CREATE INDEX lion_src_cf_fk ON lion_src_cf USING lion (fk);
CREATE INDEX lion_src_cf_bfk ON lion_src_cf (fk);
CREATE INDEX lion_src_cf_ts ON lion_src_cf USING lion (ts)
	WITH (summaries = on, summary_tids = 1024);
VACUUM (FREEZE, ANALYZE) lion_src_cf;
VACUUM (FREEZE, ANALYZE) lion_src_cd;
RESET default_statistics_target;

/*
 * lion_src_chosen() says which plan the planner, left alone, takes for q -
 * the count pushdown or core's - and checks its answer against the pushdown
 * off.
 */
CREATE FUNCTION lion_src_chosen(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	pushed boolean;
	got text;
	want text;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
	pushed := jsonb_path_exists(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO got;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO want;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF got IS DISTINCT FROM want THEN
		RETURN format('MISMATCH: pushed %s, not pushed %s', got, want);
	END IF;
	RETURN CASE WHEN pushed THEN 'the node' ELSE 'core''s plan' END || ': ' ||
		left(got, 120);
END $$;

-- ten dimension rows and a range over nearly every row: the collection
-- costs more than ten probes of the fk btree (the node was chosen before it
-- was priced, and ran three times slower than the nested loop)
SELECT lion_src_chosen('SELECT d.attr, count(*) FROM lion_src_cf f JOIN lion_src_cd d ON f.fk = d.pk WHERE d.sel < 2000 AND f.ts < 1000000 GROUP BY d.attr');
-- a hundred, and a range over a tenth of the rows: the node
SELECT lion_src_chosen('SELECT d.attr, count(*) FROM lion_src_cf f JOIN lion_src_cd d ON f.fk = d.pk WHERE d.sel < 20000 AND f.ts < 100000 GROUP BY d.attr');

-- The collection is timed, under EXPLAIN ANALYZE with TIMING, in `Fact
-- Filter Locate Time`: it happens while the fact filters are located, which
-- was in none of the node's timers.
CREATE FUNCTION lion_src_timers(q text, timing boolean) RETURNS TABLE (locate boolean, collect boolean, ranges int)
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	node jsonb;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE format('EXPLAIN (ANALYZE, TIMING %s, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) %s',
				   CASE WHEN timing THEN 'ON' ELSE 'OFF' END, q) INTO j;
	node := jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	RETURN QUERY SELECT node ? 'Fact Filter Locate Time', node ? 'Fact Filter Collect Time',
		(node ->> 'Range Sources Collected')::int;
END $$;
SELECT * FROM lion_src_timers('SELECT d.attr, count(*) FROM lion_src_cf f JOIN lion_src_cd d ON f.fk = d.pk WHERE d.sel < 2000 AND f.ts < 1000000 GROUP BY d.attr', true);
SELECT * FROM lion_src_timers('SELECT d.attr, count(*) FROM lion_src_cf f JOIN lion_src_cd d ON f.fk = d.pk WHERE d.sel < 2000 AND f.ts < 1000000 GROUP BY d.attr', false);
DROP FUNCTION lion_src_chosen(text);
DROP FUNCTION lion_src_timers(text, boolean);
DROP TABLE lion_src_cf, lion_src_cd;

DROP TABLE lion_src_t, lion_src_p, lion_src_fact, lion_src_dim;
DROP FUNCTION lion_src(text);
DROP FUNCTION lion_src_prep(text, text);
DROP FUNCTION lion_srcp_run(text);
