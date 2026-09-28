# A count pushdown paused in a cursor, and one that has returned its last row,
# while VACUUM runs (DESIGN.md §15, "Paused and finished counts").
#
# The WHERE sets are located once per scan, and an INLINE one keeps its
# directory leaf pinned while it is counted (§9).  They used to keep those
# pins between rows and after the last one, so VACUUM - which takes a cleanup
# lock on every leaf whose INLINE entries it rewrites (§11) - waited for the
# client to close the cursor; the isolation tester would wait on it until it
# timed out.  Now a paused node holds its sets as NOPIN copies and a finished
# one holds nothing, and the VACUUM steps have to COMPLETE.
#
#  * group: a GROUP BY g WHERE c = 1 parks after its first group.  The rows it
#    no longer sees were deleted before it started, so VACUUM removes their
#    TIDs from c's INLINE entry, frees their slots and marks the pages
#    all-visible, and an insert takes the slots back under c = 1 and every g.
#    The copy of c = 1 the cursor holds still lists the old TIDs, and the
#    rest of its groups must still be exactly what its snapshot sees: the
#    group's own set, located afresh under its pin, carries each count.
#  * count: a plain count, done after its one row.
#
# No injection point is needed - a cursor IS the pause - so this runs on
# every supported major.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS pg_buffercache;

	CREATE TABLE cpv (id int, g int NOT NULL, c int NOT NULL)
		WITH (autovacuum_enabled = off);
	INSERT INTO cpv SELECT i, i % 4, i % 10 FROM generate_series(1, 4000) i;
	CREATE INDEX cpv_g ON cpv USING lion (g);
	CREATE INDEX cpv_c ON cpv USING lion (c);

	CREATE TABLE cpv_got (g int, n bigint);

	/* Move up to n rows (all of them for NULL) of the cursor into cpv_got. */
	CREATE FUNCTION cpv_take(n int) RETURNS bigint
	LANGUAGE plpgsql AS $fn$
	DECLARE
		c refcursor := 'cpv_cur';
		r record;
		got bigint := 0;
	BEGIN
		LOOP
			EXIT WHEN n IS NOT NULL AND got >= n;
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			INSERT INTO cpv_got VALUES (r.g, r.count);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}
setup { VACUUM ANALYZE cpv; }

teardown
{
	DROP FUNCTION cpv_take(int);
	DROP TABLE cpv, cpv_got;
}

# The reader: the pushdown in a cursor, REPEATABLE READ so that the expected
# answer is computed under the very snapshot the cursor has.
session s1
setup
{
	LOAD 'pg_lion';
	SET pg_lion.enable_count_pushdown = on;
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
	TRUNCATE cpv_got;
}
step s1_group	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE cpv_cur CURSOR FOR SELECT g, count(*) FROM cpv WHERE c = 1 GROUP BY g;
	SELECT cpv_take(1) AS first;
}
step s1_count	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE cpv_cnt CURSOR FOR SELECT count(*) FROM cpv WHERE c = 1 AND g = 1;
	FETCH 1 FROM cpv_cnt;
}
step s1_rest	{ SELECT cpv_take(NULL) AS rest; }
# The answer against a sequential scan under the same snapshot.
step s1_check	{
	SET LOCAL enable_seqscan = on;
	SET LOCAL pg_lion.enable_count_pushdown = off;
	SELECT (SELECT count(*) FROM cpv_got) AS groups,
		   (SELECT count(*) FROM
			((SELECT g, n FROM cpv_got
			  EXCEPT ALL SELECT g, count(*) FROM cpv WHERE c = 1 GROUP BY g)
			 UNION ALL
			 (SELECT g, count(*) FROM cpv WHERE c = 1 GROUP BY g
			  EXCEPT ALL SELECT g, n FROM cpv_got)) d) AS wrong;
	COMMIT;
}
step s1_commit	{ COMMIT; }

# The writer.
session s2
step s2_delete	{ DELETE FROM cpv WHERE c = 1 AND id <= 2000; }
step s2_reuse	{ INSERT INTO cpv SELECT 10000 + i, i % 4, 1 FROM generate_series(1, 2000) i; }

# The vacuum.
session s3
# What the paused or finished node holds of the indexes: nothing.
step s3_pins	{
	SELECT count(*) AS lion_pages_pinned FROM pg_buffercache
	 WHERE reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	   AND relfilenode IN (pg_relation_filenode('cpv_g'), pg_relation_filenode('cpv_c'))
	   AND pinning_backends > 0;
}
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) cpv; }

permutation s2_delete s1_group s3_pins s3_vacuum s2_reuse s1_rest s1_check
permutation s2_delete s1_count s3_pins s3_vacuum s1_commit
