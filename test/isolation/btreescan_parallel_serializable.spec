# The write skew of btreescan_serializable.spec, read by parallel walks
# (DESIGN.md §40.9): two SERIALIZABLE transactions each sum, through a
# Parallel LionBtreeScan in its index-only mode, the rows lion says have
# c = 1 (or c = 2) - their values from the covering btree and the heap not
# read - and then each updates, HOT, a row the OTHER one read.  Whichever
# participant returned a row took its heap page's predicate lock, a worker's
# on behalf of the leader's transaction, as core's parallel Index Only Scan
# takes them, so this write skew must end in a serialization failure as the
# serial walk's does.  The table is vacuumed first, so that its pages are
# all-visible and s1_fetches shows `Heap Fetches: 0`: the conflict can only
# be the page locks' - fewer pages than a transaction's locks on one relation
# are promoted at, so that they stay page locks.  When no worker can be
# started the leader walks alone, and the outcome is the same.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE bsp (id int PRIMARY KEY, k int, c int, note text)
		WITH (fillfactor = 70, parallel_workers = 2, autovacuum_enabled = off);
	INSERT INTO bsp SELECT i, (i * 37) % 3001, i % 10, 'n' FROM generate_series(1, 3000) i;
	CREATE INDEX bsp_k ON bsp (k) INCLUDE (id);
	CREATE INDEX bsp_c ON bsp USING lion (c);
	ANALYZE bsp;
	CREATE FUNCTION bsp_fetches(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		ln text;
		r text := 'no node';
	BEGIN
		FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF ln ~ 'Custom Scan' THEN r := btrim(regexp_replace(ln, '^[\s>-]*(.*) \(actual.*$', '\1')); END IF;
			IF ln ~ 'Index:' THEN r := r || '; ' || btrim(ln); END IF;
			IF ln ~ 'Heap Fetches' THEN r := r || '; ' || btrim(ln); END IF;
		END LOOP;
		RETURN r;
	END $$;
}
teardown
{
	DROP TABLE bsp;
	DROP FUNCTION bsp_fetches(text);
}

session s0
step s0_vacuum	{ VACUUM bsp; }

session s1
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET pg_lion.enable_plain_scan = off; SET pg_lion.pushdown_margin = 1000;
	SET pg_lion.enable_count_pushdown = off;
	SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0;
	SET parallel_tuple_cost = 0; SET min_parallel_index_scan_size = 0;
}
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT count(*), sum(id) FROM bsp WHERE c = 1; }
step s1_fetches	{ SELECT bsp_fetches('SELECT count(*), sum(id) FROM bsp WHERE c = 1'); }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_read	{ SELECT count(*), sum(id) FROM bsp WHERE c = 1; }
step s1_update	{ UPDATE bsp SET note = 's1' WHERE id = 82; }
step s1_commit	{ COMMIT; }

session s2
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET pg_lion.enable_plain_scan = off; SET pg_lion.pushdown_margin = 1000;
	SET pg_lion.enable_count_pushdown = off;
	SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0;
	SET parallel_tuple_cost = 0; SET min_parallel_index_scan_size = 0;
}
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read	{ SELECT count(*), sum(id) FROM bsp WHERE c = 2; }
step s2_update	{ UPDATE bsp SET note = 's2' WHERE id = 191; }
step s2_commit	{ COMMIT; }

permutation s0_vacuum s1_plan s1_fetches s1_begin s2_begin s1_read s2_read s1_update s2_update s1_commit s2_commit
permutation s0_vacuum s1_begin s2_begin s1_read s2_read s1_update s1_commit s2_update s2_commit
