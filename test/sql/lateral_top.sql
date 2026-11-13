-- The first rows of each group - DISTINCT ON, row_number() <= k, min or max
-- per group - planned again as a probe of each group's key (DESIGN.md §57):
-- where the rewrite is tried, that the cheaper plan is kept, and that every
-- answer is the same as with the rewrite off.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
/* every row in ANALYZE's sample, so that the estimates are the same each run */
SET default_statistics_target = 1000;
CREATE EXTENSION IF NOT EXISTS pg_lion;
SET max_parallel_workers_per_gather = 0;

-- ts is unique, so that every query below has one answer
CREATE TABLE lt_e (id int8 NOT NULL, ts timestamp NOT NULL, user_id int NOT NULL,
				   status text NOT NULL, country text NOT NULL, region text,
				   kind int NOT NULL, amount int, flag bool NOT NULL, city text);
CREATE FUNCTION lt_h(i int8, seed int8, m int8) RETURNS int8
LANGUAGE sql IMMUTABLE
AS 'SELECT (hashint8extended(i, seed) & 9223372036854775807) % m';
INSERT INTO lt_e
SELECT i, timestamp '2026-01-01' + i * interval '37 seconds',
	   1 + lt_h(i, 1, 3000),
	   (ARRAY['new', 'open', 'paid', 'void', 'done'])[1 + lt_h(i, 2, 5)],
	   'c' || lt_h(i, 3, 30),
	   CASE WHEN lt_h(i, 4, 11) = 0 THEN NULL ELSE 'r' || lt_h(i, 5, 7) END,
	   lt_h(i, 6, 20),
	   CASE WHEN lt_h(i, 7, 9) = 0 THEN NULL ELSE lt_h(i, 8, 5000) END,
	   lt_h(i, 9, 3) = 0,
	   'city' || lt_h(i, 10, 40)
  FROM generate_series(1, 60000) i;
CREATE INDEX lt_e_ts ON lt_e (ts DESC);
CREATE INDEX lt_e_amount ON lt_e (amount DESC NULLS LAST);
CREATE INDEX lt_e_country ON lt_e USING lion (country);
CREATE INDEX lt_e_status ON lt_e USING lion (status);
CREATE INDEX lt_e_region ON lt_e USING lion (region);
CREATE INDEX lt_e_user ON lt_e USING lion (user_id);
CREATE INDEX lt_e_kind ON lt_e USING lion (kind);
VACUUM ANALYZE lt_e;

/*
 * lt_check() says whether the planner kept the rewritten plan (its plan
 * differs from the one with pg_lion.enable_lateral_tops off) and compares
 * the rows of the two as multisets, each run by running q itself.
 */
CREATE FUNCTION lt_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	rec record;
	p_on text := '';
	p_off text := '';
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_lateral_tops', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		p_on := p_on || ln || E'\n';
	END LOOP;
	CREATE TEMP TABLE lt_on (r text);
	FOR rec IN EXECUTE q LOOP
		INSERT INTO lt_on VALUES (rec::text);
	END LOOP;
	PERFORM set_config('pg_lion.enable_lateral_tops', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		p_off := p_off || ln || E'\n';
	END LOOP;
	CREATE TEMP TABLE lt_off (r text);
	FOR rec IN EXECUTE q LOOP
		INSERT INTO lt_off VALUES (rec::text);
	END LOOP;
	PERFORM set_config('pg_lion.enable_lateral_tops', 'on', true);
	SELECT count(*) INTO nrows FROM lt_on;
	SELECT (SELECT count(*) FROM (TABLE lt_on EXCEPT ALL TABLE lt_off) a) +
		   (SELECT count(*) FROM (TABLE lt_off EXCEPT ALL TABLE lt_on) b)
	  INTO ndiff;
	DROP TABLE lt_on, lt_off;
	RETURN CASE WHEN p_on = p_off THEN 'as written' ELSE 'per key' END ||
		   ', ' || nrows || ' rows' ||
		   CASE WHEN ndiff = 0 THEN '' ELSE ', ' || ndiff || ' DIFFER' END;
END
$$;

-- ---- DISTINCT ON: the first row of each group ----
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC;
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC LIMIT 4;
SET pg_lion.enable_lateral_tops = off;
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC LIMIT 4;
RESET pg_lion.enable_lateral_tops;
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC$$);
-- every column, another order, more filters, a LIMIT and OFFSET
SELECT lt_check($$
SELECT DISTINCT ON (country) * FROM lt_e
 WHERE status = 'paid' AND kind IN (3, 4, 5) ORDER BY country, ts DESC$$);
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id FROM lt_e
 WHERE status IN ('paid', 'void') ORDER BY country DESC, ts$$);
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC LIMIT 5 OFFSET 2$$);
-- expressions of the row, a system column, and an order with NULLs in it
SELECT lt_check($$
SELECT DISTINCT ON (country) upper(country), ts::date, amount + 1 FROM lt_e
 WHERE status = 'open' ORDER BY country, amount DESC, id$$);
SELECT lt_check($$
SELECT DISTINCT ON (country) country, ctid, tableoid::regclass FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC$$);
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id, amount FROM lt_e
 WHERE status = 'done' ORDER BY country, amount NULLS FIRST, id DESC$$);
-- a grouping column that may be NULL, where the WHERE rejects the NULLs
SELECT lt_check($$
SELECT DISTINCT ON (region) region, id, ts FROM lt_e
 WHERE status = 'paid' AND region IS NOT NULL ORDER BY region, ts DESC$$);
SELECT lt_check($$
SELECT DISTINCT ON (region) region, id, ts FROM lt_e
 WHERE region IN ('r1', 'r2', 'r5') ORDER BY region, ts$$);
-- a key with no row the WHERE keeps
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' AND kind = 7 AND country IN ('c1', 'c2', 'no such')
 ORDER BY country, ts DESC$$);

-- ---- row_number() <= k: the first k rows of each group ----
EXPLAIN (COSTS OFF)
SELECT * FROM (SELECT id, country, ts,
					  row_number() OVER (PARTITION BY country ORDER BY ts DESC) AS rn
				 FROM lt_e WHERE status = 'paid') s
 WHERE rn <= 3;
SELECT lt_check($$
SELECT * FROM (SELECT id, country, ts,
					  row_number() OVER (PARTITION BY country ORDER BY ts DESC) AS rn
				 FROM lt_e WHERE status = 'paid') s
 WHERE rn <= 3$$);
-- the bound written other ways, and beside other filters
SELECT lt_check($$
SELECT id, rn FROM (SELECT id, ts, row_number() OVER w AS rn
					  FROM lt_e WHERE status = 'void'
					WINDOW w AS (PARTITION BY country ORDER BY ts)) s
 WHERE rn < 3$$);
SELECT lt_check($$
SELECT id, rn FROM (SELECT id, row_number() OVER (PARTITION BY country
												 ORDER BY ts DESC, id) AS rn
					  FROM lt_e WHERE status = 'paid' AND kind < 10) s
 WHERE 2 >= rn AND id % 2 = 0$$);
SELECT lt_check($$
SELECT id, rn FROM (SELECT id, row_number() OVER (PARTITION BY country
												 ORDER BY ts DESC) AS rn
					  FROM lt_e WHERE status = 'paid') s
 WHERE rn = 2$$);
SELECT lt_check($$
SELECT id FROM (SELECT id, row_number() OVER (PARTITION BY country
											 ORDER BY ts DESC) AS rn
				  FROM lt_e WHERE status = 'paid') s
 WHERE rn <= 0$$);
-- joined to another relation, under a sort and an aggregate of the outer query
CREATE TABLE lt_c (code text PRIMARY KEY, zone int);
INSERT INTO lt_c SELECT 'c' || i, i % 4 FROM generate_series(0, 29) i;
ANALYZE lt_c;
SELECT lt_check($$
SELECT c.zone, count(*), max(s.ts)
  FROM (SELECT id, country, ts,
			   row_number() OVER (PARTITION BY country ORDER BY ts DESC) AS rn
		  FROM lt_e WHERE status = 'paid') s
  JOIN lt_c c ON c.code = s.country
 WHERE s.rn <= 5 AND c.zone > 0
 GROUP BY c.zone$$);

-- ---- min or max per group: the first row of each group in the aggregate's order ----
EXPLAIN (COSTS OFF)
SELECT country, max(ts) FROM lt_e WHERE status = 'paid' GROUP BY country;
SELECT lt_check($$
SELECT country, max(ts) FROM lt_e WHERE status = 'paid' GROUP BY country$$);
SELECT lt_check($$
SELECT country, min(ts) FROM lt_e WHERE status = 'paid' AND kind > 3 GROUP BY country$$);
SELECT lt_check($$
SELECT country, max(ts), max(ts)::date FROM lt_e
 WHERE status = 'paid' GROUP BY country$$);
SELECT lt_check($$
SELECT country, max(ts) FROM lt_e WHERE status = 'paid'
 GROUP BY country HAVING max(ts) > '2026-01-25' AND country > 'c1'$$);
-- a column with NULLs: the greatest value, not the NULL
SELECT lt_check($$
SELECT country, max(amount) FROM lt_e WHERE status = 'void' GROUP BY country$$);
SELECT lt_check($$
SELECT country, max(amount) FROM lt_e
 WHERE status = 'void' AND amount IS NULL GROUP BY country$$);
-- under ORDER BY and LIMIT of the groups
SELECT lt_check($$
SELECT country, max(ts) AS last FROM lt_e WHERE status = 'paid'
 GROUP BY country ORDER BY last DESC LIMIT 3$$);

-- ---- inside another query ----
SELECT lt_check($$
SELECT c.zone, l.id
  FROM (SELECT DISTINCT ON (country) country, id FROM lt_e
		 WHERE status = 'paid' ORDER BY country, ts DESC) l
  JOIN lt_c c ON c.code = l.country
 WHERE c.zone = 2$$);
SELECT lt_check($$
SELECT count(*) FROM (SELECT country, max(ts) FROM lt_e
					   WHERE status = 'paid' GROUP BY country) m$$);

-- ---- a prepared statement: custom and generic plans ----
PREPARE lt_p(text) AS
SELECT DISTINCT ON (country) country, id FROM lt_e WHERE status = $1
 ORDER BY country, ts DESC;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE lt_p('paid');
CREATE TABLE lt_g AS EXECUTE lt_p('paid');
SET plan_cache_mode = force_custom_plan;
CREATE TABLE lt_cu AS EXECUTE lt_p('paid');
SET pg_lion.enable_lateral_tops = off;
CREATE TABLE lt_off AS EXECUTE lt_p('paid');
RESET pg_lion.enable_lateral_tops;
RESET plan_cache_mode;
SELECT (SELECT count(*) FROM lt_off) AS n,
	   (SELECT count(*) FROM (TABLE lt_g EXCEPT TABLE lt_off) a) +
	   (SELECT count(*) FROM (TABLE lt_off EXCEPT TABLE lt_g) b) AS generic_differ,
	   (SELECT count(*) FROM (TABLE lt_cu EXCEPT TABLE lt_off) a) +
	   (SELECT count(*) FROM (TABLE lt_off EXCEPT TABLE lt_cu) b) AS custom_differ;
DROP TABLE lt_g, lt_cu, lt_off;
DEALLOCATE lt_p;

-- ---- a partitioned table ----
CREATE TABLE lt_pt (id int8 NOT NULL, ts timestamp NOT NULL, country text NOT NULL,
					status text NOT NULL) PARTITION BY RANGE (ts);
CREATE TABLE lt_pt1 PARTITION OF lt_pt FOR VALUES FROM (MINVALUE) TO ('2026-01-10');
CREATE TABLE lt_pt2 PARTITION OF lt_pt FOR VALUES FROM ('2026-01-10') TO (MAXVALUE);
INSERT INTO lt_pt SELECT id, ts, country, status FROM lt_e;
CREATE INDEX lt_pt_ts ON lt_pt (ts);
CREATE INDEX lt_pt_country ON lt_pt USING lion (country);
CREATE INDEX lt_pt_status ON lt_pt USING lion (status);
VACUUM ANALYZE lt_pt;
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id, ts FROM lt_pt
 WHERE status = 'paid' ORDER BY country, ts DESC$$);
SELECT lt_check($$
SELECT country, min(ts) FROM lt_pt WHERE status = 'open' GROUP BY country$$);

-- ---- many groups: the probes cost more than the sort, and the query is
-- kept as written ----
SELECT lt_check($$
SELECT DISTINCT ON (user_id) user_id, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY user_id, ts DESC$$);

-- ---- left as written ----
-- a grouping column that may be NULL, where the WHERE does not reject it
SELECT lt_check($$
SELECT DISTINCT ON (region) region, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY region, ts DESC$$);
-- no lion index on the grouping column
SELECT lt_check($$
SELECT DISTINCT ON (city) city, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY city, ts DESC$$);
-- two grouping columns, a grouping expression
SELECT lt_check($$
SELECT DISTINCT ON (country, kind) country, kind, id FROM lt_e
 WHERE status = 'paid' ORDER BY country, kind, ts DESC$$);
SELECT lt_check($$
SELECT DISTINCT ON (upper(country)) country, id FROM lt_e
 WHERE status = 'paid' ORDER BY upper(country), ts DESC$$);
-- the whole row, a volatile function, a join in the FROM list
SELECT lt_check($$
SELECT DISTINCT ON (country) e FROM lt_e e
 WHERE status = 'paid' ORDER BY country, ts DESC$$);
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id, random() >= 0 FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC$$);
SELECT lt_check($$
SELECT DISTINCT ON (e.country) e.country, e.id FROM lt_e e JOIN lt_c c ON c.code = e.country
 WHERE e.status = 'paid' ORDER BY e.country, e.ts DESC$$);
-- aggregates that need every row of the group, or two orders, or a FILTER
SELECT lt_check($$
SELECT country, max(ts), count(*) FROM lt_e WHERE status = 'paid' GROUP BY country$$);
SELECT lt_check($$
SELECT country, max(ts), min(ts) FROM lt_e WHERE status = 'paid' GROUP BY country$$);
SELECT lt_check($$
SELECT country, max(ts) FILTER (WHERE kind = 3) FROM lt_e
 WHERE status = 'paid' GROUP BY country$$);
-- another window function, or one over all the group's rows
SELECT lt_check($$
SELECT * FROM (SELECT id, rank() OVER (PARTITION BY country ORDER BY ts DESC) AS rn
				 FROM lt_e WHERE status = 'paid') s
 WHERE rn <= 3$$);
SELECT lt_check($$
SELECT * FROM (SELECT id, row_number() OVER (PARTITION BY country ORDER BY ts DESC) AS rn,
					  count(*) OVER (PARTITION BY country) AS n
				 FROM lt_e WHERE status = 'paid') s
 WHERE rn <= 3$$);
-- no bound on the row number, or one in an outer join's ON, or a LIMIT
-- inside the subquery
SELECT lt_check($$
SELECT count(*) FROM (SELECT id, row_number() OVER (PARTITION BY country ORDER BY ts) AS rn
						FROM lt_e WHERE status = 'paid') s
 WHERE rn >= 2$$);
SELECT lt_check($$
SELECT c.code, s.id
  FROM lt_c c
  LEFT JOIN (SELECT id, country,
					row_number() OVER (PARTITION BY country ORDER BY ts DESC) AS rn
			   FROM lt_e WHERE status = 'paid') s ON s.country = c.code AND s.rn <= 2$$);
SELECT lt_check($$
SELECT * FROM (SELECT id, row_number() OVER (PARTITION BY country ORDER BY ts DESC) AS rn
				 FROM lt_e WHERE status = 'paid' ORDER BY id DESC LIMIT 500) s
 WHERE rn <= 3$$);
-- row-level security
ALTER TABLE lt_e ENABLE ROW LEVEL SECURITY;
ALTER TABLE lt_e FORCE ROW LEVEL SECURITY;
CREATE POLICY lt_e_p ON lt_e USING (kind < 15);
SELECT lt_check($$
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC$$);
DROP POLICY lt_e_p ON lt_e;
ALTER TABLE lt_e DISABLE ROW LEVEL SECURITY;
ALTER TABLE lt_e NO FORCE ROW LEVEL SECURITY;
-- the setting off
SET pg_lion.enable_lateral_tops = off;
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (country) country, id, ts FROM lt_e
 WHERE status = 'paid' ORDER BY country, ts DESC;
RESET pg_lion.enable_lateral_tops;

DROP TABLE lt_e, lt_c, lt_pt;
DROP FUNCTION lt_check(text);
DROP FUNCTION lt_h(int8, int8, int8);
