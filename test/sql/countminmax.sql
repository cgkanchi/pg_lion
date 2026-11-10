-- count(*) beside min() and max() of a column over one table's WHERE: the
-- LionCountMinMax node, which takes the count from lion's count of the table
-- and WHERE and each min or max from a one-row ordered walk, an InitPlan as
-- core's MinMaxAgg makes it (DESIGN.md §10, "A count beside min or max").
-- Each query runs with the node and with pg_lion.enable_count_minmax off, and
-- the two answers are compared.  The table has NULLs in the min and max
-- columns, and dead and updated rows that VACUUM has not seen, so that the
-- count's heap checks (DESIGN.md §9) and the walks' visibility checks are
-- part of every answer.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- ts: a permutation of the ids, NULL in every tenth row; t: text, its own
-- btree; r: no index of its own
CREATE TABLE cm (id int, a int, b int, n int, ts int, t text, r int)
	WITH (autovacuum_enabled = off);
INSERT INTO cm SELECT i, i % 7, i % 5,
	   CASE WHEN i % 4 = 0 THEN NULL ELSE i % 11 END,
	   CASE WHEN i % 10 = 0 THEN NULL ELSE (i * 7919) % 30011 END,
	   'v' || ((i * 31) % 997),
	   (i * 13) % 1009
  FROM generate_series(1, 20000) i;
CREATE INDEX cm_a ON cm USING lion (a);
CREATE INDEX cm_b ON cm USING lion (b);
CREATE INDEX cm_n ON cm USING lion (n);
CREATE INDEX cm_ts ON cm (ts);
CREATE INDEX cm_t ON cm (t DESC);
VACUUM (FREEZE, ANALYZE) cm;
-- the ends move: the lowest and highest ts of a = 3 are deleted or updated
DELETE FROM cm WHERE id % 97 = 0;
UPDATE cm SET a = (a + 1) % 7, n = NULL WHERE id % 89 = 0;
DELETE FROM cm WHERE ts = (SELECT min(ts) FROM cm WHERE a = 3);
UPDATE cm SET ts = NULL WHERE ts = (SELECT max(ts) FROM cm WHERE a = 3);

/*
 * cm_q() runs q as planned and with the node off, and compares the two:
 * 'count min max: <answer>' when the node was in the plan and they agree.
 */
CREATE FUNCTION cm_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	lion text;
	core text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_minmax', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCountMinMax\)' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('SELECT string_agg(s::text, '' '' ORDER BY s::text) FROM (%s) s', q) INTO lion;
	PERFORM set_config('pg_lion.enable_count_minmax', 'off', true);
	EXECUTE format('SELECT string_agg(s::text, '' '' ORDER BY s::text) FROM (%s) s', q) INTO core;
	PERFORM set_config('pg_lion.enable_count_minmax', 'on', true);
	IF lion IS DISTINCT FROM core THEN
		RETURN format('MISMATCH: %s | %s', lion, core);
	END IF;
	RETURN format('%s: %s',
				  CASE WHEN used THEN 'count min max' ELSE 'no count min max' END,
				  lion);
END $$;

-- the plan's nodes, an InitPlan's name and a scan's relation left out, as
-- they are printed differently by each major version
CREATE FUNCTION cm_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(InitPlan|Custom Scan|Index|Seq Scan|Limit|Aggregate|Sort|Result)' AND
		   ln !~ '(Index Searches|Index Cond|Recheck Cond|Lion Indexes)' THEN
			RETURN NEXT regexp_replace(regexp_replace(ln, 'InitPlan.*$', 'InitPlan'),
									   ' on .*$', '');
		END IF;
	END LOOP;
END $$;

-- the count from the count, each end from a walk of cm_ts
SELECT cm_plan('SELECT count(*), min(ts), max(ts) FROM cm WHERE a = 3 AND b = 1');
SELECT count(*), min(ts), max(ts) FROM cm WHERE a = 3 AND b = 1;
SET pg_lion.enable_count_minmax = off;
SELECT count(*), min(ts), max(ts) FROM cm WHERE a = 3 AND b = 1;
RESET pg_lion.enable_count_minmax;

SELECT q, cm_q(q) FROM (VALUES
	('SELECT count(*), min(ts), max(ts) FROM cm WHERE a = 3'),
	('SELECT count(*), max(ts) FROM cm WHERE a = 3 AND b = 1'),
	('SELECT min(ts), count(*) FROM cm WHERE a IN (2, 4) AND n <> 3'),
	('SELECT count(*), min(ts), max(ts) FROM cm'),
	('SELECT count(*), count(1), min(ts), max(ts) FROM cm WHERE a = 3'),
	('SELECT count(*), min(ts), max(ts), min(t), max(t) FROM cm WHERE a = 3 AND b = 1'),
	('SELECT max(ts) - min(ts) AS span, count(*) + 1, max(ts) FROM cm WHERE a = 5'),
	('SELECT count(*), min(DISTINCT ts) FROM cm WHERE a = 3'),
	('SELECT count(*), max(ts) FROM cm WHERE a = 3 AND n IS NULL'),
	('SELECT count(*), min(ts), max(ts) FROM cm WHERE a = 3 AND b = 9'),
	('SELECT count(*), min(ts), max(ts) FROM cm WHERE a = 3 AND ts IS NULL'),
	('SELECT count(*), min(ts), max(ts) FROM cm WHERE a = 3 AND ts > 15000'),
	('SELECT count(*), max(ts) FROM cm WHERE a = 3 AND id % 3 = 1'),
	('SELECT count(*), min(r), max(r) FROM cm WHERE a = 3 AND b = 1'),
	('SELECT count(*), max(ts) FROM cm WHERE a = 3 ORDER BY 1'),
	-- only min and max: core's MinMaxAgg
	('SELECT min(ts), max(ts) FROM cm WHERE a = 3'),
	-- something neither the count nor a walk answers, or a shape it is not
	('SELECT count(*), sum(ts), max(ts) FROM cm WHERE a = 3'),
	('SELECT count(*), max(ts) FILTER (WHERE b = 1) FROM cm WHERE a = 3'),
	('SELECT count(*), max(ts), max(ts) FILTER (WHERE b = 1) FROM cm WHERE a = 3'),
	('SELECT count(*), max(ts + 0 * random()::int) FROM cm WHERE a = 3'),
	('SELECT b, count(*), max(ts) FROM cm WHERE a = 3 GROUP BY b'),
	('SELECT count(*), max(ts) FROM cm WHERE a = 3 HAVING max(ts) > 0'),
	('SELECT count(*), max(ts) FROM cm WHERE a = 3 AND random() >= 0'),
	('SELECT count(*), max(ts), sum(count(*)) OVER () FROM cm WHERE a = 3'),
	('SELECT count(*), max(c.ts) FROM cm c JOIN cm d ON d.id = c.id + 1 WHERE c.a = 3'),
	('SELECT count(*), max(ts) FROM cm WHERE a = 3 AND id IN (SELECT id FROM cm WHERE b = 1)')
) v(q);

-- a parameter of the WHERE: a generic plan carries it into each walk
PREPARE cmp(int) AS SELECT count(*), min(ts), max(ts) FROM cm WHERE a = $1 AND b = 1;
SET plan_cache_mode = force_generic_plan;
SELECT cm_plan('EXECUTE cmp(2)');
EXECUTE cmp(2);
EXECUTE cmp(5);
SET pg_lion.enable_count_minmax = off;
EXECUTE cmp(2);
EXECUTE cmp(5);
RESET pg_lion.enable_count_minmax;
RESET plan_cache_mode;
DEALLOCATE cmp;

-- a WHERE that reads an outer query's row is not carried into a walk
SELECT cm_q('SELECT x, (SELECT count(*)::text || ''/'' || max(ts) FROM cm WHERE a = x) FROM generate_series(0, 7) x');
-- a target that does: the node is rescanned for each outer row, its count
-- and walks made once (the Limit is a walk's)
CREATE FUNCTION cm_loops(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ '(Custom Scan \((LionCountMinMax|LionCount)\)|Limit)' THEN
			RETURN NEXT coalesce(substring(ln from 'Lion[A-Za-z]+'), 'Limit') || ': ' ||
				coalesce(substring(ln from 'loops=[0-9]+'), 'never executed');
		END IF;
	END LOOP;
END $$;
SELECT cm_loops('SELECT x, (SELECT count(*) + x + max(ts) FROM cm WHERE a = 3 AND b = 1)
  FROM generate_series(1, 3) x');
SELECT cm_q('SELECT x, (SELECT count(*) + x + max(ts) FROM cm WHERE a = 3 AND b = 1) FROM generate_series(1, 3) x');

-- a partitioned table: the count of every partition, the walks merged
CREATE TABLE cmpt (id int, a int, ts int) PARTITION BY RANGE (id);
CREATE TABLE cmpt1 PARTITION OF cmpt FOR VALUES FROM (0) TO (10000);
CREATE TABLE cmpt2 PARTITION OF cmpt FOR VALUES FROM (10000) TO (20001);
INSERT INTO cmpt SELECT i, i % 7, (i * 7919) % 30011 FROM generate_series(1, 20000) i;
CREATE INDEX cmpt_a ON cmpt USING lion (a);
CREATE INDEX cmpt_ts ON cmpt (ts);
VACUUM (FREEZE, ANALYZE) cmpt;
DELETE FROM cmpt WHERE id % 101 = 0;
SELECT cm_q('SELECT count(*), min(ts), max(ts) FROM cmpt WHERE a = 3');

-- a sliced column (DESIGN.md §52): the count could answer a min or max of it
-- from the slices, and the node walks the column instead, which is cheaper
CREATE TABLE cms (id int, g int, v int) WITH (autovacuum_enabled = off);
INSERT INTO cms SELECT i, i % 10, CASE WHEN i % 9 = 0 THEN NULL ELSE (i * 7919) % 3001 - 1500 END
  FROM generate_series(1, 20000) i;
CREATE INDEX cms_g ON cms USING lion (g);
CREATE INDEX cms_v ON cms USING lion (v) WITH (slices = on);
VACUUM (FREEZE, ANALYZE) cms;
DELETE FROM cms WHERE id % 103 = 0;
SELECT cm_q('SELECT count(*), sum(v), max(v) FROM cms WHERE v > 0');
SELECT cm_q('SELECT count(*), sum(v), min(v), max(v) FROM cms WHERE v > 0 AND g = 3');
SELECT cm_q('SELECT count(*), min(v) FROM cms WHERE g = 3');

-- row-level security: the policy is the count's to apply, which declines
CREATE TABLE cmrls (id int, a int, ts int, owner text);
INSERT INTO cmrls SELECT i, i % 7, (i * 7919) % 30011,
	   CASE WHEN i % 2 = 0 THEN 'lion_cm_user' ELSE 'other' END
  FROM generate_series(1, 5000) i;
CREATE INDEX cmrls_a ON cmrls USING lion (a);
CREATE INDEX cmrls_ts ON cmrls (ts);
VACUUM (FREEZE, ANALYZE) cmrls;
ALTER TABLE cmrls ENABLE ROW LEVEL SECURITY;
CREATE POLICY cmrls_own ON cmrls USING (owner = current_user);
CREATE ROLE lion_cm_user;
GRANT SELECT ON cm, cmrls TO lion_cm_user;
SET ROLE lion_cm_user;
SELECT cm_q('SELECT count(*), min(ts), max(ts) FROM cmrls WHERE a = 3');

-- EXECUTE on max() is asked as the Agg asks it
RESET ROLE;
REVOKE EXECUTE ON FUNCTION max(int4) FROM PUBLIC;
SET ROLE lion_cm_user;
SELECT count(*), max(ts) FROM cm WHERE a = 3 AND b = 1;
SET pg_lion.enable_count_minmax = off;
SELECT count(*), max(ts) FROM cm WHERE a = 3 AND b = 1;
RESET pg_lion.enable_count_minmax;
RESET ROLE;
GRANT EXECUTE ON FUNCTION max(int4) TO PUBLIC;
REVOKE ALL ON cm, cmrls FROM lion_cm_user;
DROP ROLE lion_cm_user;

DROP TABLE cm, cmpt, cmrls;
DROP FUNCTION cm_q(text), cm_plan(text), cm_loops(text);
