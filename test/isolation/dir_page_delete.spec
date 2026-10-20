# A page deletion in the entry directory that an ERROR stops half way
# (DESIGN.md §21, "Page deletion").
#
# VACUUM takes an emptied directory leaf out of the tree in two stages, as
# nbtree does.  Stage 1 removes the downlink of the subtree that goes - the
# leaf, with the internal pages above it that have no other child - and marks
# the leaf HALF_DEAD, with a MINUS INFINITY high key; stage 2 unlinks the
# subtree's pages from their siblings one record at a time, the top first and
# the leaf last.  Between any two of those records the tree must answer every
# read exactly, take inserts into the range that is going, pass
# lion_index_verify(), and be finished by the next VACUUM.
#
# 2,000 keys of 600 bytes make a directory of four levels: 182 leaves of
# eleven keys, and a level-1 page for every eight or nine of them.  The rows of
# the first 400 keys are deleted, which empties the first 36 leaves, and VACUUM
# meets them in key order.  Its error comes from an
# injection point attached LOCALLY, in its own session, with the 'error'
# action:
#
#  * 'lion-dir-delete-halfdead' after stage 1 of the first leaf: that leaf is
#    HALF_DEAD and still linked, and has no downlink;
#  * 'lion-dir-delete-unlinked' after the first page of a subtree of two: the
#    leaves of the leftmost level-1 page went one by one but its last, which
#    goes with that page - which is DELETED, and the leaf HALF_DEAD with no
#    parent.
#
# In both, keys 5, 100 and 399 are then inserted into the range that is
# going: the first goes to the page right of the HALF_DEAD leaf, which took
# that range over, and is found by every read.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE FUNCTION dpd_key(i int) RETURNS text
	LANGUAGE sql IMMUTABLE AS $$ SELECT 'k' || lpad(i::text, 599, '0') $$;
	CREATE TABLE dpd (id int, k text) WITH (autovacuum_enabled = off);
	ALTER TABLE dpd ALTER k SET STORAGE PLAIN;
	INSERT INTO dpd SELECT i, dpd_key(i) FROM generate_series(1, 2000) i;
	CREATE INDEX dpd_k ON dpd USING lion (k);
	DELETE FROM dpd WHERE id <= 400;
}

teardown
{
	DROP TABLE dpd;
	DROP FUNCTION dpd_key(int);
}

# The VACUUM session.
session s1
setup
{
	SET client_min_messages = error;
	SELECT injection_points_set_local();
}
step s1_halfdead	{ SELECT injection_points_attach('lion-dir-delete-halfdead', 'error'); }
step s1_unlinked	{ SELECT injection_points_attach('lion-dir-delete-unlinked', 'error'); }
step s1_detach_halfdead	{ SELECT injection_points_detach('lion-dir-delete-halfdead'); }
step s1_detach_unlinked	{ SELECT injection_points_detach('lion-dir-delete-unlinked'); }
step s1_vacuum	{ VACUUM dpd; }

# The reading and writing session: every read goes through the index.
session s2
setup
{
	SET enable_seqscan = off;
	SET enable_sort = off;
}
step s2_shape	{
	SELECT deleted_pages = 0 AS none_deleted, deleted_pages >= 30 AS all_deleted
	  FROM lion_index_stats('dpd_k');
}
step s2_verify	{ SELECT lion_index_verify('dpd_k', true) AS verify; }
step s2_read	{
	SELECT right(min(k), 4) AS min_k,
		   (SELECT count(*) FROM dpd WHERE k < dpd_key(450)) AS below_450,
		   (SELECT string_agg(id::text, ',')
			  FROM (SELECT id FROM dpd ORDER BY k LIMIT 3) t) AS first_3,
		   (SELECT string_agg(id::text, ',')
			  FROM (SELECT id FROM dpd WHERE k < dpd_key(403) ORDER BY k DESC) t) AS down_from_403,
		   (SELECT count(*) FROM dpd WHERE k = dpd_key(5)) AS key_5,
		   (SELECT count(*) FROM dpd WHERE k = dpd_key(100)) AS key_100
	  FROM dpd;
}
step s2_insert	{
	INSERT INTO dpd VALUES (5, dpd_key(5)), (100, dpd_key(100)),
						   (399, dpd_key(399));
}

permutation
	s1_halfdead
	s1_vacuum			# ERROR after stage 1 of the first leaf
	s2_shape			# nothing is DELETED yet
	s2_verify			# a HALF_DEAD leaf with no downlink is not damage
	s2_read
	s2_insert			# into the range the HALF_DEAD leaf had
	s2_read				# ... and found there
	s2_verify
	s1_detach_halfdead
	s1_vacuum			# finishes that leaf, and deletes the others
	s2_shape
	s2_verify
	s2_read

permutation
	s1_unlinked
	s1_vacuum			# ERROR after the leftmost level-1 page is DELETED
	s2_shape			# those leaves and that page are DELETED
	s2_verify			# a HALF_DEAD leaf with no parent is not damage
	s2_read
	s2_insert
	s2_read
	s2_verify
	s1_detach_unlinked
	s1_vacuum			# unlinks the leaf, and deletes the others
	s2_shape
	s2_verify
	s2_read
