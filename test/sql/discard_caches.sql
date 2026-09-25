-- Lion indexes under debug_discard_caches (2026-09-24): every catalog read may
-- flush any cache entry, this index's relcache entry and its rd_amcache
-- included.  The recorded order's comparison check (DESIGN.md §21) used to
-- read the identity out of a state that such a flush had just freed, and every
-- lion index then refused to open with a spurious REINDEX error.
--
-- The setting exists only in builds with assertions (DISCARD_CACHES_ENABLED);
-- elsewhere lion_ddc() runs the same queries without it, so the expected
-- output is the same either way.  The tables are small: every query here reads
-- the catalog afresh, many times over.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
RESET client_min_messages;
SET synchronous_commit = on;

CREATE FUNCTION lion_ddc(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	BEGIN
		PERFORM set_config('debug_discard_caches', '1', true);
	EXCEPTION WHEN OTHERS THEN
		NULL;					-- not an assert build: run it without
	END;
	EXECUTE format('SELECT string_agg(s::text, '' '' ORDER BY s::text) FROM (%s) s', q)
		INTO r;
	PERFORM set_config('debug_discard_caches', '0', true);
	RETURN r;
END $$;

CREATE FUNCTION lion_ddc_do(q text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	BEGIN
		PERFORM set_config('debug_discard_caches', '1', true);
	EXCEPTION WHEN OTHERS THEN
		NULL;
	END;
	EXECUTE q;
	PERFORM set_config('debug_discard_caches', '0', true);
END $$;

-- a custom ordered opclass: a string-body comparison a btree class sorts with
CREATE FUNCTION lion_ddc_cmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	AS 'SELECT btint4cmp($1, $2)';
CREATE FUNCTION lion_ddc_lt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 < $2';
CREATE FUNCTION lion_ddc_le(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 <= $2';
CREATE FUNCTION lion_ddc_eq(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 = $2';
CREATE FUNCTION lion_ddc_ge(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 >= $2';
CREATE FUNCTION lion_ddc_gt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 > $2';
CREATE OPERATOR <<~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ddc_lt);
CREATE OPERATOR <=~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ddc_le);
CREATE OPERATOR ==~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ddc_eq);
CREATE OPERATOR >=~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ddc_ge);
CREATE OPERATOR >>~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ddc_gt);
CREATE OPERATOR CLASS lion_ddc_bt FOR TYPE int4 USING btree AS
	OPERATOR 1 <<~, OPERATOR 2 <=~, OPERATOR 3 ==~, OPERATOR 4 >=~, OPERATOR 5 >>~,
	FUNCTION 1 lion_ddc_cmp(int4, int4);
CREATE OPERATOR FAMILY lion_ddc_fam USING lion;
CREATE OPERATOR CLASS lion_ddc_ops FOR TYPE int4 USING lion FAMILY lion_ddc_fam AS
	OPERATOR 1 = (int4, int4),
	OPERATOR 6 < (int4, int4),
	OPERATOR 9 > (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 lion_ddc_cmp(int4, int4);

CREATE TABLE lion_ddct (id int NOT NULL, t text, k int, c citext, u int);
INSERT INTO lion_ddct SELECT i, 'v' || (i % 40), i % 40, chr(65 + i % 26) || (i % 3), i % 50
  FROM generate_series(1, 600) i;
CREATE INDEX lion_ddct_t ON lion_ddct USING lion (t);
CREATE INDEX lion_ddct_k ON lion_ddct USING lion (k);
CREATE INDEX lion_ddct_c ON lion_ddct USING lion (c);
CREATE INDEX lion_ddct_u ON lion_ddct USING lion (u lion_ddc_ops);
VACUUM ANALYZE lion_ddct;
SELECT attno, ordered FROM lion_index_stats('lion_ddct_u');

SET enable_seqscan = off;
-- the count pushdown
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE t = ''v7''');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE t < ''v2''');
SELECT lion_ddc('SELECT k, count(*) FROM lion_ddct WHERE k BETWEEN 3 AND 5 GROUP BY k');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE c = ''b1''');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE c >= ''y''');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE u = 7');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE u > 45');
SELECT lion_ddc('SELECT count(DISTINCT k) FROM lion_ddct WHERE k < 10');
-- the bitmap scan
SET pg_lion.enable_count_pushdown = off;
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE t BETWEEN ''v1'' AND ''v3''');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE k = 11 AND u < 20');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE c < ''C''');
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE u = 7');
-- the plain index scan
SET enable_bitmapscan = off;
SELECT lion_ddc('SELECT id FROM lion_ddct WHERE t = ''v7'' ORDER BY id LIMIT 3');
SELECT lion_ddc('SELECT id FROM lion_ddct WHERE k = 11 ORDER BY id LIMIT 3');
-- and a write under it
SELECT lion_ddc_do('INSERT INTO lion_ddct VALUES (601, ''v7'', 7, ''b1'', 7)');
RESET enable_bitmapscan;
SELECT lion_ddc('SELECT count(*) FROM lion_ddct WHERE t = ''v7'' AND u = 7');
RESET pg_lion.enable_count_pushdown;
RESET enable_seqscan;
SELECT lion_index_verify('lion_ddct_u', true);

DROP TABLE lion_ddct;
DROP OPERATOR FAMILY lion_ddc_fam USING lion;
DROP OPERATOR CLASS lion_ddc_bt USING btree;
DROP OPERATOR <<~ (int4, int4);
DROP OPERATOR <=~ (int4, int4);
DROP OPERATOR ==~ (int4, int4);
DROP OPERATOR >=~ (int4, int4);
DROP OPERATOR >>~ (int4, int4);
DROP FUNCTION lion_ddc_lt(int4, int4), lion_ddc_le(int4, int4), lion_ddc_eq(int4, int4),
	lion_ddc_ge(int4, int4), lion_ddc_gt(int4, int4), lion_ddc_cmp(int4, int4);
DROP FUNCTION lion_ddc(text), lion_ddc_do(text);
