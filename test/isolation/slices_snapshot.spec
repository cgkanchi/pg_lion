# Snapshot semantics of lion_index_slice_agg() (DESIGN.md §51, "The SQL
# function").
#
# Every step reports the function's count, sum, min and max of the sliced
# columns next to the query they stand for, run in the same statement and so
# under the same snapshot: they must always be equal.  The absolute values
# show the snapshot behaviour: the REPEATABLE READ reader s1 keeps seeing the
# table as it was when it took its snapshot, the writer s2 sees its own
# uncommitted changes, and the READ COMMITTED reader s3 sees each committed
# change.  s2's rows take each column's extremes, so a min or a max that
# leaked them would show.  Nothing is vacuumed until the last permutation,
# whose VACUUM runs while s1's snapshot holds its dead rows.
#
# The reference queries run with the planner's lion paths off, so they are
# not answered by the code under test.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE ssnap (id int, grp int, a int4, b int8, ts timestamptz)
		WITH (autovacuum_enabled = off);
	INSERT INTO ssnap SELECT i, i % 3, (i * 37) % 1000 - 500, i * 1000003 - 400000000,
		timestamptz '2020-01-01 00:00+00' + i * interval '1 h'
	  FROM generate_series(1, 600) i;
	CREATE INDEX ssnap_grp ON ssnap USING lion (grp);
	CREATE INDEX ssnap_v ON ssnap USING lion (a, b, ts) WITH (slices = on);
	CREATE FUNCTION ssnap_both(OUT same bool, OUT lion text)
	LANGUAGE sql AS $f$
	WITH l AS (
		SELECT (SELECT format('%s %s %s %s', count, sum, min, max)
				  FROM lion_index_slice_agg('ssnap_v', 'grp = 1')) AS a,
			   (SELECT format('%s %s %s %s', count, sum, min, max)
				  FROM lion_index_slice_agg('ssnap_v', NULL, 2::int2)) AS b,
			   (SELECT format('%s %s %s', count, min, max)
				  FROM lion_index_slice_agg('ssnap_v', 'grp IN (0, 2)', 3::int2)) AS ts),
	q AS (
		SELECT (SELECT format('%s %s %s %s', count(a), sum(a), min(a), max(a))
				  FROM ssnap WHERE grp = 1) AS a,
			   (SELECT format('%s %s %s %s', count(b), sum(b), min(b), max(b))
				  FROM ssnap) AS b,
			   (SELECT format('%s %s %s', count(ts), min(ts), max(ts))
				  FROM ssnap WHERE grp IN (0, 2)) AS ts)
	SELECT (l.a, l.b, l.ts) = (q.a, q.b, q.ts), l.a || ' | ' || l.b
	  FROM l, q
	$f$;
}

teardown
{
	DROP FUNCTION ssnap_both();
	DROP TABLE ssnap;
}

# The long-lived REPEATABLE READ reader.
session s1
setup			{ SET enable_indexscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_count_pushdown = off; SET TimeZone = 'UTC'; }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_slices	{ SELECT * FROM ssnap_both(); }
step s1_commit	{ COMMIT; }

# The writer: rows at every column's extremes, an update that moves rows to
# other values, a delete.
session s2
setup			{ SET enable_indexscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_count_pushdown = off; SET TimeZone = 'UTC'; }
step s2_begin	{ BEGIN; }
step s2_write	{ INSERT INTO ssnap VALUES
					(1001, 1, 2147483647, 9223372036854775807, 'infinity'),
					(1002, 1, -2147483648, -9223372036854775808, '-infinity'),
					(1003, 0, NULL, NULL, NULL);
				  UPDATE ssnap SET a = -a, b = b * 2, grp = 1 WHERE id % 10 = 0;
				  DELETE FROM ssnap WHERE id % 7 = 3; }
step s2_slices	{ SELECT * FROM ssnap_both(); }
step s2_commit	{ COMMIT; }
step s2_rollback	{ ROLLBACK; }

# A reader that takes a fresh snapshot for every statement, and a VACUUM.
session s3
setup			{ SET enable_indexscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_count_pushdown = off; SET TimeZone = 'UTC'; }
step s3_slices	{ SELECT * FROM ssnap_both(); }
step s3_vacuum	{ VACUUM ssnap; }

# Uncommitted changes are seen by their writer only; once committed, by a new
# snapshot and never by s1's.
permutation s1_begin s1_slices s2_begin s2_write s2_slices s1_slices s3_slices
			s2_commit s1_slices s3_slices s1_commit s1_slices

# Rolled back, they are seen by no one, though the slices hold their TIDs.
permutation s2_begin s2_write s2_slices s3_slices s2_rollback s3_slices

# Committed and vacuumed while s1's snapshot still sees the old rows: VACUUM
# leaves them, and s1 still counts them through the slices.
permutation s1_begin s1_slices s2_begin s2_write s2_commit s3_vacuum
			s1_slices s3_slices s1_commit s3_vacuum s3_slices
