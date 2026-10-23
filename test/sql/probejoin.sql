-- The dimension by probe (DESIGN.md §46, LionProbeJoin):
--
--   SELECT <predicates of d's columns, CASE over them>, <f's columns>, count(*)
--     FROM f JOIN d ON f.fk = d.pk
--    WHERE <f's clauses> AND <d's clauses lion indexes answer>
--      [AND NOT EXISTS (SELECT 1 FROM s WHERE s.a = f.fk ...)]
--    GROUP BY ...
--
-- with no lion index on f.  The node reads f's rows from core's own path,
-- looks each key up in d's primary key, and tests the TID found against sets
-- of d's lion indexes built once: the WHERE's, and a label set an outcome of
-- each column the GROUP BY's predicates read.  The heap is read only on pages
-- the visibility map does not call all-visible.  Every answer is checked
-- against core's plan with the node switched off.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * pj() runs q with every join method disabled, so that core has no join the
 * node must beat on price, and says whether the node is the plan (and
 * whether in its parallel form); then again with the node and the rest of
 * lion's pushdowns off, sequential scans only and the joins on, and compares
 * the two answers as multisets.  The settings are the transaction's: they
 * go back to the caller's when it ends.
 */
CREATE FUNCTION pj(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node boolean := false;
	parallel boolean := false;
	how text;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		node := node OR ln LIKE '%Custom Scan (LionProbeJoin)%';
		parallel := parallel OR ln LIKE '%Partial %Aggregate%';
	END LOOP;
	how := CASE WHEN NOT node THEN 'no probe join'
				WHEN parallel THEN 'probe join, parallel'
				ELSE 'probe join' END;
	EXECUTE format('CREATE TEMP TABLE pj_on AS SELECT pj_q::text AS r FROM (%s) pj_q', q);

	PERFORM set_config('pg_lion.enable_probe_join', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE pj_off AS SELECT pj_q::text AS r FROM (%s) pj_q', q);

	EXECUTE 'SELECT count(*) FROM pj_off' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE pj_on EXCEPT ALL TABLE pj_off) a)'
			' + (SELECT count(*) FROM (TABLE pj_off EXCEPT ALL TABLE pj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE pj_on, pj_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/* The answer itself, through the node, the joins disabled. */
CREATE FUNCTION pj_val(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	RETURN QUERY EXECUTE format('SELECT pj_q::text FROM (%s) pj_q ORDER BY 1', q);
END $$;

/*
 * The plan of q, the joins disabled, in a form every supported release
 * prints the same way: 18's "Disabled: true" lines dropped, subplans named
 * "expr_N", the memory of the Agg and of a sort and their keys - which
 * repeat the grouped expressions whole - left out.
 * With `run`, under EXPLAIN ANALYZE: the actual row counts left out, and
 * the node's counters printed whole when `exact`, or as "> 0" and "0" when
 * they depend on what an earlier run left behind (index entries a scan
 * marked dead, which the next one skips).
 */
CREATE FUNCTION pj_explain(q text, run boolean DEFAULT false,
						   exact boolean DEFAULT true)
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	opts text := 'COSTS OFF';
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	IF run THEN
		opts := 'ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF';
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (' || opts || ') ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*(Disabled|Batches|Memory Usage|Worker \d|Storage|Index Searches|Estimates|Buffers|Sort Method)';
		CONTINUE WHEN ln ~ '^\s*(Group|Sort) Key: ';
		ln := regexp_replace(ln, '\s*\(actual.*\)', '');
		ln := regexp_replace(ln, '(InitPlan|SubPlan) (\d+)', '\1 expr_\2', 'g');
		IF NOT exact AND
		   ln ~ '^\s*(Probes|Null Keys|Rows Removed by [A-Za-z ]+|Heap TIDs [A-Za-z ]+|Lion Sets|Anti Join Keys): ' THEN
			ln := regexp_replace(ln, '\m[1-9][0-9]*\M', '> 0', 'g');
		END IF;
		RETURN NEXT ln;
	END LOOP;
END $$;

-- ---------- the tables ----------
CREATE TYPE pj_mood AS ENUM ('calm', 'busy', 'idle', 'gone');
CREATE TABLE pj_customers (
	id int PRIMARY KEY,
	region text,			-- lion: four values and NULL
	tier int NOT NULL,		-- lion, with region: 1 to 5
	active bool,			-- lion: true, false and NULL
	mood pj_mood,			-- lion: an enum
	tags int[],				-- lion: a multi-key column
	note text				-- no index: what a HOT update changes
) WITH (autovacuum_enabled = off);
INSERT INTO pj_customers
SELECT i,
	   CASE WHEN i % 23 = 0 THEN NULL
			ELSE (ARRAY['north', 'south', 'east', 'west'])[1 + i % 4] END,
	   1 + (i / 4) % 5,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE i % 3 <> 0 END,
	   (enum_range(NULL::pj_mood))[1 + (i / 7) % 4],
	   CASE WHEN i % 29 = 0 THEN NULL
			ELSE ARRAY[i % 6, 6 + i % 5] END,
	   'n' || i
  FROM generate_series(1, 6000) i;
CREATE INDEX pj_customers_rt ON pj_customers USING lion (region, tier);
CREATE INDEX pj_customers_active ON pj_customers USING lion (active);
CREATE INDEX pj_customers_mood ON pj_customers USING lion (mood);
CREATE INDEX pj_customers_tags ON pj_customers USING lion (tags);

-- the fact: no lion index; a NULL key every 50th row, and a key past the
-- dimension's every 47th
CREATE TABLE pj_orders (
	id int NOT NULL,
	customer_id int,
	channel int NOT NULL,
	amount int NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO pj_orders
SELECT i,
	   CASE WHEN i % 50 = 0 THEN NULL
			WHEN i % 47 = 0 THEN 100000 + i
			ELSE 1 + abs(hashint4(i)) % 6000 END,
	   i % 3,
	   abs(hashint4(i + 7)) % 100
  FROM generate_series(1, 30000) i;

-- the anti join's small table: keys of customers, keys of none, and NULLs
CREATE TABLE pj_blocked (customer_id int, reason text);
INSERT INTO pj_blocked
SELECT CASE WHEN i % 10 = 0 THEN NULL
			WHEN i % 10 = 1 THEN 200000 + i
			ELSE i * 37 END,
	   CASE WHEN i % 2 = 0 THEN 'audit' ELSE 'fraud' END
  FROM generate_series(1, 120) i;
VACUUM (FREEZE, ANALYZE) pj_customers, pj_orders, pj_blocked;

-- ---------- 1. the node, against core's plan ----------
-- a CASE over two lion columns, a fact column beside it
SELECT pj($$SELECT CASE WHEN c.region IN ('north', 'east') THEN 'ne'
						WHEN c.tier = 1 THEN 't1' ELSE 'other' END AS k,
				   o.channel, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.active AND c.tier < 4
			 GROUP BY 1, 2$$);
SELECT * FROM pj_val($$SELECT CASE WHEN c.region IN ('north', 'east') THEN 'ne'
								   WHEN c.tier = 1 THEN 't1' ELSE 'other' END AS k,
							  o.channel, count(*)
						 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
						WHERE c.active AND c.tier < 4
						GROUP BY 1, 2$$);
-- booleans as the group keys, a NULL truth among them
SELECT pj($$SELECT c.active, c.region = 'south' AS s, c.region IS NULL AS n, count(*)
			  FROM pj_orders o JOIN pj_customers c ON c.id = o.customer_id
			 GROUP BY 1, 2, 3$$);
SELECT * FROM pj_val($$SELECT c.active, c.region = 'south' AS s, count(*)
						 FROM pj_orders o JOIN pj_customers c ON c.id = o.customer_id
						GROUP BY 1, 2$$);
-- a WHERE on the dimension and fact columns alone in the GROUP BY
SELECT pj($$SELECT o.channel, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.region = 'west' AND c.tier IN (2, 3)
			 GROUP BY 1$$);
-- ... and no GROUP BY at all
SELECT pj($$SELECT count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.mood = 'busy'$$);
-- predicates and the fact's own WHERE, with arithmetic over the predicates
SELECT pj($$SELECT (CASE WHEN c.tier IN (4, 5) THEN 10 ELSE 0 END)
				   + (CASE WHEN c.active THEN 1 ELSE 0 END) AS score,
				   o.amount < 50 AS small, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE o.amount BETWEEN 10 AND 80
			 GROUP BY 1, 2$$);
-- an enum and a multi-key column, a HAVING
SELECT pj($$SELECT CASE WHEN c.mood IN ('calm', 'idle') THEN 'quiet'
						WHEN c.mood = 'busy' THEN 'busy' END AS m,
				   c.tags && '{1,2}' AS t12, c.tags @> '{7}' AS t7, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.region <> 'east'
			 GROUP BY 1, 2, 3
			HAVING count(*) > 100$$);
-- an aggregate of a fact column
SELECT pj($$SELECT c.tier = 5 AS top, sum(o.amount), max(o.channel), count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE NOT c.active
			 GROUP BY 1$$);
-- the dimension rows the WHERE leaves out, the keys with no dimension row
-- and the NULL keys count nowhere: the counts below add up to the join's
SELECT * FROM pj_val($$SELECT c.region IS NULL AS nul, count(*)
						 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
						WHERE c.tier = 3
						GROUP BY 1$$);
SELECT count(*) FILTER (WHERE customer_id IS NULL) AS null_keys,
	   count(*) FILTER (WHERE customer_id > 6000) AS no_row,
	   count(*) FILTER (WHERE customer_id IN (SELECT id FROM pj_customers WHERE tier = 3)) AS joined
  FROM pj_orders;

-- ---------- 2. the plan, and what it did ----------
SELECT * FROM pj_explain($$SELECT CASE WHEN c.region IN ('north', 'east') THEN 'ne'
									   WHEN c.tier = 1 THEN 't1' ELSE 'other' END AS k,
								  o.channel, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE c.active AND c.tier < 4
							GROUP BY 1, 2$$);
SELECT * FROM pj_explain($$SELECT CASE WHEN c.region IN ('north', 'east') THEN 'ne'
									   WHEN c.tier = 1 THEN 't1' ELSE 'other' END AS k,
								  o.channel, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE c.active AND c.tier < 4
							GROUP BY 1, 2$$, true);
-- no WHERE on the dimension: the labels alone
SELECT * FROM pj_explain($$SELECT c.mood = 'gone' AS g, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							GROUP BY 1$$, true);
-- no predicate: the WHERE alone
SELECT * FROM pj_explain($$SELECT o.channel, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE c.tags && '{3}'
							GROUP BY 1$$, true);

-- a WHERE lion can only bound: its set inexact, every member's heap tuple
-- fetched and the WHERE rechecked on it
SELECT pj($$SELECT c.tier = 1 AS t1, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.tags <@ '{1,2,7,8}'
			 GROUP BY 1$$);
SELECT * FROM pj_explain($$SELECT c.tier = 1 AS t1, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE c.tags <@ '{1,2,7,8}'
							GROUP BY 1$$, true);

-- ---------- 3. the anti join ----------
SELECT pj($$SELECT CASE WHEN c.region = 'north' THEN 'n' ELSE 'x' END AS k, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.tier <> 2
			   AND NOT EXISTS (SELECT 1 FROM pj_blocked b WHERE b.customer_id = o.customer_id)
			 GROUP BY 1$$);
-- the inner side's key against the dimension's own, and a filter on it
SELECT pj($$SELECT c.active, o.channel, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE NOT EXISTS (SELECT 1 FROM pj_blocked b
								WHERE b.customer_id = c.id AND b.reason = 'fraud')
			 GROUP BY 1, 2$$);
SELECT * FROM pj_explain($$SELECT CASE WHEN c.region = 'north' THEN 'n' ELSE 'x' END AS k, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE c.tier <> 2
							  AND NOT EXISTS (SELECT 1 FROM pj_blocked b WHERE b.customer_id = o.customer_id)
							GROUP BY 1$$, true);
-- what the anti join takes away: the blocked customers' orders that pass
SELECT count(*) AS blocked_orders
  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
 WHERE c.tier <> 2 AND o.customer_id IN (SELECT customer_id FROM pj_blocked);

-- ---------- 4. Params: a generic plan, and a rescan ----------
-- A Param of the WHERE is a run-time key of its lion leaf; a cell predicate
-- takes constants only.
SET plan_cache_mode = force_generic_plan;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
PREPARE pj_p(int) AS
	SELECT c.region = 'south' AS s, count(*)
	  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
	 WHERE c.tier = $1
	 GROUP BY 1 ORDER BY 1;
SELECT * FROM pj_explain('EXECUTE pj_p(2)');
EXECUTE pj_p(2);
EXECUTE pj_p(4);
DEALLOCATE pj_p;
RESET plan_cache_mode;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SET pg_lion.enable_probe_join = off;
SELECT c.region = 'south' AS s, count(*)
  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
 WHERE c.tier = 2 GROUP BY 1 ORDER BY 1;
SELECT c.region = 'south' AS s, count(*)
  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
 WHERE c.tier = 4 GROUP BY 1 ORDER BY 1;
RESET pg_lion.enable_probe_join;
-- a correlated subquery: the node rescanned with a new tier each time, its
-- sets built again for each (the builds the counters add up)
SELECT pj($$SELECT t, (SELECT count(*)
						FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
					   WHERE c.tier = t AND c.active
					   GROUP BY c.region = 'east' ORDER BY 1 LIMIT 1) AS n
			  FROM generate_series(1, 5) t$$);
SELECT * FROM pj_explain($$SELECT t, (SELECT count(*)
									   FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
									  WHERE c.tier = t AND c.active
									  GROUP BY c.region = 'east' ORDER BY 1 LIMIT 1) AS n
							 FROM generate_series(1, 5) t$$, true);

-- ---------- 5. pages not all-visible ----------
-- A HOT update of rows (their note), a non-HOT one (their tier), deletes
-- and inserts, none of them vacuumed, on the first half of the table: the
-- rows on those pages are read from the heap, the versions the snapshot does
-- not see are not counted; the second half's pages are still all-visible.
UPDATE pj_customers SET note = 'hot' WHERE id % 11 = 0 AND id <= 3000;
UPDATE pj_customers SET tier = 1 + tier % 5 WHERE id % 13 = 0 AND id <= 3000;
DELETE FROM pj_customers WHERE id % 17 = 0 AND id <= 3000;
INSERT INTO pj_customers
SELECT i, 'north', 1 + i % 5, true, 'calm', '{1}', 'new'
  FROM generate_series(6001, 6100) i;
INSERT INTO pj_orders
SELECT 30000 + i, 6001 + i % 100, i % 3, i % 100 FROM generate_series(1, 500) i;
SELECT pj($$SELECT CASE WHEN c.region IN ('north', 'east') THEN 'ne'
						WHEN c.tier = 1 THEN 't1' ELSE 'other' END AS k,
				   o.channel, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.active AND c.tier < 4
			 GROUP BY 1, 2$$);
SELECT pj($$SELECT c.mood = 'busy' AS b, c.mood IN ('calm', 'gone') AS cg, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.mood IS NOT NULL GROUP BY 1, 2$$);
SELECT pj($$SELECT c.tier = 3 AS t3, c.tags && '{1}' AS t1, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE NOT EXISTS (SELECT 1 FROM pj_blocked b WHERE b.customer_id = o.customer_id)
			 GROUP BY 1, 2$$);
SELECT * FROM pj_explain($$SELECT c.tier = 3 AS t3, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE c.active
							GROUP BY 1$$, true, false);
-- the deleted customers' orders are in no group
SELECT * FROM pj_val($$SELECT c.tier = 3 AS t3, count(*)
						 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
						WHERE c.active
						GROUP BY 1$$);
SET pg_lion.enable_probe_join = off;
SELECT c.tier = 3 AS t3, count(*)
  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
 WHERE c.active
 GROUP BY 1 ORDER BY 1;
RESET pg_lion.enable_probe_join;
-- vacuumed, every page all-visible again: no heap read
VACUUM (FREEZE) pj_customers;
SELECT * FROM pj_explain($$SELECT c.tier = 3 AS t3, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE c.active
							GROUP BY 1$$, true);

-- ---------- 6. chosen on price ----------
-- A dimension whose heap is many times the fact: core hashes the rows of its
-- WHERE, read from the heap by a bitmap scan; the node looks the fact's few
-- thousand keys up in the primary key and reads no heap at all.  Nothing is
-- disabled.
CREATE TABLE pj_accounts (
	id int PRIMARY KEY,
	segment int NOT NULL,	-- lion: ten values
	flag bool NOT NULL,		-- lion
	pad text
) WITH (autovacuum_enabled = off);
INSERT INTO pj_accounts
SELECT i, i % 10, i % 4 = 0, repeat('p', 80) FROM generate_series(1, 80000) i;
CREATE INDEX pj_accounts_segment ON pj_accounts USING lion (segment);
CREATE INDEX pj_accounts_flag ON pj_accounts USING lion (flag);
CREATE TABLE pj_events (id int NOT NULL, account_id int, kind int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO pj_events
SELECT i, 1 + abs(hashint4(i)) % 80000, i % 3 FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) pj_accounts, pj_events;
EXPLAIN (COSTS OFF)
SELECT a.flag, e.kind, count(*)
  FROM pj_events e JOIN pj_accounts a ON e.account_id = a.id
 WHERE a.segment IN (1, 2, 3, 4, 5, 6)
 GROUP BY 1, 2;
SELECT pj($$SELECT a.flag, e.kind, count(*)
			  FROM pj_events e JOIN pj_accounts a ON e.account_id = a.id
			 WHERE a.segment IN (1, 2, 3, 4, 5, 6)
			 GROUP BY 1, 2$$);
SELECT * FROM pj_explain($$SELECT a.flag, e.kind, count(*)
							 FROM pj_events e JOIN pj_accounts a ON e.account_id = a.id
							WHERE a.segment IN (1, 2, 3, 4, 5, 6)
							GROUP BY 1, 2$$, true);
DROP TABLE pj_events, pj_accounts;

-- ---------- 7. the parallel form ----------
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT pj($$SELECT CASE WHEN c.region IN ('north', 'east') THEN 'ne'
						WHEN c.tier = 1 THEN 't1' ELSE 'other' END AS k,
				   o.channel, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.active AND c.tier < 4
			 GROUP BY 1, 2$$);
SELECT pj($$SELECT c.mood = 'idle' AS idle, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE NOT EXISTS (SELECT 1 FROM pj_blocked b WHERE b.customer_id = o.customer_id)
			 GROUP BY 1$$);
SELECT pj($$SELECT count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.region = 'south' AND o.amount < 30$$);
SELECT * FROM pj_explain($$SELECT c.mood = 'idle' AS idle, count(*)
							 FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
							WHERE NOT EXISTS (SELECT 1 FROM pj_blocked b WHERE b.customer_id = o.customer_id)
							GROUP BY 1$$);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;

-- ---------- 8. not offered ----------
-- a dimension column outside a predicate: in the GROUP BY, under a CASE's
-- THEN, in an aggregate
SELECT pj($$SELECT c.region, count(*) FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.tier = 2 GROUP BY 1$$);
SELECT pj($$SELECT CASE WHEN c.active THEN c.tier END AS t, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id GROUP BY 1$$);
SELECT pj($$SELECT c.active, max(c.tier) FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 GROUP BY 1$$);
-- a predicate of a column no lion index has, in the GROUP BY or the WHERE
SELECT pj($$SELECT c.note = 'hot' AS h, count(*) FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 GROUP BY 1$$);
SELECT pj($$SELECT c.active, count(*) FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.note LIKE 'n1%' GROUP BY 1$$);
-- a clause lion answers only through a range core derives from it: a LIKE
-- over a column of "C" collation is the range of its prefix, a lossy index
-- clause, and the range's set holds rows the pattern rejects (`k1...` not
-- ending in 7).  The node declines it, alone or beside an exact clause; the
-- same range written out is the clause itself, and the node takes it.
CREATE TABLE pj_codes (
	id int PRIMARY KEY,
	code text COLLATE "C" NOT NULL,	-- lion: 'k1' .. 'k6000'
	kind int NOT NULL				-- lion: 0 to 3
) WITH (autovacuum_enabled = off);
INSERT INTO pj_codes SELECT i, 'k' || i, i % 4 FROM generate_series(1, 6000) i;
CREATE INDEX pj_codes_code ON pj_codes USING lion (code);
CREATE INDEX pj_codes_kind ON pj_codes USING lion (kind);
VACUUM (FREEZE, ANALYZE) pj_codes;
SELECT pj($$SELECT d.kind = 1 AS k1, count(*) FROM pj_orders o JOIN pj_codes d ON o.customer_id = d.id
			 WHERE d.code LIKE 'k1%7' GROUP BY 1$$);
SELECT * FROM pj_val($$SELECT d.kind = 1 AS k1, count(*) FROM pj_orders o JOIN pj_codes d ON o.customer_id = d.id
						WHERE d.code LIKE 'k1%7' GROUP BY 1$$);
SELECT pj($$SELECT d.kind = 1 AS k1, count(*) FROM pj_orders o JOIN pj_codes d ON o.customer_id = d.id
			 WHERE d.kind <> 0 AND d.code LIKE 'k2%' GROUP BY 1$$);
SELECT pj($$SELECT d.kind = 1 AS k1, count(*) FROM pj_orders o JOIN pj_codes d ON o.customer_id = d.id
			 WHERE d.code >= 'k1' AND d.code < 'k2' GROUP BY 1$$);
SELECT * FROM pj_val($$SELECT d.kind = 1 AS k1, count(*) FROM pj_orders o JOIN pj_codes d ON o.customer_id = d.id
						WHERE d.code >= 'k1' AND d.code < 'k2' GROUP BY 1$$);
DROP TABLE pj_codes;
-- a predicate no posting set decides
SELECT pj($$SELECT c.tier >= 4 AS high, count(*) FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 GROUP BY 1$$);
-- a predicate of both tables
SELECT pj($$SELECT c.tier = o.channel AS same, count(*)
			  FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id GROUP BY 1$$);
-- an outer join, and a join on a column no unique index has
SELECT pj($$SELECT c.active, count(*) FROM pj_orders o LEFT JOIN pj_customers c ON o.customer_id = c.id
			 GROUP BY 1$$);
SELECT pj($$SELECT c.active, count(*) FROM pj_orders o JOIN pj_customers c ON o.amount = c.tier
			 GROUP BY 1$$);
-- nothing for a set to answer: core's nested loop is already this
SELECT pj($$SELECT o.channel, count(*) FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 GROUP BY 1$$);
-- the switch
SET pg_lion.enable_probe_join = off;
SELECT pj($$SELECT c.active, count(*) FROM pj_orders o JOIN pj_customers c ON o.customer_id = c.id
			 WHERE c.tier = 2 GROUP BY 1$$);
RESET pg_lion.enable_probe_join;

DROP TABLE pj_orders, pj_customers, pj_blocked;
DROP TYPE pj_mood;
DROP FUNCTION pj(text), pj_val(text), pj_explain(text, boolean, boolean);
