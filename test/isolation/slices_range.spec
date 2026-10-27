# Snapshot semantics of ranges on sliced columns answered from their slices
# by the LionCount node (DESIGN.md §53).
#
# Every step reports counts under ranges on the sliced columns - folded from
# the slices into a collected copy, counted beside the column's existence
# slice - and sum(x) under a range on x (§52's aggregates over it), beside
# the same counts of expressions of the columns (`a + 0 > c`), which no
# index answers and which the ordinary plan computes from the rows, in the
# same statement and so under the same snapshot: they must always be equal.
# The absolute values show the snapshot behaviour: the REPEATABLE READ reader
# s1 keeps seeing the table as it was when it took its snapshot, the writer
# s2 sees its own uncommitted changes, and the READ COMMITTED reader s3 sees
# each committed change.  s2's rows take each column's extremes and move
# rows across the ranges' bounds, so a range that leaked them would show.
# Nothing is vacuumed until the last permutation, whose VACUUM runs while
# s1's snapshot holds its dead rows.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE srg (id int, grp int, a int4, b int8, ts timestamptz)
		WITH (autovacuum_enabled = off);
	INSERT INTO srg SELECT i, i % 3, (i * 37) % 1000 - 500, i::int8 * 1000003 - 400000000,
		timestamptz '2020-01-01 00:00+00' + i * interval '1 h'
	  FROM generate_series(1, 6000) i;
	CREATE INDEX srg_grp ON srg USING lion (grp);
	CREATE INDEX srg_v ON srg USING lion (a, b, ts) WITH (slices = on);
	CREATE FUNCTION srg_both(OUT same bool, OUT lion text)
	LANGUAGE sql AS $f$
	WITH l AS (
		SELECT (SELECT count(*) FROM srg WHERE a > 0) AS a,
			   (SELECT count(*) FROM srg WHERE b BETWEEN 0 AND 3000000000) AS b,
			   (SELECT count(*) FROM srg WHERE ts < '2020-03-01' AND grp = 1) AS ts,
			   (SELECT count(*) FROM srg WHERE a <> 0 AND a < 400) AS ne,
			   (SELECT format('%s %s %s', sum(a), min(a), max(a))
				  FROM srg WHERE a > 100) AS s,
			   (SELECT string_agg(format('%s: %s', grp, n), ', ' ORDER BY grp)
				  FROM (SELECT grp, count(*) AS n FROM srg
						 WHERE b < 0 OR a > 450 GROUP BY grp) g) AS g),
	q AS (
		SELECT (SELECT count(*) FROM srg WHERE a + 0 > 0) AS a,
			   (SELECT count(*) FROM srg WHERE b + 0 BETWEEN 0 AND 3000000000) AS b,
			   (SELECT count(*) FROM srg
				 WHERE ts + interval '0 s' < '2020-03-01' AND grp + 0 = 1) AS ts,
			   (SELECT count(*) FROM srg WHERE a + 0 <> 0 AND a + 0 < 400) AS ne,
			   (SELECT format('%s %s %s', sum(a + 0), min(a + 0), max(a + 0))
				  FROM srg WHERE a + 0 > 100) AS s,
			   (SELECT string_agg(format('%s: %s', grp, n), ', ' ORDER BY grp)
				  FROM (SELECT grp, count(*) AS n FROM srg
						 WHERE b + 0 < 0 OR a + 0 > 450 GROUP BY grp) g) AS g)
	SELECT (l.a, l.b, l.ts, l.ne, l.s, l.g) = (q.a, q.b, q.ts, q.ne, q.s, q.g),
		   format('%s %s %s %s | %s | %s', l.a, l.b, l.ts, l.ne, l.s, l.g)
	  FROM l, q
	$f$;
}
setup { VACUUM (FREEZE, ANALYZE) srg; }

teardown
{
	DROP FUNCTION srg_both();
	DROP TABLE srg;
}

# The long-lived REPEATABLE READ reader, which also shows that the node
# answers its ranges from the slices.
session s1
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off;
				  SET pg_lion.prefer_slice_ranges = on; SET TimeZone = 'UTC'; }
step s1_explain	{ EXPLAIN (COSTS OFF)
				  SELECT count(*) FROM srg WHERE b BETWEEN 0 AND 3000000000;
				  EXPLAIN (COSTS OFF)
				  SELECT sum(a), min(a), max(a) FROM srg WHERE a > 100;
				  EXPLAIN (COSTS OFF)
				  SELECT grp, count(*) FROM srg WHERE b < 0 OR a > 450 GROUP BY grp; }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_ranges	{ SELECT * FROM srg_both(); }
step s1_commit	{ COMMIT; }

# The writer: rows at every column's extremes, an update that moves rows
# across the ranges' bounds, a delete.
session s2
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off;
				  SET pg_lion.prefer_slice_ranges = on; SET TimeZone = 'UTC'; }
step s2_begin	{ BEGIN; }
step s2_write	{ INSERT INTO srg VALUES
					(10001, 1, 2147483647, 9223372036854775807, 'infinity'),
					(10002, 1, -2147483648, -9223372036854775808, '-infinity'),
					(10003, 0, NULL, NULL, NULL),
					(10004, 1, 0, 0, '2020-01-15');
				  UPDATE srg SET a = -a, b = -b, grp = 1 WHERE id % 10 = 0;
				  DELETE FROM srg WHERE id % 7 = 3; }
step s2_ranges	{ SELECT * FROM srg_both(); }
step s2_commit	{ COMMIT; }
step s2_rollback	{ ROLLBACK; }

# A reader that takes a fresh snapshot for every statement, and a VACUUM.
session s3
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off;
				  SET pg_lion.prefer_slice_ranges = on; SET TimeZone = 'UTC'; }
step s3_ranges	{ SELECT * FROM srg_both(); }
step s3_vacuum	{ VACUUM srg; }

# Uncommitted changes are seen by their writer only; once committed, by a new
# snapshot and never by s1's.
permutation s1_explain s1_begin s1_ranges s2_begin s2_write s2_ranges
			s1_ranges s3_ranges s2_commit s1_ranges s3_ranges s1_commit
			s1_ranges

# Rolled back, they are seen by no one, though the slices hold their TIDs.
permutation s2_begin s2_write s2_ranges s3_ranges s2_rollback s3_ranges

# Committed and vacuumed while s1's snapshot still sees the old rows: VACUUM
# leaves them, and s1 still counts them through the slices.
permutation s1_begin s1_ranges s2_begin s2_write s2_commit s3_vacuum
			s1_ranges s3_ranges s1_commit s3_vacuum s3_ranges
