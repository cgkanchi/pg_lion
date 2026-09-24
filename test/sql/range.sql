-- Range predicates over the sorted directory (DESIGN.md §28).
--
-- `<`, `<=`, `>=`, `>` and BETWEEN on a scalar lion column are answered by a
-- bounded walk of the directory: by the bitmap scan, and by the count
-- pushdown, where the range bounds the entry walk that drives the count.
-- Every answer is checked against a SEQUENTIAL SCAN, as a multiset in both
-- directions, so a wrong answer fails even if it happens to be stable.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- the plan-choice pins below must not depend on ANALYZE's sample
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_rq() runs a query as a BITMAP scan - every other scan disabled and
 * the pushdown off - and again as a sequential scan, and compares the two.
 * It reports whether a bitmap index scan was used at all (a collation the
 * index cannot answer, say, leaves only the sequential scan).
 */
CREATE FUNCTION lion_rq(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	bitmap boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Bitmap Index Scan%' THEN
			bitmap := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_rq_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_rq_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_rq_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_rq_on EXCEPT ALL SELECT * FROM lion_rq_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_rq_off EXCEPT ALL SELECT * FROM lion_rq_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_rq_on, lion_rq_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN bitmap THEN 'bitmap' ELSE 'no bitmap' END, nrows);
END $$;

/*
 * lion_rc() runs a query through the count pushdown - with every other scan
 * disabled when force is set, so that a shape the cost model would not pick
 * is still EXERCISED - and again as a sequential scan with the pushdown off,
 * and compares the two.  It reports whether the node was used.
 */
CREATE FUNCTION lion_rc(q text, force boolean DEFAULT true) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	IF force THEN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
	END IF;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_rc_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_rc_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_rc_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_rc_on EXCEPT ALL SELECT * FROM lion_rc_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_rc_off EXCEPT ALL SELECT * FROM lion_rc_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_rc_on, lion_rc_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* The same through a generic prepared plan, whose bounds stay Params. */
CREATE FUNCTION lion_rc_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_rcp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_rcp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_rc_on AS EXECUTE lion_rcp_on(%s)', args);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'PREPARE lion_rcp_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_rc_off AS EXECUTE lion_rcp_off(%s)', args);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_rcp_on';
	EXECUTE 'DEALLOCATE lion_rcp_off';

	EXECUTE 'SELECT count(*) FROM lion_rc_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_rc_on EXCEPT ALL SELECT * FROM lion_rc_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_rc_off EXCEPT ALL SELECT * FROM lion_rc_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_rc_on, lion_rc_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* Which plan the cost model picks, with nothing disabled: its scan nodes. */
CREATE FUNCTION lion_rpick(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	nodes text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(Custom Scan \(LionCount\)|Index Only Scan|Index Scan|Bitmap Index Scan|Seq Scan)' THEN
			nodes := nodes || CASE WHEN nodes = '' THEN '' ELSE '; ' END ||
				regexp_replace(regexp_replace(ln, '^[ ->]*', ''), ' using \S+', '');
		END IF;
	END LOOP;
	RETURN nodes;
END $$;

-- EXPLAIN in a form every supported release prints the same way (the
-- definition test/sql/citext.sql uses).
CREATE OR REPLACE FUNCTION lion_explain_norm(q text, opts text DEFAULT 'COSTS OFF')
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (' || opts || ') ' || q LOOP
		CONTINUE WHEN l ~ '^\s*Disabled: true$';
		l := regexp_replace(l, '(InitPlan|SubPlan) (\d+)', '\1 expr_\2', 'g');
		RETURN NEXT regexp_replace(l, 'rows=(\d+)\.00 ', 'rows=\1 ', 'g');
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);
END
$$;

-- ---------- the opclasses ----------
-- strategies 6..9 on every ordered scalar class, nowhere else
SELECT o.amopstrategy, count(DISTINCT c.opcname) AS classes
  FROM pg_amop o JOIN pg_opclass c ON c.opcfamily = o.amopfamily
 WHERE o.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'lion')
   AND c.opcmethod = o.amopmethod
 GROUP BY 1 ORDER BY 1;
SELECT c.opcname
  FROM pg_opclass c
 WHERE c.opcmethod = (SELECT oid FROM pg_am WHERE amname = 'lion')
   AND NOT EXISTS (SELECT 1 FROM pg_amop o
					WHERE o.amopfamily = c.opcfamily AND o.amopstrategy = 6)
 ORDER BY 1;
-- every range operator of a family sits beside a proc 4 for its type pair
SELECT count(*) AS range_members,
	   count(*) FILTER (WHERE EXISTS (
			SELECT 1 FROM pg_amproc p
			 WHERE p.amprocfamily = o.amopfamily AND p.amprocnum = 4
			   AND p.amproclefttype = o.amoplefttype
			   AND p.amprocrighttype = o.amoprighttype)) AS with_proc4
  FROM pg_amop o
 WHERE o.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'lion')
   AND o.amopstrategy BETWEEN 6 AND 9;
SELECT amvalidate(c.oid) AS valid, count(*)
  FROM pg_opclass c
 WHERE c.opcmethod = (SELECT oid FROM pg_am WHERE amname = 'lion')
 GROUP BY 1;
-- the validator refuses a range strategy without proc 4, and one in a multi-key class
CREATE OPERATOR FAMILY lion_rfam USING lion;
CREATE OPERATOR CLASS lion_rcls FOR TYPE int4 USING lion FAMILY lion_rfam AS
	OPERATOR 1 = (int4, int4),
	OPERATOR 6 < (int4, int4),
	FUNCTION 1 hashint4(int4);
SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'lion_rcls';
DROP OPERATOR FAMILY lion_rfam USING lion;

-- ---------- the data ----------
CREATE TYPE lion_mood AS ENUM ('sad', 'ok', 'happy', 'ecstatic');
CREATE TABLE lion_r (
	id	int NOT NULL,
	k	int NOT NULL,			-- 0 .. 199
	x	int NOT NULL,			-- 0 .. 9
	y	int,					-- 0 .. 6, NULL every 11th row
	i2	int2,
	i4	int4,
	i8	int8,
	f4	float4,
	f8	float8,
	t	text,
	vc	varchar(10),
	n	numeric,
	d	date,
	ts	timestamp,
	tz	timestamptz,
	u	uuid,
	b	bool,
	e	lion_mood,
	ci	citext
);
INSERT INTO lion_r
SELECT i,
	   i % 200,
	   i % 10,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE i % 7 END,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE (i % 100 - 50)::int2 END,
	   CASE WHEN i % 19 = 0 THEN NULL ELSE i % 100 END,
	   CASE WHEN i % 23 = 0 THEN NULL ELSE (i % 60 - 10)::int8 * 1000 END,
	   CASE WHEN i % 13 = 0 THEN NULL
			ELSE ('{-Infinity,-1.5,-0,0,0.25,0.5,1,2.5,NaN,Infinity}'::float4[])[i % 10 + 1] END,
	   CASE WHEN i % 13 = 0 THEN NULL
			ELSE ('{-Infinity,-1.5,-0,0,0.25,0.5,1,2.5,NaN,Infinity}'::float8[])[i % 10 + 1] END,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE chr(97 + i % 26) || (i % 7) END,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE chr(97 + i % 26) || (i % 7) END,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE round((i % 40)::numeric / 8, 3) END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE date '2024-01-01' + i % 90 END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE timestamp '2024-01-01' + (i % 48) * interval '1 hour' END,
	   CASE WHEN i % 37 = 0 THEN NULL
			ELSE timestamptz '2024-01-01 00:00:00+00' + (i % 48) * interval '30 minutes' END,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE md5((i % 50)::text)::uuid END,
	   CASE i % 3 WHEN 0 THEN true WHEN 1 THEN false ELSE NULL END,
	   CASE WHEN i % 43 = 0 THEN NULL ELSE (enum_range(NULL::lion_mood))[i % 4 + 1] END,
	   CASE WHEN i % 47 = 0 THEN NULL
			WHEN i % 2 = 0 THEN upper(chr(97 + i % 26)) ELSE chr(97 + i % 26) END
  FROM generate_series(1, 20000) i;
CREATE INDEX lion_r_k ON lion_r USING lion (k);
CREATE INDEX lion_r_x ON lion_r USING lion (x);
CREATE INDEX lion_r_y ON lion_r USING lion (y);
CREATE INDEX lion_r_i2 ON lion_r USING lion (i2);
CREATE INDEX lion_r_i4 ON lion_r USING lion (i4);
CREATE INDEX lion_r_i8 ON lion_r USING lion (i8);
CREATE INDEX lion_r_f4 ON lion_r USING lion (f4);
CREATE INDEX lion_r_f8 ON lion_r USING lion (f8);
CREATE INDEX lion_r_t ON lion_r USING lion (t);
CREATE INDEX lion_r_vc ON lion_r USING lion (vc);
CREATE INDEX lion_r_n ON lion_r USING lion (n);
CREATE INDEX lion_r_d ON lion_r USING lion (d);
CREATE INDEX lion_r_ts ON lion_r USING lion (ts);
CREATE INDEX lion_r_tz ON lion_r USING lion (tz);
CREATE INDEX lion_r_u ON lion_r USING lion (u);
CREATE INDEX lion_r_b ON lion_r USING lion (b);
CREATE INDEX lion_r_e ON lion_r USING lion (e);
CREATE INDEX lion_r_ci ON lion_r USING lion (ci);
VACUUM ANALYZE lion_r;

-- ---------- 1. every strategy, on int4 (the bitmap scan) ----------
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 10');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 <= 10');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > 90');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 >= 90');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 BETWEEN 10 AND 20');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 >= 10 AND i4 < 20');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > 10 AND i4 <= 20');
SELECT lion_rq('SELECT id FROM lion_r WHERE 10 < i4');                 -- commuted
SELECT lion_rq('SELECT id FROM lion_r WHERE 20 >= i4 AND 10 <= i4');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > 5 AND i4 < 5');       -- empty
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 BETWEEN 20 AND 10');    -- inverted
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 >= 7 AND i4 <= 7');     -- equal bounds
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > 7 AND i4 <= 7');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > 5 AND i4 > 50 AND i4 < 60 AND i4 <= 55');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 >= 60 AND i4 > 50 AND i4 < 70');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < -1000');              -- below the keys
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > -1000');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > 1000000');            -- above them
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 2147483647');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > -2147483648');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 50 AND i4 = 20');     -- equality answers
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 50 AND i4 = 70');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 50 AND i4 IN (3, 60, 49)');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 50 AND i4 IS NOT NULL');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 50 AND i4 IS NULL');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < NULL::int4');
-- `< ANY (array)` reaches the index as an array key too: one walk per element
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < ANY (''{3, 50}'')');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 >= ANY (ARRAY[90, NULL, 95])');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > ANY (''{}''::int[])');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < ANY (''{3, 50}'') AND i4 > 40');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 <= ALL (''{3, 50}'')');
SELECT lion_rq('SELECT id FROM lion_r WHERE i8 > ANY (''{2000, 40000}''::int4[])');
SELECT * FROM lion_explain_norm('SELECT id FROM lion_r WHERE i4 BETWEEN 10 AND 20') AS p("QUERY PLAN");

-- ---------- 2. cross-type integer bounds: int2, int4, int8 ----------
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 5000000000');          -- int8 bound, every key below
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > 5000000000');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 > -5000000000 AND i4 < 3::int8');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 BETWEEN 10::int2 AND 20::int8');
SELECT lion_rq('SELECT id FROM lion_r WHERE i2 < 40000');               -- int4 bound on int2
SELECT lion_rq('SELECT id FROM lion_r WHERE i2 > 40000');
SELECT lion_rq('SELECT id FROM lion_r WHERE i2 >= -10 AND i2 < 10::int8');
SELECT lion_rq('SELECT id FROM lion_r WHERE i2 < -32768');
SELECT lion_rq('SELECT id FROM lion_r WHERE i8 > 2::int2 AND i8 <= 30000::int4');
SELECT lion_rq('SELECT id FROM lion_r WHERE i8 < -9223372036854775807');
SELECT lion_rq('SELECT id FROM lion_r WHERE i8 >= 9223372036854775807');
SELECT lion_rq('SELECT id FROM lion_r WHERE 3::int8 > i4');             -- commuted cross-type

-- ---------- 3. floats: -0, NaN, Infinity, float4 against float8 ----------
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 < 0');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 <= 0');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 >= ''-0''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 > ''-0''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 >= ''NaN''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 < ''NaN''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 > ''Infinity''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 >= ''Infinity''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 <= ''-Infinity''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 BETWEEN ''-Infinity'' AND ''Infinity''');
SELECT lion_rq('SELECT id FROM lion_r WHERE f4 < 0.5::float8');
SELECT lion_rq('SELECT id FROM lion_r WHERE f4 > 0.25::float8 AND f4 <= ''NaN''::float8');
SELECT lion_rq('SELECT id FROM lion_r WHERE f8 >= 0.5::float4');
SELECT lion_rq('SELECT id FROM lion_r WHERE f4 < ''-0''::float4');
SELECT lion_rq('SELECT id FROM lion_r WHERE f4 BETWEEN ''-0''::float4 AND 0::float8');

-- ---------- 4. text and varchar under the default collation, and "C" ----------
SELECT lion_rq('SELECT id FROM lion_r WHERE t < ''k''');
SELECT lion_rq('SELECT id FROM lion_r WHERE t BETWEEN ''b'' AND ''d3''');
SELECT lion_rq('SELECT id FROM lion_r WHERE t >= ''x''');
SELECT lion_rq('SELECT id FROM lion_r WHERE t > ''z9''');
SELECT lion_rq('SELECT id FROM lion_r WHERE t < ''''');
SELECT lion_rq('SELECT id FROM lion_r WHERE vc < ''k''');               -- varchar through text_ops
SELECT lion_rq('SELECT id FROM lion_r WHERE vc >= ''m'' AND vc < ''n''::text');
-- a collation the index was not built under: not answered by it
SELECT lion_rq('SELECT id FROM lion_r WHERE t < ''k'' COLLATE "C"');
SELECT lion_rq('SELECT id FROM lion_r WHERE t < ''k'' COLLATE "POSIX"');
CREATE INDEX lion_r_tc ON lion_r USING lion (t COLLATE "C");
SELECT lion_rq('SELECT id FROM lion_r WHERE t < ''k'' COLLATE "C"');
SELECT lion_rq('SELECT id FROM lion_r WHERE t COLLATE "C" BETWEEN ''b'' AND ''d''');
SELECT lion_rq('SELECT id FROM lion_r WHERE t < ''k'' COLLATE "POSIX"');
DROP INDEX lion_r_tc;

-- ---------- 5. citext, numeric, dates and times, uuid, bool, enum ----------
SELECT lion_rq('SELECT id FROM lion_r WHERE ci < ''M''');
SELECT lion_rq('SELECT id FROM lion_r WHERE ci < ''m''');
SELECT lion_rq('SELECT id FROM lion_r WHERE ci BETWEEN ''C'' AND ''e''');
SELECT lion_rq('SELECT id FROM lion_r WHERE ci >= ''Z''');
SELECT lion_rq('SELECT id FROM lion_r WHERE n > 1.5');
SELECT lion_rq('SELECT id FROM lion_r WHERE n BETWEEN 1.0 AND 2.00');
SELECT lion_rq('SELECT id FROM lion_r WHERE n >= 2 AND n < 2.125');
SELECT lion_rq('SELECT id FROM lion_r WHERE n < 0');
SELECT lion_rq('SELECT id FROM lion_r WHERE d >= ''2024-01-10'' AND d < ''2024-02-01''');
SELECT lion_rq('SELECT id FROM lion_r WHERE d > ''2024-03-01''');
SELECT lion_rq('SELECT id FROM lion_r WHERE d < ''2023-01-01''');
SELECT lion_rq('SELECT id FROM lion_r WHERE ts BETWEEN ''2024-01-01 05:00'' AND ''2024-01-01 17:30''');
SELECT lion_rq('SELECT id FROM lion_r WHERE ts > ''2024-01-02 12:00''');
SELECT lion_rq('SELECT id FROM lion_r WHERE tz >= ''2024-01-01 06:00:00+00'' AND tz < ''2024-01-01 12:00:00+00''');
SELECT lion_rq('SELECT id FROM lion_r WHERE tz < ''2024-01-01 01:00:00-02''');
SELECT lion_rq('SELECT id FROM lion_r WHERE u < ''80000000-0000-0000-0000-000000000000''');
SELECT lion_rq('SELECT id FROM lion_r WHERE u >= ''c0000000-0000-0000-0000-000000000000''');
SELECT lion_rq('SELECT id FROM lion_r WHERE b < true');
SELECT lion_rq('SELECT id FROM lion_r WHERE b >= false');
SELECT lion_rq('SELECT id FROM lion_r WHERE b > true');
SELECT lion_rq('SELECT id FROM lion_r WHERE e > ''ok''');
SELECT lion_rq('SELECT id FROM lion_r WHERE e BETWEEN ''sad'' AND ''happy''');
SELECT lion_rq('SELECT id FROM lion_r WHERE e < ''sad''');

-- ---------- 6. the count pushdown: count(*) over a range ----------
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 20');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k BETWEEN 20 AND 40');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k > 180');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k >= 180 AND k <= 180');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k > 100 AND k < 50');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 1000000');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k > 5000000000');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE 30 >= k');
SELECT lion_rc('SELECT count(k), count(*) FROM lion_r WHERE k < 20');
SELECT lion_rc('SELECT count(i4) FROM lion_r WHERE i4 < 20');          -- nullable, but not in range
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE i4 < 20');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE i2 >= -10 AND i2 < 10::int8');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE f8 < 0');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE f8 >= ''NaN''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE t BETWEEN ''b'' AND ''d3''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE ci < ''M''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE n BETWEEN 1.0 AND 2.00');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE d >= ''2024-01-10'' AND d < ''2024-02-01''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE tz >= ''2024-01-01 06:00:00+00''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE u < ''80000000-0000-0000-0000-000000000000''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE b < true');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE e > ''ok''');
-- ... ANDed with filters on other columns, which stay sources
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND x = 3');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k BETWEEN 10 AND 150 AND x IN (1, 2, 7)');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND y IS NULL');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND y IS NOT NULL');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND k IS NOT NULL');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND (x = 1 OR y = 2)');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND x = 3 AND y = 4 AND i8 = 5000');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND x = 11');   -- a source with no entry
SELECT lion_rc('SELECT count(y) FROM lion_r WHERE k < 50 AND y IS NOT NULL');
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_r WHERE k BETWEEN 20 AND 40 AND x = 3') AS p("QUERY PLAN");
-- a GROUP BY the planner folds to one group has no row when it is empty
SELECT lion_rc('SELECT x, count(*) FROM lion_r WHERE x = 3 AND k < 50 GROUP BY x');
SELECT lion_rc('SELECT x, count(*) FROM lion_r WHERE x = 3 AND k > 1000 GROUP BY x');

-- ---------- 7. GROUP BY k WHERE <range on k>, and count(DISTINCT) ----------
SELECT lion_rc('SELECT k, count(*) FROM lion_r WHERE k < 20 GROUP BY k');
SELECT lion_rc('SELECT k, count(*) FROM lion_r WHERE k BETWEEN 20 AND 40 AND x = 3 GROUP BY k');
SELECT lion_rc('SELECT d, count(*) FROM lion_r WHERE d >= ''2024-02-01'' GROUP BY d');
SELECT lion_rc('SELECT t, count(*) FROM lion_r WHERE t < ''c'' GROUP BY t');
SELECT lion_rc('SELECT i4, count(*), count(i4) FROM lion_r WHERE i4 > 90 GROUP BY i4');
SELECT lion_rc('SELECT k, count(*) FROM lion_r WHERE k > 100 AND k < 50 GROUP BY k');
SELECT lion_rc('SELECT k, count(*) FROM lion_r WHERE k < 20 GROUP BY k HAVING count(*) > 99');
-- the walk is in key order, so ORDER BY k needs no Sort
SELECT * FROM lion_explain_norm('SELECT k, count(*) FROM lion_r WHERE k BETWEEN 20 AND 40 GROUP BY k ORDER BY k') AS p("QUERY PLAN");
SELECT k, count(*) FROM lion_r WHERE k BETWEEN 20 AND 25 GROUP BY k ORDER BY k;
SELECT lion_rc('SELECT count(DISTINCT k) FROM lion_r WHERE k < 20');
SELECT lion_rc('SELECT count(DISTINCT k) FROM lion_r WHERE k BETWEEN 20 AND 40 AND x = 3');
SELECT lion_rc('SELECT count(DISTINCT k), count(*) FROM lion_r WHERE k < 20');
SELECT lion_rc('SELECT count(DISTINCT i4) FROM lion_r WHERE i4 >= 95');
SELECT lion_rc('SELECT x, count(DISTINCT k) FROM lion_r WHERE x < 3 GROUP BY x');
SELECT lion_rc('SELECT count(DISTINCT k) FROM lion_r WHERE x = 3 AND k > 190 GROUP BY x');
SELECT * FROM lion_explain_norm('SELECT count(DISTINCT k) FROM lion_r WHERE k < 20') AS p("QUERY PLAN");

-- ---------- 8. declined shapes (the ordinary plan answers them) ----------
SELECT lion_rc('SELECT x, count(*) FROM lion_r WHERE k < 20 GROUP BY x');     -- range not the driver
SELECT lion_rc('SELECT count(DISTINCT x) FROM lion_r WHERE k < 20');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 20 AND i4 < 20');       -- two range columns
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 20 OR x = 3');          -- under an OR
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 20 AND k = 5');         -- beside = on k
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 20 AND k IN (5, 6)');
SELECT lion_rc('SELECT x, y, count(*) FROM lion_r WHERE x < 5 GROUP BY x, y');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE t < ''k'' COLLATE "C"');   -- collation mismatch
SELECT lion_rc('SELECT t, count(*) FROM lion_r WHERE t < ''k'' COLLATE "C" GROUP BY t');
SELECT lion_rc('SELECT n, count(*) FROM lion_r WHERE n > 1 GROUP BY n');      -- numeric is not printable
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE n > 1');                     -- ... but countable
-- an FK-side join's fact filter must be a source (DESIGN.md §27)
CREATE TABLE lion_rd (pk int PRIMARY KEY, name text NOT NULL);
INSERT INTO lion_rd SELECT i, 'n' || (i % 3) FROM generate_series(0, 9) i;
ANALYZE lion_rd;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT lion_rc('SELECT d.name, count(*) FROM lion_r f JOIN lion_rd d ON f.x = d.pk WHERE f.k < 20 GROUP BY d.name');
SELECT lion_rc('SELECT d.name, count(*) FROM lion_r f JOIN lion_rd d ON f.x = d.pk WHERE f.y = 2 GROUP BY d.name');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;

-- ---------- 9. parameters ----------
SELECT lion_rc_prep('SELECT count(*) FROM lion_r WHERE k >= $1 AND k < $2', '20, 40');
SELECT lion_rc_prep('SELECT count(*) FROM lion_r WHERE k >= $1 AND k < $2', '40, 20');
SELECT lion_rc_prep('SELECT count(*) FROM lion_r WHERE k >= $1 AND k < $2', 'NULL, 40');
SELECT lion_rc_prep('SELECT count(*) FROM lion_r WHERE k >= $1 AND k < $2 AND x = $3', '20, 140, 3');
SELECT lion_rc_prep('SELECT k, count(*) FROM lion_r WHERE k < $1 GROUP BY k', '15');
SELECT lion_rc_prep('SELECT k, count(*) FROM lion_r WHERE k < $1 GROUP BY k', 'NULL');
SELECT lion_rc_prep('SELECT count(DISTINCT k) FROM lion_r WHERE k > $1', '150');
SELECT lion_rc_prep('SELECT count(*) FROM lion_r WHERE i4 < $1::int8', '5000000000');
SELECT lion_rc_prep('SELECT count(*) FROM lion_r WHERE d BETWEEN $1 AND $2', '''2024-01-05'', ''2024-01-20''');
SET plan_cache_mode = force_generic_plan;
PREPARE lion_rp_q(int, int) AS SELECT count(*) FROM lion_r WHERE k >= $1 AND k < $2;
SELECT * FROM lion_explain_norm('EXECUTE lion_rp_q(20, 40)') AS p("QUERY PLAN");
EXECUTE lion_rp_q(20, 40);
DEALLOCATE lion_rp_q;
RESET plan_cache_mode;
-- an exec Param from a nested loop: the node is rescanned per outer row
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT lion_rc('SELECT v.lo, c.n FROM (VALUES (0, 10), (50, 60), (195, 1000), (300, 310), (9, 3)) v(lo, hi),
				LATERAL (SELECT count(*) AS n FROM lion_r WHERE k >= v.lo AND k < v.hi) c');
SELECT v.lo, c.n FROM (VALUES (0, 10), (50, 60), (195, 1000), (300, 310), (9, 3)) v(lo, hi),
	LATERAL (SELECT count(*) AS n FROM lion_r WHERE k >= v.lo AND k < v.hi) c ORDER BY 1;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- ---------- 10. a multicolumn index ----------
CREATE TABLE lion_rm (id int NOT NULL, a int NOT NULL, b int, c text);
INSERT INTO lion_rm SELECT i, i % 50, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 13 END,
						   chr(97 + i % 26) FROM generate_series(1, 10000) i;
CREATE INDEX lion_rm_abc ON lion_rm USING lion (a, b, c);
VACUUM ANALYZE lion_rm;
SELECT lion_rq('SELECT id FROM lion_rm WHERE a BETWEEN 3 AND 7');
SELECT lion_rq('SELECT id FROM lion_rm WHERE b > 10');
SELECT lion_rq('SELECT id FROM lion_rm WHERE c >= ''x''');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a BETWEEN 3 AND 7 AND b = 2');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a < 5 AND b > 7');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a < 5 AND b > 7 AND c < ''m''');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a < 25 AND b IS NULL');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a > 3 AND a = 5 AND c = ''f''');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a < 25 AND b IN (1, 2) AND c > ''q''');
SELECT lion_rq('SELECT id FROM lion_rm WHERE (a < 5 AND b = 1) OR (a > 45 AND c = ''a'')');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a < 0 AND b = 1');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a < ANY (''{3, 7}'') AND b = 2');
SELECT lion_rq('SELECT id FROM lion_rm WHERE a > ANY (''{40, 45}'') AND b IS NOT NULL');
SELECT lion_rc('SELECT count(*) FROM lion_rm WHERE a < 20');
SELECT lion_rc('SELECT count(*) FROM lion_rm WHERE a < 20 AND b = 2');
SELECT lion_rc('SELECT count(*) FROM lion_rm WHERE c BETWEEN ''d'' AND ''k'' AND a = 7');
SELECT lion_rc('SELECT a, count(*) FROM lion_rm WHERE a >= 40 AND b IS NOT NULL GROUP BY a');
SELECT lion_rc('SELECT b, count(*) FROM lion_rm WHERE b < 5 GROUP BY b');
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_rm WHERE a < 20 AND b = 2') AS p("QUERY PLAN");

-- ---------- 11. a partial index ----------
CREATE TABLE lion_rpi (id int NOT NULL, a int NOT NULL, flag bool NOT NULL);
INSERT INTO lion_rpi SELECT i, i % 100, i % 4 = 0 FROM generate_series(1, 8000) i;
CREATE INDEX lion_rpi_a ON lion_rpi USING lion (a) WHERE flag;
VACUUM ANALYZE lion_rpi;
SELECT lion_rq('SELECT id FROM lion_rpi WHERE a < 30 AND flag');
SELECT lion_rq('SELECT id FROM lion_rpi WHERE a BETWEEN 30 AND 70 AND flag');
SELECT lion_rq('SELECT id FROM lion_rpi WHERE a < 30');                 -- predicate not implied

-- ---------- 11b. an UNORDERED column: every entry is tested with the operator ----------
-- A proc 4 no btree family sorts with leaves the directory in hash order
-- (DESIGN.md §21, rule 3), so the walk cannot be bounded; it tests each
-- entry of the column with the range's operator instead (§28).
CREATE FUNCTION lion_rcmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	AS 'SELECT btint4cmp($1, $2)';
CREATE OPERATOR FAMILY lion_runord USING lion;
CREATE OPERATOR CLASS lion_runord_ops FOR TYPE int4 USING lion FAMILY lion_runord AS
	OPERATOR 1 = (int4, int4),
	OPERATOR 6 < (int4, int4),
	OPERATOR 7 <= (int4, int4),
	OPERATOR 8 >= (int4, int4),
	OPERATOR 9 > (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 lion_rcmp(int4, int4);
CREATE TABLE lion_ru (id int NOT NULL, a int NOT NULL);
INSERT INTO lion_ru SELECT i, i % 97 FROM generate_series(1, 5000) i;
CREATE INDEX lion_ru_a ON lion_ru USING lion (a lion_runord_ops);
VACUUM ANALYZE lion_ru;
SELECT ordered FROM lion_index_stats('lion_ru_a');
SELECT lion_rq('SELECT id FROM lion_ru WHERE a BETWEEN 10 AND 20');
SELECT lion_rq('SELECT id FROM lion_ru WHERE a > 90');
SELECT lion_rc('SELECT count(*) FROM lion_ru WHERE a < 30');
SELECT lion_rc('SELECT a, count(*) FROM lion_ru WHERE a >= 50 AND a < 60 GROUP BY a');
DROP TABLE lion_ru;
DROP OPERATOR FAMILY lion_runord USING lion;
DROP FUNCTION lion_rcmp(int4, int4);

-- ---------- 12. a partitioned table ----------
CREATE TABLE lion_rpt (id int NOT NULL, k int NOT NULL, x int NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE lion_rpt_1 PARTITION OF lion_rpt FOR VALUES FROM (0) TO (5000);
CREATE TABLE lion_rpt_2 (x int NOT NULL, k int NOT NULL, id int NOT NULL);  -- other numbering
ALTER TABLE lion_rpt ATTACH PARTITION lion_rpt_2 FOR VALUES FROM (5000) TO (20000);
INSERT INTO lion_rpt SELECT i, i % 100, i % 7 FROM generate_series(1, 12000) i;
CREATE INDEX lion_rpt_k ON lion_rpt USING lion (k);
CREATE INDEX lion_rpt_x ON lion_rpt USING lion (x);
VACUUM ANALYZE lion_rpt;
SELECT lion_rc('SELECT count(*) FROM lion_rpt WHERE k < 30');
SELECT lion_rc('SELECT count(*) FROM lion_rpt WHERE k BETWEEN 30 AND 60 AND x = 2');
SELECT lion_rc('SELECT k, count(*) FROM lion_rpt WHERE k > 90 GROUP BY k');
SELECT lion_rc('SELECT count(DISTINCT k) FROM lion_rpt WHERE k < 30');         -- declined (§26)
SELECT lion_rq('SELECT id FROM lion_rpt WHERE k BETWEEN 30 AND 33');
SELECT * FROM lion_explain_norm('SELECT k, count(*) FROM lion_rpt WHERE k > 90 GROUP BY k') AS p("QUERY PLAN");

-- ---------- 13. a dirty heap, then a clean one ----------
DELETE FROM lion_r WHERE id % 5 = 0;
UPDATE lion_r SET k = (k + 7) % 200, i4 = i4 + 1, d = d + 3, t = t || 'u' WHERE id % 7 = 0;
INSERT INTO lion_r (id, k, x, i4, d, t) SELECT 20000 + i, i % 13, i % 10, i % 100 - 5, date '2024-02-15', 'm' || i % 5
  FROM generate_series(1, 500) i;
SELECT lion_rq('SELECT id FROM lion_r WHERE k BETWEEN 20 AND 40');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 10');
SELECT lion_rq('SELECT id FROM lion_r WHERE d >= ''2024-02-10'' AND d < ''2024-03-01''');
SELECT lion_rq('SELECT id FROM lion_r WHERE t > ''m''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k BETWEEN 20 AND 40');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND x = 3');
SELECT lion_rc('SELECT k, count(*) FROM lion_r WHERE k < 20 GROUP BY k');
SELECT lion_rc('SELECT count(DISTINCT k) FROM lion_r WHERE k < 20 AND x = 2');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE i4 < 10');
SELECT lion_rc('SELECT d, count(*) FROM lion_r WHERE d >= ''2024-02-10'' GROUP BY d');
VACUUM lion_r;
SELECT lion_rq('SELECT id FROM lion_r WHERE k BETWEEN 20 AND 40');
SELECT lion_rq('SELECT id FROM lion_r WHERE i4 < 10');
SELECT lion_rq('SELECT id FROM lion_r WHERE d >= ''2024-02-10'' AND d < ''2024-03-01''');
SELECT lion_rq('SELECT id FROM lion_r WHERE t > ''m''');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k BETWEEN 20 AND 40');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE k < 50 AND x = 3');
SELECT lion_rc('SELECT k, count(*) FROM lion_r WHERE k < 20 GROUP BY k');
SELECT lion_rc('SELECT count(DISTINCT k) FROM lion_r WHERE k < 20 AND x = 2');
SELECT lion_rc('SELECT count(*) FROM lion_r WHERE i4 < 10');
SELECT lion_rc('SELECT d, count(*) FROM lion_r WHERE d >= ''2024-02-10'' GROUP BY d');
-- a clean heap is counted from the visibility map
SELECT l AS line
  FROM lion_explain_norm('SELECT count(*) FROM lion_r WHERE k BETWEEN 20 AND 40',
						 'ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF') AS l
 WHERE l ~ 'Rechecked|Summed';
SELECT lion_index_verify('lion_r_k', true);
SELECT lion_index_verify('lion_r_d', true);

-- ---------- 14. pins: a GROUP BY parked mid-walk holds one entry's ----------
CREATE FUNCTION lion_rpinned(idx regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM pg_buffercache
	 WHERE reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	   AND relfilenode = pg_relation_filenode(idx)
	   AND pinning_backends > 0
$$;
BEGIN;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
DECLARE lion_rcur CURSOR FOR
	SELECT k, count(*) FROM lion_r WHERE k BETWEEN 0 AND 199 GROUP BY k;
FETCH 3 FROM lion_rcur;
SELECT lion_rpinned('lion_r_k') <= 1 AS one_entry_pinned;
FETCH 100 FROM lion_rcur \g /dev/null
SELECT lion_rpinned('lion_r_k') <= 1 AS one_entry_pinned;
COMMIT;

-- ---------- 15. plan choice, nothing disabled ----------
-- lo: 200 values; uq: a timestamp that is unique and in heap order
CREATE TABLE lion_rp (id int NOT NULL, lo int NOT NULL, uq timestamp NOT NULL, pad text);
INSERT INTO lion_rp SELECT i, (i * 7919) % 200, timestamp '2024-01-01' + i * interval '1 second', repeat('x', 40)
  FROM generate_series(1, 100000) i;
CREATE INDEX lion_rp_lo_lion ON lion_rp USING lion (lo);
CREATE INDEX lion_rp_lo_bt ON lion_rp (lo);
CREATE INDEX lion_rp_uq_lion ON lion_rp USING lion (uq);
CREATE INDEX lion_rp_uq_bt ON lion_rp (uq);
VACUUM ANALYZE lion_rp;
SELECT lion_rpick('SELECT count(*) FROM lion_rp WHERE lo BETWEEN 10 AND 30');
SELECT lion_rpick('SELECT count(*) FROM lion_rp WHERE lo < 100');
SELECT lion_rpick('SELECT sum(length(pad)) FROM lion_rp WHERE lo BETWEEN 10 AND 11');
SELECT lion_rpick('SELECT count(*) FROM lion_rp WHERE uq BETWEEN ''2024-01-01 01:00'' AND ''2024-01-01 02:00''');
SELECT lion_rpick('SELECT sum(length(pad)) FROM lion_rp WHERE uq BETWEEN ''2024-01-01 01:00'' AND ''2024-01-01 02:00''');
SELECT lion_rpick('SELECT count(*) FROM lion_rp WHERE uq > ''2024-01-01 12:00''');
SELECT lion_rpick('SELECT lo, count(*) FROM lion_rp WHERE lo BETWEEN 10 AND 30 GROUP BY lo');
SELECT lion_rpick('SELECT uq, count(*) FROM lion_rp WHERE uq BETWEEN ''2024-01-01 01:00'' AND ''2024-01-01 02:00'' GROUP BY uq');
SELECT lion_rc('SELECT count(*) FROM lion_rp WHERE lo BETWEEN 10 AND 30', false);
SELECT lion_rc('SELECT count(*) FROM lion_rp WHERE uq BETWEEN ''2024-01-01 01:00'' AND ''2024-01-01 02:00''');

DROP FUNCTION lion_rpinned(regclass);
DROP TABLE lion_r, lion_rd, lion_rm, lion_rpi, lion_rpt, lion_rp;
DROP TYPE lion_mood;
DROP FUNCTION lion_rq(text);
DROP FUNCTION lion_rc(text, boolean);
DROP FUNCTION lion_rc_prep(text, text);
DROP FUNCTION lion_rpick(text);
