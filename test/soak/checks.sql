-- The soak's reader side: every lion-answered query against the same query
-- with the lion paths off, under ONE snapshot (test/soak/README.md).  Load
-- after schema.sql; everything here is read-only, so it runs on a hot
-- standby too.
\set ON_ERROR_STOP 1
SET client_min_messages = warning;

-- A random integer in [lo, hi].
CREATE OR REPLACE FUNCTION soak.ri(lo int, hi int) RETURNS int
LANGUAGE sql VOLATILE AS $$ SELECT lo + floor(random() * (hi - lo + 1))::int $$;

-- A random element of an array.
CREATE OR REPLACE FUNCTION soak.pick(a text[]) RETURNS text
LANGUAGE sql VOLATILE AS $$ SELECT a[1 + floor(random() * cardinality(a))::int] $$;

-- A random value of a column: mostly ones that occur, now and then one
-- that does not (an absent key, an empty posting set).
CREATE OR REPLACE FUNCTION soak.val(col text) RETURNS text
LANGUAGE plpgsql VOLATILE AS $$
BEGIN
	RETURN CASE col
		WHEN 'c2' THEN soak.ri(0, 1)
		WHEN 'c20' THEN CASE WHEN random() < 0.03 THEN 25 ELSE soak.ri(0, 19) END
		WHEN 'c200' THEN CASE WHEN random() < 0.03 THEN 230 ELSE soak.ri(0, 199) END
		WHEN 'c20k' THEN soak.ri(0, 20500)
		WHEN 'c1m' THEN soak.ri(0, 999999)
		WHEN 'cn' THEN soak.ri(0, 52)
		END::text;
END $$;

-- n random values of a column as a comma list, for IN.
CREATE OR REPLACE FUNCTION soak.vals(col text, n int) RETURNS text
LANGUAGE sql VOLATILE AS $$
	SELECT string_agg(soak.val(col), ', ') FROM generate_series(1, n)
$$;

-- The lion index of a column of tbl, and the relation it belongs to: soak.t
-- has its own names; soak.p's are its partitions' (a partitioned table's
-- index has no storage, and the SQL functions take a partition's).
CREATE OR REPLACE FUNCTION soak.idx(tbl text, col text, OUT idx text, OUT rel text)
LANGUAGE plpgsql VOLATILE AS $$
DECLARE part text;
BEGIN
	IF tbl = 'soak.t' THEN
		idx := 'soak.t_l_' || col; rel := 'soak.t';
	ELSE
		part := 'p' || soak.ri(0, 2);
		idx := 'soak.' || part || '_' || col || '_idx'; rel := 'soak.' || part;
	END IF;
END $$;

-- The lion side: the pushdown and LionOrdered on, a sequential scan only as
-- a last resort, and the randomized switches of this query.
CREATE OR REPLACE FUNCTION soak.lion_mode(gucs text[]) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE g text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_semijoin', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	FOREACH g IN ARRAY gucs LOOP
		PERFORM set_config(split_part(g, '=', 1), split_part(g, '=', 2), true);
	END LOOP;
END $$;

-- The reference: no lion path at all, a sequential scan, serial.
CREATE OR REPLACE FUNCTION soak.ref_mode() RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_semijoin', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('work_mem', '64MB', true);
	PERFORM set_config('enable_hashagg', 'on', true);
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
END $$;

-- What answered a query, from its plan.
CREATE OR REPLACE FUNCTION soak.classify(plan text) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE
		WHEN plan ~ 'Lion(Semi|Anti)Join' THEN 'semijoin'
		WHEN plan ~ 'LionCount' AND plan ~ 'Aggregates Over Keys' THEN 'keyaggs'
		WHEN plan ~ 'LionCount' THEN 'count'
		WHEN plan ~ 'LionOrdered' THEN 'ordered'
		WHEN plan ~ 'Bitmap Index Scan on \S*(_l_\w+|_(c2|c20|c200|c20k|c1m|cn|tags)_idx)' THEN 'bitmap'
		WHEN plan ~ 'Index (Only )?Scan (Backward )?using \S*(_l_\w+|_(c2|c20|c200|c20k|c1m|cn|tags)_idx)' THEN 'plain'
		ELSE 'none' END
$$;

-- Random settings for the lion side of one query: memory small enough to
-- make the counts batch and spill, the paths' own switches, and now and
-- then a parallel plan.
CREATE OR REPLACE FUNCTION soak.rand_gucs() RETURNS text[]
LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	g text[] := '{}';
	r text;
BEGIN
	IF random() < 0.15 THEN g := g || 'work_mem=64kB'::text;
	ELSIF random() < 0.3 THEN g := g || 'work_mem=1MB'::text; END IF;
	IF random() < 0.3 THEN g := g || 'pg_lion.enable_decoded_walk=off'::text; END IF;
	IF random() < 0.2 THEN g := g || 'pg_lion.enable_topk=off'::text; END IF;
	IF random() < 0.3 THEN g := g || 'pg_lion.enable_lazy_set=off'::text; END IF;
	IF random() < 0.2 THEN g := g || 'pg_lion.enable_intersection_probe=off'::text; END IF;
	IF random() < 0.2 THEN g := g || 'pg_lion.enable_union_probe=off'::text; END IF;
	IF random() < 0.2 THEN g := g || 'pg_lion.enable_tree_probe=off'::text; END IF;
	IF random() < 0.2 THEN g := g || 'pg_lion.enable_plain_scan=off'::text; END IF;
	IF random() < 0.3 THEN
		-- leave the pushdown and LionOrdered nothing to lose to
		g := g || ARRAY['enable_bitmapscan=off', 'enable_indexscan=off', 'enable_indexonlyscan=off'];
	ELSIF random() < 0.15 THEN
		g := g || 'enable_bitmapscan=off'::text;
	END IF;
	IF random() < 0.1 THEN g := g || 'enable_hashagg=off'::text; END IF;
	IF random() < 0.1 THEN g := g || 'enable_sort=off'::text; END IF;
	-- core's join methods, one or all of them: what the FK-side and semi
	-- joins compete with, or a plan they are forced into
	IF random() < 0.08 THEN
		g := g || ARRAY['enable_hashjoin=off', 'enable_mergejoin=off', 'enable_nestloop=off'];
	ELSE
		IF random() < 0.1 THEN g := g || 'enable_hashjoin=off'::text; END IF;
		IF random() < 0.1 THEN g := g || 'enable_mergejoin=off'::text; END IF;
		IF random() < 0.1 THEN g := g || 'enable_nestloop=off'::text; END IF;
	END IF;
	-- The units and the margin a lion path is priced in (DESIGN.md §39), on a
	-- build that has them: each now and then at a bound or between, so that
	-- lion's paths are chosen where they never are by default, and not where
	-- they always are.  No answer may depend on it.
	IF current_setting('pg_lion.pushdown_margin', true) IS NOT NULL THEN
		IF random() < 0.4 THEN
			g := g || ('pg_lion.pushdown_margin=' || soak.pick('{0.01,0.1,0.5,0.99,1}'));
		END IF;
		FOREACH r IN ARRAY '{hashagg,agg,hashjoin,mergejoin,nestloop,bitmap}'::text[] LOOP
			IF random() < 0.25 THEN
				g := g || format('pg_lion.%s_rate=%s', r, soak.pick('{0.001,0.05,1,20,1000}'));
			END IF;
		END LOOP;
		IF random() < 0.2 THEN
			g := g || ('pg_lion.resident_page_cost=' || soak.pick('{0,1,120,100000}'));
		END IF;
		IF random() < 0.2 THEN
			g := g || ('effective_cache_size=' || soak.pick('{8kB,1MB,4GB,1TB}'));
		END IF;
	END IF;
	IF random() < 0.1 THEN
		g := g || ARRAY['max_parallel_workers_per_gather=2', 'parallel_setup_cost=0',
						'parallel_tuple_cost=0', 'min_parallel_table_scan_size=0',
						'min_parallel_index_scan_size=0'];
	ELSE
		g := g || 'max_parallel_workers_per_gather=0'::text;
	END IF;
	RETURN g;
END $$;

-- Run one query the lion way and the reference way in the current snapshot
-- and compare the answers as multisets of rows.  One row back: ok, FAIL (the
-- answers differ), ERROR (an error other than a serialization failure, a
-- deadlock or a recovery conflict), or expected (one of those).
CREATE OR REPLACE FUNCTION soak.cmp(fam text, lq text, rq text, gucs text[],
						 OUT status text, OUT family text, OUT path text, OUT detail text)
LANGUAGE plpgsql AS $f$
DECLARE
	wrap constant text := 'SELECT coalesce(array_agg(x::text ORDER BY x::text), ''{}'') FROM (%s) x';
	a text[];
	b text[];
	ln text;
	plan text := '';
	da text[];
	db text[];
	st text;
	msg text;
BEGIN
	family := fam;
	path := 'none';
	IF current_setting('transaction_isolation') = 'read committed' THEN
		-- Each statement would take its own snapshot: the two answers would
		-- not be comparable.  A harness error, never a pass.
		status := 'ERROR';
		detail := 'harness: soak.cmp() called outside a REPEATABLE READ or SERIALIZABLE transaction';
		RETURN;
	END IF;
	BEGIN
		PERFORM soak.lion_mode(gucs);
		FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || format(wrap, lq) LOOP
			plan := plan || ln || E'\n';
		END LOOP;
		path := soak.classify(plan);
		IF path = 'keyaggs' THEN
			-- Which way the aggregates over keys went: from the entries'
			-- own counts (the headers, §37) or each entry counted.  Asked
			-- of an EXPLAIN ANALYZE run just before the compared one, in
			-- the same snapshot.
			plan := '';
			FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || lq LOOP
				plan := plan || ln || E'\n';
			END LOOP;
			path := CASE WHEN plan ~ 'Key Walks Counted: [1-9]' THEN 'keyaggs-counted'
						 WHEN plan ~ 'Key Walks From Entry Counts: [1-9]' THEN 'keyaggs-headers'
						 ELSE 'keyaggs' END;
		END IF;
		EXECUTE format(wrap, lq) INTO a;
		PERFORM soak.ref_mode();
		EXECUTE format(wrap, rq) INTO b;
	EXCEPTION WHEN OTHERS THEN
		GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT;
		PERFORM soak.ref_mode();
		IF st IN ('40001', '40P01') THEN
			status := 'expected';
			detail := st || ' ' || msg;
		ELSE
			status := 'ERROR';
			detail := format('sqlstate=%s message=%s snapshot=%s gucs=%s lion_query=%s ref_query=%s plan=%s',
							 st, msg, pg_current_snapshot(), gucs, lq, rq, plan);
		END IF;
		RETURN;
	END;
	IF a = b THEN
		status := 'ok';
		RETURN;
	END IF;
	da := ARRAY(SELECT unnest(a) EXCEPT ALL SELECT unnest(b));
	db := ARRAY(SELECT unnest(b) EXCEPT ALL SELECT unnest(a));
	status := 'FAIL';
	detail := format('lion_only=%s ref_only=%s lion_rows=%s ref_rows=%s snapshot=%s isolation=%s gucs=%s lion_query=%s ref_query=%s plan=%s',
					 left(da::text, 3000), left(db::text, 3000), cardinality(a), cardinality(b),
					 pg_current_snapshot(), current_setting('transaction_isolation'),
					 gucs, lq, rq, plan);
END $f$;

-- One random query of tbl ('soak.t' or 'soak.p'): its family, the lion
-- side's SQL and the reference's (the same SQL but for the direct counts).
CREATE OR REPLACE FUNCTION soak.query(tbl text, OUT fam text, OUT lq text, OUT rq text)
LANGUAGE plpgsql VOLATILE AS $f$
DECLARE
	scal constant text[] := '{c2,c20,c200,c20k,c1m,cn}';
	ord constant text[] := '{c20,c200,c20k,c1m,cn}';
	grp constant text[] := '{c2,c20,c200,cn}';
	col text; col2 text; v text; x int; k int;
	ix record; idx2 text;
	tag1 text := 't' || CASE WHEN random() < 0.5 THEN soak.ri(0, 9) ELSE soak.ri(0, 205) END;
	tag2 text := 't' || soak.ri(0, 199);
	n int := floor(random() * 20)::int;
BEGIN
	CASE n
	WHEN 0 THEN
		fam := 'eq'; col := soak.pick(scal);
		lq := format('SELECT count(*) FROM %s WHERE %s = %s', tbl, col, soak.val(col));
	WHEN 1 THEN
		fam := 'in'; col := soak.pick(scal);
		lq := format('SELECT count(*) FROM %s WHERE %s IN (%s)', tbl, col,
					 soak.vals(col, CASE WHEN random() < 0.2 THEN soak.ri(20, 60) ELSE soak.ri(2, 6) END));
	WHEN 2 THEN
		fam := 'range'; col := soak.pick(ord);
		x := soak.val(col)::int;
		lq := CASE soak.ri(0, 3)
			WHEN 0 THEN format('SELECT count(*) FROM %s WHERE %s BETWEEN %s AND %s', tbl, col, x,
							   x + CASE col WHEN 'c1m' THEN soak.ri(0, 200000) WHEN 'c20k' THEN soak.ri(0, 3000) ELSE soak.ri(0, 8) END)
			WHEN 1 THEN format('SELECT count(*) FROM %s WHERE %s < %s', tbl, col, x)
			WHEN 2 THEN format('SELECT count(*) FROM %s WHERE %s >= %s AND c20 = %s', tbl, col, x, soak.val('c20'))
			ELSE format('SELECT count(*) FROM %s WHERE %s > %s AND %s <= %s', tbl, col, x, col, x + soak.ri(1, 40)) END;
	WHEN 3 THEN
		fam := 'ne'; col := soak.pick(scal);
		lq := format('SELECT count(*) FROM %s WHERE %s <> %s%s', tbl, col, soak.val(col),
					 CASE WHEN random() < 0.5 THEN format(' AND c200 = %s', soak.val('c200')) ELSE '' END);
	WHEN 4 THEN
		fam := 'null';
		lq := format('SELECT count(*) FROM %s WHERE %s', tbl, soak.pick(ARRAY[
			'cn IS NULL', 'cn IS NOT NULL', format('cn IS NULL AND c20 = %s', soak.val('c20')),
			format('cn IS NOT NULL AND c200 = %s', soak.val('c200')), 'tags IS NULL',
			format('(cn IS NULL OR cn = %s) AND c2 = 1', soak.val('cn'))]));
	WHEN 5 THEN
		fam := 'and';
		lq := format('SELECT count(*) FROM %s WHERE %s', tbl, soak.pick(ARRAY[
			format('c20 = %s AND c200 = %s', soak.val('c20'), soak.val('c200')),
			format('c2 = %s AND c20 = %s AND cn = %s', soak.val('c2'), soak.val('c20'), soak.val('cn')),
			format('c2 = %s AND c20k BETWEEN %s AND %s', soak.val('c2'), soak.ri(0, 19000), soak.ri(0, 20000)),
			format('c20 = %s AND tags @> ARRAY[%L]', soak.val('c20'), tag1),
			format('c20 IN (%s) AND c200 IN (%s)', soak.vals('c20', 3), soak.vals('c200', 5)),
			format('c1m < %s AND c20 = %s', soak.val('c1m'), soak.val('c20'))]));
	WHEN 6 THEN
		fam := 'or';
		lq := format('SELECT count(*) FROM %s WHERE %s', tbl, soak.pick(ARRAY[
			format('c20 = %s OR c200 = %s', soak.val('c20'), soak.val('c200')),
			format('(c2 = 0 AND c20 = %s) OR cn IS NULL', soak.val('c20')),
			format('c1m < %s OR c20 IN (%s)', soak.ri(0, 50000), soak.vals('c20', 2)),
			format('(c20 = %s OR c200 = %s) AND c2 = %s', soak.val('c20'), soak.val('c200'), soak.val('c2')),
			format('(c20 = %s AND c200 = %s) OR (c20 = %s AND cn = %s)', soak.val('c20'), soak.val('c200'), soak.val('c20'), soak.val('cn')),
			format('tags && ARRAY[%L] OR c200 = %s', tag1, soak.val('c200'))]));
	WHEN 7 THEN
		fam := 'tags';
		lq := format('SELECT count(*) FROM %s WHERE %s', tbl, soak.pick(ARRAY[
			format('tags @> ARRAY[%L]', tag1), format('tags && ARRAY[%L, %L]', tag1, tag2),
			format('tags @> ARRAY[%L, %L]', tag1, tag2),
			format('tags && ARRAY[%L] AND tags && ARRAY[%L]', tag1, tag2)]));
	WHEN 8 THEN
		fam := 'group1'; col := soak.pick(grp);
		lq := soak.pick(ARRAY[
			format('SELECT %s, count(*) FROM %s GROUP BY %s', col, tbl, col),
			format('SELECT %s, count(*) FROM %s WHERE c2 = %s GROUP BY %s', col, tbl, soak.val('c2'), col),
			format('SELECT c20k, count(*) FROM %s WHERE c20 = %s GROUP BY c20k', tbl, soak.val('c20')),
			format('SELECT c20, count(*) FROM %s WHERE c1m < %s GROUP BY c20', tbl, soak.val('c1m')),
			format('SELECT c200, count(*) FROM %s WHERE c20 IN (%s) GROUP BY c200 HAVING count(*) > %s', tbl, soak.vals('c20', 2), soak.ri(0, 100)),
			format('SELECT c20, count(*) FROM %s WHERE tags && ARRAY[%L, %L] GROUP BY c20', tbl, tag1, tag2)]);
	WHEN 9 THEN
		fam := 'group2';
		col := soak.pick(grp);
		col2 := soak.pick(array_remove(grp, col));
		lq := format('SELECT %s, %s, count(*) FROM %s%s GROUP BY %s, %s', col, col2, tbl,
					 CASE WHEN random() < 0.4 THEN format(' WHERE c200 = %s', soak.val('c200')) ELSE '' END, col, col2);
	WHEN 10 THEN
		fam := 'group3';
		lq := format('SELECT c2, c20, %s, count(*) FROM %s%s GROUP BY 1, 2, 3', soak.pick('{cn,c200}'), tbl,
					 CASE WHEN random() < 0.3 THEN format(' WHERE c1m < %s', soak.val('c1m')) ELSE '' END);
	WHEN 11 THEN
		fam := 'distinct';
		lq := soak.pick(ARRAY[
			format('SELECT count(DISTINCT c200) FROM %s WHERE c20 = %s', tbl, soak.val('c20')),
			format('SELECT count(DISTINCT c20k) FROM %s', tbl),
			format('SELECT count(DISTINCT cn) FROM %s WHERE c2 = %s', tbl, soak.val('c2')),
			format('SELECT c2, count(DISTINCT c20) FROM %s GROUP BY c2', tbl),
			format('SELECT count(DISTINCT c20), count(*) FROM %s WHERE c200 IN (%s)', tbl, soak.vals('c200', 4))]);
	WHEN 12 THEN
		fam := 'all';
		lq := format('SELECT count(*) FROM %s', tbl);
	WHEN 13 THEN
		fam := 'keyaggs';
		lq := format(soak.pick(ARRAY[
			'SELECT sum(c200), avg(c20), min(c1m), max(c1m) FROM %s',
			'SELECT sum(c20k), avg(c20k), max(c1m %% 1000), min(c200 - 7) FROM %s',
			'SELECT min(cn), max(cn), sum(cn), avg(cn) FROM %s',
			'SELECT bool_or(c2 = 1), bool_and(c20 < 19), count(*) FROM %s',
			'SELECT sum(c1m), max(c20k), count(*) FROM %s']), tbl);
	WHEN 14 THEN
		fam := 'topk'; k := soak.ri(1, 12);
		lq := soak.pick(ARRAY[
			-- no tie-breaker: only the counts are compared, which are determined
			format('SELECT count(*) FROM %s GROUP BY c20k ORDER BY count(*) DESC LIMIT %s', tbl, k),
			format('SELECT count(*) FROM %s GROUP BY c1m ORDER BY count(*) DESC LIMIT %s', tbl, k),
			format('SELECT c200, count(*) FROM %s GROUP BY c200 ORDER BY count(*) DESC, c200 LIMIT %s', tbl, k),
			format('SELECT count(*) FROM %s WHERE c2 = %s GROUP BY c200 ORDER BY count(*) DESC LIMIT %s', tbl, soak.val('c2'), k)]);
	WHEN 15 THEN
		fam := 'direct';
		col := soak.pick(scal);
		ix := soak.idx(tbl, col);
		CASE soak.ri(0, 4)
		WHEN 0 THEN
			v := soak.val(col);
			lq := format('SELECT lion_index_count(%L, %s)', ix.idx, v);
			rq := format('SELECT count(*) FROM ONLY %s WHERE %s = %s', ix.rel, col, v);
		WHEN 1 THEN
			v := soak.vals(col, soak.ri(1, 30));
			lq := format('SELECT lion_index_count_any(%L, ARRAY[%s])', ix.idx, v);
			rq := format('SELECT count(*) FROM ONLY %s WHERE %s IN (%s)', ix.rel, col, v);
		WHEN 2 THEN
			col2 := soak.pick(array_remove(scal, col));
			-- the second index of the same relation
			idx2 := regexp_replace(ix.idx, '(_l_|_)' || col || '(_idx)?$', '\1' || col2 || '\2');
			v := soak.val(col);
			x := soak.val(col2)::int;
			lq := format('SELECT lion_index_count(%L, %s, %L, %s)', ix.idx, v, idx2, x);
			rq := format('SELECT count(*) FROM ONLY %s WHERE %s = %s AND %s = %s', ix.rel, col, v, col2, x);
		WHEN 3 THEN
			lq := format('SELECT groups, count FROM lion_index_count_group_stats(%L, %s)', ix.idx, (random() < 0.5)::text);
			rq := format('SELECT count(*) AS groups, coalesce(sum(n), 0)::bigint AS count FROM (SELECT %s, count(*) n FROM ONLY %s GROUP BY %s) s',
						 col, ix.rel, col);
		ELSE
			IF tbl = 'soak.t' THEN
				lq := 'SELECT groups, count FROM lion_index_count_group_stats(''soak.t_l_c20_c200'', true, 2::int2)';
				rq := 'SELECT count(*) AS groups, coalesce(sum(n), 0)::bigint AS count FROM (SELECT c200, count(*) n FROM soak.t GROUP BY c200) s';
			ELSE
				v := soak.val(col);
				lq := format('SELECT count FROM lion_index_count_stats(%L, %s)', ix.idx, v);
				rq := format('SELECT count(*) FROM ONLY %s WHERE %s = %s', ix.rel, col, v);
			END IF;
		END CASE;
	WHEN 16 THEN
		fam := 'ordered'; k := soak.ri(1, 40);
		lq := soak.pick(ARRAY[
			format('SELECT id, c20 FROM %s WHERE c20 = %s ORDER BY id LIMIT %s', tbl, soak.val('c20'), k),
			format('SELECT id FROM %s WHERE c200 IN (%s) AND c2 = %s ORDER BY id DESC LIMIT %s', tbl, soak.vals('c200', 3), soak.val('c2'), k),
			format('SELECT id FROM %s WHERE tags && ARRAY[%L] ORDER BY id LIMIT %s', tbl, tag1, k),
			format('SELECT id FROM %s WHERE cn IS NULL AND c20 = %s ORDER BY id DESC LIMIT %s', tbl, soak.val('c20'), k),
			format('SELECT c1m FROM %s WHERE c20 = %s ORDER BY c1m LIMIT %s', tbl, soak.val('c20'), k),
			format('SELECT c20k FROM %s WHERE c2 = %s ORDER BY c20k DESC LIMIT %s', tbl, soak.val('c2'), k),
			format('SELECT c200 FROM %s WHERE c200 <> %s ORDER BY c200 LIMIT %s', tbl, soak.val('c200'), k)]);
	WHEN 17 THEN
		fam := 'fetch';
		lq := soak.pick(ARRAY[
			format('SELECT id, c20, c200 FROM %s WHERE c20 = %s AND c200 = %s', tbl, soak.val('c20'), soak.val('c200')),
			format('SELECT id FROM %s WHERE c20k BETWEEN %s AND %s', tbl, soak.ri(0, 19900), soak.ri(0, 20000)),
			format('SELECT id FROM %s WHERE cn IS NULL AND c200 = %s', tbl, soak.val('c200')),
			format('SELECT id FROM %s WHERE tags @> ARRAY[%L] AND c20 = %s', tbl, tag1, soak.val('c20')),
			format('SELECT id, c1m FROM %s WHERE c1m IN (%s)', tbl, soak.vals('c1m', 20)),
			format('SELECT id FROM %s WHERE c200 = %s AND c20 <> %s AND cn > %s', tbl, soak.val('c200'), soak.val('c20'), soak.val('cn'))]);
	WHEN 18 THEN
		fam := 'fkjoin';
		lq := soak.pick(ARRAY[
			format('SELECT count(*) FROM %s f JOIN soak.d d ON d.k = f.c200 WHERE d.g = %s', tbl, soak.ri(0, 6)),
			format('SELECT d.g, count(*) FROM %s f JOIN soak.d d ON d.k = f.c200 WHERE f.c20 = %s GROUP BY d.g', tbl, soak.val('c20')),
			format('SELECT count(*) FROM soak.d d WHERE EXISTS (SELECT 1 FROM %s f WHERE f.c200 = d.k AND f.c20 = %s AND f.cn = %s)', tbl, soak.val('c20'), soak.val('cn')),
			format('SELECT count(*) FROM soak.d d WHERE NOT EXISTS (SELECT 1 FROM %s f WHERE f.c200 = d.k AND f.c20 = %s AND f.c2 = 1 AND f.cn = %s)', tbl, soak.val('c20'), soak.val('cn'))]);
	ELSE
		fam := 'twocol';
		-- the two-column index's second key column (soak.t only has one)
		lq := soak.pick(ARRAY[
			format('SELECT count(*) FROM %s WHERE c200 = %s', tbl, soak.val('c200')),
			format('SELECT c200, count(*) FROM %s WHERE c20 = %s GROUP BY c200', tbl, soak.val('c20')),
			format('SELECT count(*) FROM %s WHERE c20 = %s AND c200 BETWEEN %s AND %s', tbl, soak.val('c20'), soak.ri(0, 150), soak.ri(100, 199))]);
	END CASE;
	IF rq IS NULL THEN rq := lq; END IF;
END $f$;

-- n random queries of tbl, each compared in the current snapshot; with
-- fam_only, n queries of that family alone.
CREATE OR REPLACE FUNCTION soak.round(tbl text, n int, fam_only text DEFAULT NULL)
RETURNS TABLE (status text, family text, path text, detail text)
LANGUAGE plpgsql AS $$
DECLARE q record;
BEGIN
	FOR i IN 1 .. n LOOP
		LOOP
			q := soak.query(tbl);
			EXIT WHEN fam_only IS NULL OR q.fam = fam_only;
		END LOOP;
		RETURN QUERY SELECT * FROM soak.cmp(q.fam, q.lq, q.rq, soak.rand_gucs());
	END LOOP;
END $$;

-- A query read through a CURSOR that pauses part way, for pause seconds -
-- long enough for a VACUUM to come and wait for, or pass, whatever the
-- paused node holds - and then compared, in the same snapshot, with the
-- reference.
CREATE OR REPLACE FUNCTION soak.cursor_check(tbl text, pause float8,
								  OUT status text, OUT family text, OUT path text, OUT detail text)
LANGUAGE plpgsql AS $f$
DECLARE
	c refcursor;
	r record;
	a text[] := '{}';
	b text[];
	lq text;
	gucs text[] := soak.rand_gucs();
	ln text;
	plan text := '';
	st text;
	msg text;
	n int := 0;
	stop int := soak.ri(1, 6);
BEGIN
	family := 'cursor';
	path := 'none';
	IF current_setting('transaction_isolation') = 'read committed' THEN
		status := 'ERROR';
		detail := 'harness: soak.cursor_check() called outside a REPEATABLE READ or SERIALIZABLE transaction';
		RETURN;
	END IF;
	lq := soak.pick(ARRAY[
		format('SELECT c20k, count(*) FROM %s WHERE c20 = %s GROUP BY c20k', tbl, soak.val('c20')),
		format('SELECT c200, count(*) FROM %s GROUP BY c200', tbl),
		format('SELECT c2, c20, count(*) FROM %s GROUP BY c2, c20', tbl),
		format('SELECT c20, count(DISTINCT c200) FROM %s GROUP BY c20', tbl),
		format('SELECT id FROM %s WHERE c20 = %s AND c2 = %s ORDER BY id', tbl, soak.val('c20'), soak.val('c2')),
		format('SELECT id, c200 FROM %s WHERE c200 IN (%s)', tbl, soak.vals('c200', 3))]);
	BEGIN
		PERFORM soak.lion_mode(gucs);
		FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || lq LOOP
			plan := plan || ln || E'\n';
		END LOOP;
		path := soak.classify(plan);
		OPEN c FOR EXECUTE lq;
		LOOP
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			a := a || r::text;
			n := n + 1;
			IF n = stop THEN PERFORM pg_sleep(pause); END IF;
		END LOOP;
		CLOSE c;
		PERFORM soak.ref_mode();
		EXECUTE format('SELECT coalesce(array_agg(x::text), ''{}'') FROM (%s) x', lq) INTO b;
	EXCEPTION WHEN OTHERS THEN
		GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT;
		PERFORM soak.ref_mode();
		IF st IN ('40001', '40P01') THEN
			status := 'expected'; detail := st || ' ' || msg;
		ELSE
			status := 'ERROR';
			detail := format('sqlstate=%s message=%s snapshot=%s gucs=%s lion_query=%s plan=%s',
							 st, msg, pg_current_snapshot(), gucs, lq, plan);
		END IF;
		RETURN;
	END;
	a := ARRAY(SELECT unnest(a) ORDER BY 1);
	b := ARRAY(SELECT unnest(b) ORDER BY 1);
	IF a = b THEN
		status := 'ok';
	ELSE
		status := 'FAIL';
		detail := format('lion_only=%s ref_only=%s snapshot=%s isolation=%s pause=%s gucs=%s lion_query=%s plan=%s',
						 left(ARRAY(SELECT unnest(a) EXCEPT ALL SELECT unnest(b))::text, 3000),
						 left(ARRAY(SELECT unnest(b) EXCEPT ALL SELECT unnest(a))::text, 3000),
						 pg_current_snapshot(), current_setting('transaction_isolation'), pause,
						 gucs, lq, plan);
	END IF;
END $f$;

-- Every family once, for the final check of a quiet table.
CREATE OR REPLACE FUNCTION soak.sweep(tbl text, per int)
RETURNS TABLE (status text, family text, path text, detail text)
LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT * FROM soak.round(tbl, per * 20);
END $$;

-- lion_index_stats() against the rows the same snapshot counts: a scalar
-- index holds a TID for every row (dead ones too until VACUUM removes them),
-- so its ntids is at least count(*) while this snapshot holds VACUUM back -
-- and, with exact, after a VACUUM of a quiet table, equal to it.
CREATE OR REPLACE FUNCTION soak.stats_check(exact bool)
RETURNS TABLE (status text, family text, path text, detail text)
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	s record;
	n bigint;
BEGIN
	PERFORM soak.ref_mode();
	FOR r IN SELECT * FROM soak.lion_indexes WHERE scalar ORDER BY idx::text LOOP
		EXECUTE format('SELECT count(*) FROM ONLY %s', r.tbl) INTO n;
		FOR s IN SELECT * FROM lion_index_stats(r.idx) LOOP
			family := 'stats'; path := 'none';
			IF s.ntids < n OR (exact AND s.ntids <> n) OR s.entries < 0 OR s.leaf_pages < 1 THEN
				status := 'FAIL';
				detail := format('index=%s attno=%s ntids=%s rows=%s entries=%s leaf_pages=%s exact=%s snapshot=%s',
								 r.idx, s.attno, s.ntids, n, s.entries, s.leaf_pages, exact, pg_current_snapshot());
			ELSE
				status := 'ok'; detail := NULL;
			END IF;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;
