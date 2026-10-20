-- Long keys (DESIGN.md §4 "Long keys", §17 "Long elements"): a lion index
-- takes every value a btree or GIN index on the same column takes, short of
-- an incompressible scalar past 2047 bytes.  A varlena key that does not fit
-- in 2047 bytes is stored compressed, as btree stores a long value, and a
-- text, varchar or bytea array element longer than 2043 bytes is filed under
-- its first bytes and SHA-256, as a long tsvector lexeme is, with a recheck.
-- Every answer must be a sequential scan's: equality, IN, ranges, GROUP BY,
-- count(DISTINCT), min/max, ORDER BY through LionOrdered, the count
-- functions and lion_index_facets(), around every size boundary, compressible
-- and not, before and after writes.  Only the plan's lion node and whether
-- the rows are the same print.

\set VERBOSITY terse
SET client_min_messages = warning;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- the heap compresses with pglz whatever the server's default (lz4 on 19
-- built with it), as the sizes, row positions and plans below are pglz's;
-- lk4 stores lz4's form
SET default_toast_compression = pglz;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;

-- q's rows in its order, under lion's plan and under a sequential scan's:
-- the lion node the plan uses, and whether the rows are the same
CREATE FUNCTION lk_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; node text; a text[]; b text[]; m text[];
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		m := regexp_match(l, '(Custom Scan \(\w+\)|Bitmap Index Scan|Index Scan|Index Only Scan|Aggregates Over Keys)');
		IF m IS NOT NULL AND node IS NULL THEN node := m[1]; END IF;
	END LOOP;
	EXECUTE format('SELECT array_agg(md5(x::text) ORDER BY n) FROM (SELECT x, row_number() OVER () AS n FROM (%s) x) y', q) INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_semijoin', 'off', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'off', true);
	EXECUTE format('SELECT array_agg(md5(x::text) ORDER BY n) FROM (SELECT x, row_number() OVER () AS n FROM (%s) x) y', q) INTO b;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_semijoin', 'on', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'on', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	RETURN coalesce(node, 'no lion node') || ': ' ||
		CASE WHEN a IS NOT DISTINCT FROM b THEN 'same' ELSE 'DIFFER' END ||
		' (' || coalesce(cardinality(b), 0) || ' rows)';
END $$;

-- incompressible text of n bytes
CREATE FUNCTION lk_rand(seed int, n int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
SELECT left(string_agg(md5(seed::text || ':' || j), '' ORDER BY j), n)
  FROM generate_series(1, n / 32 + 1) j $$;

-- ---------------------------------------------------------------------
-- text: compressible values of every size around the limit, longer ones
-- sharing long beginnings, multibyte, and incompressible ones that fit
-- ---------------------------------------------------------------------
CREATE TABLE lk (id int, v text) WITH (autovacuum_enabled = off);
INSERT INTO lk SELECT i, 'k' || lpad((i % 500)::text, 4, '0') FROM generate_series(1, 4000) i;
INSERT INTO lk SELECT 10000 + n, repeat('m', n)
	FROM unnest(ARRAY[2042, 2043, 2044, 2045, 3000, 100000]) n;
INSERT INTO lk SELECT 20000 + i, repeat('abc', 20000) FROM generate_series(1, 3) i;
INSERT INTO lk SELECT 20010 + i, repeat('abc', 20000) || 'd' FROM generate_series(1, 2) i;
INSERT INTO lk VALUES (20020, repeat('hello world ', 300)), (20021, repeat('é', 1500)),
					  (20022, repeat('é', 1500) || 'z'), (20023, lk_rand(7, 1500) || repeat('x', 5000)),
					  (20024, lk_rand(7, 1500) || repeat('x', 5001));
INSERT INTO lk SELECT 30000 + i, lk_rand(i, 2043) FROM generate_series(1, 3) i;
SELECT count(*), max(octet_length(v)), max(pg_column_size(v)) FROM lk WHERE octet_length(v) > 2043;
CREATE INDEX lk_v ON lk USING lion (v);
-- btree takes them too
CREATE INDEX lk_bt ON lk (v);
DROP INDEX lk_bt;
-- the same values stored uncompressed in the heap, inserted into the index
ALTER TABLE lk ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO lk SELECT 40000 + i, repeat('abc', 20000) FROM generate_series(1, 2) i;
INSERT INTO lk VALUES (40010, repeat('m', 3000)), (40011, repeat('é', 1500));
ALTER TABLE lk ALTER COLUMN v SET STORAGE EXTENDED;
VACUUM (FREEZE, ANALYZE) lk;
SELECT lion_index_verify('lk_v', true);
-- one entry per distinct value, however the heap stored it
SELECT (SELECT entries FROM lion_index_stats('lk_v')) = (SELECT count(DISTINCT v) FROM lk) AS one_entry_each;

CREATE TABLE lk_q (q text);
INSERT INTO lk_q VALUES
	($$SELECT id FROM lk WHERE v = repeat('abc', 20000) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = repeat('abc', 20000) || 'd' ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = repeat('m', 2043) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = repeat('m', 2044) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = repeat('m', 100000) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = repeat('é', 1500) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = lk_rand(7, 1500) || repeat('x', 5001) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = lk_rand(2, 2043) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = repeat('abc', 19999) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v IN (repeat('abc', 20000), repeat('m', 3000), 'k0001', repeat('m', 7)) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v = ANY (ARRAY[repeat('é', 1500) || 'z', repeat('m', 2045)]) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v > repeat('m', 2043) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v >= repeat('m', 2044) AND v < repeat('m', 100000) ORDER BY id$$),
	($$SELECT id FROM lk WHERE v BETWEEN repeat('abc', 20000) AND repeat('abc', 20000) || 'd' ORDER BY id$$),
	($$SELECT id FROM lk WHERE v < repeat('abc', 20001) ORDER BY id$$),
	($$SELECT count(*) FROM lk WHERE v = repeat('abc', 20000)$$),
	($$SELECT count(*) FROM lk WHERE v IN (repeat('abc', 20000), repeat('m', 3000), 'k0007')$$),
	($$SELECT count(*) FROM lk WHERE v > repeat('m', 2044)$$),
	($$SELECT md5(v), count(*) FROM lk GROUP BY v ORDER BY 2 DESC, 1 LIMIT 30$$),
	($$SELECT md5(v), count(*) FROM lk WHERE v > 'l' GROUP BY v ORDER BY 1$$),
	($$SELECT count(DISTINCT v) FROM lk$$),
	($$SELECT count(DISTINCT v) FROM lk WHERE v > 'l'$$),
	($$SELECT md5(min(v)), md5(max(v)) FROM lk$$),
	($$SELECT md5(min(v)), md5(max(v)) FROM lk WHERE v > 'k0499'$$),
	($$SELECT id, md5(v) FROM lk ORDER BY v DESC, id LIMIT 25$$),
	($$SELECT id, md5(v) FROM lk WHERE v >= 'k0498' ORDER BY v, id LIMIT 40$$),
	($$SELECT id FROM lk WHERE v > 'l' ORDER BY v, id LIMIT 5 OFFSET 3$$);
SELECT lk_same(q) FROM lk_q;
SELECT lion_index_count('lk_v', repeat('abc', 20000)) AS lion,
	   (SELECT count(*) FROM lk WHERE v = repeat('abc', 20000)) AS query;
SELECT lion_index_count('lk_v', repeat('é', 1500)) AS lion,
	   (SELECT count(*) FROM lk WHERE v = repeat('é', 1500)) AS query;
SELECT count(*) AS keys, count(*) FILTER (WHERE f.count IS DISTINCT FROM g.count) AS differ
  FROM lion_index_facets('lk_v') f FULL JOIN (SELECT v, count(*) FROM lk GROUP BY v) g ON f.key = g.v;

-- an incompressible value past the limit: btree takes it (up to its 2704
-- bytes), lion names the index and the row
\set VERBOSITY default
INSERT INTO lk VALUES (50000, lk_rand(1, 2044));
\set VERBOSITY terse
CREATE TABLE lk_big (v text);
INSERT INTO lk_big VALUES (lk_rand(1, 2500));
CREATE INDEX lk_big_bt ON lk_big (v);
CREATE INDEX lk_big_l ON lk_big USING lion (v);
DROP TABLE lk_big;

-- writes: long values inserted, deleted and updated, without and then with
-- VACUUM, and a REINDEX
BEGIN;
INSERT INTO lk SELECT 60000 + i, repeat('abc', 20000) || repeat('d', i) FROM generate_series(1, 30) i;
SELECT lk_same(q) FROM lk_q WHERE q ~ 'count\(\*\)|GROUP BY|ORDER BY v';
COMMIT;
DELETE FROM lk WHERE id IN (20001, 10044, 20021);
UPDATE lk SET v = v || 'u' WHERE id IN (20002, 40010, 20023);
SELECT lk_same(q) FROM lk_q;
VACUUM lk;
SELECT lk_same(q) FROM lk_q;
SELECT lion_index_verify('lk_v', true);
REINDEX INDEX lk_v;
SELECT lk_same(q) FROM lk_q;
SELECT lion_index_verify('lk_v', true);

-- the same values compressed with lz4 in the heap, where the server has it
-- (pglz otherwise, with the same output): a key that fits in lz4's form is
-- stored in it, and is still one key with its pglz and uncompressed forms.
-- v || '' makes each value anew: a value copied as it is keeps lk's form
CREATE TABLE lk4 (id int, v text) WITH (autovacuum_enabled = off);
CREATE FUNCTION lk_lz4() RETURNS boolean LANGUAGE sql AS $$
	SELECT 'lz4' = ANY (enumvals) FROM pg_settings WHERE name = 'default_toast_compression' $$;
DO $$
BEGIN
	IF lk_lz4() THEN
		ALTER TABLE lk4 ALTER COLUMN v SET COMPRESSION lz4;
	END IF;
END $$;
INSERT INTO lk4 SELECT id, v || '' FROM lk WHERE id < 40000;
SELECT bool_and(pg_column_compression(v) = CASE WHEN lk_lz4() THEN 'lz4' ELSE 'pglz' END) AS lz4_where_it_can
  FROM lk4 WHERE octet_length(v) > 2043;
CREATE INDEX lk4_v ON lk4 USING lion (v);
ALTER TABLE lk4 ALTER COLUMN v SET COMPRESSION pglz;
INSERT INTO lk4 SELECT 100000 + id, v || '' FROM lk WHERE octet_length(v) > 2043 AND id < 40000;
ALTER TABLE lk4 ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO lk4 SELECT 200000 + id, v || '' FROM lk WHERE octet_length(v) > 2043 AND id < 40000;
VACUUM (FREEZE, ANALYZE) lk4;
SELECT lion_index_verify('lk4_v', true);
SELECT (SELECT entries FROM lion_index_stats('lk4_v')) = (SELECT count(DISTINCT v) FROM lk4) AS one_entry_each;
-- the plan's node is left out: it follows the sizes, which are lz4's
SELECT split_part(lk_same(replace(q, 'FROM lk ', 'FROM lk4 ')), ': ', 2) FROM lk_q;
SELECT lion_index_count('lk4_v', repeat('abc', 20000)) AS lion,
	   (SELECT count(*) FROM lk4 WHERE v = repeat('abc', 20000)) AS query;

-- ---------------------------------------------------------------------
-- char(n) wider than a key: every value is blank-padded to 2500 bytes
-- ---------------------------------------------------------------------
CREATE TABLE lk_ch (id int, c char(2500)) WITH (autovacuum_enabled = off);
INSERT INTO lk_ch SELECT i, 'code' || (i % 300) FROM generate_series(1, 3000) i;
INSERT INTO lk_ch VALUES (5000, repeat('w', 2500)), (5001, lk_rand(3, 1800)), (5002, NULL);
CREATE INDEX lk_ch_c ON lk_ch USING lion (c);
VACUUM (FREEZE, ANALYZE) lk_ch;
TRUNCATE lk_q;
INSERT INTO lk_q VALUES
	($$SELECT id FROM lk_ch WHERE c = 'code7' ORDER BY id$$),
	($$SELECT id FROM lk_ch WHERE c = 'code7   ' ORDER BY id$$),
	($$SELECT id FROM lk_ch WHERE c IN ('code1', 'code299', repeat('w', 2500)) ORDER BY id$$),
	($$SELECT id FROM lk_ch WHERE c > 'code298' ORDER BY id$$),
	($$SELECT count(*) FROM lk_ch WHERE c = 'code42'$$),
	($$SELECT rtrim(c), count(*) FROM lk_ch GROUP BY c ORDER BY 2 DESC, 1 LIMIT 5$$),
	($$SELECT count(DISTINCT c) FROM lk_ch$$),
	($$SELECT md5(min(c)), md5(max(c)) FROM lk_ch$$),
	($$SELECT id, rtrim(c) FROM lk_ch ORDER BY c DESC NULLS LAST, id LIMIT 12$$);
SELECT lk_same(q) FROM lk_q;
SELECT lion_index_count('lk_ch_c', 'code42'::char(2500)) AS lion;
SELECT count(*) AS keys, count(*) FILTER (WHERE f.count IS DISTINCT FROM g.count) AS differ
  FROM lion_index_facets('lk_ch_c') f
  FULL JOIN (SELECT c, count(*) FROM lk_ch GROUP BY c) g ON coalesce(rtrim(f.key), '-') = coalesce(rtrim(g.c), '-');
SELECT lion_index_verify('lk_ch_c', true);

-- ---------------------------------------------------------------------
-- numeric of huge precision, citext, and whole jsonb documents
-- ---------------------------------------------------------------------
CREATE TABLE lk_n (id int, n numeric) WITH (autovacuum_enabled = off);
INSERT INTO lk_n SELECT i, (i % 50)::numeric FROM generate_series(1, 2000) i;
INSERT INTO lk_n VALUES (5000, ('1' || repeat('7', 5000))::numeric), (5001, ('1' || repeat('7', 5000) || '.000')::numeric),
						(5002, ('1' || repeat('7', 4999) || '8')::numeric), (5003, ('0.' || repeat('1', 5000))::numeric);
CREATE INDEX lk_n_n ON lk_n USING lion (n);
VACUUM (FREEZE, ANALYZE) lk_n;
TRUNCATE lk_q;
INSERT INTO lk_q VALUES
	($$SELECT id FROM lk_n WHERE n = ('1' || repeat('7', 5000))::numeric ORDER BY id$$),
	($$SELECT id FROM lk_n WHERE n > 49 ORDER BY id$$),
	($$SELECT id FROM lk_n WHERE n < 1 ORDER BY id$$),
	($$SELECT count(*) FROM lk_n WHERE n = ('1' || repeat('7', 5000) || '.0')::numeric$$),
	($$SELECT md5(n::text), count(*) FROM lk_n WHERE n > 40 GROUP BY n ORDER BY 1$$),
	($$SELECT count(DISTINCT n) FROM lk_n$$),
	($$SELECT md5(max(n)::text), md5(min(n)::text) FROM lk_n$$),
	($$SELECT id FROM lk_n ORDER BY n DESC, id LIMIT 6$$);
SELECT lk_same(q) FROM lk_q;
SELECT lion_index_verify('lk_n_n', true);

CREATE TABLE lk_ci (id int, s citext) WITH (autovacuum_enabled = off);
INSERT INTO lk_ci SELECT i, 'name' || (i % 100) FROM generate_series(1, 2000) i;
INSERT INTO lk_ci VALUES (5000, repeat('AbC', 1000)), (5001, repeat('abc', 1000)), (5002, repeat('ABC', 1000) || 'x'),
						 (5003, lk_rand(5, 2043)), (5004, upper(lk_rand(5, 2043)));
CREATE INDEX lk_ci_s ON lk_ci USING lion (s);
VACUUM (FREEZE, ANALYZE) lk_ci;
TRUNCATE lk_q;
INSERT INTO lk_q VALUES
	($$SELECT id FROM lk_ci WHERE s = repeat('aBc', 1000)::citext ORDER BY id$$),
	($$SELECT id FROM lk_ci WHERE s = lower(lk_rand(5, 2043))::citext ORDER BY id$$),
	($$SELECT id FROM lk_ci WHERE s > 'name99' ORDER BY id$$),
	($$SELECT count(*) FROM lk_ci WHERE s IN (repeat('ABC', 1000)::citext, 'NAME7')$$),
	($$SELECT count(*), count(DISTINCT s) FROM lk_ci WHERE s > 'n'$$),
	($$SELECT lower(md5(lower(s::text))), count(*) FROM lk_ci GROUP BY s ORDER BY 2 DESC, 1 LIMIT 5$$),
	($$SELECT md5(lower(min(s)::text)), md5(lower(max(s)::text)) FROM lk_ci$$),
	($$SELECT id FROM lk_ci WHERE s >= 'name98' ORDER BY s, id LIMIT 50$$);
SELECT lk_same(q) FROM lk_q;
SELECT lion_index_verify('lk_ci_s', true);

CREATE TABLE lk_j (id int, d jsonb) WITH (autovacuum_enabled = off);
INSERT INTO lk_j SELECT i, jsonb_build_object('k', i % 40) FROM generate_series(1, 2000) i;
INSERT INTO lk_j VALUES (5000, jsonb_build_object('a', repeat('x', 2100))), (5001, jsonb_build_object('a', repeat('x', 2100))),
						(5002, (SELECT jsonb_agg(g) FROM generate_series(1, 300) g)),
						(5003, (SELECT jsonb_object_agg('key' || g, repeat('v', g % 50)) FROM generate_series(1, 200) g));
CREATE INDEX lk_j_d ON lk_j USING lion (d jsonb_ops);
VACUUM (FREEZE, ANALYZE) lk_j;
TRUNCATE lk_q;
INSERT INTO lk_q VALUES
	($$SELECT id FROM lk_j WHERE d = jsonb_build_object('a', repeat('x', 2100)) ORDER BY id$$),
	($$SELECT id FROM lk_j WHERE d = (SELECT jsonb_agg(g) FROM generate_series(1, 300) g) ORDER BY id$$),
	($$SELECT id FROM lk_j WHERE d > '{"k": 39}' ORDER BY id$$),
	($$SELECT count(*) FROM lk_j WHERE d = jsonb_build_object('a', repeat('x', 2100))$$),
	($$SELECT count(DISTINCT d) FROM lk_j$$),
	($$SELECT md5(d::text), count(*) FROM lk_j GROUP BY d ORDER BY 2 DESC, 1 LIMIT 5$$),
	($$SELECT id FROM lk_j ORDER BY d DESC, id LIMIT 6$$);
SELECT lk_same(q) FROM lk_q;
SELECT lion_index_verify('lk_j_d', true);

-- ---------------------------------------------------------------------
-- array elements: a long text, varchar or bytea one is hashed and
-- rechecked, a long element of another type is compressed
-- ---------------------------------------------------------------------
CREATE TABLE lk_w (name text PRIMARY KEY, w text);
INSERT INTO lk_w SELECT 'b' || n, repeat('b', n)
	FROM unnest(ARRAY[2042, 2043, 2044, 2045, 2700, 100000]) n;
INSERT INTO lk_w SELECT 'c' || k, repeat('c', 2030) || lpad(k::text, 15, '0') FROM generate_series(1, 3) k;
-- hx: c1's key spelled out as an element of its own
INSERT INTO lk_w SELECT 'hx', left(w, 1978) || '#' ||
	encode(sha256(convert_to(w, current_setting('server_encoding'))), 'hex')
	FROM lk_w WHERE name = 'c1';
INSERT INTO lk_w VALUES ('m682', repeat('中', 682)), ('r', lk_rand(9, 2600));
SELECT name, octet_length(w) FROM lk_w ORDER BY name;

CREATE TABLE lk_a (id int, a text[] COLLATE "C", v varchar[] COLLATE "C", b bytea[], n numeric[]) WITH (autovacuum_enabled = off);
INSERT INTO lk_a SELECT i,
	ARRAY(SELECT CASE WHEN (i * 31 + p * 17) % 5 < 2 THEN (SELECT w FROM lk_w ORDER BY name OFFSET (i * 7 + p) % 13 LIMIT 1)
					  ELSE 'e' || ((i + p) % 9) END FROM generate_series(1, 1 + i % 4) p),
	NULL, NULL, NULL
	FROM generate_series(1, 1500) i;
UPDATE lk_a SET a = a || NULL::text WHERE id % 50 = 0;
UPDATE lk_a SET a = '{}' WHERE id % 97 = 0;
UPDATE lk_a SET v = a::varchar[], b = ARRAY(SELECT convert_to(x, 'UTF8') FROM unnest(a) x),
				n = ARRAY[(i % 3)::numeric, ('1' || repeat('7', 5000 + i % 3))::numeric]
	FROM generate_series(1, 1500) i WHERE id = i;
CREATE TABLE lk_ag AS SELECT * FROM lk_a;
CREATE INDEX lk_ag_a ON lk_ag USING gin (a);
CREATE INDEX lk_a_a ON lk_a USING lion (a);
CREATE INDEX lk_a_v ON lk_a USING lion (v);
CREATE INDEX lk_a_b ON lk_a USING lion (b);
CREATE INDEX lk_a_n ON lk_a USING lion (n);
VACUUM (FREEZE, ANALYZE) lk_a, lk_ag;

-- the elements named by template, {name} for a word of lk_w
CREATE FUNCTION lk_sub(t text) RETURNS text[] LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
	FOR r IN SELECT name, w FROM lk_w ORDER BY length(name) DESC LOOP
		t := replace(t, '{' || r.name || '}', '"' || r.w || '"');
	END LOOP;
	RETURN t::text[];
END $$;
CREATE TABLE lk_aq (op text, t text);
INSERT INTO lk_aq SELECT '@>', '{{' || name || '}}' FROM lk_w;
INSERT INTO lk_aq SELECT '&&', '{{' || name || '},e1}' FROM lk_w;
INSERT INTO lk_aq VALUES ('@>', '{{c1},{hx}}'), ('@>', '{{b2044},e2}'), ('&&', '{{c2},{c3},{b2043}}'),
	('<@', '{{b2044},{b2045},e1,e2,e3}'), ('@>', '{{b100000},{r}}'), ('&&', '{{m682},e0}');
CREATE FUNCTION lk_acheck() RETURNS TABLE (q text, col text, how text, result text) LANGUAGE plpgsql AS $$
DECLARE r record; c text; ref int[]; got int[]; s text; arg text;
BEGIN
	FOR r IN SELECT * FROM lk_aq LOOP
		PERFORM set_config('enable_seqscan', 'off', true);
		EXECUTE format('SELECT array_agg(id ORDER BY id) FROM lk_ag WHERE a %s %L', r.op, lk_sub(r.t)) INTO ref;
		PERFORM set_config('enable_seqscan', 'on', true);
		FOREACH c IN ARRAY ARRAY['a', 'v', 'b'] LOOP
			arg := CASE c WHEN 'a' THEN quote_literal(lk_sub(r.t)) || '::text[]'
						  WHEN 'v' THEN quote_literal(lk_sub(r.t)) || '::varchar[]'
						  ELSE format('ARRAY(SELECT convert_to(x, %L) FROM unnest(%L::text[]) x)', 'UTF8', lk_sub(r.t)) END;
			s := format('SELECT id FROM lk_a WHERE %s %s %s ORDER BY id', c, r.op, arg);
			q := r.op || ' ' || r.t; col := c;
			how := lk_same(s);
			EXECUTE format('SELECT array_agg(id ORDER BY id) FROM (%s) x', s) INTO got;
			result := CASE WHEN how NOT LIKE '%same%' THEN 'DIFFER from seq'
						   WHEN got IS DISTINCT FROM ref THEN 'DIFFER from gin' ELSE 'ok' END;
			IF result <> 'ok' OR how NOT LIKE 'Bitmap%' THEN RETURN NEXT; END IF;
			how := lk_same(format('SELECT count(*) FROM lk_a WHERE %s %s %s', c, r.op, arg));
			IF how NOT LIKE '%same%' THEN result := 'count DIFFER'; RETURN NEXT; END IF;
		END LOOP;
	END LOOP;
END $$;
SELECT * FROM lk_acheck();
-- a long element is rechecked, a short one answered from the keys
SELECT lk_same(format('SELECT count(*) FROM lk_a WHERE a @> %L', lk_sub('{e1}')));
SELECT lk_same(format('SELECT count(*) FROM lk_a WHERE a @> %L', lk_sub('{{b2042}}')));
SELECT lk_same(format('SELECT count(*) FROM lk_a WHERE a @> %L', lk_sub('{{b2043}}')));
-- numeric elements of 5000 digits, compressed
SELECT lk_same($$SELECT id FROM lk_a WHERE n @> ARRAY[('1' || repeat('7', 5001))::numeric] ORDER BY id$$);
SELECT lk_same($$SELECT count(*) FROM lk_a WHERE n && ARRAY[('1' || repeat('7', 5002))::numeric, 7]$$);
SELECT lion_index_verify('lk_a_a', true), lion_index_verify('lk_a_v', true),
	   lion_index_verify('lk_a_b', true), lion_index_verify('lk_a_n', true);

-- facets show each element, the long ones and hx too, with its own rows
CREATE FUNCTION lk_facets_same(idx text, k int, filter text, ref text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text[]; b text[];
BEGIN
	SELECT array_agg(md5(key) || ':' || count ORDER BY count DESC, key COLLATE "C") INTO a
	  FROM lion_index_facets(idx::regclass, k, filter);
	EXECUTE format('SELECT array_agg(md5(e) || '':'' || c ORDER BY c DESC, e COLLATE "C") FROM (%s) x', ref) INTO b;
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same' ELSE 'DIFFER' END || ' (' || coalesce(cardinality(b), 0) || ' keys)';
END $$;
SELECT lk_facets_same('lk_a_a', NULL, NULL,
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a) e WHERE e IS NOT NULL GROUP BY e$$);
SELECT lk_facets_same('lk_a_a', 8, NULL,
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a) e WHERE e IS NOT NULL GROUP BY e ORDER BY 2 DESC, e COLLATE "C" LIMIT 8$$);
SELECT lk_facets_same('lk_a_a', NULL, format('a && %L', lk_sub('{{b2042},e3}')),
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a) e WHERE e IS NOT NULL AND a && lk_sub('{{b2042},e3}') GROUP BY e$$);
SELECT lk_facets_same('lk_a_b', NULL, NULL,
	$$SELECT e::text AS e, count(DISTINCT ctid) AS c FROM lk_a, unnest(b) e WHERE e IS NOT NULL GROUP BY e$$);
SELECT octet_length(key), count FROM lion_index_facets('lk_a_a') WHERE octet_length(key) > 2042 ORDER BY 1, 2;
-- the filter answered by another lion index: many rows, so the keys are
-- counted, and a few, so the rows are read
ALTER TABLE lk_a ADD COLUMN grp int;
UPDATE lk_a SET grp = id % 3;
CREATE INDEX lk_a_grp ON lk_a USING lion (grp);
CREATE INDEX lk_a_id ON lk_a USING lion (id);
VACUUM (FREEZE, ANALYZE) lk_a;
SELECT lk_facets_same('lk_a_a', NULL, 'grp = 1',
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a) e WHERE e IS NOT NULL AND grp = 1 GROUP BY e$$);
SELECT lk_facets_same('lk_a_a', 5, 'grp = 2',
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a) e WHERE e IS NOT NULL AND grp = 2 GROUP BY e ORDER BY 2 DESC, e COLLATE "C" LIMIT 5$$);
SELECT lk_facets_same('lk_a_a', NULL, 'id = 7 OR id = 77 OR id = 777',
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a) e WHERE e IS NOT NULL AND id IN (7, 77, 777) GROUP BY e$$);
-- an expression index, whose value is the expression's
CREATE INDEX lk_a_x ON lk_a USING lion ((a || ARRAY[repeat('z', 2050)]));
SELECT lk_facets_same('lk_a_x', 4, NULL,
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a || ARRAY[repeat('z', 2050)]) e WHERE e IS NOT NULL GROUP BY e ORDER BY 2 DESC, e COLLATE "C" LIMIT 4$$);

-- writes
BEGIN;
INSERT INTO lk_a (id, a) SELECT 10000 + i, ARRAY[repeat('b', 2044 + i % 3), 'e1'] FROM generate_series(1, 30) i;
INSERT INTO lk_ag (id, a) SELECT 10000 + i, ARRAY[repeat('b', 2044 + i % 3), 'e1'] FROM generate_series(1, 30) i;
UPDATE lk_a SET v = a::varchar[], b = ARRAY(SELECT convert_to(x, 'UTF8') FROM unnest(a) x) WHERE id > 10000;
SELECT * FROM lk_acheck();
COMMIT;
DELETE FROM lk_a WHERE id % 11 = 0; DELETE FROM lk_ag WHERE id % 11 = 0;
SELECT * FROM lk_acheck();
VACUUM lk_a;
SELECT * FROM lk_acheck();
SELECT lk_facets_same('lk_a_a', NULL, NULL,
	$$SELECT e, count(DISTINCT ctid) AS c FROM lk_a, unnest(a) e WHERE e IS NOT NULL GROUP BY e$$);
SELECT lion_index_verify('lk_a_a', true), lion_index_verify('lk_a_v', true),
	   lion_index_verify('lk_a_b', true), lion_index_verify('lk_a_n', true);

-- an element of another type that does not fit compressed is an error, as
-- a GIN index tuple too long is
CREATE TABLE lk_an (n numeric[]);
CREATE INDEX lk_an_n ON lk_an USING lion (n);
INSERT INTO lk_an VALUES (ARRAY[('0.' || (SELECT string_agg(lpad((((g * 7919) % 10000))::text, 4, '0'), '') FROM generate_series(1, 1100) g))::numeric]);
DROP TABLE lk_an;

-- ---------------------------------------------------------------------
-- facets of a tsvector: each long lexeme, and a 2043-byte one spelled like
-- another's hashed key, with ts_stat()'s ndoc
-- ---------------------------------------------------------------------
CREATE TABLE lk_t (id int, tsv tsvector) WITH (autovacuum_enabled = off);
INSERT INTO lk_t SELECT i, array_to_tsvector(ARRAY(SELECT w FROM lk_w WHERE name IN ('b2043', 'b2044', 'c1', 'c2', 'hx', 'm682')
											   ORDER BY name OFFSET i % 5 LIMIT 1 + i % 3) || ('t' || i % 4))
	FROM generate_series(1, 400) i;
CREATE INDEX lk_t_tsv ON lk_t USING lion (tsv);
VACUUM (FREEZE, ANALYZE) lk_t;
SELECT lk_facets_same('lk_t_tsv', NULL, NULL, $$SELECT word AS e, ndoc AS c FROM ts_stat('SELECT tsv FROM lk_t')$$);
SELECT lk_facets_same('lk_t_tsv', 3, NULL, $$SELECT word AS e, ndoc AS c FROM ts_stat('SELECT tsv FROM lk_t') ORDER BY 2 DESC, word COLLATE "C" LIMIT 3$$);
SELECT lk_facets_same('lk_t_tsv', NULL, 'tsv @@ ''t1''', $$SELECT word AS e, ndoc AS c FROM ts_stat('SELECT tsv FROM lk_t WHERE tsv @@ ''t1''')$$);
SELECT octet_length(key), count FROM lion_index_facets('lk_t_tsv') ORDER BY 1, 2;

DROP TABLE lk, lk4, lk_q, lk_ch, lk_n, lk_ci, lk_j, lk_w, lk_a, lk_ag, lk_aq, lk_t;
DROP FUNCTION lk_same(text), lk_lz4(), lk_rand(int, int), lk_sub(text), lk_acheck(), lk_facets_same(text, int, text, text);
DROP EXTENSION pg_lion_citext;
DROP EXTENSION citext;
