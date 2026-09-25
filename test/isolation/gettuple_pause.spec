# A plain index scan paused between two amgettuple calls (DESIGN.md §29.5).
#
# The executor hands a row to its caller between two calls of amgettuple, so
# a scan can stand still for as long as a client leaves a cursor open.  Under
# an MVCC snapshot the lion scan holds NO pin while it waits - only private
# copies: of the posting leaf its cursor is on (with the right link it had),
# of an INLINE payload, of the directory leaf a walk is on.  Every permutation
# below parks a cursor after five rows, changes the index under it, and then
# drains the cursor: the rows must be exactly the ones the transaction's
# snapshot sees, none twice.  No injection point is needed - a cursor IS the
# pause - so this runs on every supported major.
#
#  * posting_split: the posting leaves of the set the cursor is reading split
#    (an update that is rolled back leaves its index entries behind, so the
#    leaves really fill up while the answer stays the same);
#  * inline_spill: the INLINE set the cursor copied spills to a posting tree;
#  * dir_split: the directory leaf under a range walk splits;
#  * vacuum_next: VACUUM deletes the NEXT entry of a paused range walk and
#    frees its posting tree, and an insert may take the pages back.  The
#    walk reaches that entry through its stale copy of the leaf and ends it
#    at the owner check (§18).  The VACUUM step carries no (*) marker: it
#    has to COMPLETE while the cursor is open, which it could not if the
#    paused scan held a pin on the set it stands in (§11 cleanup-locks every
#    page) - the isolation tester would wait on it until it timed out.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS pg_buffercache;

	CREATE TABLE gp (id int, k int, pad text)
		WITH (fillfactor = 50, autovacuum_enabled = off);
	INSERT INTO gp
	SELECT i, CASE i % 20 WHEN 0 THEN 1 WHEN 1 THEN 3 ELSE 2 END,
		   repeat('p', 40)
	  FROM generate_series(1, 40000) i;
	INSERT INTO gp SELECT 40000 + i, 5, 'five' FROM generate_series(1, 40) i;
	/*
	 * Key 4 on heap pages of its own: the executor keeps the HEAP page of
	 * the row it last returned pinned, and a dead key-4 row there would stay
	 * for the next VACUUM - which is core's behaviour, not the index's.
	 */
	INSERT INTO gp SELECT 41000 + i, 4, repeat('q', 40) FROM generate_series(1, 2000) i;
	CREATE INDEX gp_k ON gp USING lion (k) WITH (inline_limit = 64);

	CREATE TABLE gpt (id int, k text NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO gpt
	SELECT i, 'key-' || lpad((2 * (i % 30))::text, 90, '0')
	  FROM generate_series(1, 3000) i;
	CREATE INDEX gpt_k ON gpt USING lion (k) WITH (inline_limit = 64);

	CREATE TABLE gp_got (id int);

	/* Move up to n rows (all of them for NULL) of a cursor into gp_got. */
	CREATE FUNCTION gp_take(cname text, n int) RETURNS bigint
	LANGUAGE plpgsql AS $fn$
	DECLARE
		c refcursor := cname;
		r record;
		got bigint := 0;
	BEGIN
		LOOP
			EXIT WHEN n IS NOT NULL AND got >= n;
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			INSERT INTO gp_got VALUES (r.id);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}

teardown
{
	DROP FUNCTION gp_take(text, int);
	DROP TABLE gp, gpt, gp_got;
}

# The reader: a plain index scan in a cursor, REPEATABLE READ so that the
# expected answer is computed under the very snapshot the cursor has.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = off;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	TRUNCATE gp_got;
}
step s1_k1		{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR SELECT id FROM gp WHERE k = 1;
	SELECT gp_take('c', 5) AS first;
}
step s1_k5		{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR SELECT id FROM gp WHERE k = 5;
	SELECT gp_take('c', 5) AS first;
}
step s1_k34		{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR SELECT id FROM gp WHERE k BETWEEN 3 AND 4;
	SELECT gp_take('c', 5) AS first;
}
step s1_t		{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR SELECT id FROM gpt
	 WHERE k BETWEEN 'key-' || lpad('10', 90, '0') AND 'key-' || lpad('50', 90, '0');
	SELECT gp_take('c', 5) AS first;
}
step s1_rest	{ SELECT gp_take('c', NULL) AS rest; }
# The answer against a sequential scan under the same snapshot.
step s1_check_k1	{
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off;
	SELECT (SELECT count(*) FROM gp_got) AS got,
		   (SELECT count(DISTINCT id) FROM gp_got) AS distinct_ids,
		   (SELECT count(*) FROM gp WHERE k = 1) AS expected,
		   (SELECT count(*) FROM gp_got g WHERE NOT EXISTS
			 (SELECT 1 FROM gp WHERE gp.id = g.id AND gp.k = 1)) AS wrong;
	COMMIT;
}
step s1_check_k5	{
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off;
	SELECT (SELECT count(*) FROM gp_got) AS got,
		   (SELECT count(DISTINCT id) FROM gp_got) AS distinct_ids,
		   (SELECT count(*) FROM gp WHERE k = 5) AS expected,
		   (SELECT count(*) FROM gp_got g WHERE NOT EXISTS
			 (SELECT 1 FROM gp WHERE gp.id = g.id AND gp.k = 5)) AS wrong;
	COMMIT;
}
step s1_check_k34	{
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off;
	SELECT (SELECT count(*) FROM gp_got) AS got,
		   (SELECT count(DISTINCT id) FROM gp_got) AS distinct_ids,
		   (SELECT count(*) FROM gp WHERE k BETWEEN 3 AND 4) AS expected,
		   (SELECT count(*) FROM gp_got g WHERE NOT EXISTS
			 (SELECT 1 FROM gp WHERE gp.id = g.id AND gp.k BETWEEN 3 AND 4)) AS wrong;
	COMMIT;
}
step s1_check_t	{
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off;
	SELECT (SELECT count(*) FROM gp_got) AS got,
		   (SELECT count(DISTINCT id) FROM gp_got) AS distinct_ids,
		   (SELECT count(*) FROM gpt WHERE k BETWEEN 'key-' || lpad('10', 90, '0')
								  AND 'key-' || lpad('50', 90, '0')) AS expected;
	COMMIT;
}

# The writer.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_pages	{
	SELECT container_pages, max_posting_height FROM lion_index_stats('gp_k');
}
# Grow key 1's containers on the leaves the cursor has not reached, and on
# the one it stands on: the new versions land on the pages of the old ones
# (fillfactor 50).  Rolled back, so the answer does not change, but the
# index entries stay and the leaves split.
step s2_grow	{
	BEGIN;
	UPDATE gp SET k = 1 WHERE k = 2 AND id <= 30000;
	ROLLBACK;
}
step s2_spill	{
	INSERT INTO gp SELECT 50000 + i, 5, 'more' FROM generate_series(1, 3000) i;
	SELECT lion_index_posting_root('gp_k', 5) IS NOT NULL AS spilled;
}
step s2_split	{
	BEGIN;
	INSERT INTO gpt
	SELECT 10000 + i, 'key-' || lpad((2 * (i % 60) + 1)::text, 90, '0')
	  FROM generate_series(1, 60) i;
	ROLLBACK;
	SELECT leaf_pages > 1 AS leaf_split FROM lion_index_stats('gpt_k');
}
step s2_del4	{ DELETE FROM gp WHERE k = 4; }
step s2_reuse	{
	INSERT INTO gp SELECT 60000 + i, 9, 'nine' FROM generate_series(1, 20000) i;
}
step s2_free	{ SELECT deleted_pages AS pages_left_free FROM lion_index_stats('gp_k'); }

# The vacuum.
session s3
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) gp; }
# What the paused scan holds of the index: nothing (§29.5).
step s3_pins	{
	SELECT count(*) AS index_pages_pinned FROM pg_buffercache
	 WHERE reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	   AND relfilenode = pg_relation_filenode('gp_k') AND pinning_backends > 0;
}
step s3_gone	{
	SELECT lion_index_posting_root('gp_k', 4) IS NULL AS entry_4_deleted,
		   deleted_pages > 0 AS pages_freed
	  FROM lion_index_stats('gp_k');
}
step s3_verify	{
	SELECT lion_index_verify('gp_k', true);
	SELECT lion_index_verify('gpt_k', true);
}

permutation s2_pages s1_k1 s3_pins s2_grow s2_pages s1_rest s1_check_k1 s3_verify
permutation s1_k5 s2_spill s1_rest s1_check_k5 s3_verify
permutation s1_t s2_split s1_rest s1_check_t s3_verify
permutation s2_del4 s1_k34 s3_pins s3_vacuum s3_gone s2_reuse s2_free s1_rest s1_check_k34 s3_verify
