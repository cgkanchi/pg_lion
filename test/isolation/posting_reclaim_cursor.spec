# A cursor paused over a key's posting set while VACUUM takes leaves ahead of
# it out of the set (DESIGN.md §22, "Empty leaves"; §29.5).
#
# A plain index scan under an MVCC snapshot holds no pin between rows: it has
# a copy of the posting leaf it is reading and the right link that leaf had.
# The rows of ids 60,001 .. 180,000 are deleted before the cursor starts, so
# its snapshot holds nothing back.  The cursor stops at the last row of key 1
# before that run, and VACUUM empties the leaves the run filled and takes them
# out of the set: the right link the cursor holds names one of them.  It is
# still an empty leaf of the set with the right link it had, and the cursor
# walks through it to the rows after the run.
#
# Meanwhile 120,000 new rows go into the heap's free space, where the run
# was, and need new leaves; the pages VACUUM took out are not among them
# while the cursor's transaction lasts, and are once it has ended.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE prc (id int, k int) WITH (autovacuum_enabled = off);
	CREATE INDEX prc_k ON prc USING lion (k);
	INSERT INTO prc SELECT g, g % 4 FROM generate_series(1, 240000) g;
	DELETE FROM prc WHERE id BETWEEN 60001 AND 180000;
}

teardown
{
	DROP TABLE prc;
}

session s1
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET pg_lion.enable_count_pushdown = off;
}
step s1_begin	{
	BEGIN;
	DECLARE c CURSOR FOR SELECT id FROM prc WHERE k = 1;
	MOVE 14999 FROM c;
}
step s1_fetch	{ FETCH 3 FROM c; }
step s1_tail	{ MOVE 14990 FROM c; FETCH 10 FROM c; }
step s1_commit	{ COMMIT; }

session s2
setup
{
	SET enable_seqscan = off;
	SET pg_lion.enable_count_pushdown = off;
}
step s2_vacuum	{ VACUUM prc; }
step s2_leaves	{
	SELECT container_pages AS leaves, deleted_pages AS unlinked
	  FROM lion_index_stats('prc_k');
}
step s2_shape	{
	SELECT container_pages AS leaves, deleted_pages AS unlinked
	  FROM lion_index_stats('prc_k');
}
step s2_insert	{
	INSERT INTO prc SELECT g, g % 4 FROM generate_series(300001, 420000) g;
}
step s2_size	{
	SELECT container_pages AS leaves, deleted_pages AS still_unlinked
	  FROM lion_index_stats('prc_k');
}
step s2_verify	{ SELECT lion_index_verify('prc_k', true) AS verify; }
step s2_count	{
	SELECT k, count(*) FROM prc WHERE k IN (0, 1, 2, 3) GROUP BY k ORDER BY k;
}
step s2_reuse	{
	INSERT INTO prc SELECT g, g % 4 FROM generate_series(500001, 620000) g;
	SELECT container_pages AS leaves, deleted_pages AS unlinked
	  FROM lion_index_stats('prc_k');
}

permutation
	s2_leaves
	s1_begin			# the cursor stands at id 59993
	s2_vacuum			# empties the leaves of the run and takes them out
	s2_shape
	s2_insert			# new leaves, and not those pages
	s2_size
	s2_verify
	s1_fetch			# 59997, then through the pages that went to 180001
	s1_tail				# ... and every row after them
	s1_commit
	s2_count
	s2_vacuum
	s2_reuse			# now they are used again
	s2_verify
