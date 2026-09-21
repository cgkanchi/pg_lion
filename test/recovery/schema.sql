-- Fixture and check helpers for test/recovery/run.sh.
--
-- Everything a check needs lives in the database, for two reasons: the checks
-- must run identically on the primary, on a hot standby and on a promoted
-- standby, and a standby cannot be given new functions (nor temp tables) once
-- it exists.  So the helpers are created here, before pg_basebackup, and are
-- written to be read-only: no temp tables, no DDL, no writes of any kind.
-- The multiset comparison the SQL regression tests spell as two CREATE TEMP
-- TABLEs plus EXCEPT ALL is done here over two materialised text arrays,
-- still with EXCEPT ALL in both directions.
--
-- The library is in session_preload_libraries (see run.sh), so
-- pg_lion.enable_count_pushdown exists in every session, including the
-- ones the helpers below run in.

\set ON_ERROR_STOP on
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;

/*
 * One column per operator class kind that has to survive a crash: int4, text,
 * citext, bool, a nullable int, int4[] (with NULL, '{}' and all-NULL-element
 * rows, so both reserved entries of section 14 / section 17 are populated)
 * and tsvector (with NULL and empty rows, likewise).
 */
CREATE TABLE lion_rec (
	id   bigint PRIMARY KEY,
	k4   int4   NOT NULL,
	t    text   NOT NULL,
	ct   citext NOT NULL,
	b    bool   NOT NULL,
	nn   int4,
	arr  int4[],
	tsv  tsvector
);

/*
 * The single definition of a row, shared by the initial load and by the
 * pgbench writer, so the two never disagree about what is in the table.
 *
 * t has 3000 distinct values over ~800 heap pages, i.e. about one member per
 * container key: that is what puts sparse segments (section 13) in the index
 * and what makes inserts promote a ckey out of a segment into a real
 * container once it reaches LION_SPARSE_THRESHOLD members.  k4/ct/nn/b are
 * dense enough for array, run and bitset containers.
 */
CREATE FUNCTION lion_rec_gen(i bigint) RETURNS lion_rec
LANGUAGE sql IMMUTABLE AS $$
	SELECT i,
		   (i % 200)::int4,
		   'v' || (i % 3000),
		   ('Mix' || (i % 40))::citext,
		   (i % 3 = 0),
		   CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 30)::int4 END,
		   CASE WHEN i % 13 = 0 THEN NULL					   -- NULL entry
				WHEN i % 13 = 1 THEN '{}'::int4[]			   -- EMPTY entry
				WHEN i % 13 = 2 THEN ARRAY[NULL, NULL]::int4[] -- EMPTY entry
				ELSE ARRAY[(i % 17)::int4, (i % 23)::int4,
						   CASE WHEN i % 5 = 0 THEN NULL
								ELSE (i % 7)::int4 END]
		   END,
		   CASE WHEN i % 19 = 0 THEN ''::tsvector			   -- EMPTY entry
				WHEN i % 19 = 1 THEN NULL					   -- NULL entry
				ELSE to_tsvector('simple',
								 'w' || (i % 29) || ' w' || (i % 31) ||
								 ' w' || (i % 37))
		   END
$$;

/*
 * Three indexes are created on the empty table and filled by aminsert, so
 * their entries go through INLINE -> CHAIN spills, segment promotions and
 * container-page splits under WAL; the other four are built by ambuild after
 * the load.  inline_limit = 64 forces a spill on the first few members, which
 * is what makes the split and spill records of section 4 frequent; buckets = 8
 * forces bucket-page chains.
 */
CREATE INDEX lion_rec_ct  ON lion_rec USING lion (ct)  WITH (buckets = 8);
CREATE INDEX lion_rec_b   ON lion_rec USING lion (b)   WITH (inline_limit = 64);
CREATE INDEX lion_rec_arr ON lion_rec USING lion (arr) WITH (inline_limit = 64, buckets = 16);

INSERT INTO lion_rec
SELECT g.* FROM generate_series(1, 40000) i, lion_rec_gen(i) g;

CREATE INDEX lion_rec_k4  ON lion_rec USING lion (k4) WITH (inline_limit = 64);
CREATE INDEX lion_rec_t   ON lion_rec USING lion (t);
CREATE INDEX lion_rec_nn  ON lion_rec USING lion (nn) WITH (buckets = 8);
CREATE INDEX lion_rec_tsv ON lion_rec USING lion (tsv);

/*
 * The writer's id stream.  Each pgbench client c only ever touches rows with
 * id % 8 = c, so four clients never contend for a row and the run cannot
 * produce a deadlock or a serialization failure that would abort a client.
 * The sequence hands out new ids as c + 8*n, above the loaded 1..40000 once
 * n > 5000; the writer deletes its three lowest-numbered rows instead of
 * tracking a second stream, so nothing a crash could desynchronise from the
 * table decides what gets deleted.
 *
 * A crash loses the uncommitted tail of the sequence but never replays a
 * value: nextval logs SEQ_LOG_VALS ahead of the value it returns, and a
 * committed INSERT has flushed that record, so after recovery the sequence
 * resumes at or above the last id used and the primary key cannot collide.
 */
CREATE SEQUENCE lion_rec_ins START 5001;

/* ------------------------------------------------------------------ */
-- What the checks probe.  One definition, used by the crash checks, by the
-- primary/standby comparison and by the post-promotion checks.
/* ------------------------------------------------------------------ */

/*
 * Single-key probes.  `val` is a SQL expression so cross-type and citext
 * cases can be written out in full; `direct` says whether
 * lion_index_count() accepts the key (it insists the key type match the
 * index's opcintype, which a multi-key opclass has no scalar version of).
 */
CREATE FUNCTION lion_rec_keys()
RETURNS TABLE (idx text, col text, val text, direct boolean)
LANGUAGE sql IMMUTABLE AS $$
	VALUES ('lion_rec_k4'::text, 'k4'::text, '0::int4'::text, true),
		   ('lion_rec_k4', 'k4', '1::int4', true),
		   ('lion_rec_k4', 'k4', '7::int4', true),
		   ('lion_rec_k4', 'k4', '199::int4', true),
		   ('lion_rec_k4', 'k4', '12345::int4', true),		-- absent key
		   ('lion_rec_t', 't', '''v0''::text', true),
		   ('lion_rec_t', 't', '''v13''::text', true),
		   ('lion_rec_t', 't', '''v2999''::text', true),
		   ('lion_rec_t', 't', '''nosuchvalue''::text', true),
		   ('lion_rec_ct', 'ct', '''mix0''::citext', true),	-- lower case on purpose
		   ('lion_rec_ct', 'ct', '''Mix7''::citext', true),
		   ('lion_rec_ct', 'ct', '''MIX39''::citext', true),
		   ('lion_rec_ct', 'ct', '''nosuchvalue''::citext', true),
		   ('lion_rec_b', 'b', 'true', true),
		   ('lion_rec_b', 'b', 'false', true),
		   ('lion_rec_nn', 'nn', '0::int4', true),
		   ('lion_rec_nn', 'nn', '5::int4', true),
		   ('lion_rec_nn', 'nn', '29::int4', true),
		   ('lion_rec_nn', 'nn', '-1::int4', true)			-- absent key
$$;

/*
 * Multi-row probes: GROUP BY over every column kind (including the NULL
 * group), the IS NULL / IS NOT NULL drivers of section 14, and the
 * multi-key operators of section 17.
 */
CREATE FUNCTION lion_rec_queries() RETURNS TABLE (q text)
LANGUAGE sql IMMUTABLE AS $$
	VALUES ('select k4, count(*) from lion_rec group by k4'::text),
		   ('select t, count(*) from lion_rec group by t'),
		   ('select ct, count(*) from lion_rec group by ct'),
		   ('select ct, count(*) from lion_rec where k4 = 5 group by ct'),
		   ('select b, count(*) from lion_rec group by b'),
		   ('select nn, count(*) from lion_rec group by nn'),
		   ('select k4, count(*) from lion_rec where b group by k4'),
		   ('select nn, count(*) from lion_rec where k4 = 7 group by nn'),
		   ('select b, count(*) from lion_rec where nn is null group by b'),
		   ('select count(*) from lion_rec where nn is not null'),
		   ('select count(*) from lion_rec where nn is null'),
		   ('select count(*) from lion_rec where k4 = 3 and b'),
		   ('select count(*) from lion_rec where t = ''v13'' and not b'),
		   ('select count(*) from lion_rec where ct = ''mix3''::citext'),
		   ('select count(*) from lion_rec where arr @> array[3]'),
		   ('select count(*) from lion_rec where arr && array[1,2]'),
		   ('select count(*) from lion_rec where arr @> ''{}''::int4[]'),
		   ('select count(*) from lion_rec where tsv @@ ''w1''::tsquery'),
		   ('select count(*) from lion_rec where tsv @@ ''w2 & w3''::tsquery'),
		   ('select count(*) from lion_rec where tsv @@ ''w5 | w7''::tsquery')
$$;

/* Every lion index on the table, with what its ntids must add up to. */
CREATE FUNCTION lion_rec_indexes() RETURNS TABLE (idx text, ntids_q text)
LANGUAGE sql IMMUTABLE AS $$
	VALUES
	  /* A scalar opclass stores exactly one TID per row (a NULL key goes to
	   * the reserved NULL entry, which ntids counts like any other). */
	  ('lion_rec_k4'::text, 'select count(*) from lion_rec'::text),
	  ('lion_rec_t',  'select count(*) from lion_rec'),
	  ('lion_rec_ct', 'select count(*) from lion_rec'),
	  ('lion_rec_b',  'select count(*) from lion_rec'),
	  ('lion_rec_nn', 'select count(*) from lion_rec'),
	  /* A multi-key opclass stores one TID per (key, row) pair, and one in
	   * the reserved EMPTY entry for a row it extracted no key from. */
	  ('lion_rec_arr',
	   'select coalesce(sum(greatest(1, cardinality(u))), 0) from '
	   '(select array(select distinct e from unnest(arr) x(e) where e is not null) u '
	   'from lion_rec) s'),
	  ('lion_rec_tsv',
	   'select coalesce(sum(greatest(1, coalesce(array_length(tsvector_to_array(tsv), 1), 0))), 0) '
	   'from lion_rec')
$$;

/* ------------------------------------------------------------------ */
-- The checks themselves.
/* ------------------------------------------------------------------ */

/*
 * Which node answers a query when the index path is forced.  Reported by
 * every check below, and compared between primary and standby: a standby that
 * quietly stopped using the pushdown would still produce the right numbers,
 * and the multiset comparisons would then be checking a sequential scan
 * against itself.
 */
CREATE FUNCTION lion_rec_node(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	node text := 'other';
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			node := 'pushdown';
		ELSIF node <> 'pushdown' AND ln LIKE '%Bitmap Index Scan%' THEN
			node := 'bitmap';
		END IF;
	END LOOP;
	RETURN node;
END $$;

/*
 * Run one query twice - once through the index (count pushdown on, seqscan
 * off) and once through a forced sequential scan with no index path at all -
 * and prove the two results are equal as multisets, with EXCEPT ALL in both
 * directions.  Returns 'ok ...' or a string starting with MISMATCH.
 */
CREATE FUNCTION lion_rec_qcmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text[];
	b text[];
	ndiff bigint;
	node text;
BEGIN
	node := lion_rec_node(q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	EXECUTE format('SELECT array_agg(r::text ORDER BY r::text) FROM (%s) r', q)
		INTO a;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('SELECT array_agg(r::text ORDER BY r::text) FROM (%s) r', q)
		INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);

	SELECT (SELECT count(*) FROM (SELECT unnest(a) EXCEPT ALL SELECT unnest(b)) x)
		 + (SELECT count(*) FROM (SELECT unnest(b) EXCEPT ALL SELECT unnest(a)) y)
	  INTO ndiff;

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH %s rows differ (index node = %s, %s vs %s rows): %s',
					  ndiff, node, coalesce(cardinality(a), 0),
					  coalesce(cardinality(b), 0), q);
	END IF;
	RETURN format('ok node=%s rows=%s: %s', node, coalesce(cardinality(a), 0), q);
END $$;

/*
 * One single-key probe, four ways: forced index path, forced sequential scan,
 * lion_index_count() (which reads nothing but the index and the visibility
 * map) and the count pushdown.  All four must agree.
 */
CREATE FUNCTION lion_rec_kcmp(idx text, col text, val text, direct boolean)
RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	viaidx bigint;
	viaseq bigint;
	viafun bigint;
	viapd bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	EXECUTE format('SELECT count(*) FROM lion_rec WHERE %I = %s', col, val)
		INTO viaidx;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('SELECT count(*) FROM lion_rec WHERE %I = %s', col, val)
		INTO viaseq;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	EXECUTE format('SELECT count(*) FROM lion_rec WHERE %I = %s', col, val)
		INTO viapd;

	IF direct THEN
		EXECUTE format('SELECT lion_index_count(%L::regclass, %s)', idx, val)
			INTO viafun;
	ELSE
		viafun := viaseq;
	END IF;

	IF viaidx <> viaseq OR viafun <> viaseq OR viapd <> viaseq THEN
		RETURN format('MISMATCH %s = %s: index=%s seqscan=%s count()=%s pushdown=%s',
					  col, val, viaidx, viaseq, viafun, viapd);
	END IF;
	RETURN format('ok %s = %s: %s', col, val, viaseq);
END $$;

/*
 * The whole check battery.  Returns one row per check; run.sh fails the run on
 * any row with ok = false.
 *
 *   heapallindexed  pass true to lion_index_verify (the expensive pass that
 *                   also proves every heap tuple is indexed under every key it
 *                   extracts to).
 *   exact_ntids     require lion_index_stats().ntids to equal the heap
 *                   exactly, which only holds when the table has just been
 *                   vacuumed with nothing else running; otherwise ntids may
 *                   still hold TIDs of dead tuples and only >= is required.
 */
CREATE FUNCTION lion_rec_check(heapallindexed boolean DEFAULT true,
							  exact_ntids boolean DEFAULT false)
RETURNS TABLE (ok boolean, detail text)
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	res text;
	got bigint;
	want bigint;
	nrows bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM lion_rec' INTO nrows;
	ok := true; detail := format('heap has %s rows', nrows); RETURN NEXT;

	/* 1. structural verification, and every heap tuple indexed. */
	FOR r IN SELECT * FROM lion_rec_indexes() LOOP
		BEGIN
			EXECUTE format('SELECT lion_index_verify(%L::regclass, %L)',
						   r.idx, heapallindexed);
			ok := true;
			detail := format('verify(%s, %s): ok', r.idx, heapallindexed);
		EXCEPTION WHEN OTHERS THEN
			ok := false;
			detail := format('verify(%s, %s) FAILED: %s',
							 r.idx, heapallindexed, SQLERRM);
		END;
		RETURN NEXT;
	END LOOP;

	/* 2. ntids against the heap. */
	FOR r IN SELECT * FROM lion_rec_indexes() LOOP
		EXECUTE format('SELECT ntids FROM lion_index_stats(%L::regclass)', r.idx)
			INTO got;
		EXECUTE r.ntids_q INTO want;
		IF exact_ntids THEN
			ok := (got = want);
			detail := format('ntids(%s) = %s, heap wants %s%s', r.idx, got, want,
							 CASE WHEN ok THEN '' ELSE ' MISMATCH (exact)' END);
		ELSE
			ok := (got >= want);
			detail := format('ntids(%s) = %s >= heap %s%s', r.idx, got, want,
							 CASE WHEN ok THEN '' ELSE ' MISMATCH (index is missing TIDs)' END);
		END IF;
		RETURN NEXT;
	END LOOP;

	/*
	 * 3. the reserved NULL and EMPTY entries (sections 14 and 17).  Both are
	 * key-less entries found by a flag, so a lost flag is invisible to the
	 * ntids total above: count them separately.
	 */
	FOR r IN
		SELECT * FROM (VALUES
			('lion_rec_nn null_tids'::text,
			 'select null_tids from lion_index_stats(''lion_rec_nn'')'::text,
			 'select count(*) from lion_rec where nn is null'::text),
			('lion_rec_arr null_tids',
			 'select null_tids from lion_index_stats(''lion_rec_arr'')',
			 'select count(*) from lion_rec where arr is null'),
			('lion_rec_arr empty_tids',
			 'select empty_tids from lion_index_stats(''lion_rec_arr'')',
			 'select count(*) from lion_rec where arr is not null and not exists '
			 '(select 1 from unnest(arr) e where e is not null)'),
			('lion_rec_tsv null_tids',
			 'select null_tids from lion_index_stats(''lion_rec_tsv'')',
			 'select count(*) from lion_rec where tsv is null'),
			('lion_rec_tsv empty_tids',
			 'select empty_tids from lion_index_stats(''lion_rec_tsv'')',
			 'select count(*) from lion_rec where tsv is not null and tsv = ''''::tsvector')
		) v(what, gotq, wantq)
	LOOP
		EXECUTE r.gotq INTO got;
		EXECUTE r.wantq INTO want;
		ok := (got >= want) AND (NOT exact_ntids OR got = want);
		detail := format('%s = %s, heap wants %s%s', r.what, got, want,
						 CASE WHEN ok THEN '' ELSE ' MISMATCH' END);
		RETURN NEXT;
	END LOOP;

	/* 4. single-key counts: index vs seqscan vs count() vs pushdown. */
	FOR r IN SELECT * FROM lion_rec_keys() LOOP
		res := lion_rec_kcmp(r.idx, r.col, r.val, r.direct);
		ok := res NOT LIKE 'MISMATCH%';
		detail := res;
		RETURN NEXT;
	END LOOP;

	/* 5. GROUP BY and the multi-key operators, EXCEPT ALL both ways. */
	FOR r IN SELECT * FROM lion_rec_queries() LOOP
		res := lion_rec_qcmp(r.q);
		ok := res NOT LIKE 'MISMATCH%';
		detail := res;
		RETURN NEXT;
	END LOOP;
END $$;

/*
 * A canonical, line-per-probe picture of what the index answers, for
 * comparing a standby against its primary.  Everything goes through the index:
 * lion_index_count() for the single keys, the count pushdown for the rest.
 */
CREATE FUNCTION lion_rec_probe() RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	v bigint;
	agg text;
	node text;
	n bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);

	FOR r IN SELECT * FROM lion_rec_keys() WHERE direct ORDER BY idx, val LOOP
		EXECUTE format('SELECT lion_index_count(%L::regclass, %s)', r.idx, r.val)
			INTO v;
		RETURN NEXT format('count %s %s = %s', r.idx, r.val, v);
	END LOOP;

	FOR r IN SELECT * FROM lion_rec_queries() ORDER BY q LOOP
		node := lion_rec_node(r.q);
		PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
		PERFORM set_config('enable_seqscan', 'off', true);
		EXECUTE format('SELECT count(*), coalesce(string_agg(s, E''\n'' ORDER BY s), '''') '
					   'FROM (SELECT x::text AS s FROM (%s) x) y', r.q)
			INTO n, agg;
		RETURN NEXT format('query node=%s rows=%s md5=%s : %s',
						   node, n, md5(agg), r.q);
	END LOOP;
END $$;

/*
 * Section 9's hot-standby rule, as an assertion: in recovery the count must
 * never trust the visibility map, so it skips no heap block and rechecks every
 * candidate TID.  On a quiet, freshly vacuumed table every candidate is
 * visible, so tids_rechecked must equal the count exactly.
 *
 * `recovery` says which side we are on.  On a primary the same probes must
 * show the opposite - at least one block skipped via the map - or the standby
 * assertion would be vacuously true.
 */
CREATE FUNCTION lion_rec_check_count_stats(recovery boolean)
RETURNS TABLE (ok boolean, detail text)
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	st record;
	anyskipped bigint := 0;
BEGIN
	IF recovery <> pg_is_in_recovery() THEN
		ok := false;
		detail := format('expected pg_is_in_recovery() = %s, got %s',
						 recovery, pg_is_in_recovery());
		RETURN NEXT;
		RETURN;
	END IF;

	FOR r IN SELECT * FROM lion_rec_keys() WHERE direct ORDER BY idx, val LOOP
		EXECUTE format('SELECT * FROM lion_index_count_stats(%L::regclass, %s)',
					   r.idx, r.val)
			INTO st;
		anyskipped := anyskipped + st.blocks_skipped;
		IF recovery THEN
			ok := (st.blocks_skipped = 0 AND st.tids_rechecked = st.count);
			detail := format('%s %s: count=%s blocks_skipped=%s tids_rechecked=%s%s',
							 r.idx, r.val, st.count, st.blocks_skipped,
							 st.tids_rechecked,
							 CASE WHEN ok THEN ''
								  WHEN st.blocks_skipped <> 0
								  THEN ' MISMATCH: a standby must not trust the visibility map'
								  ELSE ' MISMATCH: tids_rechecked <> count' END);
			RETURN NEXT;
		END IF;
	END LOOP;

	IF NOT recovery THEN
		ok := (anyskipped > 0);
		detail := format('primary skipped %s heap blocks via the visibility map%s',
						 anyskipped,
						 CASE WHEN anyskipped > 0 THEN ''
							  ELSE ' MISMATCH: expected the VM path to be used here' END);
		RETURN NEXT;
	END IF;
END $$;
