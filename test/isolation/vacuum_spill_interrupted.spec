# A multi-leaf INLINE spill that an ERROR stops before its last record
# (DESIGN.md §18, §22; lion_entry_spill()).
#
# VACUUM's filtering can make an INLINE posting set outgrow its entry by an
# order of magnitude: here one key over 150,000 clustered rows is eleven RUN
# containers of a few hundred bytes, and deleting every tenth row turns ten of
# them into 4104-byte BITSETs, one leaf each.  The spill writes the leaves one
# record at a time and the root together with the entry in a last record of
# its own, so an ERROR - or a crash, test/recovery/run.sh phase 1e - between
# the two must leave:
#
#  * the entry INLINE and holding every TID it held, the dead ones included,
#    which the next VACUUM removes: nothing is lost and nothing is half-CHAIN;
#  * ten FULL leaves that nothing references, stamped with a root that was
#    never written.  lion_index_verify() must take them for the leak they are
#    and not for corruption (it did not: an unreferenced page that is neither
#    empty nor internal was "not reachable from the meta page").  Its
#    warnings are not printed here; lion_index_stats() counts the pages;
#  * and the next VACUUM spills the set properly while its sweep frees the
#    ten leaves, recognising them by their root, which is not a live root of
#    their key (lion_posting_root_live()).  The sweep used to free only EMPTY
#    unreferenced leaves, so these stayed leaked for good.
#
# The injection point 'lion-spill-leaves-written' fires between the last leaf
# record and the root-and-entry record.  It is attached LOCALLY, in the
# VACUUM's own session, with the 'error' action.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE vsi (i int4 NOT NULL, k int4 NOT NULL)
		WITH (autovacuum_enabled = off);
	INSERT INTO vsi SELECT i, 0 FROM generate_series(1, 150000) i;
	CREATE INDEX vsi_k ON vsi USING lion (k);
	DELETE FROM vsi WHERE i % 10 = 3;
}

teardown
{
	DROP TABLE vsi;
}

session s1
setup
{
	SET client_min_messages = error;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-spill-leaves-written', 'error');
}
step s1_stats	{
	SELECT entries, inline_entries, container_pages, posting_internal_pages,
		   deleted_pages, ntids
	  FROM lion_index_stats('vsi_k');
}
step s1_vacuum	{ VACUUM vsi; }
step s1_detach	{ SELECT injection_points_detach('lion-spill-leaves-written'); }
step s1_verify	{ SELECT lion_index_verify('vsi_k', true) AS verify; }
step s1_count	{
	SET enable_seqscan = off; SET enable_indexscan = off;
	SELECT count(*) AS through_the_index FROM vsi WHERE k = 0;
	RESET enable_seqscan; RESET enable_indexscan;
}

permutation
	s1_stats		# one INLINE entry of 150,000 TIDs
	s1_vacuum		# ERROR between the leaves and the root
	s1_stats		# still INLINE with every TID; ten orphan leaves
	s1_verify		# the orphans are a leak, not corruption
	s1_count		# and no TID is lost: 135,000 rows
	s1_detach
	s1_vacuum		# spills the set, and the sweep frees the orphans
	s1_stats		# CHAIN: one root, ten leaves; ten DELETED pages
	s1_verify
	s1_count
