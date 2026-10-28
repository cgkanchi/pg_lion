# Snapshot semantics of the aggregates of sliced columns over a PARTITIONED
# table, answered by one LionCount node from every partition's own slices
# and added up across the partitions (DESIGN.md §52, "Partitioned tables").
#
# Every step reports count, sum, min and max of the sliced columns under a
# WHERE, as the node answers them, next to the same aggregates of
# expressions of the columns (`a + 0`), which no slice answers and which
# the ordinary plan computes from the rows - in the same statement, and so
# under the same snapshot: they must always be equal.  The REPEATABLE READ
# reader s1 keeps seeing the table as it was when it took its snapshot, the
# writer s2 sees its own uncommitted changes, and the READ COMMITTED reader
# s3 sees each committed change.  s2's rows take each column's extremes in
# different partitions, its updates move rows from one partition to another
# (a delete there and an insert here), and its deletes empty a partition of
# the WHERE's rows; the last permutation VACUUMs while s1's snapshot holds
# the dead rows, each partition's heap and indexes on their own.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE sptp (id int, grp int, a int4, b int8, ts timestamptz)
		PARTITION BY RANGE (id);
	CREATE TABLE sptp_1 PARTITION OF sptp FOR VALUES FROM (0) TO (2000)
		WITH (autovacuum_enabled = off);
	CREATE TABLE sptp_2 PARTITION OF sptp FOR VALUES FROM (2000) TO (4000)
		WITH (autovacuum_enabled = off);
	CREATE TABLE sptp_3 PARTITION OF sptp FOR VALUES FROM (4000) TO (20000)
		WITH (autovacuum_enabled = off);
	INSERT INTO sptp SELECT i, i % 3, (i * 37) % 1000 - 500,
		i::int8 * 1000003 - 400000000,
		timestamptz '2020-01-01 00:00+00' + i * interval '1 h'
	  FROM generate_series(1, 6000) i;
	CREATE INDEX sptp_grp ON sptp USING lion (grp);
	CREATE INDEX sptp_v ON sptp USING lion (a, b, ts) WITH (slices = on);
	CREATE FUNCTION sptp_both(OUT same bool, OUT lion text)
	LANGUAGE sql AS $f$
	WITH l AS (
		SELECT (SELECT format('%s %s %s %s', count(a), sum(a), min(a), max(a))
				  FROM sptp WHERE grp = 1) AS a,
			   (SELECT format('%s %s %s %s %s', count(b), sum(b), avg(b),
							  min(b), max(b))
				  FROM sptp WHERE grp IN (0, 1)) AS b,
			   (SELECT format('%s %s %s', count(ts), min(ts), max(ts))
				  FROM sptp WHERE grp <> 1) AS ts),
	q AS (
		SELECT (SELECT format('%s %s %s %s', count(a + 0), sum(a + 0),
							  min(a + 0), max(a + 0))
				  FROM sptp WHERE grp = 1) AS a,
			   (SELECT format('%s %s %s %s %s', count(b + 0), sum(b + 0),
							  avg(b + 0), min(b + 0), max(b + 0))
				  FROM sptp WHERE grp IN (0, 1)) AS b,
			   (SELECT format('%s %s %s', count(ts + interval '0 s'),
							  min(ts + interval '0 s'), max(ts + interval '0 s'))
				  FROM sptp WHERE grp <> 1) AS ts)
	SELECT (l.a, l.b, l.ts) = (q.a, q.b, q.ts), l.a || ' | ' || l.b
	  FROM l, q
	$f$;
}
setup { VACUUM (FREEZE, ANALYZE) sptp; }

teardown
{
	DROP FUNCTION sptp_both();
	DROP TABLE sptp;
}

# The long-lived REPEATABLE READ reader, which also shows that the node
# answers its aggregates from the slices.
session s1
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off; SET TimeZone = 'UTC'; }
step s1_explain	{ EXPLAIN (COSTS OFF)
				  SELECT count(a), sum(a), min(a), max(a) FROM sptp WHERE grp = 1; }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_slices	{ SELECT * FROM sptp_both(); }
step s1_commit	{ COMMIT; }

# The writer: rows at every column's extremes in two partitions, an update
# that moves rows to other values and other partitions, a delete of every
# row of grp 1 in one partition.
session s2
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off; SET TimeZone = 'UTC'; }
step s2_begin	{ BEGIN; }
step s2_write	{ INSERT INTO sptp VALUES
					(10001, 1, 2147483647, 9223372036854775807, 'infinity'),
					(1, 1, -2147483648, -9223372036854775808, '-infinity'),
					(2001, 0, NULL, NULL, NULL);
				  UPDATE sptp SET a = -a, b = b * 2, grp = 1, id = id + 4000
				   WHERE id % 10 = 0 AND id < 4000;
				  DELETE FROM sptp WHERE id % 7 = 3;
				  DELETE FROM sptp WHERE grp = 1 AND id >= 2000 AND id < 4000; }
step s2_slices	{ SELECT * FROM sptp_both(); }
step s2_commit	{ COMMIT; }
step s2_rollback	{ ROLLBACK; }

# A reader that takes a fresh snapshot for every statement, and a VACUUM.
session s3
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_plain_scan = off; SET TimeZone = 'UTC'; }
step s3_slices	{ SELECT * FROM sptp_both(); }
step s3_vacuum	{ VACUUM sptp; }

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
