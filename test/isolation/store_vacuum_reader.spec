# A reader of the window store (DESIGN.md §40) against the VACUUM that
# clears, rewrites and frees store pages under it.
#
# lion_store_gather() reads the head of a window's chain from the map and
# only then locks a store page; between the two it holds no lock and no
# pin, and the injection point "lion-store-gather-head" parks it there.  A
# reader that comes back must take the page it was led to only if that page
# is still this window's store for this column - and still a head - and read
# the map again otherwise (§40, "Reading").  lion_index_stored() is such a
# reader - of the one column this index stores, so it parks once - and its
# answer is checked against what the store holds by then:
#
#  * rewritten: three rows in four die and VACUUM writes every store page
#    again from its live slots; the parked reader then reads the rewritten
#    page and gets the live row's values.
#  * freed: every row dies, VACUUM clears the slots and cuts the heap, and
#    a second VACUUM frees every window past the heap's end - the reader's
#    head among them; the reader finds a deleted page, reads the map again,
#    finds no head, and says NULL.
#  * reused: as freed, and then a row is inserted at the same TID, whose
#    store pages come from the free space the VACUUM recorded; whichever
#    page the reader's stale head is now, it gets the new row's values.
#
# The DELETE commits before the reader starts, so the reader's snapshot
# does not hold the dead rows back from VACUUM, and lion_index_stored()
# locks nothing but the index, so the heap can be truncated under it.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE stvr (k int, t text) WITH (autovacuum_enabled = off);
	INSERT INTO stvr SELECT i, 'v' || i FROM generate_series(1, 3000) i;
	CREATE INDEX stvr_i ON stvr USING lion (k) INCLUDE (t);
}

teardown
{
	DROP TABLE stvr;
	DO $$ BEGIN PERFORM injection_points_detach('lion-store-gather-head');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-store-gather-head', 'wait');
}
step s1_read	{ SELECT lion_index_stored('stvr_i', '(0,1)') AS stored; }
# after s2_detach: it reads the store too
step s1_check	{
	SELECT k, t, lion_index_stored('stvr_i', ctid) AS stored
	  FROM stvr WHERE ctid = '(0,1)';
	SELECT lion_index_verify('stvr_i', true);
}

session s2
step s2_delete_most	{ DELETE FROM stvr WHERE k % 4 <> 1; }
step s2_delete_all	{ DELETE FROM stvr; }
step s2_vacuum		{ VACUUM stvr; }
step s2_vacuum_again	{ VACUUM stvr; }
step s2_reinsert	{ INSERT INTO stvr VALUES (-1, 'new'); }
step s2_pages		{
	SELECT pg_relation_size('stvr') / current_setting('block_size')::int AS heap_pages,
		   store_pages
	  FROM lion_index_stats('stvr_i') WHERE attno = 1;
}
# (*, s1_read): the wakeup is reported as waiting, and complete once the reader
# it woke is, so the reader's completion is reported in one place whatever the
# tester sees first (on master it saw the detach first)
step s2_wakeup		{ SELECT injection_points_wakeup('lion-store-gather-head'); }
step s2_detach		{ SELECT injection_points_detach('lion-store-gather-head'); }

# rewritten: the reader reads the page VACUUM wrote again
permutation
	s2_delete_most
	s1_read				# parks with the head in hand
	s2_vacuum
	s2_pages
	s2_wakeup(*, s1_read)
	s2_detach
	s1_check

# freed: the reader's head is a deleted page; the map has no head any more
permutation
	s2_delete_all
	s1_read
	s2_vacuum
	s2_vacuum_again
	s2_pages
	s2_wakeup(*, s1_read)
	s2_detach
	s1_check

# reused: the pages are free and taken again by the new row's store
permutation
	s2_delete_all
	s1_read
	s2_vacuum
	s2_vacuum_again
	s2_reinsert
	s2_pages
	s2_wakeup(*, s1_read)
	s2_detach
	s1_check
