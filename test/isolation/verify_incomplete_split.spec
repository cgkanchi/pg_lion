# lion_index_verify() on an index with an unfinished split (DESIGN.md §21, §22).
#
# A split writes the new right sibling in one record and its downlink in the
# next, and leaves the left page flagged LION_PAGE_INCOMPLETE_SPLIT in
# between.  A crash - or an error - there leaves the sibling reachable through
# the left page's right link and from nothing above, until the next writer
# that descends to the left page finishes the split.  That is a normal state,
# which verify() has always warned about, and it then went on to report it as
# corruption anyway:
#
#	posting level 1 of chain entry ... has 4 downlinks but level 0 has 5 pages
#
# The injection points at the end of each split make the insert fail right
# there, which stands in for the crash test/recovery/run.sh uses (phases 1c
# and 1d): the pages stay as the first record left them, since an aborted
# transaction does not undo index writes.  verify() must warn, and pass.
#
#  * the posting tree: two keys taking alternate TIDs, so each one's set is a
#    tree of dense bitset leaves after a few thousand rows, and the next leaf
#    split is not a root push-down;
#  * the directory: 200 keys of about a hundred bytes are a few leaves under
#    a root, and new keys among them split a leaf that is not the root (a
#    root split is atomic and never flagged).

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE vis_p (id int, k int NOT NULL) WITH (autovacuum_enabled = off);
	CREATE INDEX vis_p_k ON vis_p USING lion (k);
	INSERT INTO vis_p SELECT i, i % 2 FROM generate_series(1, 60000) i;
	CREATE TABLE vis_d (k text NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO vis_d SELECT 'key-' || lpad((2 * i)::text, 90, '0')
	  FROM generate_series(1, 200) i;
	CREATE INDEX vis_d_k ON vis_d USING lion (k);
}

teardown
{
	DROP TABLE vis_p, vis_d;
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-split-incomplete');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-dir-split-incomplete');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup			{ SELECT injection_points_set_local(); }
step s1_shape	{
	SELECT max_posting_height > 0 AS posting_is_a_tree FROM lion_index_stats('vis_p_k');
	SELECT directory_height > 0 AS directory_has_a_root FROM lion_index_stats('vis_d_k');
}
# The insert splits a posting leaf and fails between its two records.
step s1_split_posting	{
	SELECT injection_points_attach('lion-posting-split-incomplete', 'error');
	INSERT INTO vis_p SELECT 100000 + i, i % 2 FROM generate_series(1, 30000) i;
}
step s1_split_dir	{
	SELECT injection_points_attach('lion-dir-split-incomplete', 'error');
	INSERT INTO vis_d SELECT 'key-' || lpad((2 * i + 1)::text, 90, '0')
	  FROM generate_series(1, 20) i;
}
step s1_detach_posting	{ SELECT injection_points_detach('lion-posting-split-incomplete'); }
step s1_detach_dir	{ SELECT injection_points_detach('lion-dir-split-incomplete'); }
step s1_verify_posting	{
	SELECT lion_index_verify('vis_p_k');
	SELECT lion_index_verify('vis_p_k', true);
}
step s1_verify_dir	{
	SELECT lion_index_verify('vis_d_k');
	SELECT lion_index_verify('vis_d_k', true);
}
# Every row is still found through the index, the aborted ones excepted.
step s1_answers	{
	SET enable_seqscan = off;
	SELECT (SELECT count(*) FROM vis_p WHERE k = 0) = 30000 AS k0_exact,
		   (SELECT count(*) FROM vis_p WHERE k = 1) = 30000 AS k1_exact,
		   (SELECT bool_and((SELECT count(*) FROM vis_d d2 WHERE d2.k = d1.k) = 1)
			  FROM vis_d d1) AS every_key_is_found;
	RESET enable_seqscan;
}

permutation
	s1_shape
	s1_split_posting
	s1_detach_posting
	s1_split_dir
	s1_detach_dir
	s1_verify_posting
	s1_verify_dir
	s1_answers
