-- Other spellings of clauses the count pushdown answers (DESIGN.md §10,
-- "Other spellings of answered clauses"): `c = ANY (array_col)` as
-- `array_col @> ARRAY[c]`, `col IS NOT DISTINCT FROM c` as `col = c` or
-- `col IS NULL`, and `col LIKE 'p%'` as the range of the strings that begin
-- with p.  Each query runs through the LionCount node, with every other scan
-- disabled so that a node that is built at all is the plan, and again as a
-- sequential scan with the pushdown off; the two answers are compared as
-- multisets in both directions.  The table has NULL columns, arrays with NULL
-- elements and empty arrays, strings that end in bytes a naive successor
-- would get wrong, and dead and updated rows that VACUUM has not seen, so
-- that the node's heap checks (DESIGN.md §9) are part of every answer.
--
-- The collations here are "C" and "POSIX", which every build has;
-- spellings_coll.sql has the ones that need ICU.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

SELECT getdatabaseencoding() = 'UTF8' AS sp_utf8 \gset

CREATE TABLE sp (id int, s text COLLATE "C", v varchar(20) COLLATE "C",
				 x text COLLATE "POSIX", n int, b bool, tags text[],
				 vt varchar[], ids int8[], i4 int4[])
	WITH (autovacuum_enabled = off);
INSERT INTO sp
SELECT i,
	   CASE WHEN i % 10 = 0 THEN NULL
			ELSE (ARRAY['pa', 'paid', 'pb', 'p', 'pa%x', 'pa_x', 'pay', 'q', 'oz', ''])[1 + (i / 3) % 10] END,
	   CASE WHEN i % 11 = 0 THEN NULL
			ELSE (ARRAY['pa', 'paid', 'pb', 'void', 'pending'])[1 + i % 5] END,
	   CASE WHEN i % 13 = 0 THEN NULL
			ELSE (ARRAY['pa', 'paid', 'pb', 'p'])[1 + i % 4] END,
	   CASE WHEN i % 4 = 0 THEN NULL ELSE i % 7 END,
	   CASE WHEN i % 6 = 0 THEN NULL ELSE i % 2 = 0 END,
	   CASE WHEN i % 5 = 0 THEN NULL
			WHEN i % 7 = 0 THEN ARRAY['t1', NULL]
			WHEN i % 17 = 0 THEN '{}'::text[]
			WHEN i % 19 = 0 THEN ARRAY[NULL, NULL]::text[]
			ELSE ARRAY['t' || (i % 3), 't' || (i % 11)] END,
	   CASE WHEN i % 8 = 0 THEN NULL ELSE ARRAY['t' || (i % 3), NULL]::varchar[] END,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE ARRAY[i % 3, i % 5, 5000000000 + i % 2]::int8[] END,
	   CASE WHEN i % 12 = 0 THEN NULL ELSE ARRAY[i % 4, NULL]::int4[] END
  FROM generate_series(1, 20000) i;
-- strings whose last character a byte-wise successor gets wrong: U+043F
-- ends in byte 0xBF, U+007F is the last one-byte character, U+D7FF is
-- followed by the surrogates, which no string holds, and U+10FFFF has no
-- successor at all
\if :sp_utf8
INSERT INTO sp (id, s)
SELECT 20000 + i, w
  FROM generate_series(1, 30) i,
	   unnest(ARRAY['п', 'пa', 'р', 'рa', 'a' || chr(127), 'a' || chr(127) || 'b',
					'a' || chr(128), 'a' || chr(55295), 'a' || chr(55295) || 'z',
					'a' || chr(57344), 'a' || chr(1114111), 'a' || chr(1114111) || 'b',
					'pá', 'páx', 'pb']) w;
\endif
CREATE INDEX sp_s ON sp USING lion (s);
CREATE INDEX sp_v ON sp USING lion (v);
CREATE INDEX sp_x ON sp USING lion (x);
CREATE INDEX sp_n ON sp USING lion (n);
CREATE INDEX sp_b ON sp USING lion (b);
CREATE INDEX sp_tags ON sp USING lion (tags);
CREATE INDEX sp_vt ON sp USING lion (vt);
CREATE INDEX sp_ids ON sp USING lion (ids);
CREATE INDEX sp_i4 ON sp USING lion (i4);
VACUUM (FREEZE, ANALYZE) sp;
-- dead and updated rows the visibility map no longer covers
DELETE FROM sp WHERE id % 97 = 0;
UPDATE sp SET s = 'paid', tags = ARRAY['t1'], n = NULL WHERE id % 89 = 0;
UPDATE sp SET s = NULL, tags = NULL, n = 3 WHERE id % 83 = 0;

/*
 * sp_q() runs q through the LionCount node and as a sequential scan with the
 * pushdown off, and compares the two: 'count, N rows' when the node was used
 * and they agree.
 */
CREATE FUNCTION sp_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE sp_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE sp_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM sp_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM sp_on EXCEPT ALL SELECT * FROM sp_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM sp_off EXCEPT ALL SELECT * FROM sp_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE sp_on, sp_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', CASE WHEN used THEN 'count' ELSE 'no count' END,
				  nrows);
END $$;

-- the plan, with the node the only scan allowed
CREATE FUNCTION sp_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

-- ---- c = ANY (array_col): array_col @> ARRAY[c] ----
SELECT sp_plan('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags)');
SELECT sp_plan('SELECT count(*) FROM sp WHERE ''t1'' = ANY (vt)');
SELECT sp_plan('SELECT count(*) FROM sp WHERE 1 = ANY (ids)');
SELECT q, sp_q(q) FROM (VALUES
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags)'),
	('SELECT count(*) FROM sp WHERE ''t2'' = ANY (tags)'),
	('SELECT count(*) FROM sp WHERE ''t7'' = ANY (tags)'),
	('SELECT count(*) FROM sp WHERE ''nope'' = ANY (tags)'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (vt)'),
	('SELECT count(*) FROM sp WHERE ''t1''::varchar = ANY (vt)'),
	('SELECT count(*) FROM sp WHERE 1 = ANY (ids)'),
	('SELECT count(*) FROM sp WHERE 1::int8 = ANY (ids)'),
	('SELECT count(*) FROM sp WHERE 1::int2 = ANY (ids)'),
	('SELECT count(*) FROM sp WHERE 5000000001 = ANY (ids)'),
	('SELECT count(*) FROM sp WHERE 2 = ANY (i4)'),
	('SELECT count(*) FROM sp WHERE 2::int8 = ANY (i4)'),
	('SELECT count(*) FROM sp WHERE 3::int2 = ANY (i4)'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags) AND n = 3'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags) AND ''t2'' = ANY (tags)'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags) AND 2 = ANY (i4)'),
	('SELECT n, count(*) FROM sp WHERE ''t2'' = ANY (tags) GROUP BY n'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags) OR n = 3'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags) OR ''t5'' = ANY (tags)'),
	('SELECT count(*) FILTER (WHERE ''t1'' = ANY (tags)), count(*) FROM sp WHERE n = 2'),
	('SELECT count(*) FROM sp WHERE tags @> ARRAY[''t1''] AND ''t1'' = ANY (tags)')
) v(q);
-- the ordinary plan's: a negation, where the two forms differ on a NULL
-- element (the planner makes it `<> ALL`); a constant the element's type
-- cannot hold; a NULL; an operator that is not the element type's equality;
-- and an array that is not a column
SELECT q, sp_q(q) FROM (VALUES
	('SELECT count(*) FROM sp WHERE NOT (''t1'' = ANY (tags))'),
	('SELECT count(*) FROM sp WHERE 5000000000 = ANY (i4)'),
	('SELECT count(*) FROM sp WHERE NULL::text = ANY (tags)'),
	('SELECT count(*) FROM sp WHERE ''t1'' < ANY (tags)'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ANY (tags || ''{x}''::text[])'),
	('SELECT count(*) FROM sp WHERE ''t1'' = ALL (tags)')
) v(q);
-- a generic plan's `$1 = ANY (tags)`: the run-time containment of the array
-- of $1, rechecked; a NULL $1 selects nothing either way
SET plan_cache_mode = force_generic_plan;
PREPARE sp_any(text) AS SELECT count(*) FROM sp WHERE $1 = ANY (tags);
SELECT sp_plan('EXECUTE sp_any(''t1'')');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXECUTE sp_any('t1');
EXECUTE sp_any('t5');
EXECUTE sp_any(NULL);
RESET enable_seqscan;
RESET enable_bitmapscan;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM sp WHERE 't1' = ANY (tags);
SELECT count(*) FROM sp WHERE 't5' = ANY (tags);
RESET pg_lion.enable_count_pushdown;
DEALLOCATE sp_any;
RESET plan_cache_mode;

-- ---- col IS NOT DISTINCT FROM c: col = c, or col IS NULL ----
SELECT sp_plan('SELECT count(*) FROM sp WHERE s IS NOT DISTINCT FROM ''paid''');
SELECT sp_plan('SELECT count(*) FROM sp WHERE 3 IS NOT DISTINCT FROM n');
SELECT q, sp_q(q) FROM (VALUES
	('SELECT count(*) FROM sp WHERE s IS NOT DISTINCT FROM ''paid'''),
	('SELECT count(*) FROM sp WHERE ''paid'' IS NOT DISTINCT FROM s'),
	('SELECT count(*) FROM sp WHERE v IS NOT DISTINCT FROM ''void'''),
	('SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM 3'),
	('SELECT count(*) FROM sp WHERE 3 IS NOT DISTINCT FROM n'),
	('SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM 3::int8'),
	('SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM NULL'),
	('SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM NULL::int'),
	('SELECT count(*) FROM sp WHERE b IS NOT DISTINCT FROM true'),
	('SELECT count(*) FROM sp WHERE tags IS NOT DISTINCT FROM ARRAY[''t1'']'),
	('SELECT count(*) FROM sp WHERE tags IS NOT DISTINCT FROM ''{}'''),
	('SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM 3 AND s IS NOT DISTINCT FROM ''paid'''),
	('SELECT s, count(*) FROM sp WHERE n IS NOT DISTINCT FROM 2 GROUP BY s'),
	('SELECT n, count(*) FROM sp WHERE n IS NOT DISTINCT FROM 2 GROUP BY n'),
	('SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM 3 OR s = ''pb'''),
	('SELECT count(*) FILTER (WHERE n IS NOT DISTINCT FROM 1), count(*) FROM sp WHERE s = ''paid'''),
	('SELECT count(*) FROM sp WHERE NOT (n IS NOT DISTINCT FROM 3)')
) v(q);
-- a generic plan's `n IS NOT DISTINCT FROM $1` is the ordinary plan's: a
-- NULL $1 makes it `n IS NULL`, which `n = $1` is not
SET plan_cache_mode = force_generic_plan;
PREPARE sp_isnd(int) AS SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM $1;
SELECT p ~ 'LionCount' AS lioncount FROM sp_plan('EXECUTE sp_isnd(3)') p LIMIT 1;
EXECUTE sp_isnd(3);
EXECUTE sp_isnd(NULL);
DEALLOCATE sp_isnd;
RESET plan_cache_mode;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM sp WHERE n IS NOT DISTINCT FROM 3;
SELECT count(*) FROM sp WHERE n IS NULL;
RESET pg_lion.enable_count_pushdown;

-- ---- col LIKE 'p%': the strings that begin with p ----
SELECT sp_plan('SELECT count(*) FROM sp WHERE s LIKE ''pa%''');
SELECT sp_plan('SELECT count(*) FROM sp WHERE v LIKE ''pa%''');
SELECT sp_plan('SELECT count(*) FROM sp WHERE s LIKE ''paid''');
SELECT sp_plan('SELECT count(*) FROM sp WHERE s LIKE ''%''');
SELECT q, sp_q(q) FROM (VALUES
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''p%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pai%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''paid%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''zz%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''paid'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa'''),
	('SELECT count(*) FROM sp WHERE s LIKE '''''),
	('SELECT count(*) FROM sp WHERE s LIKE ''%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''%%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa\%%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa\_%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa\%x'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''\p\a%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa#%%'' ESCAPE ''#'''),
	('SELECT count(*) FROM sp WHERE v LIKE ''pa%'''),
	('SELECT count(*) FROM sp WHERE v LIKE ''pending'''),
	('SELECT count(*) FROM sp WHERE x LIKE ''pa%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%'' AND n = 3'),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%'' AND v LIKE ''p%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%'' AND s >= ''pai'''),
	('SELECT n, count(*) FROM sp WHERE s LIKE ''pa%'' GROUP BY n'),
	('SELECT s, count(*) FROM sp WHERE s LIKE ''pa%'' GROUP BY s'),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%'' OR n = 3'),
	('SELECT count(*) FROM sp WHERE s LIKE ''paid'' OR n = 3'),
	('SELECT count(*) FILTER (WHERE s LIKE ''pa%''), count(*) FILTER (WHERE s LIKE ''q''), count(*) FROM sp WHERE n = 1'),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%'' AND ''t1'' = ANY (tags) AND n IS NOT DISTINCT FROM 1')
) v(q);
-- the last character's successor, not the last byte's
\if :sp_utf8
SELECT q, sp_q(q) FROM (VALUES
	('SELECT count(*) FROM sp WHERE s LIKE ''п%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''a'' || chr(127) || ''%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''a'' || chr(55295) || ''%'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pá%''')
) v(q);
-- a character with no successor is the ordinary plan's
SELECT sp_q('SELECT count(*) FROM sp WHERE s LIKE ''a'' || chr(1114111) || ''%''');
\endif
-- the ordinary plan's: a wildcard before the end, NOT LIKE, ILIKE, and a
-- pattern known only at run time
SELECT q, sp_q(q) FROM (VALUES
	('SELECT count(*) FROM sp WHERE s LIKE ''p_id'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%d'''),
	('SELECT count(*) FROM sp WHERE s LIKE ''pa%_'''),
	('SELECT count(*) FROM sp WHERE s NOT LIKE ''pa%'''),
	('SELECT count(*) FROM sp WHERE s ILIKE ''pa%'''),
	('SELECT count(*) FROM sp WHERE s LIKE (SELECT ''pa%'')')
) v(q);

-- ---- over a partitioned table, whose bounds may imply or refute them ----
CREATE TABLE spp (id int, s text COLLATE "C", n int, tags text[])
	PARTITION BY RANGE (s);
CREATE TABLE spp1 PARTITION OF spp FOR VALUES FROM (MINVALUE) TO ('pa')
	WITH (autovacuum_enabled = off);
CREATE TABLE spp2 PARTITION OF spp FOR VALUES FROM ('pa') TO ('pb')
	WITH (autovacuum_enabled = off);
CREATE TABLE spp3 PARTITION OF spp FOR VALUES FROM ('pb') TO (MAXVALUE)
	WITH (autovacuum_enabled = off);
CREATE TABLE spp4 PARTITION OF spp DEFAULT WITH (autovacuum_enabled = off);
INSERT INTO spp SELECT id, s, n, tags FROM sp WHERE id <= 20000;
CREATE INDEX spp_s ON spp USING lion (s);
CREATE INDEX spp_n ON spp USING lion (n);
CREATE INDEX spp_tags ON spp USING lion (tags);
VACUUM (FREEZE, ANALYZE) spp;
SELECT sp_plan('SELECT count(*) FROM spp WHERE s LIKE ''pa%''');
SELECT q, sp_q(q) FROM (VALUES
	('SELECT count(*) FROM spp WHERE s LIKE ''pa%'''),
	('SELECT count(*) FROM spp WHERE s LIKE ''p%'''),
	('SELECT count(*) FROM spp WHERE s LIKE ''paid'''),
	('SELECT count(*) FROM spp WHERE s LIKE ''%'''),
	('SELECT count(*) FROM spp WHERE s LIKE ''pa%'' AND n = 2'),
	('SELECT n, count(*) FROM spp WHERE s LIKE ''pa%'' GROUP BY n'),
	('SELECT count(*) FROM spp WHERE s IS NOT DISTINCT FROM ''paid'''),
	('SELECT count(*) FROM spp WHERE s IS NOT DISTINCT FROM NULL'),
	('SELECT count(*) FROM spp WHERE n IS NOT DISTINCT FROM 4'),
	('SELECT count(*) FROM spp WHERE ''t1'' = ANY (tags)'),
	('SELECT count(*) FROM spp WHERE ''t1'' = ANY (tags) AND s LIKE ''pa%''')
) v(q);

DROP TABLE spp;
DROP TABLE sp;
DROP FUNCTION sp_q(text);
DROP FUNCTION sp_plan(text);
