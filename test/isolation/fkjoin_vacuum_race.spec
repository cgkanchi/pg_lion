# The FK-side join's semi join and its COLLECTED fact filters racing a VACUUM
# (DESIGN.md §27).  The node copies the rows of the fact filters into private
# memory once, before the first dimension row is counted, and every count then
# intersects a dimension row's fk set with that copy.  The copy holds no pin,
# so it may list rows a VACUUM has removed since; it is safe because it is only
# ever counted beside the fk set, which is located under its own pin and
# carries the §9 interlock - exactly the argument a materialized WHERE set
# rests on.  This is count_distinct_vacuum_race.spec for that copy.
#
# The fact table is laid out so that each key's rows have heap pages of their
# own: fk = 0 is ids 1-1000, fk = 1 ids 1001-2000, and so on; x has 20 values.
#
#  * s2_predel deletes every row of fk = 0 and commits before s1 takes its
#    snapshot, so they are dead to everyone and VACUUM may remove them - and,
#    having removed them, mark their pages all-visible.
#  * s2_delete then deletes every row of fk = 2 and commits after s1's
#    snapshot, so s1 still sees them: their pages are dirty and go through the
#    heap recheck, under s1's snapshot.
#  * fk = 1 and fk = 3 are on all-visible pages and are settled from the map.
#
# The copy is made first, and it lists fk = 0's dead rows (nothing has
# vacuumed them out of the x index yet).  The dimension is read in key order,
# so the FIRST container a count puts through the visibility map is fk = 0's,
# and s1 parks there, on the existing injection point
# 'lion-count-containers-pinned', with the page of fk = 0's set pinned.  A
# VACUUM started now must wait for the pin (fj_wait_for_vacuum() asserts it);
# had it not, it could remove fk = 0's TIDs and set their pages all-visible
# before s1 asks the map about the containers it holds - its own copy of the
# fk set's and the collected copy of the filter's - and s1 would count fk = 0:
# 4 dimension rows with a match instead of 3.  Making the copy asks nothing of
# the map, and never stops at the point.
#
# s2_wakeup DETACHES the point before it wakes s1, because the counts reach it
# once per container and only the first one is the interesting one; the rest
# of the run then races the VACUUM freely, and the answer must be exact
# however that race goes.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE fj_d (pk int PRIMARY KEY);
	INSERT INTO fj_d SELECT i FROM generate_series(0, 3) i;
	CREATE TABLE fj_f (id int, fk int NOT NULL, x int NOT NULL);
	INSERT INTO fj_f SELECT i, (i - 1) / 1000, i % 20 FROM generate_series(1, 4000) i;
	CREATE INDEX fj_f_fk ON fj_f USING lion (fk);
	CREATE INDEX fj_f_x ON fj_f USING lion (x);
	ANALYZE fj_d;
	ANALYZE fj_f;

	CREATE FUNCTION fj_wait_for_vacuum() RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	DECLARE
		waited boolean := false;
		active boolean;
		seen boolean := false;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			/* pg_stat_activity is otherwise read once per transaction */
			PERFORM pg_stat_clear_snapshot();
			SELECT count(*) FILTER (WHERE wait_event IN ('BufferCleanup', 'BufferPin')) > 0,
				   count(*) > 0
			  INTO waited, active
			  FROM pg_stat_activity
			 WHERE datname = current_database()
			   AND pid <> pg_backend_pid()
			   AND state = 'active'
			   AND query LIKE 'VACUUM%';
			EXIT WHEN waited;
			seen := seen OR active;
			/* the VACUUM came and went without ever waiting for the pin */
			EXIT WHEN seen AND NOT active;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN waited;
	END $fn$;
}

teardown
{
	DROP FUNCTION fj_wait_for_vacuum();
	DROP TABLE fj_f, fj_d;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-containers-pinned');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The counting session.  Only the pushdown's steps run into the point: the
# reference counts turn it off.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_hashjoin = off;
	SET enable_mergejoin = off;
	SET enable_nestloop = off;
	-- the race is the collected copy's: on a table this small the model may
	-- probe the fact filters instead
	SET pg_lion.fkjoin_count_cost = 1e6;
	-- and the upper node's: a count over the semi join path is not it
	SET pg_lion.enable_semijoin = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_plan	{
	EXPLAIN (COSTS OFF) SELECT count(*) FROM fj_d d WHERE EXISTS (SELECT 1 FROM fj_f f WHERE f.fk = d.pk AND f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19));
	EXPLAIN (COSTS OFF) SELECT count(*) FROM fj_f f JOIN fj_d d ON f.fk = d.pk WHERE f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19);
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_plain	{
	SET LOCAL pg_lion.enable_count_pushdown = off;
	SELECT count(*) FROM fj_d d WHERE EXISTS (SELECT 1 FROM fj_f f WHERE f.fk = d.pk AND f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19));
	SELECT count(*) FROM fj_f f JOIN fj_d d ON f.fk = d.pk WHERE f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19);
	SET LOCAL pg_lion.enable_count_pushdown = on;
}
step s1_semi	{ SELECT count(*) FROM fj_d d WHERE EXISTS (SELECT 1 FROM fj_f f WHERE f.fk = d.pk AND f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19)); }
step s1_join	{ SELECT count(*) FROM fj_f f JOIN fj_d d ON f.fk = d.pk WHERE f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19); }
step s1_commit	{ COMMIT; }

# The writer, which also releases s1 from the injection point.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_predel	{ DELETE FROM fj_f WHERE fk = 0; }
step s2_delete	{ DELETE FROM fj_f WHERE fk = 2; }
# Assert the wait, then stop the point from firing again and release s1.
step s2_wakeup	{
	SELECT fj_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_detach('lion-count-containers-pinned');
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}

# The vacuuming session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM fj_f; }
step s3_vacuum	{ VACUUM fj_f; }
step s3_count	{
	SELECT count(*) FROM fj_d d WHERE EXISTS (SELECT 1 FROM fj_f f WHERE f.fk = d.pk AND f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19));
	SELECT count(*) FROM fj_f f JOIN fj_d d ON f.fk = d.pk WHERE f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19);
}
step s3_pushed	{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_hashjoin = off;
	SET enable_mergejoin = off;
	SET enable_nestloop = off;
	SELECT count(*) FROM fj_d d WHERE EXISTS (SELECT 1 FROM fj_f f WHERE f.fk = d.pk AND f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19));
	SELECT count(*) FROM fj_f f JOIN fj_d d ON f.fk = d.pk WHERE f.x IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19);
	RESET enable_hashjoin;
	RESET enable_mergejoin;
	RESET enable_nestloop;
	RESET pg_lion.enable_count_pushdown;
}
step s3_verify	{
	SELECT lion_index_verify('fj_f_fk', true);
	SELECT lion_index_verify('fj_f_x', true);
}

# The semi join: VACUUM waits for the pin on fk = 0's set, and 3 dimension
# rows have a match.
permutation
	s3_prep					# make the heap all-visible
	s1_plan					# the plans are the pushdown's, and collect
	s2_predel				# fk = 0: rows nobody can see any more
	s1_begin s1_plain		# s1's snapshot: 3 rows, 1500 pairs
	s2_delete				# fk = 2, committed after s1's snapshot
	s1_semi					# parks with fk = 0's container pinned
	s3_vacuum(*, s1_semi)	# blocks for the cleanup lock; cannot finish
							# before s1_semi lets go of the pin
	s2_wakeup				# asserts the wait, then releases s1
	s1_commit
	s3_count				# 2 rows and 1000 pairs now: fk = 1 and fk = 3
	s3_pushed				# and the same through the pushdown
	s3_verify

# The inner join's counts: the same race, the same copy, 1500 pairs.
permutation
	s3_prep
	s2_predel
	s1_begin s1_plain
	s2_delete
	s1_join					# parks with fk = 0's container pinned
	s3_vacuum(*, s1_join)
	s2_wakeup
	s1_commit
	s3_count
	s3_pushed
	s3_verify
