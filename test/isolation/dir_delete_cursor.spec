# Ordered walks paused beside directory pages that VACUUM deletes (DESIGN.md
# §21, "Page deletion"; §27).
#
# An ordered walk holds no pin between rows: it remembers its leaf and the
# last key it returned, and reads the leaf again - or its right or left
# sibling - when it goes on.  The rows of keys 101 .. 1900 are deleted before
# two cursors start, one walking up and one walking down, so the cursors'
# snapshot holds nothing back and VACUUM empties the 160-odd leaves between
# them and takes them out of the tree, with the internal pages above them.
# Each cursor stands at the last live key before that run, and the next leaf
# it would read is one of the deleted pages: the walk up moves right from it
# to the leaf of key 1901, as from any page that is not its key's, and the
# walk down steps left over it to the leaf of key 100.
#
# The deleted pages are not reused while the cursors' transaction lasts: their
# safexid is not behind its snapshot.  The keys inserted meanwhile go to the
# leaves around the run, where the cursors cannot see them.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE FUNCTION ddc_key(i int) RETURNS text
	LANGUAGE sql IMMUTABLE AS $$ SELECT 'k' || lpad(i::text, 599, '0') $$;
	CREATE TABLE ddc (id int, k text) WITH (autovacuum_enabled = off);
	ALTER TABLE ddc ALTER k SET STORAGE PLAIN;
	INSERT INTO ddc SELECT i, ddc_key(i) FROM generate_series(1, 2000) i;
	CREATE INDEX ddc_k ON ddc USING lion (k);
	DELETE FROM ddc WHERE id BETWEEN 101 AND 1900;
}

teardown
{
	DROP TABLE ddc;
	DROP FUNCTION ddc_key(int);
}

session s1
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_sort = off;
}
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id FROM ddc ORDER BY k; }
step s1_begin	{
	BEGIN;
	DECLARE up CURSOR FOR SELECT id FROM ddc ORDER BY k;
	DECLARE down CURSOR FOR SELECT id FROM ddc ORDER BY k DESC;
	MOVE 99 FROM up;
	MOVE 99 FROM down;
}
step s1_up1		{ FETCH 1 FROM up; }
step s1_down1	{ FETCH 1 FROM down; }
step s1_up		{ FETCH 3 FROM up; }
step s1_down	{ FETCH 3 FROM down; }
step s1_commit	{ COMMIT; }

session s2
step s2_vacuum	{ VACUUM ddc; }
step s2_insert	{
	INSERT INTO ddc SELECT i, ddc_key(i) FROM generate_series(101, 300) i;
}
step s2_shape	{
	SELECT deleted_pages > 150 AS pages_deleted, leaf_pages < 40 AS few_leaves
	  FROM lion_index_stats('ddc_k');
}
step s2_verify	{ SELECT lion_index_verify('ddc_k', true) AS verify; }
step s2_count	{
	SELECT count(*) AS rows, min(id) AS lo, max(id) AS hi FROM ddc;
}

permutation
	s1_plan
	s1_begin
	s1_up1			# key 100: the last before the run
	s1_down1		# key 1901: the first after it
	s2_vacuum		# deletes the run's leaves
	s2_shape
	s2_insert		# keys 101 .. 300, which the cursors cannot see
	s1_up			# 1901, 1902, 1903
	s1_down			# 100, 99, 98
	s1_commit
	s2_verify
	s2_count
