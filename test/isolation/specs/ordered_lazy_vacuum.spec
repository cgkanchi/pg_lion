# A LionOrdered scan that starts its set lazily (DESIGN.md §30.4, "The set,
# lazily") meets dead members, VACUUM removes them while the scan is paused,
# and the scan then builds its set.
#
# c = 1 holds 50 live rows at the front of the k order, 9,900 deleted rows
# behind them, and 1,950 live rows at the back.  The cursor's LIMIT is a small
# share of the rows the planner expects, so the node probes the lion sets per
# container key met instead of building its set first, and counts every TID
# the probes answer for as a member met: the 50 live ones and the 9,900 dead
# ones, which the index still holds.  The cursor pauses after 60 rows, with
# the walk short of the 10,000 entries that make it build the set; VACUUM
# removes the dead TIDs from both indexes - the paused node holds no pin
# (§29.5) - and the next FETCH builds the set from the index as it is now,
# with 2,000 members.  The members met have to be counted again against that
# set: carried over, 9,960 met against 2,000 members would stop the scan as
# having met them all, and the cursor would end 200 rows short of its LIMIT.
setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE lz (id int, k int, c int) WITH (autovacuum_enabled = off);
	INSERT INTO lz SELECT i, i, 1 FROM generate_series(1, 50) i;
	INSERT INTO lz SELECT 10000 + i, 1000 + i, 1 FROM generate_series(1, 9900) i;
	INSERT INTO lz SELECT 20000 + i, 100000 + i, 1 FROM generate_series(1, 1950) i;
	INSERT INTO lz SELECT 30000 + i, 200000 + i, 0 FROM generate_series(1, 2000) i;
	CREATE INDEX lz_k ON lz (k);
	CREATE INDEX lz_c ON lz USING lion (c);
	DELETE FROM lz WHERE id BETWEEN 10001 AND 19900;
	ANALYZE lz;
	CREATE TABLE lz_got (n serial, id int, k int);

	/* Move up to n rows (all of them for NULL) of the cursor into lz_got. */
	CREATE FUNCTION lz_take(n int) RETURNS bigint
	LANGUAGE plpgsql AS $fn$
	DECLARE
		c refcursor := 'lz_cur';
		r record;
		got bigint := 0;
	BEGIN
		LOOP
			EXIT WHEN n IS NOT NULL AND got >= n;
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			INSERT INTO lz_got (id, k) VALUES (r.id, r.k);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}
teardown
{
	DROP FUNCTION lz_take(int);
	DROP TABLE lz, lz_got;
}

session s1
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off; SET enable_indexonlyscan = off;
}
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id, k FROM lz WHERE c = 1 ORDER BY k LIMIT 300; }
step s1_begin	{ BEGIN; }
step s1_declare	{ DECLARE lz_cur NO SCROLL CURSOR FOR SELECT id, k FROM lz WHERE c = 1 ORDER BY k LIMIT 300; }
step s1_take60	{ SELECT lz_take(60); }
step s1_rest	{ SELECT lz_take(NULL); }
step s1_commit	{ COMMIT; }
# the cursor's rows against the same query's answer by a sequential scan
step s1_check
{
	SET enable_seqscan = on;
	SET pg_lion.enable_ordered_scan = off;
	SET pg_lion.enable_btree_scan = off;
	SELECT (SELECT count(*) FROM lz_got) AS got,
		   (SELECT count(*) FROM lz_got g
			 FULL JOIN (SELECT row_number() OVER (ORDER BY k) AS n, id, k
						  FROM (SELECT id, k FROM lz WHERE c = 1 ORDER BY k LIMIT 300) q) e
			 USING (n)
			WHERE g.id IS DISTINCT FROM e.id OR g.k IS DISTINCT FROM e.k) AS differ;
}

session s2
step s2_vacuum	{ VACUUM lz; }

permutation s1_plan s1_begin s1_declare s1_take60 s2_vacuum s1_rest s1_commit s1_check
