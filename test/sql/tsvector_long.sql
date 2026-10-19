-- Long lexemes (DESIGN.md §17 "Long lexemes"): a tsvector lexeme can be up to
-- 2047 bytes, more than a lion key holds whole.  One of up to 2043 bytes is
-- its own key; a longer one is filed under its first bytes and SHA-256, a key
-- of exactly 2043 bytes, and a query naming a key of that length is
-- rechecked.  Every way lion answers must return the rows a sequential scan
-- and GIN return: long words of every length around the limit, words sharing
-- a long beginning, multibyte ones, and a 2043-byte word spelled like another
-- one's hashed key; in AND, OR, NOT, phrases, weights and prefixes; before and
-- after writes.  Only differences print, and the words are named in braces.

\set VERBOSITY terse
SET client_min_messages = warning;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
CREATE EXTENSION IF NOT EXISTS pg_lion;
SELECT setseed(0.29) IS NULL AS seeded;

CREATE TABLE tl_w (name text PRIMARY KEY, w text);
INSERT INTO tl_w SELECT 'b' || n, repeat('b', n)
	FROM unnest(ARRAY[1990, 2000, 2042, 2043, 2044, 2045, 2046, 2047]) n;
-- one long beginning: c0 is 2043 bytes, c1 .. c4 are hashed under the same
-- first bytes, c5 is short enough to be its own key
INSERT INTO tl_w SELECT 'c' || k, repeat('c', 2030) || lpad(k::text, 15, '0')
	FROM generate_series(1, 4) k;
INSERT INTO tl_w VALUES ('c0', repeat('c', 2030) || repeat('0', 13)),
						('c5', repeat('c', 2030) || '0000000005');
-- hx: c1's key spelled out as a word of its own
INSERT INTO tl_w SELECT 'hx', left(w, 1978) || '#' ||
	encode(sha256(convert_to(w, current_setting('server_encoding'))), 'hex')
	FROM tl_w WHERE name = 'c1';
-- multibyte: 2043 bytes, two hashed ones sharing their first characters, and
-- one whose first bytes end inside a character unless clipped
INSERT INTO tl_w VALUES ('m681', repeat('中', 681)), ('m682', repeat('中', 682)),
						('m682e', repeat('中', 681) || 'é'), ('qm', 'q' || repeat('中', 682));
SELECT name, octet_length(w) FROM tl_w ORDER BY name;

-- a lexeme of 2047 bytes is longer than tsvector and tsquery text input take,
-- but a dictionary can return one and binary input takes it: the rows, and
-- the queries naming one, arrive through binary COPY
CREATE FUNCTION tl_copy_in(tab text, cols text, src text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('COPY (%s) TO PROGRAM %L (FORMAT binary)', src, 'cat > pg_lion_tsvector_long.copy');
	EXECUTE format('COPY %I (%s) FROM PROGRAM %L (FORMAT binary)', tab, cols,
				   'cat pg_lion_tsvector_long.copy && rm pg_lion_tsvector_long.copy');
END $$;
-- a tsvector in binary: lexeme i at position i with weight wt[i] (3 is A),
-- or none if wt[i] is NULL
CREATE FUNCTION tl_send(lex text[], wt int[]) RETURNS bytea LANGUAGE sql AS $$
SELECT int4send(count(*)::int) ||
	   coalesce(string_agg(convert_to(l, current_setting('server_encoding')) || '\x00'::bytea ||
						   int2send(n::int2) || ps, ''::bytea ORDER BY l COLLATE "C"), '')
  FROM (SELECT l, count(w) AS n,
			   coalesce(string_agg(substring(int4send((w << 14) | p::int) FROM 3), ''::bytea ORDER BY p), '') AS ps
		  FROM unnest(lex, wt) WITH ORDINALITY u(l, w, p) GROUP BY l) x $$;
CREATE TABLE tl_wq (name text, wq tsquery);
SELECT tl_copy_in('tl_wq', 'name, wq', $$SELECT name, int4send(1) || '\x010000'::bytea ||
	convert_to(w, current_setting('server_encoding')) || '\x00'::bytea FROM tl_w WHERE octet_length(w) > 2046$$);
SELECT name, octet_length(wq::text) - 2 AS bytes FROM tl_wq ORDER BY name;

-- a template with {name} for a word and {name:n} for its first n characters;
-- a 2047-byte word stands in as tlph<name> until the tsquery is built
CREATE FUNCTION tl_sub(t text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r record; m text[];
BEGIN
	FOR m IN SELECT regexp_matches(t, '\{(\w+):(\d+)\}', 'g') LOOP
		t := replace(t, '{' || m[1] || ':' || m[2] || '}',
					 (SELECT left(w, m[2]::int) FROM tl_w WHERE name = m[1]));
	END LOOP;
	FOR r IN SELECT name, w FROM tl_w ORDER BY length(name) DESC LOOP
		t := replace(t, '{' || r.name || '}',
					 CASE WHEN octet_length(r.w) > 2046 THEN 'tlph' || r.name ELSE r.w END);
	END LOOP;
	RETURN t;
END $$;
CREATE FUNCTION tl_q(t text) RETURNS tsquery LANGUAGE plpgsql AS $$
DECLARE q tsquery := tl_sub(t)::tsquery; r record;
BEGIN
	FOR r IN SELECT name, wq FROM tl_wq LOOP
		q := ts_rewrite(q, ('tlph' || r.name)::tsquery, r.wq);
	END LOOP;
	RETURN q;
END $$;

-- one to six words a row, two in five of them long
CREATE TABLE tl (id int, tsv tsvector) WITH (autovacuum_enabled = off);
SELECT tl_copy_in('tl', 'id, tsv', $$
	WITH lw AS (SELECT array_agg(w ORDER BY name) AS ws FROM tl_w),
	occ AS (SELECT i, p, CASE WHEN random() < 0.4 THEN lw.ws[1 + (random() * (cardinality(lw.ws) - 1))::int]
							  ELSE 'w' || (random() * 9)::int END AS lex,
				   (random() * 3)::int AS wt
			  FROM generate_series(1, 3000) i, generate_series(1, 6) p, lw WHERE p <= 1 + i % 6)
	SELECT i, CASE WHEN i % 101 = 0 THEN NULL
				   WHEN i % 97 = 0 THEN tl_send('{}', '{}')
				   ELSE tl_send(array_agg(lex ORDER BY p), array_agg(CASE WHEN i % 13 <> 0 THEN wt END ORDER BY p)) END
	  FROM occ GROUP BY i ORDER BY i$$);
CREATE TABLE tl_g WITH (autovacuum_enabled = off) AS SELECT * FROM tl; CREATE INDEX tl_g_i ON tl_g USING gin (tsv);
CREATE TABLE tl_l WITH (autovacuum_enabled = off) AS SELECT * FROM tl; CREATE INDEX tl_l_i ON tl_l USING lion (tsv) WITH (store_positions = false);
CREATE TABLE tl_lp WITH (autovacuum_enabled = off) AS SELECT * FROM tl; CREATE INDEX tl_lp_i ON tl_lp USING lion (tsv) WITH (store_positions = true);
VACUUM (FREEZE, ANALYZE) tl, tl_g, tl_l, tl_lp;
-- every long word is in some rows
SELECT count(*) FROM tl_w WHERE NOT EXISTS (SELECT FROM tl WHERE tsv @@ tl_q('{' || name || '}'));

CREATE FUNCTION tl_ids(tab text, q tsquery, mode text) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
	PERFORM set_config('enable_seqscan', CASE WHEN mode = 'seq' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN mode IN ('bitmap','count') THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_indexscan', CASE WHEN mode IN ('index','count','literal') THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_indexonlyscan', CASE WHEN mode IN ('count','literal') THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('pg_lion.enable_plain_scan', (mode <> 'literal')::text, true);
	IF mode = 'count' THEN
		EXECUTE format('SELECT ARRAY[count(*)::int] FROM %I WHERE tsv @@ $1', tab) INTO r USING q;
	ELSIF mode = 'literal' THEN
		-- a literal, so the planner sees the query (a count pushed down as KEYS)
		EXECUTE format('SELECT ARRAY[count(*)::int] FROM %I WHERE tsv @@ %L::tsquery', tab, q) INTO r;
	ELSE
		EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM %I WHERE tsv @@ $1', tab) INTO r USING q;
	END IF;
	RETURN r;
END $$;

-- each template's rows from the sequential scan against GIN's and every way
-- lion has; the template, not the query, prints.  A query no literal can
-- spell (a 2047-byte operand) only ever arrives as a parameter.
CREATE FUNCTION tl_check(qs text[]) RETURNS TABLE (q text, path text, got int, want int) LANGUAGE plpgsql AS $$
DECLARE qq text; tq tsquery; spelt bool; ref int[]; t text; m text; g int[];
BEGIN
	FOREACH qq IN ARRAY qs LOOP
		tq := tl_q(qq);
		BEGIN
			spelt := tq::text::tsquery IS NOT NULL;
		EXCEPTION WHEN others THEN
			spelt := false;
		END;
		ref := tl_ids('tl', tq, 'seq');
		FOREACH t IN ARRAY ARRAY['tl_g','tl_l','tl_lp'] LOOP
			FOREACH m IN ARRAY CASE WHEN t = 'tl_g' THEN ARRAY['bitmap']
									WHEN spelt THEN ARRAY['bitmap','index','count','literal']
									ELSE ARRAY['bitmap','index','count'] END LOOP
				BEGIN
					g := tl_ids(t, tq, m);
				EXCEPTION WHEN others THEN
					q := qq; path := t || '/' || m || ' ERROR ' || left(SQLERRM, 60); got := -1; want := cardinality(ref);
					RETURN NEXT; CONTINUE;
				END;
				IF (m IN ('count','literal') AND g[1] <> cardinality(ref)) OR
				   (m NOT IN ('count','literal') AND g IS DISTINCT FROM ref) THEN
					q := qq; path := t || '/' || m;
					got := CASE WHEN m IN ('count','literal') THEN g[1] ELSE cardinality(g) END;
					want := cardinality(ref);
					RETURN NEXT;
				END IF;
			END LOOP;
		END LOOP;
	END LOOP;
END $$;

CREATE TABLE tl_queries (q text);
INSERT INTO tl_queries SELECT '{' || name || '}' FROM tl_w;
INSERT INTO tl_queries VALUES
	('{c1} & {c2}'), ('{c1} | {c2}'), ('{c1} & !{c2}'), ('!{c1}'), ('!{hx}'), ('{hx} & !{c1}'),
	('{c1} & {hx}'), ('{c1} | w1'), ('{b2046} & w2'), ('!({b2044} | w3)'), ('{c0} | {b2043}'),
	('{m682} & !{m682e}'), ('{b2042} & {b2047}'), ('w1 & !{b2045}'), ('{qm} | {b2047} | {c4}'),
	('!{b2047} & w5'),
	-- phrases and weights
	('{c1} <-> w1'), ('w1 <-> {b2046}'), ('{c1} <2> {c3}'), ('{hx} <-> w2'), ('{b2043} <-> !w1'),
	('!{c1} <-> w1'), ('{m682} <-> {qm}'), ('({c2} | {hx}) <-> w4'), ('{b2047} <-> w3'),
	('{c1}:A'), ('{b2046}:AB'), ('{hx}:C'), ('{m682}:D & w1'), ('{c0}:B | {c4}:C'),
	-- prefixes, at and past the longest every hashed key keeps whole
	('{b2047:1970}:*'), ('{b2047:1976}:*'), ('{b2047:1977}:*'), ('{b2047:1978}:*'),
	('{b2047:1979}:*'), ('{b2047:2000}:*'), ('{b2047:2043}:*'), ('{b2047:2046}:*'),
	('{c1:2030}:*'), ('{c1:2044}:*'), ('{c1:1978}:*'), ('{hx:1979}:*'), ('{hx:2000}:*'),
	('{m682:600}:*'), ('{m682:659}:*'), ('{m682:660}:*'), ('{qm:659}:*'), ('{qm:660}:*'),
	('{c1:1970}:*A'), ('{c1:1970}:* <-> w1'), ('w2 <-> {b2047:1900}:*'), ('{c1:1970}:* | w1'),
	('!{b2047:1970}:*'), ('{b2047:1970}:* & !{c1:1970}:*');
SELECT * FROM tl_check(ARRAY(SELECT q FROM tl_queries));

-- the plans: a word of up to 2042 bytes is answered from the index alone, one
-- with a key of 2043 bytes is rechecked
CREATE FUNCTION tl_plan(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; node text := 'other'; rechecked text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
		IF l ~ 'LionCount' THEN node := 'LionCount'; END IF;
		IF l ~ 'Heap TIDs Rechecked: [1-9]' THEN rechecked := ', rechecked'; END IF;
	END LOOP;
	RETURN node || rechecked;
END $$;
SET enable_seqscan = off;
SELECT tl_plan(format('SELECT count(*) FROM tl_l WHERE tsv @@ %L', tl_sub('{b2042}')));
SELECT tl_plan(format('SELECT count(*) FROM tl_l WHERE tsv @@ %L', tl_sub('{b2043}')));
SELECT tl_plan(format('SELECT count(*) FROM tl_l WHERE tsv @@ %L', tl_sub('{b2046}')));
SELECT tl_plan(format('SELECT count(*) FROM tl_lp WHERE tsv @@ %L', tl_sub('{c5} <-> w1')));
SELECT tl_plan(format('SELECT count(*) FROM tl_lp WHERE tsv @@ %L', tl_sub('{c1} <-> w1')));
RESET enable_seqscan;

-- BM25 over the hashed words: the scores SQL computes from the rows, each
-- lexeme filed under its key as the index files it (tl_key).  hx and c1 share
-- one, which scores as one word: the first of them in the row's order.
CREATE FUNCTION tl_key(l text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n int := least(length(l), 1978);
BEGIN
	IF octet_length(l) <= 2043 THEN RETURN l; END IF;
	WHILE octet_length(left(l, n)) > 1978 LOOP n := n - 1; END LOOP;
	RETURN rpad(left(l, n), n + 1979 - octet_length(left(l, n)), '#') ||
		   encode(sha256(convert_to(l, current_setting('server_encoding'))), 'hex');
END $$;
CREATE FUNCTION tl_bm_ref(q text[], k int) RETURNS TABLE (id int, score float8) LANGUAGE sql AS $$
WITH lex AS (SELECT t.id, t.ctid AS tid, u.lexeme, tl_key(u.lexeme) AS key,
					greatest(coalesce(array_length(u.positions, 1), 0), 1)::float8 AS tf
			   FROM tl_lp t, unnest(t.tsv) u),
len AS (SELECT lex.id, sum(tf) AS dl FROM lex GROUP BY lex.id),
occ AS (SELECT DISTINCT ON (lex.id, key) lex.id, tid, key, tf FROM lex ORDER BY lex.id, key, lexeme COLLATE "C"),
st AS (SELECT count(*)::float8 AS n, avg(dl) AS avgdl FROM len),
df AS (SELECT key, count(*)::float8 AS df FROM occ WHERE key IN (SELECT tl_key(x) FROM unnest(q) x) GROUP BY key)
SELECT occ.id, sum(ln(1 + (st.n - df.df + 0.5) / (df.df + 0.5)) * occ.tf * 2.2 /
				   (occ.tf + 1.2 * (0.25 + 0.75 * len.dl / st.avgdl))) AS s
  FROM occ JOIN df USING (key) JOIN len USING (id), st
 GROUP BY occ.id, occ.tid ORDER BY s DESC, occ.tid LIMIT k $$;
-- lex: the query's words, by name or as themselves
CREATE FUNCTION tl_bm_same(qt text, lex text[], k int) RETURNS text LANGUAGE sql AS $$
WITH l AS (SELECT row_number() OVER (ORDER BY s.score DESC, t.ctid) AS r, t.id, round(s.score::numeric, 9) AS sc
			 FROM lion_bm25('tl_lp_i', tl_q(qt), k) s JOIN tl_lp t ON t.ctid = s.ctid),
	 f AS (SELECT row_number() OVER () AS r, id, round(score::numeric, 9) AS sc
			 FROM tl_bm_ref(ARRAY(SELECT coalesce((SELECT w FROM tl_w WHERE name = x), x) FROM unnest(lex) x), k))
SELECT count(*) || ' rows, ' ||
	   count(*) FILTER (WHERE l.id IS DISTINCT FROM f.id OR l.sc IS DISTINCT FROM f.sc) || ' differ'
  FROM l FULL JOIN f USING (r) $$;
SELECT tl_bm_same('{b2046}', '{b2046}', 20);
SELECT tl_bm_same('{c2} | {c3}', '{c2,c3}', 50);
SELECT tl_bm_same('{b2044} | w1 | {m682}', '{b2044,w1,m682}', 100);
SELECT tl_bm_same('{b2043} | {c0} | {m681}', '{b2043,c0,m681}', 40);
SELECT tl_bm_same('{b2047} | {qm} & w2', '{b2047,qm,w2}', 60);
SELECT tl_bm_same('{c1} | {hx} | w4', '{c1,hx,w4}', 80);
-- one row's score agrees with the ranking, the shared key included
SELECT count(*) AS rows, count(*) FILTER (WHERE abs(s.score - lion_bm25_score(t.tsv, q, 'tl_lp_i')) > 1e-9) AS differ
  FROM (SELECT tl_q('{c1} | {c2} | {hx} | w3 | {b2047}') AS q) x,
	   lion_bm25('tl_lp_i', x.q, 300) s JOIN tl_lp t ON t.ctid = s.ctid;
-- and the ranked scan returns what scoring and sorting every match returns
CREATE FUNCTION tl_bm_scan(qt text, k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE q tsquery := tl_q(qt); sql text; l text; scanned bool := false; a int[]; b int[];
BEGIN
	sql := format('SELECT id FROM tl_lp WHERE tsv @@ %L ORDER BY lion_bm25_score(tsv, %L, ''tl_lp_i'') DESC, ctid LIMIT %s', q, q, k);
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL pg_lion.enable_plain_scan = off;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l LIKE '%LionBm25%' THEN scanned := true; END IF;
	END LOOP;
	EXECUTE format('SELECT array_agg(id) FROM (%s) x', sql) INTO a;
	RESET enable_seqscan; RESET enable_bitmapscan; RESET pg_lion.enable_plain_scan;
	SET LOCAL pg_lion.enable_bm25_scan = off;
	EXECUTE format('SELECT array_agg(id) FROM (%s) x', sql) INTO b;
	RETURN CASE WHEN NOT scanned THEN 'not scanned' WHEN a IS DISTINCT FROM b THEN 'differ' ELSE 'same' END;
END $$;
SELECT tl_bm_scan('{c1} | {c2}', 10);
SELECT tl_bm_scan('{hx} | w2', 25);
SELECT tl_bm_scan('{b2046} & w1', 10);

-- writes: new long words in this transaction and committed, deleted and
-- updated rows, not vacuumed
BEGIN;
INSERT INTO tl_w VALUES ('n1', repeat('n', 2046)), ('n2', repeat('n', 2046) || 'o');
SELECT tl_copy_in('tl_wq', 'name, wq', $$SELECT name, int4send(1) || '\x010000'::bytea ||
	convert_to(w, current_setting('server_encoding')) || '\x00'::bytea FROM tl_w WHERE name = 'n2'$$);
CREATE TEMP TABLE tl_new (id int, tsv tsvector);
SELECT tl_copy_in('tl_new', 'id, tsv', $$SELECT 100000 + i,
	tl_send(ARRAY(SELECT w FROM tl_w WHERE name = 'n' || (1 + i % 2)) || ('w' || i % 3) ||
			ARRAY(SELECT w FROM tl_w WHERE name = 'c' || i % 5), '{0,0,2}')
	FROM generate_series(1, 40) i$$);
INSERT INTO tl SELECT * FROM tl_new; INSERT INTO tl_g SELECT * FROM tl_new;
INSERT INTO tl_l SELECT * FROM tl_new; INSERT INTO tl_lp SELECT * FROM tl_new;
SELECT * FROM tl_check(ARRAY['{n1}', '{n2}', '{n1} | {n2}', '{n1} <-> w1', '{n2} <-> w2', '{n1:1970}:*',
							 '{c3}:B & {n2}', '{n2:2046}:*']);
COMMIT;
UPDATE tl SET tsv = tsv || tl_sub('{n1}:30A')::tsvector WHERE id % 7 = 0;
UPDATE tl_g SET tsv = tsv || tl_sub('{n1}:30A')::tsvector WHERE id % 7 = 0;
UPDATE tl_l SET tsv = tsv || tl_sub('{n1}:30A')::tsvector WHERE id % 7 = 0;
UPDATE tl_lp SET tsv = tsv || tl_sub('{n1}:30A')::tsvector WHERE id % 7 = 0;
DELETE FROM tl WHERE id % 11 = 0; DELETE FROM tl_g WHERE id % 11 = 0;
DELETE FROM tl_l WHERE id % 11 = 0; DELETE FROM tl_lp WHERE id % 11 = 0;
SELECT * FROM tl_check(ARRAY(SELECT q FROM tl_queries) ||
					   ARRAY['{n1}', '{n2}', '{n1}:A', '{n1} & {hx}', '{n2} | {b2047}', '{n1:2000}:*']);
VACUUM tl, tl_g, tl_l, tl_lp;
SELECT * FROM tl_check(ARRAY(SELECT q FROM tl_queries));
SELECT lion_index_verify('tl_l_i', true), lion_index_verify('tl_lp_i', true);
REINDEX INDEX tl_l_i; REINDEX INDEX tl_lp_i;
SELECT * FROM tl_check(ARRAY(SELECT q FROM tl_queries) || ARRAY['{n1}', '{n2}', '{n1}:A']);
SELECT lion_index_verify('tl_l_i', true), lion_index_verify('tl_lp_i', true);

-- the index holds each word under its key: none longer than 2043 bytes
SELECT count(*) FILTER (WHERE octet_length(key) > 2043) AS too_long,
	   count(*) FILTER (WHERE octet_length(key) = 2043) AS at_limit,
	   (SELECT count(*) FROM tl_w WHERE tl_key(w) NOT IN
		   (SELECT key FROM lion_index_facets('tl_l_i', 1000))) AS missing
  FROM lion_index_facets('tl_l_i', 1000);

-- other keys are not hashed: one of more than 2047 bytes is an error
CREATE TABLE tl_t (t text, a text[]);
CREATE INDEX tl_t_t ON tl_t USING lion (t);
CREATE INDEX tl_t_a ON tl_t USING lion (a);
INSERT INTO tl_t VALUES (repeat('x', 2043), ARRAY[repeat('y', 2043)]);
INSERT INTO tl_t VALUES (repeat('x', 2044), NULL);
INSERT INTO tl_t VALUES (NULL, ARRAY[repeat('y', 2044)]);
SELECT count(*) FROM tl_t WHERE t = repeat('x', 2043);

DROP TABLE tl, tl_g, tl_l, tl_lp, tl_w, tl_wq, tl_new, tl_queries, tl_t;
DROP FUNCTION tl_copy_in(text, text, text), tl_send(text[], int[]), tl_sub(text), tl_q(text),
	tl_ids(text, tsquery, text), tl_check(text[]), tl_plan(text), tl_key(text), tl_bm_ref(text[], int),
	tl_bm_same(text, text[], int), tl_bm_scan(text, int);
