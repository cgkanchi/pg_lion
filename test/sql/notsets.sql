-- A negated multi-key query in a lion set (DESIGN.md §30.2, "A negated
-- multi-key query"): `NOT (col op const)` of an array, tsvector or jsonb
-- column, beside other clauses of the same lion index, is answered by the
-- set - the other clauses' rows less the query's and less the column's
-- NULLs - so a LionBtreeScan or LionOrdered walk, or a nested loop's probe,
-- never reads the heap for it.  Every query is run through the node (core's
-- scans disabled) and through a seq scan, and the answers compared.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
SET work_mem = '64MB';

CREATE TABLE ns (
	id int PRIMARY KEY,
	k int NOT NULL,
	c int,				-- ten values, some NULL
	tags text[],		-- NULL, empty, NULL elements, one to three tags
	tsv tsvector,
	doc jsonb,
	pad text
) WITH (autovacuum_enabled = off);
INSERT INTO ns
SELECT i,
	   (i::bigint * 7919 % 30011)::int,
	   CASE WHEN i % 97 = 0 THEN NULL ELSE i % 10 END,
	   CASE WHEN i % 13 = 0 THEN NULL
			WHEN i % 17 = 0 THEN '{}'::text[]
			WHEN i % 19 = 0 THEN ARRAY['a', NULL]
			ELSE ARRAY['a' || (i % 3), 'b' || (i % 7)] ||
				 CASE WHEN i % 5 = 0 THEN ARRAY['z'] ELSE '{}'::text[] END
	   END,
	   CASE WHEN i % 23 = 0 THEN NULL
			ELSE to_tsvector('simple', 'w' || (i % 4) || ' v' || (i % 9)) END,
	   CASE WHEN i % 29 = 0 THEN NULL
			ELSE jsonb_build_object('k', i % 6, 'tag', 'a' || (i % 3)) END,
	   repeat('p', 200)
  FROM generate_series(1, 30000) i;
CREATE INDEX ns_k ON ns (k) INCLUDE (id, c);
CREATE INDEX ns_lion ON ns USING lion (c, tags, tsv, doc);
VACUUM (FREEZE, ANALYZE) ns;
-- dead and moved rows the sets still hold entries of
DELETE FROM ns WHERE id % 31 = 0;
UPDATE ns SET tags = ARRAY['a1'] WHERE id % 37 = 0;

CREATE TABLE nsd (id int PRIMARY KEY, w int);
INSERT INTO nsd SELECT i, i % 4 FROM generate_series(1, 30000, 3) i;
ANALYZE nsd;

/*
 * lion_ns() runs q with core's scans disabled, so that lion's node is the
 * path left, and through a seq scan with lion's nodes off; compares the two
 * answers as multisets, and names the node and whether its set took the NOT
 * (a `NOT` in its Lion Cond).
 */
CREATE FUNCTION lion_ns(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	node text := 'no node';
	took text := '';
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(Lion(Ordered|BtreeScan)\)' THEN
			node := substring(ln from 'Custom Scan \((Lion\w+)\)');
		ELSIF ln ~ '^\s*Lion Cond: .*NOT ' THEN
			took := ', NOT in the set';
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'off', true);
	FOR r IN EXECUTE q LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'on', true);

	SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO a FROM unnest(a) e;
	SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO b FROM unnest(b) e;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s%s, %s rows', node, took, coalesce(array_length(a, 1), 0));
END $$;

-- the walk's plan: the NOT is in the set, nothing left to a filter
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SET pg_lion.pushdown_margin = 1000;
EXPLAIN (COSTS OFF)
SELECT id, k FROM ns WHERE c = 3 AND NOT (tags && '{a1}') ORDER BY k;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
RESET pg_lion.pushdown_margin;

SELECT q, lion_ns(q) FROM (VALUES
	-- arrays: overlap, containment, NULL arrays, empty arrays, NULL elements
	('SELECT id, k FROM ns WHERE c = 3 AND NOT (tags && ''{a1}'')'),
	('SELECT id, k FROM ns WHERE c = 3 AND NOT (tags && ''{a1}'') ORDER BY k'),
	('SELECT id, k FROM ns WHERE c = 4 AND NOT (tags @> ''{a1,b4}'')'),
	('SELECT id, k FROM ns WHERE c = 5 AND NOT (tags && ''{a1,z}'')'),
	('SELECT id, k FROM ns WHERE c = 6 AND NOT (tags && ''{}'')'),
	('SELECT id, k FROM ns WHERE c = 6 AND NOT (tags @> ''{a,NULL}'')'),
	('SELECT id, k FROM ns WHERE c = 7 AND NOT (tags && ''{nope}'')'),
	-- two negations, and one beside an IN list
	('SELECT id, k FROM ns WHERE c = 2 AND NOT (tags && ''{a2}'') AND NOT (tags && ''{z}'')'),
	('SELECT id, k FROM ns WHERE c IN (1, 8) AND NOT (tags && ''{b1}'')'),
	-- tsvector and jsonb
	('SELECT id, k FROM ns WHERE c = 1 AND NOT (tsv @@ ''w1 & v1''::tsquery)'),
	('SELECT id, k FROM ns WHERE c = 1 AND NOT (tsv @@ ''w2''::tsquery)'),
	('SELECT id, k FROM ns WHERE c = 9 AND NOT (doc @> ''{"k": 3}'')'),
	('SELECT id, k FROM ns WHERE c = 9 AND NOT (doc ? ''k'')'),
	('SELECT id, k FROM ns WHERE c = 8 AND NOT (doc @> ''{"tag": "a1"}'')'),
	-- a NOT of no other clause of the index: left to the filter
	('SELECT id, k FROM ns WHERE k < 3000 AND NOT (tags && ''{a1}'')'),
	-- a NOT under an OR is not a restriction of its own
	('SELECT id, k FROM ns WHERE c = 3 AND (NOT (tags && ''{a1}'') OR k < 100)'),
	-- a nested loop's probe of ns
	('SELECT nsd.id, ns.id FROM nsd JOIN ns ON ns.k = nsd.id WHERE nsd.w = 0 AND ns.c = 4 AND NOT (ns.tags && ''{a1}'')')
) v(q);

-- the probe's plan at the default settings: index only, nothing on the heap
EXPLAIN (COSTS OFF)
SELECT nsd.id, ns.id FROM nsd JOIN ns ON ns.k = nsd.id
 WHERE nsd.w = 0 AND ns.c = 4 AND NOT (ns.tags && '{a1}');

-- off, the NOT goes back to the filter, and the answers are the same
SET pg_lion.enable_not_sets = off;
SELECT q, lion_ns(q) FROM (VALUES
	('SELECT id, k FROM ns WHERE c = 3 AND NOT (tags && ''{a1}'')'),
	('SELECT id, k FROM ns WHERE c = 9 AND NOT (doc @> ''{"k": 3}'')')
) v(q);
RESET pg_lion.enable_not_sets;

DROP TABLE ns, nsd;
DROP FUNCTION lion_ns(text);
