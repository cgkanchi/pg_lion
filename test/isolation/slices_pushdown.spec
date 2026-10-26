# Snapshot semantics of the aggregates of sliced columns answered from their
# slices by the LionCount node (DESIGN.md §52).
#
# Every step reports count, sum, min and max of the sliced columns under a
# WHERE, and of each group of a GROUP BY, as the node answers them next to the same aggregates of expressions
# of the columns (`a + 0`), which no slice answers and which the ordinary
# plan computes from the rows - in the same statement, and so under the same
# snapshot: they must always be equal.  The absolute values show the
# snapshot behaviour: the REPEATABLE READ reader s1 keeps seeing the table
# as it was when it took its snapshot, the writer s2 sees its own uncommitted
# changes, and the READ COMMITTED reader s3 sees each committed change.  s2's
# rows take each column's extremes, so a min or a max that leaked them would
# show.  Nothing is vacuumed until the last permutation, whose VACUUM runs
# while s1's snapshot holds its dead rows.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE spd (id int, grp int, a int4, b int8, ts timestamptz)
		WITH (autovacuum_enabled = off);
	INSERT INTO spd SELECT i, i % 3, (i * 37) % 1000 - 500, i::int8 * 1000003 - 400000000,
		timestamptz '2020-01-01 00:00+00' + i * interval '1 h'
	  FROM generate_series(1, 6000) i;
	CREATE INDEX spd_grp ON spd USING lion (grp);
	CREATE INDEX spd_v ON spd USING lion (a, b, ts) WITH (slices = on);
	-- the GROUP BY's groups one at a time, as the decode route is the
	-- cheaper here; STABLE, so that it reads its caller's snapshot
	CREATE FUNCTION spd_pairs() RETURNS text
	LANGUAGE sql STABLE SET pg_lion.enable_slice_decode = off AS $f$
	SELECT string_agg(format('%s: %s %s %s', grp, s, mn, mx), ', ' ORDER BY grp)
	  FROM (SELECT grp, sum(a) AS s, min(b) AS mn, max(ts) AS mx
			  FROM spd GROUP BY grp) g
	$f$;
	CREATE FUNCTION spd_both(OUT same bool, OUT lion text)
	LANGUAGE sql AS $f$
	WITH l AS (
		SELECT (SELECT format('%s %s %s %s', count(a), sum(a), min(a), max(a))
				  FROM spd WHERE grp = 1) AS a,
			   (SELECT format('%s %s %s %s', count(b), sum(b), min(b), max(b))
				  FROM spd WHERE grp IN (0, 1)) AS b,
			   (SELECT format('%s %s %s', count(ts), min(ts), max(ts))
				  FROM spd WHERE grp <> 1) AS ts,
			   (SELECT string_agg(format('%s: %s %s %s', grp, s, mn, mx), ', '
								  ORDER BY grp)
				  FROM (SELECT grp, sum(a) AS s, min(b) AS mn, max(ts) AS mx
						  FROM spd GROUP BY grp) g) AS g,
			   spd_pairs() AS gp),
	q AS (
		SELECT (SELECT format('%s %s %s %s', count(a + 0), sum(a + 0),
							  min(a + 0), max(a + 0))
				  FROM spd WHERE grp = 1) AS a,
			   (SELECT format('%s %s %s %s', count(b + 0), sum(b + 0),
							  min(b + 0), max(b + 0))
				  FROM spd WHERE grp IN (0, 1)) AS b,
			   (SELECT format('%s %s %s', count(ts + interval '0 s'),
							  min(ts + interval '0 s'), max(ts + interval '0 s'))
				  FROM spd WHERE grp <> 1) AS ts,
			   (SELECT string_agg(format('%s: %s %s %s', grp, s, mn, mx), ', '
								  ORDER BY grp)
				  FROM (SELECT grp, sum(a + 0) AS s, min(b + 0) AS mn,
							   max(ts + interval '0 s') AS mx
						  FROM spd GROUP BY grp) g) AS g)
	SELECT (l.a, l.b, l.ts, l.g, l.gp) = (q.a, q.b, q.ts, q.g, q.g),
		   l.a || ' | ' || l.b
	  FROM l, q
	$f$;
}
setup { VACUUM (FREEZE, ANALYZE) spd; }

teardown
{
	DROP FUNCTION spd_both();
	DROP FUNCTION spd_pairs();
	DROP TABLE spd;
}

# The long-lived REPEATABLE READ reader, which also shows that the node
# answers its aggregates from the slices.
session s1
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off; SET TimeZone = 'UTC'; }
step s1_explain	{ EXPLAIN (COSTS OFF)
				  SELECT count(a), sum(a), min(a), max(a) FROM spd WHERE grp = 1;
				  EXPLAIN (COSTS OFF)
				  SELECT grp, sum(a), min(b), max(ts) FROM spd GROUP BY grp;
				  SET pg_lion.enable_slice_decode = off;
				  EXPLAIN (COSTS OFF)
				  SELECT grp, sum(a), min(b), max(ts) FROM spd GROUP BY grp;
				  RESET pg_lion.enable_slice_decode; }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_slices	{ SELECT * FROM spd_both(); }
step s1_commit	{ COMMIT; }

# The writer: rows at every column's extremes, an update that moves rows to
# other values, a delete.
session s2
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off; SET TimeZone = 'UTC'; }
step s2_begin	{ BEGIN; }
step s2_write	{ INSERT INTO spd VALUES
					(10001, 1, 2147483647, 9223372036854775807, 'infinity'),
					(10002, 1, -2147483648, -9223372036854775808, '-infinity'),
					(10003, 0, NULL, NULL, NULL);
				  UPDATE spd SET a = -a, b = b * 2, grp = 1 WHERE id % 10 = 0;
				  DELETE FROM spd WHERE id % 7 = 3; }
step s2_slices	{ SELECT * FROM spd_both(); }
step s2_commit	{ COMMIT; }
step s2_rollback	{ ROLLBACK; }

# A reader that takes a fresh snapshot for every statement, and a VACUUM.
session s3
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off; SET TimeZone = 'UTC'; }
step s3_slices	{ SELECT * FROM spd_both(); }
step s3_vacuum	{ VACUUM spd; }

# Uncommitted changes are seen by their writer only; once committed, by a new
# snapshot and never by s1's.
permutation s1_explain s1_begin s1_slices s2_begin s2_write s2_slices
			s1_slices s3_slices s2_commit s1_slices s3_slices s1_commit
			s1_slices

# Rolled back, they are seen by no one, though the slices hold their TIDs.
permutation s2_begin s2_write s2_slices s3_slices s2_rollback s3_slices

# Committed and vacuumed while s1's snapshot still sees the old rows: VACUUM
# leaves them, and s1 still counts them through the slices.
permutation s1_begin s1_slices s2_begin s2_write s2_commit s3_vacuum
			s1_slices s3_slices s1_commit s3_vacuum s3_slices
