-- The LionCount CustomScan (DESIGN.md section 10).
--
-- Two things are checked for every query: that the plan is (or deliberately
-- is not) the custom node, and that the rows it produces are exactly the rows
-- the ordinary plan produces.  The second check is done inside the test with
-- EXCEPT ALL in both directions, so a wrong answer fails even if it happens
-- to be stable.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';

/*
 * The visibility map is what makes this feature worth having, and a heap page
 * only becomes all-visible once the inserting transaction's commit record has
 * reached disk.  The dev cluster runs with synchronous_commit = off, so ask
 * for flushed commits here; otherwise whether VACUUM can set the bits depends
 * on how fast the WAL writer happens to be.
 */
SET synchronous_commit = on;

/*
 * Plans are compared literally below, so the statistics must not depend on
 * ANALYZE's random sample: a target of 1000 makes it read every row of the
 * 100k-row table used here.
 */
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS pg_lion;

/*
 * lion_pd() runs one query twice, once with the pushdown enabled and once
 * without, and reports whether the custom node was used and how many rows
 * came out - after proving the two result sets are equal as multisets.
 */
CREATE OR REPLACE FUNCTION lion_pd(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_pd_on AS %s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_pd_off AS %s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_pd_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_pd_on EXCEPT ALL SELECT * FROM lion_pd_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_pd_off EXCEPT ALL SELECT * FROM lion_pd_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_pd_on, lion_pd_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* Show both plans for one query. */
CREATE OR REPLACE FUNCTION lion_plans(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN NEXT '-- on:';
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	RETURN NEXT '-- off:';
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
END $$;

/*
 * The instrumentation EXPLAIN ANALYZE prints for the custom node, reduced to
 * zero / non-zero: the exact counts depend on how the rows fall on heap
 * pages, but whether the heap had to be visited at all does not.
 */
CREATE OR REPLACE FUNCTION lion_pd_counters(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	nm text;
	val bigint;
	pushed boolean := false;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
		nm := btrim(split_part(ln, ':', 1));
		IF nm IN ('Heap Blocks Skipped via VM', 'Heap TIDs Rechecked',
				  'Heap Blocks Rechecked', 'Containers Visited',
				  'Heap Blocks From Cache',
				  'Heap Blocks Past Cache Budget') THEN
			val := btrim(split_part(ln, ':', 2))::bigint;
			RETURN NEXT format('%s %s', nm,
							   CASE WHEN val = 0 THEN '= 0' ELSE '> 0' END);
		END IF;
	END LOOP;
	IF NOT pushed THEN
		RETURN NEXT 'NOT PUSHED DOWN';
	END IF;
END $$;

CREATE TABLE lion_pdt (
	id	int		NOT NULL,
	a	int		NOT NULL,
	b	int		NOT NULL,
	c	text	NOT NULL,
	n	int
);
INSERT INTO lion_pdt
SELECT i,
	   i % 10,
	   i % 7,
	   'c' || (i % 4),
	   CASE WHEN i % 101 = 0 THEN NULL ELSE i % 5 END
  FROM generate_series(1, 100000) i;

CREATE INDEX lion_pdt_a ON lion_pdt USING lion (a);
CREATE INDEX lion_pdt_b ON lion_pdt USING lion (b);
CREATE INDEX lion_pdt_c ON lion_pdt USING lion (c);
CREATE INDEX lion_pdt_n ON lion_pdt USING lion (n);
-- The pushdown only wins when most of the heap is all-visible, which is the
-- case it exists for; VACUUM makes that true and deterministic here.
VACUUM ANALYZE lion_pdt;

-- ---- plans -------------------------------------------------------------
SELECT lion_plans('SELECT count(*) FROM lion_pdt WHERE a = 3');
SELECT lion_plans('SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2');
SELECT lion_plans($$SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2 AND c = 'c1'$$);
SELECT lion_plans('SELECT a, count(*) FROM lion_pdt GROUP BY a');
SELECT lion_plans('SELECT a, count(*) FROM lion_pdt WHERE b = 2 GROUP BY a');
SELECT lion_plans('SELECT count(*) FROM lion_pdt GROUP BY a');
SELECT lion_plans('SELECT count(a) FROM lion_pdt WHERE a = 3');
SELECT lion_plans('SELECT a, count(*) FROM lion_pdt WHERE a = 3 GROUP BY a');
SELECT lion_plans('SELECT n, count(*) FROM lion_pdt GROUP BY n');
SELECT lion_plans('SELECT count(*) FROM lion_pdt WHERE n IS NULL');

-- ---- shapes that must push down ----------------------------------------
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3');
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2');
SELECT lion_pd($$SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2 AND c = 'c1'$$);
SELECT lion_pd($$SELECT count(*) FROM lion_pdt WHERE c = 'c2'$$);
-- the constant on the left, and a cross-type constant
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE 3 = a');
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3::int8');
-- a constant that matches no key at all
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 999');
-- the same column constrained twice with the same constant
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND a = 3');
-- GROUP BY, with and without a WHERE clause
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt GROUP BY a');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt WHERE b = 2 GROUP BY a');
SELECT lion_pd($$SELECT a, count(*) FROM lion_pdt WHERE b = 2 AND c = 'c1' GROUP BY a$$);
SELECT lion_pd('SELECT count(*) FROM lion_pdt GROUP BY a');
SELECT lion_pd('SELECT b, count(*) FROM lion_pdt WHERE b = 2 GROUP BY b');
/*
 * GROUP BY a column the WHERE clause pins to a single value.  The planner
 * folds such a column out of the group clause, so the node emits at most one
 * row and reports the key the index stored for it - which is what makes a
 * cross-type constant safe here.
 */
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt WHERE a = 3 GROUP BY a');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt WHERE a = 3::int8 GROUP BY a');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt WHERE a = 999 GROUP BY a');
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdt WHERE a = 3 GROUP BY a, b');
SELECT lion_pd($$SELECT c, count(*) FROM lion_pdt WHERE c = 'c2' GROUP BY c$$);
SELECT lion_pd('SELECT b, count(*) FROM lion_pdt WHERE b = 2 GROUP BY b');
-- a by-reference group key whose type prints what it stores
SELECT lion_pd('SELECT c, count(*) FROM lion_pdt GROUP BY c');
-- count(col) where col cannot be NULL
SELECT lion_pd('SELECT count(a) FROM lion_pdt WHERE a = 3');
SELECT lion_pd('SELECT a, count(a) FROM lion_pdt GROUP BY a');
SELECT lion_pd('SELECT count(*) AS c1, count(a) AS c2, count(*) AS c3 FROM lion_pdt WHERE a = 3');

/*
 * A nullable group column is fine: the NULL keys have an entry of their own,
 * so the NULL group is produced like any other (DESIGN.md section 14, and
 * test/sql/null.sql for the whole story).
 */
SELECT lion_pd('SELECT n, count(*) FROM lion_pdt GROUP BY n');

-- ---- shapes that must NOT push down ------------------------------------
-- count() of a column that neither the GROUP BY nor a WHERE clause constrains
SELECT lion_pd('SELECT count(n) FROM lion_pdt WHERE a = 3');
-- HAVING
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt GROUP BY a HAVING count(*) > 9000');
-- an aggregate we cannot answer
SELECT lion_pd('SELECT sum(id) FROM lion_pdt WHERE a = 3');
SELECT lion_pd('SELECT count(DISTINCT b) FROM lion_pdt WHERE a = 3');
SELECT lion_pd('SELECT count(*) FILTER (WHERE b = 2) FROM lion_pdt WHERE a = 3');
-- a qual that is not an indexed equality to a constant
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND id < 500');
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a > 3');
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND id = 5');
-- two different constants on one column
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND a = 4');
-- no equality key and no GROUP BY at all
SELECT lion_pd('SELECT count(*) FROM lion_pdt');
-- GROUP BY an unindexed column, and by more than two columns
SELECT lion_pd('SELECT id, count(*) FROM lion_pdt WHERE a = 3 GROUP BY id');
SELECT lion_pd('SELECT a, b, c, count(*) FROM lion_pdt GROUP BY a, b, c');
-- two indexed columns are the nested loop of DESIGN.md section 20, below
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b');
-- GROUP BY an expression
SELECT lion_pd('SELECT a + 1, count(*) FROM lion_pdt GROUP BY a + 1');
-- a join
SELECT lion_pd('SELECT count(*) FROM lion_pdt x, lion_pdt y WHERE x.a = 3 AND y.b = 2');

/*
 * Applicable, but the cost model prefers the ordinary plan: a key the
 * statistics say matches nothing makes the bitmap plan look almost free.
 */
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 999');
-- ORDER BY is fine, but the node produces no ordering of its own
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt GROUP BY a ORDER BY a');
SELECT lion_plans('SELECT a, count(*) FROM lion_pdt GROUP BY a ORDER BY a');

-- ---- the results themselves --------------------------------------------
SELECT a, count(*) FROM lion_pdt GROUP BY a ORDER BY a;
SELECT a, count(*) FROM lion_pdt WHERE b = 2 GROUP BY a ORDER BY a;
SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2;
SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2 AND c = 'c1';
SELECT count(*) FROM lion_pdt WHERE a = 999;
SELECT a, count(*) FROM lion_pdt WHERE a = 3 GROUP BY a;
SELECT a, b, count(*) FROM lion_pdt WHERE a = 3 GROUP BY a, b ORDER BY b;

-- ---- after a DELETE, before VACUUM (the heap recheck path) -------------
DELETE FROM lion_pdt WHERE id % 3 = 0;

SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3');
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt GROUP BY a');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt WHERE b = 2 GROUP BY a');
SELECT a, count(*) FROM lion_pdt GROUP BY a ORDER BY a;

-- ---- and after VACUUM (the visibility-map path) ------------------------
VACUUM lion_pdt;

SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3');
SELECT lion_pd('SELECT count(*) FROM lion_pdt WHERE a = 3 AND b = 2');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt GROUP BY a');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt WHERE b = 2 GROUP BY a');
SELECT a, count(*) FROM lion_pdt GROUP BY a ORDER BY a;

-- ---- a group whose rows have all been deleted disappears ---------------
DELETE FROM lion_pdt WHERE a = 5;
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt GROUP BY a');
SELECT a, count(*) FROM lion_pdt GROUP BY a ORDER BY a;
VACUUM lion_pdt;
SELECT lion_pd('SELECT a, count(*) FROM lion_pdt GROUP BY a');
SELECT a, count(*) FROM lion_pdt GROUP BY a ORDER BY a;

-- ---- an empty table ----------------------------------------------------
CREATE TABLE lion_pde (k int NOT NULL);
CREATE INDEX lion_pde_k ON lion_pde USING lion (k);
VACUUM ANALYZE lion_pde;
SELECT lion_pd('SELECT count(*) FROM lion_pde WHERE k = 1');
SELECT lion_pd('SELECT k, count(*) FROM lion_pde GROUP BY k');
SELECT count(*) FROM lion_pde WHERE k = 1;

-- ---- the node inside a larger plan --------------------------------------
SELECT lion_pd('SELECT x, c FROM (VALUES (1), (2)) v(x), LATERAL (SELECT count(*) c FROM lion_pdt WHERE a = 3) s');
SELECT lion_pd('SELECT * FROM (SELECT a, count(*) AS c FROM lion_pdt GROUP BY a) s WHERE c > 0');

-- ---- what EXPLAIN ANALYZE reports ---------------------------------------
VACUUM lion_pdt;
SELECT lion_pd_counters('SELECT count(*) FROM lion_pdt WHERE a = 3');
-- the pages a DELETE dirties are no longer all-visible, so their TIDs are
-- fetched from the heap and counted as rechecked blocks
DELETE FROM lion_pdt WHERE id % 500 = 0;
SELECT lion_pd_counters('SELECT count(*) FROM lion_pdt WHERE a = 3');

-- ---- a grouping over a heap that is not all-visible ---------------------
/*
 * A thousand groups whose rows are spread over every heap page, on a heap the
 * visibility map cannot vouch for.  Every TID has to be resolved against the
 * snapshot - but each dirty BLOCK is fetched only once per query, because the
 * per-query visibility cache answers every later group from memory
 * (DESIGN.md §9), so the node makes one pass over the heap and not one per
 * group.  It is therefore pushed down here, and measurably should be: on a
 * 100k-row table in this state the node ran in 17.9 ms against the
 * sequential aggregate's 26.8 ms, and once the pages had become all-visible
 * again 8.0 ms against 18.4 ms (2026-09-21).  Before the cache, and before
 * the estimate stopped charging numgroups random reads per dirty page, this
 * was the case that had to lose.
 */
CREATE TABLE lion_pdd (g int NOT NULL, pad text NOT NULL);
INSERT INTO lion_pdd
SELECT i % 1000, repeat('x', 200) FROM generate_series(1, 100000) i;
CREATE INDEX lion_pdd_g ON lion_pdd USING lion (g);
ANALYZE lion_pdd;			-- no VACUUM: relallvisible stays 0
SELECT lion_pd('SELECT g, count(*) FROM lion_pdd GROUP BY g');
-- and once it is all-visible there is nothing left to recheck at all
VACUUM ANALYZE lion_pdd;
SELECT lion_pd('SELECT g, count(*) FROM lion_pdd GROUP BY g');

-- ---- the cost model still refuses a grouping it cannot win -------------
/*
 * The other end of the same model.  Twenty thousand groups of five rows over
 * a hundred thousand rows: the recheck is not what costs here, the per-group
 * work is - one entry lookup and one container per group - and it is more
 * than the sequential scan plus a HashAggregate does.  The planner must
 * refuse, and measurably should: 28.0 ms for the sequential aggregate
 * against 40.8 ms for the node when sequential scans are discouraged
 * (2026-09-21).  Nothing is disabled here; which plan the cost model picks IS
 * the test.
 */
CREATE TABLE lion_pdh (g int NOT NULL, pad text NOT NULL);
INSERT INTO lion_pdh
SELECT i % 20000, repeat('x', 200) FROM generate_series(1, 100000) i;
CREATE INDEX lion_pdh_g ON lion_pdh USING lion (g);
ANALYZE lion_pdh;
SELECT lion_pd('SELECT g, count(*) FROM lion_pdh GROUP BY g');
SELECT lion_plans('SELECT g, count(*) FROM lion_pdh GROUP BY g');

-- ---- a grouping over a heap a few per cent of which is dirty -----------
/*
 * The case the 2026-09-21 follow-up review measured: a vacuumed table, then
 * five per cent of its rows updated, which leaves about a tenth of its heap
 * pages unable to be vouched for - the ordinary state of a large table that
 * is mostly read and occasionally written.  A GROUP BY returns to those
 * pages for every group, but the per-query visibility cache fetches each of
 * them once (DESIGN.md §9), so the estimate must charge one pass over the
 * dirty working set and not numgroups random reads per page.  Charging the
 * latter asked 1.8M cost units against the sequential aggregate's 175k on
 * five million rows and lost a query the node wins by 7.9x; here the node
 * runs in 2.0 ms against 15.8 ms (2026-09-21).  Nothing is disabled: which
 * plan the cost model picks IS the test.
 */
CREATE TABLE lion_pdg (id int NOT NULL, k int NOT NULL, pad text NOT NULL);
INSERT INTO lion_pdg
SELECT i, i % 20, repeat('x', 200) FROM generate_series(1, 100000) i;
CREATE INDEX lion_pdg_k ON lion_pdg USING lion (k);
VACUUM ANALYZE lion_pdg;
UPDATE lion_pdg SET pad = pad || 'y' WHERE id <= 5000;
ANALYZE lion_pdg;			-- no VACUUM: the updated pages stay dirty
SELECT relallvisible > 0 AND relallvisible < relpages AS mostly_all_visible
  FROM pg_class WHERE relname = 'lion_pdg';
SELECT lion_pd('SELECT k, count(*) FROM lion_pdg GROUP BY k');
SELECT lion_plans('SELECT k, count(*) FROM lion_pdg GROUP BY k');
/*
 * ... and this is the cache the estimate is allowed to assume: every group
 * comes back to the same dirty pages, and every visit after the first is
 * answered out of memory instead of pinning the page again.  A single count
 * walks the result in TID order and never returns to a block, so it has no
 * cache hits at all (the calls further up).
 */
SELECT lion_pd_counters('SELECT k, count(*) FROM lion_pdg GROUP BY k');

-- ---- an IN list long enough to belong to a B-tree ----------------------
/*
 * Every element of an IN list is a bucket lookup, a container chain of its
 * own and one more sub-cursor in the union the merge evaluates, and the
 * estimate has to say so: a B-tree descends once per element and then reads
 * its leaves in order, which is less work per element by a factor that grows
 * with the list.  Measured on this table, three values take 0.021 ms through
 * the node against the B-tree index-only scan's 0.063 ms, and a thousand
 * values 14.1 ms against 13.9 ms - and at one million rows of the benchmark's
 * wider table a thousand values took 15.4 ms against 3.1 ms and were pushed
 * down anyway, because each element was priced as a single bucket page (the
 * 2026-09-21 follow-up review).
 */
CREATE TABLE lion_pdi (id int NOT NULL, k int NOT NULL);
INSERT INTO lion_pdi SELECT i, i % 1000 FROM generate_series(1, 200000) i;
CREATE INDEX lion_pdi_r ON lion_pdi USING lion (k);
CREATE INDEX lion_pdi_b ON lion_pdi (k);
VACUUM ANALYZE lion_pdi;
/* The list is generated rather than written out, so that the plan text this
 * reports stays short; lion_pd() prints the choice and the row count only. */
CREATE OR REPLACE FUNCTION lion_pd_in(n int) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	RETURN lion_pd(format('SELECT count(*) FROM lion_pdi WHERE k IN (%s)',
						 (SELECT string_agg(g::text, ',')
							FROM generate_series(0, n - 1) g)));
END $$;
SELECT lion_pd_in(3);
SELECT lion_pd_in(10);
SELECT lion_pd_in(1000);
DROP FUNCTION lion_pd_in(int);


-- ---- a nearly all-visible heap is what the pushdown is for --------------
/*
 * The other side of the same model: 200k rows vacuumed, then a few thousand
 * more inserted, so that relallvisible is just short of relpages - the
 * ordinary state of a large table that is mostly read.  The rechecks can only
 * touch the pages the visibility map cannot vouch for, and the estimate has
 * to say so; charging random reads across the whole heap made the node lose
 * to plans doing far more work (the 2026-09-20 review, finding 5).  Nothing
 * is disabled here: which plan the cost model picks IS the test.
 */
CREATE TABLE lion_pdw (id int NOT NULL, k int NOT NULL);
INSERT INTO lion_pdw SELECT i, i % 10 FROM generate_series(1, 200000) i;
CREATE INDEX lion_pdw_k ON lion_pdw USING lion (k);
VACUUM ANALYZE lion_pdw;
INSERT INTO lion_pdw SELECT 200000 + i, i % 10 FROM generate_series(1, 4000) i;
ANALYZE lion_pdw;
SELECT relallvisible > 0 AND relallvisible < relpages AS nearly_all_visible
  FROM pg_class WHERE relname = 'lion_pdw';
SELECT lion_pd('SELECT count(*) FROM lion_pdw WHERE k = 3');
SELECT lion_plans('SELECT count(*) FROM lion_pdw WHERE k = 3');
SELECT lion_pd('SELECT k, count(*) FROM lion_pdw GROUP BY k');
SELECT lion_plans('SELECT k, count(*) FROM lion_pdw GROUP BY k');

-- ---- an opclass whose equality is not the grouping equality -------------
/*
 * An operator class may define a coarser equality than the type's own, and
 * this one does: it calls two strings equal when they differ only in case.
 * It is a valid roaring opclass - its hash agrees with its equality - but its
 * entries are already-merged groups, and no GROUP BY may be answered from
 * them, not even a count-only one whose key never reaches the output: the
 * counts themselves would belong to the wrong groups (the 2026-09-20 review,
 * finding 3).
 */
CREATE FUNCTION lion_lower_eq(text, text) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT lower($1) = lower($2) $$;
CREATE OPERATOR === (LEFTARG = text, RIGHTARG = text, FUNCTION = lion_lower_eq,
					 COMMUTATOR = ===);
CREATE FUNCTION lion_lower_hash(text) RETURNS integer
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT hashtext(lower($1)) $$;
CREATE OPERATOR CLASS lion_lower_ops FOR TYPE text USING lion AS
	OPERATOR 1 === (text, text),
	FUNCTION 1 lion_lower_hash(text);

CREATE TABLE lion_pdc (v text NOT NULL);
INSERT INTO lion_pdc SELECT 'A' FROM generate_series(1, 50000);
INSERT INTO lion_pdc SELECT 'a' FROM generate_series(1, 50000);
CREATE INDEX lion_pdc_v ON lion_pdc USING lion (v lion_lower_ops);
VACUUM ANALYZE lion_pdc;
-- one entry for the two spellings, which is what the opclass says
SELECT entries FROM lion_index_stats('lion_pdc_v');
SELECT lion_pd('SELECT v, count(*) FROM lion_pdc GROUP BY v');
SELECT lion_plans('SELECT v, count(*) FROM lion_pdc GROUP BY v');
SELECT lion_pd('SELECT count(*) FROM lion_pdc GROUP BY v');
SELECT v, count(*) FROM lion_pdc GROUP BY v ORDER BY v;
/*
 * The very same data under the default text opclass does drive the node, so
 * what the queries above turned down is the opclass and not the table or the
 * cost model.
 */
CREATE TABLE lion_pdv (v text NOT NULL);
INSERT INTO lion_pdv SELECT 'A' FROM generate_series(1, 50000);
INSERT INTO lion_pdv SELECT 'a' FROM generate_series(1, 50000);
CREATE INDEX lion_pdv_v ON lion_pdv USING lion (v);
VACUUM ANALYZE lion_pdv;
SELECT entries FROM lion_index_stats('lion_pdv_v');
SELECT lion_pd('SELECT v, count(*) FROM lion_pdv GROUP BY v');
SELECT lion_plans('SELECT v, count(*) FROM lion_pdv GROUP BY v');
SELECT v, count(*) FROM lion_pdv GROUP BY v ORDER BY v;

-- ---- a type whose equality does not preserve the representation ---------
/*
 * numeric 1.0 and 1.00 are equal and share one posting-set entry, but they
 * are different strings, and the entry keeps whichever of them was indexed
 * first.  Printing that key could produce a value no visible row holds, so
 * every value-producing shape steps aside; counting the class is exact
 * either way and is still pushed down (the 2026-09-20 review, finding 4).
 */
CREATE TABLE lion_pdn (v numeric NOT NULL);
INSERT INTO lion_pdn SELECT 1.0 FROM generate_series(1, 20000);
INSERT INTO lion_pdn SELECT 1.00 FROM generate_series(1, 20000);
INSERT INTO lion_pdn SELECT 2.5 FROM generate_series(1, 20000);
CREATE INDEX lion_pdn_v ON lion_pdn USING lion (v);
VACUUM ANALYZE lion_pdn;
SELECT entries FROM lion_index_stats('lion_pdn_v');
SELECT lion_pd('SELECT v, count(*) FROM lion_pdn GROUP BY v');
SELECT lion_pd('SELECT v, count(*) FROM lion_pdn WHERE v = 1.000 GROUP BY v');
/*
 * With every other plan disabled the node would be chosen if the planner had
 * built it at all, which makes the two answers here the plan-time rule and
 * not the cost model: no node for the grouping, and the count as before.
 */
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT lion_plans('SELECT v, count(*) FROM lion_pdn GROUP BY v');
SELECT lion_plans('SELECT count(*) FROM lion_pdn WHERE v = 1.000');
SELECT lion_pd('SELECT count(*) FROM lion_pdn WHERE v = 1.000');
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT v, count(*) FROM lion_pdn GROUP BY v ORDER BY v;

-- ---- prepared statements: a parameter where a literal may stand -------
/*
 * A GENERIC plan keeps `a = $1` as a Param, and the pushdown used to accept
 * nothing but a literal: the count of a dense key then fell back to the
 * ordinary plan and lost the whole point of the node (a measured 5.6 ms
 * sequential scan against 0.025 ms, and 207 ms against 3.1 ms on five
 * million rows - the 2026-09-21 follow-up review).  A Param is now accepted
 * wherever a Const is, for equality and for the array of an IN list, and the
 * node evaluates it through its own ExprContext at the start of every scan
 * (DESIGN.md §10).  EXPLAIN prints it as `$1`.
 *
 * lion_pd_prep() prepares one query, forces a generic plan so that every
 * parameter really stays a Param, and proves the answer is the one the same
 * query gives with the pushdown switched off - as a multiset, both ways
 * round.
 */
CREATE OR REPLACE FUNCTION lion_pd_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	rec record;
	pushed boolean := false;
	onrows text[] := '{}';
	offrows text[] := '{}';
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_pp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_pp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	FOR rec IN EXECUTE 'EXECUTE lion_pp_on(' || args || ')' LOOP
		onrows := onrows || rec::text;
	END LOOP;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'PREPARE lion_pp_off AS ' || q;
	FOR rec IN EXECUTE 'EXECUTE lion_pp_off(' || args || ')' LOOP
		offrows := offrows || rec::text;
	END LOOP;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_pp_on';
	EXECUTE 'DEALLOCATE lion_pp_off';

	SELECT (SELECT count(*) FROM (SELECT unnest(onrows)
								  EXCEPT ALL SELECT unnest(offrows)) a)
		 + (SELECT count(*) FROM (SELECT unnest(offrows)
								  EXCEPT ALL SELECT unnest(onrows)) b)
	  INTO ndiff;
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;

	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  coalesce(array_length(onrows, 1), 0));
END $$;

SELECT lion_pd_prep('SELECT count(*) FROM lion_pdt WHERE a = $1', '3');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdt WHERE a = $1', '-1');
-- a NULL parameter means zero rows, which is not the same as one group of 0
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdt WHERE a = $1', 'NULL::int');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdt WHERE a = ANY ($1)',
				   'ARRAY[1,3]');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdt WHERE a = ANY ($1)',
				   'NULL::int[]');
-- `a IN ($1, $2)` keeps an ARRAY[] of Params in a generic plan
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdt WHERE a IN ($1, $2)', '1, 3');
SELECT lion_pd_prep('SELECT b, count(*) FROM lion_pdt WHERE a = $1'
				   ' GROUP BY b ORDER BY b', '3');
-- the pinned column is printed from the entry's stored key, parameter or not
SELECT lion_pd_prep('SELECT a, count(*) FROM lion_pdt WHERE a = $1 GROUP BY a',
				   '3');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdt WHERE a = $1 AND n IS NULL',
				   '3');

SET plan_cache_mode = force_generic_plan;
PREPARE lion_pp(int) AS SELECT count(*) FROM lion_pdt WHERE a = $1;
EXPLAIN (COSTS OFF) EXECUTE lion_pp(3);
EXECUTE lion_pp(3);
EXECUTE lion_pp(4);
DEALLOCATE lion_pp;
RESET plan_cache_mode;

/*
 * An exec Param, which a nested loop changes between rescans: the node has to
 * re-evaluate it every time rather than count the first outer row's key
 * again.
 */
EXPLAIN (COSTS OFF)
SELECT v.k, s.c FROM (VALUES (1), (3), (-1)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_pdt WHERE a = v.k) s;
SELECT v.k, s.c FROM (VALUES (1), (3), (-1)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_pdt WHERE a = v.k) s ORDER BY v.k;
SET pg_lion.enable_count_pushdown = off;
SELECT v.k, s.c FROM (VALUES (1), (3), (-1)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_pdt WHERE a = v.k) s ORDER BY v.k;
RESET pg_lion.enable_count_pushdown;

-- ---- OR across columns (DESIGN.md section 19) ---------------------------
/*
 * `count(*) WHERE a = 17 OR b = 3` used to be declined and run as BitmapOr
 * over a Bitmap Heap Scan, which at a few per cent selectivity touches nearly
 * every heap page.  Now the whole restriction is ONE source - the union of
 * its arms - which is ANDed with the other clauses and with a GROUP BY driver
 * like any other.  Measured on a million rows of (c200, c20) on this
 * assert-enabled build: 0.6 ms for the node against 18-30 ms for BitmapOr +
 * heap (2026-09-21).
 *
 * Every arm has to be a positive clause the posting sets can answer, or an
 * AND of such clauses, each on a column of the same relation with an index of
 * its own; a negated arm is declined, because the complement of a posting set
 * is not a posting set and under a union there is nothing to subtract it
 * from.
 */
CREATE TABLE lion_pdo (id int NOT NULL, a int NOT NULL, b int NOT NULL,
					   c text NOT NULL, n int);
INSERT INTO lion_pdo
SELECT i, i % 200, i % 20, 'c' || (i % 4),
	   CASE WHEN i % 101 = 0 THEN NULL ELSE i % 5 END
  FROM generate_series(1, 200000) i;
CREATE INDEX lion_pdo_a ON lion_pdo USING lion (a);
CREATE INDEX lion_pdo_b ON lion_pdo USING lion (b);
CREATE INDEX lion_pdo_c ON lion_pdo USING lion (c);
CREATE INDEX lion_pdo_n ON lion_pdo USING lion (n);
VACUUM ANALYZE lion_pdo;

SELECT lion_plans('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3');
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3');
-- three arms
SELECT lion_pd($$SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3 OR c = 'c1'$$);
-- an arm that is an AND
SELECT lion_pd($$SELECT count(*) FROM lion_pdo WHERE (a = 17 AND b = 3) OR c = 'c1'$$);
SELECT lion_pd($$SELECT count(*) FROM lion_pdo WHERE (a = 17 AND b = 3) OR (c = 'c1' AND n = 2)$$);
SELECT lion_plans($$SELECT count(*) FROM lion_pdo WHERE (a = 17 AND b = 3) OR (c = 'c1' AND n = 2)$$);
-- an OR next to a plain AND clause, and under a GROUP BY
SELECT lion_pd($$SELECT count(*) FROM lion_pdo WHERE (a = 17 OR b = 3) AND c = 'c1'$$);
SELECT lion_plans($$SELECT count(*) FROM lion_pdo WHERE (a = 17 OR b = 3) AND c = 'c1'$$);
SELECT lion_pd('SELECT c, count(*) FROM lion_pdo WHERE a = 17 OR b = 3 GROUP BY c');
SELECT lion_plans('SELECT c, count(*) FROM lion_pdo WHERE a = 17 OR b = 3 GROUP BY c');
-- two OR restrictions in one query are two sources
SELECT lion_pd($$SELECT count(*) FROM lion_pdo WHERE (a = 17 OR b = 3) AND (c = 'c1' OR n = 2)$$);
SELECT lion_plans($$SELECT count(*) FROM lion_pdo WHERE (a = 17 OR b = 3) AND (c = 'c1' OR n = 2)$$);
-- IN and IS NULL arms
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a IN (17, 18) OR n IS NULL');
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b IN (3, 4) OR n IS NULL');
SELECT lion_plans('SELECT count(*) FROM lion_pdo WHERE a IN (17, 18) OR n IS NULL');
-- an arm whose value has no entry, and an OR none of whose arms has one
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 999 OR b = 3');
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 999 OR b = 998');
-- the results themselves
SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3;
SELECT c, count(*) FROM lion_pdo WHERE a = 17 OR b = 3 GROUP BY c ORDER BY c;

-- ---- ORs the node must decline -----------------------------------------
-- a negated arm
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR n IS NOT NULL');
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR NOT (b = 3)');
SELECT lion_plans('SELECT count(*) FROM lion_pdo WHERE a = 17 OR n IS NOT NULL');
-- an arm on a column with no lion index
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR id = 5');
-- an arm the posting sets cannot answer without a recheck
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b > 3');
-- one unusable leaf spoils its AND arm and with it the whole restriction
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE (a = 17 AND id = 5) OR b = 3');
/*
 * A column constrained only INSIDE an OR is constrained in no row of the
 * result: the other arm selects rows it says nothing about.  So it makes no
 * count(col) answerable, which is what these two check - the first would come
 * out as count(*) and the second as 0 if an OR leaf were treated like a
 * top-level clause.
 */
SELECT lion_pd('SELECT count(n) FROM lion_pdo WHERE n = 2 OR a = 17');
SELECT lion_pd('SELECT count(n) FROM lion_pdo WHERE n IS NULL OR a = 17');

-- ---- parameters inside the arms ----------------------------------------
/*
 * A Param stands wherever a Const may (DESIGN.md section 10), inside an arm
 * as anywhere else, and an arm whose parameter comes out NULL selects nothing
 * - the OR of the remaining arms is still the answer.
 */
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdo WHERE a = $1 OR b = $2',
				   '17, 3');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdo WHERE a = $1 OR b = $2',
				   'NULL::int, 3');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdo WHERE a = $1 OR b = $2',
				   '17, NULL::int');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdo WHERE a = $1 OR b = $2',
				   'NULL::int, NULL::int');
SELECT lion_pd_prep('SELECT count(*) FROM lion_pdo WHERE a = ANY ($1) OR n IS NULL',
				   'ARRAY[17,18]');
SELECT lion_pd_prep('SELECT c, count(*) FROM lion_pdo WHERE a = $1 OR b = $2'
				   ' GROUP BY c ORDER BY c', '17, 3');

SET plan_cache_mode = force_generic_plan;
PREPARE lion_ppo(int, int) AS
	SELECT count(*) FROM lion_pdo WHERE a = $1 OR b = $2;
EXPLAIN (COSTS OFF) EXECUTE lion_ppo(17, 3);
EXECUTE lion_ppo(17, 3);
EXECUTE lion_ppo(NULL, 3);
EXECUTE lion_ppo(NULL, NULL);
DEALLOCATE lion_ppo;
RESET plan_cache_mode;

-- an exec Param that changes between rescans
EXPLAIN (COSTS OFF)
SELECT v.k, s.c FROM (VALUES (17), (3), (-1)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_pdo WHERE a = v.k OR b = v.k) s;
SELECT v.k, s.c FROM (VALUES (17), (3), (-1)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_pdo WHERE a = v.k OR b = v.k) s
	 ORDER BY v.k;
SET pg_lion.enable_count_pushdown = off;
SELECT v.k, s.c FROM (VALUES (17), (3), (-1)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_pdo WHERE a = v.k OR b = v.k) s
	 ORDER BY v.k;
RESET pg_lion.enable_count_pushdown;

-- ---- a dirty heap, then an all-visible one ------------------------------
/*
 * Under a union a dead TID may be contributed by a single leaf, so every leaf
 * of an OR is read the pinned way until the merged container has been through
 * the visibility map (DESIGN.md section 19) and the TIDs the map cannot vouch
 * for are resolved against the snapshot like any other.
 */
DELETE FROM lion_pdo WHERE id % 7 = 0;
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3');
SELECT lion_pd('SELECT c, count(*) FROM lion_pdo WHERE a = 17 OR b = 3 GROUP BY c');
SELECT lion_pd_counters('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3');
VACUUM lion_pdo;
SELECT lion_pd('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3');
SELECT lion_pd('SELECT c, count(*) FROM lion_pdo WHERE a = 17 OR b = 3 GROUP BY c');
SELECT lion_pd_counters('SELECT count(*) FROM lion_pdo WHERE a = 17 OR b = 3');
SELECT c, count(*) FROM lion_pdo WHERE a = 17 OR b = 3 GROUP BY c ORDER BY c;

-- ---- a multi-key arm (DESIGN.md section 17) -----------------------------
CREATE TABLE lion_pdmo (id int NOT NULL, k int NOT NULL, tags text[]);
INSERT INTO lion_pdmo
SELECT i, i % 50, ARRAY['t' || (i % 13), 't' || (i % 29)]
  FROM generate_series(1, 50000) i;
CREATE INDEX lion_pdmo_k ON lion_pdmo USING lion (k);
CREATE INDEX lion_pdmo_tags ON lion_pdmo USING lion (tags);
VACUUM ANALYZE lion_pdmo;
SELECT lion_plans($$SELECT count(*) FROM lion_pdmo WHERE tags @> '{t5}' OR k = 7$$);
SELECT lion_pd($$SELECT count(*) FROM lion_pdmo WHERE tags @> '{t5}' OR k = 7$$);
SELECT lion_pd($$SELECT count(*) FROM lion_pdmo WHERE tags @> '{t5,t7}' OR k = 7$$);
SELECT lion_pd($$SELECT count(*) FROM lion_pdmo WHERE tags && '{t5,t7}' OR k = 7$$);
-- an ALL-mode query, and a parameter that has no shape at plan time: declined
SELECT lion_pd($$SELECT count(*) FROM lion_pdmo WHERE tags @> '{}' OR k = 7$$);
SELECT lion_pd_prep($$SELECT count(*) FROM lion_pdmo WHERE tags @> $1 OR k = 7$$,
				   $$'{t5}'::text[]$$);

-- ---- a partitioned table (DESIGN.md section 16) -------------------------
CREATE TABLE lion_pdpo (id int NOT NULL, a int NOT NULL, b int NOT NULL)
	PARTITION BY RANGE (id);
CREATE TABLE lion_pdpo1 PARTITION OF lion_pdpo FOR VALUES FROM (0) TO (50000);
CREATE TABLE lion_pdpo2 PARTITION OF lion_pdpo FOR VALUES FROM (50000) TO (100000);
INSERT INTO lion_pdpo SELECT i, i % 200, i % 20 FROM generate_series(0, 99999) i;
CREATE INDEX ON lion_pdpo1 USING lion (a);
CREATE INDEX ON lion_pdpo1 USING lion (b);
CREATE INDEX ON lion_pdpo2 USING lion (a);
CREATE INDEX ON lion_pdpo2 USING lion (b);
VACUUM ANALYZE lion_pdpo;
SELECT lion_plans('SELECT count(*) FROM lion_pdpo WHERE a = 17 OR b = 3');
SELECT lion_pd('SELECT count(*) FROM lion_pdpo WHERE a = 17 OR b = 3');
SELECT lion_plans('SELECT a, count(*) FROM lion_pdpo WHERE a = 17 OR b = 3 GROUP BY a');
SELECT lion_pd('SELECT a, count(*) FROM lion_pdpo WHERE a = 17 OR b = 3 GROUP BY a');
SELECT count(*) FROM lion_pdpo WHERE a = 17 OR b = 3;

-- ---- GROUP BY two indexed columns (DESIGN.md section 20) ----------------
/*
 * A nested loop over the two indexes' entries: the column with FEWER distinct
 * values drives the scan (it is the outer one, and EXPLAIN names it first),
 * the other index's keys are read once per relation, and each (outer, inner)
 * pair is counted as the intersection of the two groups' posting sets with
 * the WHERE sources.  Only a pair with a visible row is a group.
 *
 * The cost of that is the product of the two cardinalities, and the estimate
 * has to say so.  Measured on the 200k-row table further down (uncorrelated
 * columns, 100-byte rows, assert build, 2026-09-21): 20 x 2 groups run in
 * 5.6 ms against the sequential aggregate's 37.3 ms and 200 x 2 in 14.9
 * against 39.3, while 200 x 20 is 60.7 against 36.8 and 20000 x 2 is 169.8
 * against 51.6 - so the middle of that range is where the node stops winning,
 * and the model must stop choosing it there.
 */
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b');
SELECT lion_plans('SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b');
SELECT lion_pd($$SELECT a, b, count(*) FROM lion_pdt WHERE c = 'c1' GROUP BY a, b$$);
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdt WHERE b = 2 GROUP BY a, b');
SELECT lion_pd('SELECT b, a, count(*) FROM lion_pdt GROUP BY b, a');
-- NULLs in either column: each one's NULL group is its index's NULL entry
SELECT lion_pd('SELECT a, n, count(*) FROM lion_pdt GROUP BY a, n');
SELECT lion_pd('SELECT n, a, count(*) FROM lion_pdt GROUP BY n, a');
SELECT lion_pd('SELECT n, c, count(*) FROM lion_pdt GROUP BY n, c');
-- count(col) of a group column is 0 in that column's NULL group (section 14)
SELECT lion_pd('SELECT a, n, count(n) FROM lion_pdt GROUP BY a, n');
SELECT lion_pd('SELECT a, n, count(a) FROM lion_pdt GROUP BY a, n');
-- an OR restriction under a two-column grouping
SELECT lion_pd($$SELECT a, b, count(*) FROM lion_pdt WHERE c = 'c1' OR n = 2 GROUP BY a, b$$);
-- the results themselves
SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b ORDER BY a, b LIMIT 8;
SELECT a, n, count(*) FROM lion_pdt GROUP BY a, n ORDER BY a, n NULLS LAST LIMIT 8;

-- a dirty heap, then a clean one
DELETE FROM lion_pdt WHERE id % 11 = 0;
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b');
SELECT lion_pd('SELECT a, n, count(*) FROM lion_pdt GROUP BY a, n');
SELECT lion_pd_counters('SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b');
VACUUM lion_pdt;
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b');
SELECT lion_pd_counters('SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b');
SELECT a, b, count(*) FROM lion_pdt GROUP BY a, b ORDER BY a, b LIMIT 8;

-- a partitioned table: one partial aggregate per pair per partition (§16)
CREATE TABLE lion_pdq (id int NOT NULL, a int NOT NULL, b int NOT NULL)
	PARTITION BY RANGE (id);
CREATE TABLE lion_pdq1 PARTITION OF lion_pdq FOR VALUES FROM (0) TO (50000);
CREATE TABLE lion_pdq2 PARTITION OF lion_pdq FOR VALUES FROM (50000) TO (100000);
INSERT INTO lion_pdq SELECT i, i % 10, i % 4 FROM generate_series(0, 99999) i;
CREATE INDEX ON lion_pdq1 USING lion (a);
CREATE INDEX ON lion_pdq1 USING lion (b);
CREATE INDEX ON lion_pdq2 USING lion (a);
CREATE INDEX ON lion_pdq2 USING lion (b);
VACUUM ANALYZE lion_pdq;
SELECT lion_plans('SELECT a, b, count(*) FROM lion_pdq GROUP BY a, b');
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdq GROUP BY a, b');
SELECT lion_pd('SELECT a, b, count(*) FROM lion_pdq WHERE b = 2 GROUP BY a, b');
SELECT a, b, count(*) FROM lion_pdq GROUP BY a, b ORDER BY a, b LIMIT 6;

-- ---- which cardinalities the cost model accepts -------------------------
/*
 * Nothing is disabled here: which plan the model picks IS the test.  The
 * intersection of a pair scans the members of the smaller of the two groups,
 * so summed over every pair that is Min(entries, entries) x rows - and it is
 * that term, not the pair count alone, that has to refuse the wide cases.
 */
CREATE TABLE lion_pd2 (id int NOT NULL, c2 int NOT NULL, c20 int NOT NULL,
					   c200 int NOT NULL, c20k int NOT NULL, pad text NOT NULL);
INSERT INTO lion_pd2
SELECT i, i % 2, i % 20, i % 200, i % 20000, repeat('x', 200)
  FROM generate_series(1, 100000) i;
CREATE INDEX lion_pd2_c2 ON lion_pd2 USING lion (c2);
CREATE INDEX lion_pd2_c20 ON lion_pd2 USING lion (c20);
CREATE INDEX lion_pd2_c200 ON lion_pd2 USING lion (c200);
CREATE INDEX lion_pd2_c20k ON lion_pd2 USING lion (c20k);
VACUUM ANALYZE lion_pd2;
-- 20 x 2 and 200 x 2 are worth it
SELECT lion_pd('SELECT c20, c2, count(*) FROM lion_pd2 GROUP BY c20, c2');
SELECT lion_plans('SELECT c20, c2, count(*) FROM lion_pd2 GROUP BY c20, c2');
SELECT lion_pd('SELECT c200, c2, count(*) FROM lion_pd2 GROUP BY c200, c2');
-- 200 x 20 and 20000 x 200 are not, and are refused
SELECT lion_pd('SELECT c200, c20, count(*) FROM lion_pd2 GROUP BY c200, c20');
SELECT lion_plans('SELECT c200, c20, count(*) FROM lion_pd2 GROUP BY c200, c20');
SELECT lion_pd('SELECT c20k, c200, count(*) FROM lion_pd2 GROUP BY c20k, c200');
SELECT lion_pd('SELECT c20k, c2, count(*) FROM lion_pd2 GROUP BY c20k, c2');
DROP TABLE lion_pd2;
DROP TABLE lion_pdq;

DROP TABLE lion_pdpo;
DROP TABLE lion_pdmo;
DROP TABLE lion_pdo;

DROP FUNCTION lion_pd_prep(text, text);

DROP TABLE lion_pdn;
DROP TABLE lion_pdv;
DROP TABLE lion_pdc;
DROP OPERATOR CLASS lion_lower_ops USING lion;
DROP OPERATOR === (text, text);
DROP FUNCTION lion_lower_eq(text, text);
DROP FUNCTION lion_lower_hash(text);
DROP TABLE lion_pdw;
DROP TABLE lion_pdd;
DROP TABLE lion_pdh;
DROP TABLE lion_pdg;
DROP TABLE lion_pdi;
DROP TABLE lion_pde;
DROP TABLE lion_pdt;
DROP FUNCTION lion_pd(text);
DROP FUNCTION lion_pd_counters(text);
DROP FUNCTION lion_plans(text);
-- enable_partitionwise_aggregate must not switch the pushdown off for a plain table
CREATE TABLE lion_pwa (k int NOT NULL);
INSERT INTO lion_pwa SELECT g % 10 FROM generate_series(1, 20000) g;
CREATE INDEX lion_pwa_k ON lion_pwa USING lion (k);
SET synchronous_commit = on;
VACUUM ANALYZE lion_pwa;
SET enable_partitionwise_aggregate = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_pwa WHERE k = 3;
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM lion_pwa GROUP BY k;
RESET enable_partitionwise_aggregate;
DROP TABLE lion_pwa;
