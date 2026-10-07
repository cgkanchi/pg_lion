-- The invariant: a tsquery filter answered through lion - bitmap scan, plain
-- index scan, the count pushdown, with stored positions or without - returns
-- exactly the rows a sequential scan and a GIN index return.  Random
-- documents (positions, weights, stripped, empty, NULL) against random
-- queries (AND, OR, NOT, phrase and distance, weights, prefixes, a lexeme no
-- row has), before and after updates and deletes; only differences print.

\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;
CREATE EXTENSION IF NOT EXISTS pg_lion;
SELECT setseed(0.42) IS NULL AS seeded;
CREATE TABLE fz (id int, tsv tsvector);
-- random documents: small vocabulary, random weights, some stripped, empty, NULL
INSERT INTO fz
SELECT i, CASE WHEN i % 97 = 0 THEN NULL
			   WHEN i % 89 = 0 THEN ''::tsvector
			   WHEN i % 13 = 0 THEN strip(d)
			   ELSE d END
FROM (SELECT i, (SELECT coalesce(string_agg(format('w%s:%s%s', (random()*9)::int, p,
						(ARRAY['','A','B','C','D'])[1 + (random()*4)::int]), ' '), '')
				 FROM generate_series(1, 1 + (random()*12)::int) p
				 WHERE i > 0)::tsvector AS d
	  FROM generate_series(1, 4000) i) s;
CREATE TABLE fz_g AS SELECT * FROM fz; CREATE INDEX ON fz_g USING gin (tsv);
CREATE TABLE fz_l AS SELECT * FROM fz; CREATE INDEX ON fz_l USING lion (tsv);
CREATE TABLE fz_lp AS SELECT * FROM fz; CREATE INDEX fz_lp_i ON fz_lp USING lion (tsv) WITH (store_positions = true);
VACUUM ANALYZE fz, fz_g, fz_l, fz_lp;

CREATE FUNCTION fz_q(depth int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r float8 := random();
BEGIN
	IF depth = 0 OR r < 0.3 THEN
		RETURN 'w' || (random()*10)::int ||		-- w10 is in no document
			(ARRAY['','','','','','',':*',':A',':B',':AB',':CD',':*A',':*BC'])[1 + (random()*12)::int];
	ELSIF r < 0.42 THEN RETURN '!' || fz_q(depth - 1);
	ELSIF r < 0.62 THEN RETURN '(' || fz_q(depth - 1) || ' & ' || fz_q(depth - 1) || ')';
	ELSIF r < 0.82 THEN RETURN '(' || fz_q(depth - 1) || ' | ' || fz_q(depth - 1) || ')';
	ELSIF r < 0.92 THEN RETURN '(' || fz_q(depth - 1) || ' <-> ' || fz_q(depth - 1) || ')';
	ELSE RETURN '(' || fz_q(depth - 1) || ' <' || (random()*3)::int || '> ' || fz_q(depth - 1) || ')';
	END IF;
END $$;

CREATE FUNCTION fz_ids(tab text, q tsquery, mode text) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
	PERFORM set_config('enable_seqscan', CASE WHEN mode = 'seq' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN mode IN ('bitmap','count') THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_indexscan', CASE WHEN mode IN ('index','count') THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	IF mode = 'count' THEN
		EXECUTE format('SELECT ARRAY[count(*)::int] FROM %I WHERE tsv @@ $1', tab) INTO r USING q;
	ELSE
		EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM %I WHERE tsv @@ $1', tab) INTO r USING q;
	END IF;
	RETURN r;
END $$;

CREATE FUNCTION fz_run(n int) RETURNS TABLE (q text, path text, got int, want int) LANGUAGE plpgsql AS $$
DECLARE qs text; ref int[]; t text; m text; g int[];
BEGIN
	FOR i IN 1..n LOOP
		qs := fz_q(3);
		ref := fz_ids('fz', qs::tsquery, 'seq');
		FOREACH t IN ARRAY ARRAY['fz_g','fz_l','fz_lp'] LOOP
			FOREACH m IN ARRAY ARRAY['bitmap','index','count'] LOOP
				CONTINUE WHEN t = 'fz_g' AND m = 'index';
				BEGIN
					g := fz_ids(t, qs::tsquery, m);
				EXCEPTION WHEN others THEN
					q := qs; path := t || '/' || m || ' ERROR ' || SQLERRM; got := -1; want := cardinality(ref);
					RETURN NEXT; CONTINUE;
				END;
				IF (m = 'count' AND g[1] <> cardinality(ref)) OR (m <> 'count' AND g IS DISTINCT FROM ref) THEN
					q := qs; path := t || '/' || m;
					got := CASE WHEN m = 'count' THEN g[1] ELSE cardinality(g) END; want := cardinality(ref);
					RETURN NEXT;
				END IF;
			END LOOP;
		END LOOP;
	END LOOP;
END $$;
SELECT * FROM fz_run(150);
-- updates and deletes, not vacuumed: dead rows and pages not all-visible
UPDATE fz SET tsv = tsv || 'w3:20A w9:21'::tsvector WHERE id % 7 = 0;
UPDATE fz_g SET tsv = tsv || 'w3:20A w9:21'::tsvector WHERE id % 7 = 0;
UPDATE fz_l SET tsv = tsv || 'w3:20A w9:21'::tsvector WHERE id % 7 = 0;
UPDATE fz_lp SET tsv = tsv || 'w3:20A w9:21'::tsvector WHERE id % 7 = 0;
DELETE FROM fz WHERE id % 11 = 0; DELETE FROM fz_g WHERE id % 11 = 0;
DELETE FROM fz_l WHERE id % 11 = 0; DELETE FROM fz_lp WHERE id % 11 = 0;
SELECT * FROM fz_run(150);
-- fixed edge cases
SELECT * FROM (SELECT x.* FROM unnest(ARRAY['', '!w1', '!!w1', 'w1 <-> !w2', '!w1 <-> w2', '!(w1 <-> w2)',
	'w1 <0> w1', 'w1 <0> w2', '(w1 | w2) <-> (w3 & w4)', '!w10', 'w10:*', 'w:*', 'w1:*D <-> w2:*',
	'(w1 <-> w2) <-> w3', 'w1 <-> (w2 <-> w3)', '!(w1 & !w2)', 'w1 <3> !w2']) q,
	LATERAL (SELECT q AS q, t || '/' || m AS path,
				CASE WHEN m = 'count' THEN (fz_ids(t, q::tsquery, m))[1] ELSE cardinality(fz_ids(t, q::tsquery, m)) END AS got,
				cardinality(fz_ids('fz', q::tsquery, 'seq')) AS want,
				(fz_ids(t, q::tsquery, m) = fz_ids('fz', q::tsquery, 'seq') OR m = 'count') AS sameids
			   FROM unnest(ARRAY['fz_g','fz_l','fz_lp']) t, unnest(ARRAY['bitmap','index','count']) m
			  WHERE NOT (t = 'fz_g' AND m = 'index')) x) y
 WHERE NOT sameids OR (path LIKE '%count' AND got <> want);

DROP TABLE fz, fz_g, fz_l, fz_lp;
DROP FUNCTION fz_run(int);
DROP FUNCTION fz_ids(text, tsquery, text);
DROP FUNCTION fz_q(int);
