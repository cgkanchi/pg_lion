# The aggregates over keys of DESIGN.md §37 on a heap with a page that is not
# all-visible, and a write between the walk and its second look at the map.
#
# One page not all-visible (s2_dirty, before s1's snapshot) sends the walks
# past the dirty pages: an entry with a member under a container key that
# holds a page not all-visible is counted, and every other entry's rows are
# its ntids - which is right only if no clean key had a change while the
# walks ran.  The injection point "lion-wagg-dirty-walked" parks s1 after the
# walks and before the second look, and s2_insert puts rows after s1's
# snapshot onto the heap's last page and past it.  Their keys' pages are no
# longer all-visible at the second look, so the walks are thrown away and
# made again, each entry's map asked under its leaf's pin: s1 sees its
# snapshot's rows, which the sequential scan in the same transaction
# confirms, and none of s2's.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE wd (id int, u int8, w int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO wd SELECT g, g % 5000, g % 7, 'p' FROM generate_series(1, 20000) g;
	CREATE INDEX wd_u ON wd USING lion (u);
	CREATE INDEX wd_w ON wd USING lion (w);

	CREATE FUNCTION wd_walks() RETURNS SETOF text
	LANGUAGE plpgsql AS $fn$
	DECLARE
		ln text;
	BEGIN
		FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
						  'SELECT sum(u), avg(w), max(u) FROM wd' LOOP
			IF ln ~ '(Aggregates Over Keys|Key Walks)' THEN
				RETURN NEXT btrim(ln);
			END IF;
		END LOOP;
	END $fn$;
}

teardown
{
	DROP FUNCTION wd_walks();
	DROP TABLE wd;
}

session s1
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SET max_parallel_workers_per_gather = 0;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-wagg-dirty-walked', 'wait');
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_snap	{ SELECT count(*) FROM wd WHERE id < 0; }
step s1_walks	{ SELECT * FROM wd_walks(); }
step s1_aggs	{ SELECT sum(u), avg(w), max(u) FROM wd; }
step s1_seq		{
	SET LOCAL pg_lion.enable_count_pushdown = off;
	SET LOCAL enable_seqscan = on;
	SELECT sum(u), avg(w), max(u) FROM wd;
}
step s1_commit	{ COMMIT; }

session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_vacuum	{ VACUUM wd; }
step s2_dirty	{ UPDATE wd SET pad = 'q' WHERE id = 1; }
step s2_insert	{ INSERT INTO wd SELECT g, 1000000 + g, 100, 'n' FROM generate_series(1, 300) g; }
step s2_wakeup	{
	SELECT injection_points_detach('lion-wagg-dirty-walked');
	SELECT injection_points_wakeup('lion-wagg-dirty-walked');
}

# No write while s1 is parked: the walks past the dirty page stand.
permutation
	s2_vacuum s2_dirty s1_begin s1_snap
	s1_walks
	s2_wakeup
	s1_aggs s1_seq s1_commit

permutation
	s2_vacuum				# every page all-visible
	s2_dirty				# one not, before s1's snapshot
	s1_begin s1_snap		# s1's snapshot
	s1_walks				# parks after the walks past the dirty page
	s2_insert				# rows s1 must not see, on clean keys' pages
	s2_wakeup				# the second look finds them: walked again
	s1_aggs					# the snapshot's answer
	s1_seq					# the same, read from the heap
	s1_commit
