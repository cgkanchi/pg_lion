-- The frequencies of the values a query lists, from a lion index on the
-- column (DESIGN.md §33, "The frequencies of the listed values").
--
-- Core estimates `col = c` from ANALYZE's most common values when c is one
-- of them, and otherwise as one of the other distinct values' equal share of
-- what those leave; `col IN (...)` and `col = ANY (array)` add that up per
-- value.  On a skewed column - a few values with thousands of rows, most with
-- a handful - a heavy value ANALYZE's short list left out is estimated at a
-- rare value's share, and a list of them at a handful of rows a value.  A
-- lion index on the column knows every value's rows exactly, and the row
-- pg_lion hands the planner for the column carries the values the relation's
-- clauses compare it with among its most common values, at their exact
-- frequencies: core's estimate is then each value's rows.
--
-- events.kind: eight heavy values, 1 to 8, with 1,750 to 3,500 rows, and
-- 3,000 rare ones, 1000 to 3999, with 1 to 5 rows each: 30,000 rows in all,
-- in no order.  Statistics target 3 keeps three of the heavy values in the
-- most common list, and the other columns' target of 100 samples 30,000 rows:
-- ANALYZE reads the whole table, so its statistics, and every estimate
-- below, are the same at every run.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

/* The rows the planner expects of q's top node. */
CREATE FUNCTION mcv_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (j -> 0 -> 'Plan' ->> 'Plan Rows')::numeric::bigint;
END $$;

/*
 * q's rows as the planner expects them with pg_lion.enable_index_mcv on and
 * off, and as q returns them.
 */
CREATE FUNCTION mcv_show(q text, OUT est_on bigint, OUT est_off bigint,
						 OUT actual bigint)
LANGUAGE plpgsql AS $$
DECLARE
	saved text := current_setting('pg_lion.enable_index_mcv');
BEGIN
	PERFORM set_config('pg_lion.enable_index_mcv', 'on', true);
	est_on := mcv_rows(q);
	PERFORM set_config('pg_lion.enable_index_mcv', 'off', true);
	est_off := mcv_rows(q);
	PERFORM set_config('pg_lion.enable_index_mcv', saved, true);
	EXECUTE 'SELECT count(*) FROM (' || q || ') s' INTO actual;
END $$;

/* The rows the planner expects of each scan of rel in q, by alias. */
CREATE FUNCTION mcv_scans(q text, rel text) RETURNS TABLE (alias text, est bigint)
LANGUAGE plpgsql AS $$
DECLARE
	j	jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN QUERY
	SELECT n ->> 'Alias', (n ->> 'Plan Rows')::numeric::bigint
	  FROM jsonb_path_query(j, 'strict $.** ? (@."Relation Name" == $r)',
							jsonb_build_object('r', rel)) n
	 ORDER BY 1;
END $$;

/* An IN list of n values: the heavy ones, then rare ones and absent ones. */
CREATE FUNCTION mcv_list(n int, first int DEFAULT 1000) RETURNS text
LANGUAGE sql AS $$
	SELECT string_agg(k::text, ', ' ORDER BY ord)
	  FROM (SELECT k, k AS ord FROM generate_series(1, least(n, 8)) k
			UNION ALL
			SELECT first + 7 * i, 8 + i FROM generate_series(1, n - 8) i) s
$$;

CREATE TABLE events (id int NOT NULL, kind int NOT NULL, owner_id int NOT NULL,
					 payload text) WITH (autovacuum_enabled = off);
ALTER TABLE events ALTER kind SET STATISTICS 3;
INSERT INTO events
SELECT row_number() OVER (ORDER BY (i * 7919) % 30011), kind, owner_id,
	   repeat('x', 200)
  FROM (SELECT row_number() OVER () AS i, k AS kind, (r * 7 + k) % 500 AS owner_id
		  FROM generate_series(1, 8) k, generate_series(1, 1500 + 250 * k) r
		UNION ALL
		SELECT 100000 + row_number() OVER (), k, k % 500
		  FROM generate_series(1000, 3999) k, generate_series(1, 1 + k % 5) r) s
 ORDER BY (i * 7919) % 30011;
CREATE INDEX events_kind_btree ON events (kind);
CREATE INDEX events_lion ON events USING lion (kind, owner_id);
VACUUM ANALYZE events;
CREATE TABLE owners (id int PRIMARY KEY, region text NOT NULL);
INSERT INTO owners SELECT i, 'r' || (i % 7) FROM generate_series(0, 499) i;
ANALYZE owners;

-- ---------- 1. what ANALYZE has, and the switch ----------
SELECT count(*) AS rows, count(DISTINCT kind) AS kinds FROM events;
SELECT most_common_vals FROM pg_stats
 WHERE tablename = 'events' AND attname = 'kind';
SELECT ndistinct FROM lion_index_stats('events_lion');
SHOW pg_lion.enable_index_mcv;

-- ---------- 2. col = const ----------
-- A heavy value in ANALYZE's list, heavy ones outside it, a rare one and one
-- the index does not have (0 rows, which the planner rounds up to 1).  The
-- switch off, each value outside the list is core's share of the rest.
SELECT q, s.*
  FROM (VALUES ('kind = 8'), ('kind = 1'), ('kind = 5'), ('5 = kind'),
			   ('kind = 1002'), ('kind = 99999')) v(q),
	   LATERAL mcv_show('SELECT * FROM events WHERE ' || q) s;

-- ---------- 3. IN lists and = ANY (array) ----------
SELECT q, s.*
  FROM (VALUES ('kind IN (1, 2, 8, 1002, 1003, 99999)'),
			   ('kind IN (1, 2, 3, 4, 5)'),
			   ('kind IN (1000, 1001, 1002, 1003, 1004)'),
			   ('kind = ANY (''{1, 2, 99999}''::int[])'),
			   ('kind = ANY (ARRAY[3, 4, 1004])'),
			   ('kind IN (1, NULL)'),
			   -- a value listed twice is counted twice, by core, either way
			   ('kind IN (1, 1, 2, 2)'),
			   ('kind IN (1, 2) AND kind IN (2, 3)'),
			   ('kind IN (1, 2) AND owner_id < 250')) v(q),
	   LATERAL mcv_show('SELECT * FROM events WHERE ' || q) s;

-- ---------- 4. ORs ----------
-- An OR of equalities on the column, which lion takes as the IN list it
-- spells; arms of an OR with other columns, and ANDs inside its arms.
SELECT q, s.*
  FROM (VALUES ('kind = 1 OR kind = 2 OR kind = 99999'),
			   ('kind = 1 OR owner_id = 3'),
			   ('(kind = 1 AND id > 0) OR kind = 1002'),
			   ('kind IN (1, 2) OR (kind = 3 AND owner_id = 4)')) v(q),
	   LATERAL mcv_show('SELECT * FROM events WHERE ' || q) s;

-- ---------- 5. every key column of the index has its keys ----------
SELECT q, s.*
  FROM (VALUES ('owner_id IN (3, 7, 499)'),
			   ('owner_id = 3 AND kind = 1'),
			   ('owner_id IN (3, 7) AND kind IN (1, 2, 1002)')) v(q),
	   LATERAL mcv_show('SELECT * FROM events WHERE ' || q) s;

-- ---------- 6. Params stay core's ----------
-- A generic plan's `kind = $1` is estimated from the number of distinct
-- values, the switch on or off, and `kind IN (1, 2, $1)` adds that to the
-- frequencies of 1 and 2; a custom plan has the value, and its frequency.
-- The switch is read when a statement is planned, and a generic plan is
-- planned once: each setting prepares its own.
SET plan_cache_mode = force_generic_plan;
PREPARE by_kind(int) AS SELECT * FROM events WHERE kind = $1;
PREPARE by_list(int) AS SELECT * FROM events WHERE kind IN (1, 2, $1);
SELECT mcv_rows('EXECUTE by_kind(1)') AS by_kind,
	   mcv_rows('EXECUTE by_list(1002)') AS by_list;
DEALLOCATE by_kind;
DEALLOCATE by_list;
SET pg_lion.enable_index_mcv = off;
PREPARE by_kind(int) AS SELECT * FROM events WHERE kind = $1;
PREPARE by_list(int) AS SELECT * FROM events WHERE kind IN (1, 2, $1);
SELECT mcv_rows('EXECUTE by_kind(1)') AS by_kind,
	   mcv_rows('EXECUTE by_list(1002)') AS by_list;
RESET pg_lion.enable_index_mcv;
SET plan_cache_mode = force_custom_plan;
SELECT mcv_rows('EXECUTE by_kind(1)') AS by_kind,
	   mcv_rows('EXECUTE by_list(1002)') AS by_list;
RESET plan_cache_mode;
DEALLOCATE by_kind;
DEALLOCATE by_list;

-- ---------- 7. each reference to the relation has its own values ----------
-- A self-join's two scans of events, each with the frequencies of its own
-- list.
SELECT * FROM mcv_scans('SELECT * FROM events a JOIN events b
	ON a.owner_id = b.owner_id WHERE a.kind = 1 AND b.kind IN (2, 1002)',
	'events');

-- ---------- 8. the cap: 1,000 values ----------
-- 1,000 values of one column are looked up; a list of 1,001, or two lists
-- that together name more than 1,000, are estimated as core estimates them.
-- A comparison that is not taken leaves every value of the column to core,
-- not only its own: a list too long beside a short one, and a constant of
-- another type (int48eq is not the index's int4 =; a `<>` is no comparison
-- for equality and changes nothing).
SELECT what, s.*
  FROM (VALUES ('1,000 values', 'kind IN (' || mcv_list(1000) || ')'),
			   ('1,001 values', 'kind IN (' || mcv_list(1001) || ')'),
			   ('two lists of 600, 1,192 values',
				'kind IN (' || mcv_list(600) || ') AND kind IN (' ||
				mcv_list(600, 1001) || ')'),
			   ('2 values and a list of 1,001',
				'kind IN (1, 2) AND kind IN (' || mcv_list(1001) || ')'),
			   ('2 values or a bigint', 'kind IN (1, 2) OR kind = 5::bigint'),
			   ('2 values and not a bigint',
				'kind IN (1, 2) AND kind <> 5::bigint')) v(what, q),
	   LATERAL mcv_show('SELECT * FROM events WHERE ' || q) s;

-- ---------- 9. plans ----------
-- The planner that expected 34 rows of five heavy values reads them through
-- an index scan and joins or groups them; with their 11,250 rows it counts
-- them from the index.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM events WHERE kind IN (1, 2, 3, 4, 5);
EXPLAIN (COSTS OFF)
SELECT o.region, count(*) FROM events e JOIN owners o ON e.owner_id = o.id
 WHERE e.kind IN (1, 2, 3, 4, 5) GROUP BY o.region;
EXPLAIN (COSTS OFF)
SELECT owner_id, count(*) FROM events WHERE kind IN (1, 2, 3, 4, 5)
 GROUP BY owner_id ORDER BY count(*) DESC, owner_id LIMIT 3;
SET pg_lion.enable_index_mcv = off;
EXPLAIN (COSTS OFF) SELECT * FROM events WHERE kind IN (1, 2, 3, 4, 5);
EXPLAIN (COSTS OFF)
SELECT o.region, count(*) FROM events e JOIN owners o ON e.owner_id = o.id
 WHERE e.kind IN (1, 2, 3, 4, 5) GROUP BY o.region;
EXPLAIN (COSTS OFF)
SELECT owner_id, count(*) FROM events WHERE kind IN (1, 2, 3, 4, 5)
 GROUP BY owner_id ORDER BY count(*) DESC, owner_id LIMIT 3;
RESET pg_lion.enable_index_mcv;
RESET enable_seqscan;
-- the same answers either way
SELECT o.region, count(*) FROM events e JOIN owners o ON e.owner_id = o.id
 WHERE e.kind IN (1, 2, 3, 4, 5) GROUP BY o.region ORDER BY o.region;
SET pg_lion.enable_index_mcv = off;
SELECT o.region, count(*) FROM events e JOIN owners o ON e.owner_id = o.id
 WHERE e.kind IN (1, 2, 3, 4, 5) GROUP BY o.region ORDER BY o.region;
RESET pg_lion.enable_index_mcv;
SELECT owner_id, count(*) FROM events WHERE kind IN (1, 2, 3, 4, 5)
 GROUP BY owner_id ORDER BY count(*) DESC, owner_id LIMIT 3;

-- ---------- 10. what is not served ----------
-- A copy of events whose only lion index on kind is partial, and one on an
-- expression, whose statistics ANALYZE keeps as short as kind's: both are
-- core's, the switch on or off.
CREATE TABLE events_p (id int NOT NULL, kind int NOT NULL, owner_id int NOT NULL)
	WITH (autovacuum_enabled = off);
ALTER TABLE events_p ALTER kind SET STATISTICS 3;
INSERT INTO events_p SELECT id, kind, owner_id FROM events;
CREATE INDEX events_p_kind ON events_p USING lion (kind) WHERE owner_id >= 0;
CREATE INDEX events_p_mod ON events_p USING lion ((kind % 1000));
ALTER INDEX events_p_mod ALTER COLUMN 1 SET STATISTICS 3;
ANALYZE events_p;
SELECT q, s.*
  FROM (VALUES ('kind IN (1, 2, 1002)'),
			   ('kind % 1000 IN (1, 2)')) v(q),
	   LATERAL mcv_show('SELECT * FROM events_p WHERE ' || q) s;
DROP TABLE events_p;

-- A user who may not read every row: under a row-level policy the planner
-- has core's estimate, as the frequencies would count the rows it hides.
CREATE ROLE lion_mcv_reader;
GRANT SELECT ON events TO lion_mcv_reader;
CREATE POLICY events_low ON events FOR SELECT TO lion_mcv_reader
	USING (owner_id < 250);
ALTER TABLE events ENABLE ROW LEVEL SECURITY;
SELECT * FROM mcv_show('SELECT * FROM events WHERE kind IN (1, 2)');
SET ROLE lion_mcv_reader;
SELECT * FROM mcv_show('SELECT * FROM events WHERE kind IN (1, 2)');
RESET ROLE;
ALTER TABLE events DISABLE ROW LEVEL SECURITY;
DROP POLICY events_low ON events;
REVOKE SELECT ON events FROM lion_mcv_reader;
DROP ROLE lion_mcv_reader;

-- ---------- 11. other key types ----------
-- varchar under text's class, and an enum under the class of every enum.
CREATE TYPE mood AS ENUM ('sad', 'calm', 'glad', 'rapt');
CREATE TABLE tags (id int NOT NULL, label varchar(20) NOT NULL, mood mood NOT NULL)
	WITH (autovacuum_enabled = off);
ALTER TABLE tags ALTER label SET STATISTICS 1, ALTER mood SET STATISTICS 1;
INSERT INTO tags
SELECT i,
	   CASE WHEN i % 10 < 4 THEN 'a' WHEN i % 10 < 7 THEN 'b'
			WHEN i % 10 < 9 THEN 'c' ELSE 'x' || (i / 10) END,
	   CASE WHEN i % 100 < 50 THEN 'sad' WHEN i % 100 < 80 THEN 'calm'
			WHEN i % 1000 = 999 THEN 'rapt' ELSE 'glad' END::mood
  FROM generate_series(0, 9999) i;
CREATE INDEX tags_label ON tags USING lion (label);
CREATE INDEX tags_mood ON tags USING lion (mood);
ANALYZE tags;
SELECT q, s.*
  FROM (VALUES ('label = ''b'''), ('label IN (''b'', ''c'', ''x7'', ''zz'')'),
			   ('label = ANY (ARRAY[''c'', ''x9'']::varchar[])'),
			   ('mood = ''calm'''), ('mood IN (''glad'', ''rapt'')')) v(q),
	   LATERAL mcv_show('SELECT * FROM tags WHERE ' || q) s;
DROP TABLE tags;
DROP TYPE mood;

-- ---------- 12. n_distinct is served beside them ----------
-- A column whose ANALYZE samples 3,000 of 60,000 rows: ten heavy owners and
-- 15,000 with two rows each, which ANALYZE puts at a fraction of their
-- number.  The values listed are exact whatever the sample saw, and the
-- groups of the same column are the index's count.
CREATE TABLE accounts (id int NOT NULL, owner_id int NOT NULL)
	WITH (autovacuum_enabled = off);
ALTER TABLE accounts ALTER id SET STATISTICS 10,
	ALTER owner_id SET STATISTICS 10;
INSERT INTO accounts
SELECT i, CASE WHEN i % 2 = 0 THEN (i / 2) % 10 ELSE 1000 + i / 4 END
  FROM generate_series(1, 60000) i;
CREATE INDEX accounts_owner ON accounts USING lion (owner_id);
ANALYZE accounts;
SELECT ndistinct FROM lion_index_stats('accounts_owner');
SELECT est_on, actual
  FROM mcv_show('SELECT * FROM accounts WHERE owner_id IN (5, 1500, 99999)');
-- the groups of `owner_id = 5 OR owner_id > 0`, whose row is the one with the
-- frequency of 5: the index's 15,010, within 3%
SELECT abs(mcv_rows('SELECT owner_id FROM accounts WHERE owner_id = 5 OR owner_id > 0 GROUP BY owner_id') - 15010) <= 450 AS index_ndistinct;
SET pg_lion.enable_index_ndistinct = off;
SELECT mcv_rows('SELECT owner_id FROM accounts WHERE owner_id = 5 OR owner_id > 0 GROUP BY owner_id') < 15010 / 2 AS analyze_ndistinct;
SELECT est_on, actual
  FROM mcv_show('SELECT * FROM accounts WHERE owner_id IN (5, 1500, 99999)');
RESET pg_lion.enable_index_ndistinct;
DROP TABLE accounts;

-- ---------- 13. dead row versions ----------
-- The index counts a value's dead row versions until VACUUM.  Where the
-- list's frequencies then add up to more than the table's rows, they are
-- scaled down to fit: past all of the rows, core takes the values of an IN
-- list to be independent of each other, and a list of every value of the
-- column would come out at about two thirds of the rows.  Eight values of
-- 1,750 rows each, which ANALYZE puts at an eighth each; a fifth of the rows
-- then updated, so that each value's entry has 2,100 TIDs, of which 350 are
-- dead.  The whole list is the table's rows; a shorter one still counts its
-- values' dead versions, less what the scaling takes off, until VACUUM.
CREATE TABLE versions (id int NOT NULL, k int NOT NULL)
	WITH (autovacuum_enabled = off, fillfactor = 70);
INSERT INTO versions SELECT i, i % 8 FROM generate_series(1, 14000) i;
CREATE INDEX versions_k ON versions USING lion (k);
VACUUM ANALYZE versions;
-- a new version of a fifth of the rows, each beside the old one on its page
UPDATE versions SET k = (k + 1) % 8 WHERE id % 5 = 0;
SELECT q, s.*
  FROM (VALUES ('k IN (0, 1, 2, 3, 4, 5, 6, 7)'), ('k IN (0, 1)'), ('k = 3')) v(q),
	   LATERAL mcv_show('SELECT * FROM versions WHERE ' || q) s;
DROP TABLE versions;

DROP TABLE events, owners;
DROP FUNCTION mcv_list(int, int);
DROP FUNCTION mcv_show(text);
DROP FUNCTION mcv_scans(text, text);
DROP FUNCTION mcv_rows(text);
