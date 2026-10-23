# Two SERIALIZABLE transactions each read through a nested loop whose inner
# side is a LionBtreeScan parameterized by the outer row (DESIGN.md §40.10) -
# for each key of a small table, the descent of the dimension's primary key to
# it, the entry probed against the set of the rows lion says have c = 1 (or
# c = 2), the row's values from the index tuple and the heap not read - and
# then each updates, HOT, a row the OTHER one returned.  A row returned
# without a heap visit has no tuple to predicate-lock, so the probe locks its
# heap page as core's Index Only Scan does, and this write skew must end in a
# serialization failure, as it does for the ordinary plan.  The table is
# vacuumed first, so that its pages are all-visible and the heap really is not
# read - s1_fetches shows the mode and `Heap Fetches: 0` - and the conflict
# can only be the page lock's; the fillfactor keeps the updates HOT.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE bps_d (id int PRIMARY KEY, c int, note text)
		WITH (fillfactor = 70);
	INSERT INTO bps_d SELECT i, i % 10, 'n' FROM generate_series(1, 1000) i;
	CREATE INDEX bps_d_c ON bps_d USING lion (c);
	CREATE TABLE bps_o (k int);
	INSERT INTO bps_o SELECT i FROM generate_series(1, 20) i;
	ANALYZE bps_d;
	ANALYZE bps_o;
	CREATE FUNCTION bps_fetches(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		ln text;
		r text := 'no node';
	BEGIN
		FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
			IF ln ~ 'Index:' THEN r := btrim(ln); END IF;
			IF ln ~ 'Heap Fetches' THEN r := r || '; ' || btrim(ln); END IF;
		END LOOP;
		RETURN r;
	END $$;
}
teardown
{
	DROP TABLE bps_d, bps_o;
	DROP FUNCTION bps_fetches(text);
}

session s0
step s0_vacuum	{ VACUUM bps_d; }

session s1
setup
{
	SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_memoize = off;
	SET pg_lion.pushdown_margin = 1000;
}
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT d.id FROM bps_o o JOIN bps_d d ON d.id = o.k WHERE d.c = 1; }
step s1_fetches	{ SELECT bps_fetches('SELECT d.id FROM bps_o o JOIN bps_d d ON d.id = o.k WHERE d.c = 1'); }
step s1_read	{ SELECT d.id FROM bps_o o JOIN bps_d d ON d.id = o.k WHERE d.c = 1 ORDER BY d.id; }
step s1_update	{ UPDATE bps_d SET note = 's1' WHERE id = 2; }
step s1_commit	{ COMMIT; }

session s2
setup
{
	SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_memoize = off;
	SET pg_lion.pushdown_margin = 1000;
}
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read	{ SELECT d.id FROM bps_o o JOIN bps_d d ON d.id = o.k WHERE d.c = 2 ORDER BY d.id; }
step s2_update	{ UPDATE bps_d SET note = 's2' WHERE id = 1; }
step s2_commit	{ COMMIT; }

permutation s0_vacuum s1_plan s1_fetches s1_begin s2_begin s1_read s2_read s1_update s2_update s1_commit s2_commit
permutation s0_vacuum s1_fetches s1_begin s2_begin s1_read s2_read s1_update s1_commit s2_update s2_commit
