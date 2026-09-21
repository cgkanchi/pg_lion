-- roaring_index_count(): heap-skipping count(*) (DESIGN.md section 9).
--
-- Every assertion here is "the function agrees with the equivalent SELECT
-- under the same snapshot".  The counting must therefore never be allowed to
-- answer the reference query as well, so the phase-2b pushdown is switched
-- off for this file.  LOAD makes the GUC exist before we set it: the library
-- is otherwise only loaded when the planner first opens a roaring index.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'roaring_index';
SET roaring_index.enable_count_pushdown = off;

/*
 * The visibility map is what makes this feature worth having, and a heap page
 * only becomes all-visible once the inserting transaction's commit record has
 * reached disk.  The dev cluster runs with synchronous_commit = off, so ask
 * for flushed commits here; otherwise whether VACUUM can set the bits depends
 * on how fast the WAL writer happens to be.
 */
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS roaring_index;

/*
 * rbi_ccmp() compares roaring_index_count() against count(*) for one key.
 * 'val' is a SQL expression, so cross-type calls can be written out in full.
 */
CREATE OR REPLACE FUNCTION rbi_ccmp(idx text, tbl text, col text, val text)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint;
	b bigint;
BEGIN
	EXECUTE format('SELECT roaring_index_count(%L::regclass, %s)', idx, val) INTO a;
	EXECUTE format('SELECT count(*) FROM %s WHERE %I = %s', tbl, col, val) INTO b;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH roaring=%s select=%s', a, b);
	END IF;
	RETURN format('ok %s', a);
END $$;

CREATE OR REPLACE FUNCTION rbi_ccmp2(idx1 text, val1 text, idx2 text, val2 text,
									 tbl text, col1 text, col2 text)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint;
	b bigint;
BEGIN
	EXECUTE format('SELECT roaring_index_count(%L::regclass, %s, %L::regclass, %s)',
				   idx1, val1, idx2, val2) INTO a;
	EXECUTE format('SELECT count(*) FROM %s WHERE %I = %s AND %I = %s',
				   tbl, col1, val1, col2, val2) INTO b;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH roaring=%s select=%s', a, b);
	END IF;
	RETURN format('ok %s', a);
END $$;

/*
 * 200k rows over ~1100 heap pages, i.e. about 18 containers per full-table
 * posting set.  c10 (20000 rows per key) spills to a chain of container
 * pages; c5000 (40 rows per key) stays inline in its entry tuple; both paths
 * of rbi_count.c are therefore exercised by the queries below.
 */
CREATE TABLE rbi_cnt AS
SELECT i,
       (i % 10)::int4                                   AS c10,
       ((i::bigint * 7919) % 5000)::int4                AS c5000,
       ((i::bigint * 104729) % 100000)::int8            AS c100k,
       ('key ' || ((i::bigint * 31) % 1000))::text      AS t1000,
       (i % 7)::int4                                    AS m7,
       (i % 3 = 0)                                      AS b3
  FROM generate_series(1, 200000) i;

CREATE INDEX rbi_cnt_c10 ON rbi_cnt USING roaring (c10);
CREATE INDEX rbi_cnt_c5000 ON rbi_cnt USING roaring (c5000);
CREATE INDEX rbi_cnt_c100k ON rbi_cnt USING roaring (c100k);
CREATE INDEX rbi_cnt_t1000 ON rbi_cnt USING roaring (t1000);
CREATE INDEX rbi_cnt_m7 ON rbi_cnt USING roaring (m7);
CREATE INDEX rbi_cnt_b3 ON rbi_cnt USING roaring (b3);
ANALYZE rbi_cnt;

-- chain entries (20000 TIDs per key)
SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '0');
SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '7');
SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '9');
-- the widest posting sets of all: two thirds / one third of the table
SELECT rbi_ccmp('rbi_cnt_b3', 'rbi_cnt', 'b3', 'true');
SELECT rbi_ccmp('rbi_cnt_b3', 'rbi_cnt', 'b3', 'false');
-- inline entries (40 TIDs per key)
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '0');
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '1234');
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '4999');
/*
 * Two TIDs per key: c100k's posting sets are sparse segments, not containers
 * (DESIGN.md section 13), so the set cursor has to present each of their
 * container keys as a temporary container of its own.
 */
SELECT containers, sparse_segments > 0 AS has_segments,
	   sparse_members = ntids AS every_tid_in_a_segment
  FROM roaring_index_stats('rbi_cnt_c100k');
SELECT rbi_ccmp('rbi_cnt_c100k', 'rbi_cnt', 'c100k', '29');
SELECT rbi_ccmp('rbi_cnt_c100k', 'rbi_cnt', 'c100k', '99729');
-- by-reference keys
SELECT rbi_ccmp('rbi_cnt_t1000', 'rbi_cnt', 't1000', '''key 0''::text');
SELECT rbi_ccmp('rbi_cnt_t1000', 'rbi_cnt', 't1000', '''key 777''::text');

-- keys that match nothing at all
SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '-1');
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '1000000');
SELECT rbi_ccmp('rbi_cnt_t1000', 'rbi_cnt', 't1000', '''no such key''::text');

-- cross-type keys resolved through the shared integer operator family
SELECT rbi_ccmp('rbi_cnt_c100k', 'rbi_cnt', 'c100k', '29::int4');
SELECT rbi_ccmp('rbi_cnt_c100k', 'rbi_cnt', 'c100k', '29::int2');
SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '7::int8');
SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '7::int2');

/*
 * Two keys at once: the merge of two posting sets.  Both keys share a single
 * anyelement in the function's signature, so they have to resolve to the same
 * SQL type; the index each one is looked up in may of course be on a
 * different (cross-type compatible) type, as the c100k case below shows.
 */
SELECT rbi_ccmp2('rbi_cnt_c10', '3', 'rbi_cnt_m7', '2',
				 'rbi_cnt', 'c10', 'm7');
SELECT rbi_ccmp2('rbi_cnt_m7', '0', 'rbi_cnt_c10', '9',
				 'rbi_cnt', 'm7', 'c10');
-- chain entry AND inline entry (every c5000 = 1234 row has c10 = 6)
SELECT rbi_ccmp2('rbi_cnt_c10', '6', 'rbi_cnt_c5000', '1234',
				 'rbi_cnt', 'c10', 'c5000');
SELECT rbi_ccmp2('rbi_cnt_c10', '3', 'rbi_cnt_c5000', '1234',
				 'rbi_cnt', 'c10', 'c5000');
-- an int8 index probed with an int4 constant, ANDed with an int4 index
SELECT rbi_ccmp2('rbi_cnt_c10', '1', 'rbi_cnt_c100k', '29',
				 'rbi_cnt', 'c10', 'c100k');
SELECT rbi_ccmp2('rbi_cnt_c10', '9', 'rbi_cnt_c100k', '29',
				 'rbi_cnt', 'c10', 'c100k');
-- an intersection that is empty although both keys exist
SELECT rbi_ccmp2('rbi_cnt_c10', '1', 'rbi_cnt_c5000', '0',
				 'rbi_cnt', 'c10', 'c5000');
-- one key that does not exist at all
SELECT rbi_ccmp2('rbi_cnt_c10', '1', 'rbi_cnt_c5000', '1000000',
				 'rbi_cnt', 'c10', 'c5000');

-- An empty table, and a table whose only key has one row.
CREATE TABLE rbi_cnt_tiny (k int4);
CREATE INDEX rbi_cnt_tiny_k ON rbi_cnt_tiny USING roaring (k);
SELECT rbi_ccmp('rbi_cnt_tiny_k', 'rbi_cnt_tiny', 'k', '1');
DROP INDEX rbi_cnt_tiny_k;
INSERT INTO rbi_cnt_tiny VALUES (1);
CREATE INDEX rbi_cnt_tiny_k ON rbi_cnt_tiny USING roaring (k);
SELECT rbi_ccmp('rbi_cnt_tiny_k', 'rbi_cnt_tiny', 'k', '1');
SELECT rbi_ccmp('rbi_cnt_tiny_k', 'rbi_cnt_tiny', 'k', '2');

/*
 * The visibility map interlock.  How much of a freshly loaded heap is
 * all-visible before the first VACUUM is NOT deterministic: on-access pruning
 * (heap_page_prune_opt(), which sets visibility map bits as well as pruning)
 * runs during every sequential scan, so it depends on what has read the table
 * and on the oldest snapshot in the cluster while it did -- running this file
 * on its own and running it after another test file give different answers.
 * What must hold either way is that the count is right and that the two
 * numbers agree with each other: a posting set with no dead TIDs in it has
 * every member either on a block the map answered or in the recheck list, so
 * "nothing was skipped" and "everything was rechecked" are the same
 * statement.  The states where the numbers themselves are pinned down are
 * asserted below, after VACUUM and after the DELETE.
 */
SELECT count = 20000 AS count_ok,
       tids_rechecked <= 20000 AS no_tid_rechecked_twice,
       (blocks_skipped = 0) = (tids_rechecked = 20000) AS map_and_recheck_agree
  FROM roaring_index_count_stats('rbi_cnt_c10', 0);

VACUUM rbi_cnt;

SELECT count = 20000 AS count_ok, blocks_skipped > 0 AS skipped_some,
       tids_rechecked = 0 AS nothing_rechecked
  FROM roaring_index_count_stats('rbi_cnt_c10', 0);

/*
 * After a DELETE the affected heap pages lose their all-visible bit, so those
 * blocks come back through the recheck path and the deleted rows are not
 * counted -- while the index still lists them, because VACUUM has not run.
 */
DELETE FROM rbi_cnt WHERE i % 4 = 0;

SELECT count = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS count_ok,
       tids_rechecked > 0 AS rechecked_some
  FROM roaring_index_count_stats('rbi_cnt_c10', 0);

SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '0');
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '1234');
SELECT rbi_ccmp('rbi_cnt_t1000', 'rbi_cnt', 't1000', '''key 777''::text');
SELECT rbi_ccmp2('rbi_cnt_c10', '3', 'rbi_cnt_m7', '2',
				 'rbi_cnt', 'c10', 'm7');

-- ... and after VACUUM the dead TIDs are gone and the map answers again.
VACUUM rbi_cnt;

SELECT count = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS count_ok,
       blocks_skipped > 0 AS skipped_some,
       tids_rechecked = 0 AS nothing_rechecked
  FROM roaring_index_count_stats('rbi_cnt_c10', 0);

SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '0');
SELECT rbi_ccmp('rbi_cnt_b3', 'rbi_cnt', 'b3', 'true');
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '1234');

/*
 * Snapshot behaviour.  A cross-session test needs isolationtester (see
 * test/isolation/count_snapshot.spec); what a single session can show is that
 * the count uses the *active* snapshot, so inside a REPEATABLE READ
 * transaction it tracks the transaction's own changes and forgets them again
 * when they are rolled back.
 */
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT roaring_index_count('rbi_cnt_c10', 0) = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS agrees;
DELETE FROM rbi_cnt WHERE c10 = 0 AND i % 30 = 10;
SELECT roaring_index_count('rbi_cnt_c10', 0) = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS agrees_after_own_delete;
SAVEPOINT s;
DELETE FROM rbi_cnt WHERE c10 = 0 AND i % 30 = 20;
SELECT roaring_index_count('rbi_cnt_c10', 0) = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS agrees_in_subxact;
ROLLBACK TO s;
SELECT roaring_index_count('rbi_cnt_c10', 0) = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS agrees_after_rollback_to;
ROLLBACK;

SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '0');

BEGIN ISOLATION LEVEL READ COMMITTED;
SELECT roaring_index_count('rbi_cnt_c10', 0) = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS agrees;
DELETE FROM rbi_cnt WHERE c10 = 0 AND i % 50 = 30;
SELECT roaring_index_count('rbi_cnt_c10', 0) = (SELECT count(*) FROM rbi_cnt WHERE c10 = 0) AS agrees_after_own_delete;
ROLLBACK;

-- Serializable transactions must take the same predicate locks an
-- index-only scan would, and must still produce the right answer.
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT rbi_ccmp('rbi_cnt_c10', 'rbi_cnt', 'c10', '0');
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '1234');
COMMIT;

/*
 * Bounded recheck batches (2026-09-20 review, finding 6).
 *
 * The candidate TIDs of blocks that are not all-visible used to be kept until
 * the whole merge was over: 20 million of them - which a hot standby produces
 * for any 20-million-row posting set, because it never trusts the visibility
 * map - meant a 192 MiB array.  The list is now flushed whenever it reaches a
 * budget taken from work_mem, at a block boundary so that no heap block is
 * visited twice.  With the minimum work_mem the budget is about 10900 TIDs,
 * so the counts below take several batches each, and every answer and every
 * statistic must be exactly what one single batch produces.
 */
DELETE FROM rbi_cnt WHERE i % 37 = 0;	-- every heap page is dirty again
CREATE TEMP TABLE rbi_wm (q text, budget text, cnt bigint, blocks_skipped bigint,
						  tids_rechecked bigint, blocks_rechecked bigint,
						  cache_hits bigint);
SET work_mem = '64kB';
INSERT INTO rbi_wm SELECT 'b3f', 'min', *
  FROM roaring_index_count_stats('rbi_cnt_b3', false);
INSERT INTO rbi_wm SELECT 'c10', 'min', *
  FROM roaring_index_count_stats('rbi_cnt_c10', 0);
SELECT rbi_ccmp('rbi_cnt_b3', 'rbi_cnt', 'b3', 'false');
SELECT rbi_ccmp('rbi_cnt_c5000', 'rbi_cnt', 'c5000', '1234');
SELECT rbi_ccmp2('rbi_cnt_c10', '3', 'rbi_cnt_m7', '2', 'rbi_cnt', 'c10', 'm7');
RESET work_mem;
INSERT INTO rbi_wm SELECT 'b3f', 'big', *
  FROM roaring_index_count_stats('rbi_cnt_b3', false);
INSERT INTO rbi_wm SELECT 'c10', 'big', *
  FROM roaring_index_count_stats('rbi_cnt_c10', 0);
-- one answer per query, whatever the budget was
SELECT q, count(DISTINCT (cnt, blocks_skipped, tids_rechecked, blocks_rechecked))
			AS answers
  FROM rbi_wm GROUP BY q ORDER BY q;
SELECT cnt = (SELECT count(*) FROM rbi_cnt WHERE NOT b3) AS count_ok,
	   tids_rechecked > 40000 AS enough_tids_for_several_batches
  FROM rbi_wm WHERE q = 'b3f' AND budget = 'min';
-- a single count visits every heap block once, so it never has a cache hit
SELECT count(*) FILTER (WHERE cache_hits <> 0) AS single_counts_with_cache_hits
  FROM rbi_wm;
DROP TABLE rbi_wm;

/*
 * The batched heap recheck.
 *
 * Blocks that are not all-visible are visited one BLOCK at a time: the buffer
 * is read and share-locked once and every TID of the posting set that lives
 * on it is resolved under that one lock (rbi_count.c, rbi_recheck_heap_heap).
 * roaring_index_count_stats() reports both numbers, so a posting set that is
 * dense enough to put many members on one page must report far fewer blocks
 * than TIDs -- that ratio IS the batching.
 *
 * The table is also the HOT test.  fillfactor leaves room on every page, and
 * payload is in no index, so UPDATEs of it are HOT: the index keeps pointing
 * at the root of the chain and the recheck has to follow the chain to find
 * the version this snapshot can see.  That is what heap_hot_search_buffer()
 * does, and getting it wrong would show up as a count that disagrees with
 * count(*) -- or, with a broken chain walk, as a count that agrees today and
 * not after the second UPDATE.
 */
CREATE TABLE rbi_cnt_hot (id int4 NOT NULL, k int4 NOT NULL, g int4 NOT NULL,
						  payload int4 NOT NULL) WITH (fillfactor = 50);
INSERT INTO rbi_cnt_hot
SELECT i, i % 5, i % 37, 0 FROM generate_series(1, 20000) i;
CREATE INDEX rbi_cnt_hot_k ON rbi_cnt_hot USING roaring (k);
CREATE INDEX rbi_cnt_hot_g ON rbi_cnt_hot USING roaring (g);
VACUUM ANALYZE rbi_cnt_hot;

-- everything all-visible: no heap visit, so no blocks either
SELECT count = 4000 AS count_ok, blocks_skipped > 0 AS skipped_some,
       tids_rechecked = 0 AS nothing_rechecked,
       blocks_rechecked = 0 AS no_blocks_rechecked
  FROM roaring_index_count_stats('rbi_cnt_hot_k', 0);

-- HOT updates: the chains the recheck has to walk.  A HOT update adds no
-- index entry, which is exactly why the index's TID count must not move.
UPDATE rbi_cnt_hot SET payload = payload + 1 WHERE id % 3 = 0;
UPDATE rbi_cnt_hot SET payload = payload + 1 WHERE id % 6 = 0;
SELECT ntids = 20000 AS index_unchanged_by_hot_updates
  FROM roaring_index_stats('rbi_cnt_hot_k');

DELETE FROM rbi_cnt_hot WHERE id % 7 = 0;

/*
 * Every page has been touched by now, so the whole posting set comes back
 * through the recheck: 4000 TIDs on a couple of hundred blocks.
 */
SELECT count = (SELECT count(*) FROM rbi_cnt_hot WHERE k = 0) AS count_ok,
       tids_rechecked = 4000 AS all_rechecked,
       blocks_rechecked > 0 AS visited_blocks,
       blocks_rechecked * 10 < tids_rechecked AS batched_by_block
  FROM roaring_index_count_stats('rbi_cnt_hot_k', 0);

SELECT rbi_ccmp('rbi_cnt_hot_k', 'rbi_cnt_hot', 'k', '0');
SELECT rbi_ccmp('rbi_cnt_hot_k', 'rbi_cnt_hot', 'k', '3');
SELECT rbi_ccmp('rbi_cnt_hot_g', 'rbi_cnt_hot', 'g', '0');
SELECT rbi_ccmp2('rbi_cnt_hot_k', '1', 'rbi_cnt_hot_g', '11',
				 'rbi_cnt_hot', 'k', 'g');

-- A third update on top of the deleted rows: longer chains, and roots whose
-- chain ends in a dead tuple.
UPDATE rbi_cnt_hot SET payload = payload + 1 WHERE id % 11 = 0;
SELECT rbi_ccmp('rbi_cnt_hot_k', 'rbi_cnt_hot', 'k', '0');
SELECT rbi_ccmp('rbi_cnt_hot_k', 'rbi_cnt_hot', 'k', '4');

/*
 * The same thing in a SERIALIZABLE transaction.  The recheck resolves each
 * TID with heap_hot_search_buffer(), which is the function the table AM's own
 * fetch uses, so it takes the same per-tuple predicate locks; the all-visible
 * blocks that are never looked at are locked page-wise.  Either way a
 * serializable reader must leave SIReadLocks behind on the table.
 */
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT rbi_ccmp('rbi_cnt_hot_k', 'rbi_cnt_hot', 'k', '0');
SELECT rbi_ccmp2('rbi_cnt_hot_k', '2', 'rbi_cnt_hot_g', '5',
				 'rbi_cnt_hot', 'k', 'g');
SELECT count(*) > 0 AS took_predicate_locks
  FROM pg_locks
 WHERE mode = 'SIReadLock'
   AND relation = 'rbi_cnt_hot'::regclass;
COMMIT;

-- ... and once more with every block all-visible again.
VACUUM rbi_cnt_hot;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT rbi_ccmp('rbi_cnt_hot_k', 'rbi_cnt_hot', 'k', '0');
SELECT count(*) > 0 AS took_predicate_locks
  FROM pg_locks
 WHERE mode = 'SIReadLock'
   AND relation = 'rbi_cnt_hot'::regclass;
COMMIT;

SELECT count = (SELECT count(*) FROM rbi_cnt_hot WHERE k = 0) AS count_ok,
       tids_rechecked = 0 AS nothing_rechecked,
       blocks_rechecked = 0 AS no_blocks_rechecked
  FROM roaring_index_count_stats('rbi_cnt_hot_k', 0);

/*
 * The GROUP BY path of DESIGN.md section 10 intersects the same WHERE posting
 * sets with every group, and rbi_count.c answers those from a materialized
 * copy after the first group (the group's own set is still read page by page,
 * which is what keeps the visibility-map interlock).  The pushdown is what
 * drives that code, so this one section turns it on -- and compares it
 * against the same query with the pushdown off, which is the reference.
 */
SET roaring_index.enable_count_pushdown = on;
CREATE TEMP TABLE rbi_grp_on AS
SELECT g, count(*) AS n FROM rbi_cnt_hot WHERE k = 0 GROUP BY g;
CREATE TEMP TABLE rbi_grp_on2 AS
SELECT g, count(*) AS n FROM rbi_cnt_hot GROUP BY g;
SET roaring_index.enable_count_pushdown = off;
CREATE TEMP TABLE rbi_grp_off AS
SELECT g, count(*) AS n FROM rbi_cnt_hot WHERE k = 0 GROUP BY g;
CREATE TEMP TABLE rbi_grp_off2 AS
SELECT g, count(*) AS n FROM rbi_cnt_hot GROUP BY g;

SELECT (SELECT count(*) FROM (SELECT * FROM rbi_grp_on EXCEPT ALL SELECT * FROM rbi_grp_off) x) AS extra_rows,
       (SELECT count(*) FROM (SELECT * FROM rbi_grp_off EXCEPT ALL SELECT * FROM rbi_grp_on) x) AS missing_rows,
       (SELECT count(*) FROM rbi_grp_on) AS groups;
SELECT (SELECT count(*) FROM (SELECT * FROM rbi_grp_on2 EXCEPT ALL SELECT * FROM rbi_grp_off2) x) AS extra_rows,
       (SELECT count(*) FROM (SELECT * FROM rbi_grp_off2 EXCEPT ALL SELECT * FROM rbi_grp_on2) x) AS missing_rows,
       (SELECT count(*) FROM rbi_grp_on2) AS groups;

-- and again with the dirty heap under it, so the materialized WHERE set is
-- ANDed against groups whose blocks all go through the recheck
DELETE FROM rbi_cnt_hot WHERE id % 13 = 0;
SET roaring_index.enable_count_pushdown = on;
CREATE TEMP TABLE rbi_grp_on3 AS
SELECT g, count(*) AS n FROM rbi_cnt_hot WHERE k = 0 GROUP BY g;
SET roaring_index.enable_count_pushdown = off;
CREATE TEMP TABLE rbi_grp_off3 AS
SELECT g, count(*) AS n FROM rbi_cnt_hot WHERE k = 0 GROUP BY g;
SELECT (SELECT count(*) FROM (SELECT * FROM rbi_grp_on3 EXCEPT ALL SELECT * FROM rbi_grp_off3) x) AS extra_rows,
       (SELECT count(*) FROM (SELECT * FROM rbi_grp_off3 EXCEPT ALL SELECT * FROM rbi_grp_on3) x) AS missing_rows,
       (SELECT sum(n) FROM rbi_grp_on3) AS total;

DROP TABLE rbi_grp_on, rbi_grp_on2, rbi_grp_on3,
		   rbi_grp_off, rbi_grp_off2, rbi_grp_off3;

/*
 * The per-query visibility cache (DESIGN.md section 9).
 *
 * A grouped count rechecks the dirty heap blocks of every group separately,
 * so 200 groups used to fetch the same dirty pages 200 times.  A TID's
 * visibility under one MVCC snapshot cannot change while that snapshot is
 * held, so the answer for every root line pointer of a page is resolved once
 * and reused: the block visits of the later groups become bitmap lookups with
 * no buffer access at all.
 *
 * roaring_index_count_group_stats(idx, use_cache) is the SQL image of the
 * GROUP BY driver -- the same entry scan, one count per group, one cache for
 * the whole run -- so both halves of the A/B fit in one query.  What must
 * hold:
 *
 *	- the answer does not depend on the cache, and is the answer of the
 *	  equivalent GROUP BY;
 *	- the number of block visits does not depend on the cache either; what
 *	  changes is how many of them are fetches (blocks_rechecked) and how many
 *	  are cache hits;
 *	- the fetches collapse to about two per dirty page (the first visit only
 *	  records the block, the second resolves it), whatever the group count.
 */
CREATE TEMP TABLE rbi_vc (idx text, mode text, groups bigint, cnt bigint,
						  blocks_skipped bigint, tids_rechecked bigint,
						  blocks_rechecked bigint, cache_hits bigint,
						  cache_full bigint);
INSERT INTO rbi_vc
SELECT 'hot_g', 'nocache', * FROM roaring_index_count_group_stats('rbi_cnt_hot_g', false);
INSERT INTO rbi_vc
SELECT 'hot_g', 'cached', * FROM roaring_index_count_group_stats('rbi_cnt_hot_g', true);
INSERT INTO rbi_vc
SELECT 'hot_k', 'nocache', * FROM roaring_index_count_group_stats('rbi_cnt_hot_k', false);
INSERT INTO rbi_vc
SELECT 'hot_k', 'cached', * FROM roaring_index_count_group_stats('rbi_cnt_hot_k', true);

-- one answer per index, whether or not the cache was used ...
SELECT idx, count(DISTINCT (groups, cnt, blocks_skipped, tids_rechecked)) AS answers,
	   count(DISTINCT blocks_rechecked + cache_hits) AS block_visits
  FROM rbi_vc GROUP BY idx ORDER BY idx;
-- ... and it is the answer of the equivalent GROUP BY
SELECT idx,
	   cnt = (SELECT count(*) FROM rbi_cnt_hot) AS rows_ok,
	   groups = CASE idx
					WHEN 'hot_g' THEN (SELECT count(*) FROM
									   (SELECT g FROM rbi_cnt_hot GROUP BY g) x)
					WHEN 'hot_k' THEN (SELECT count(*) FROM
									   (SELECT k FROM rbi_cnt_hot GROUP BY k) x)
				END AS groups_ok
  FROM rbi_vc WHERE mode = 'cached' ORDER BY idx;
/*
 * The cache is used and it replaces fetches with hits.  How big the win is
 * depends on how many groups there are to share the answer: hot_g has 37
 * groups and hot_k has 5, and the cached run fetches each dirty page about
 * twice either way, so the ratio is the group count minus a constant.
 */
SELECT idx,
	   c.cache_hits > 0 AS cache_used,
	   c.cache_full = 0 AS budget_was_enough,
	   n.cache_hits = 0 AS nocache_has_no_hits,
	   n.blocks_rechecked > c.blocks_rechecked AS fewer_fetches,
	   n.blocks_rechecked > 5 * c.blocks_rechecked AS many_fewer_fetches
  FROM (SELECT * FROM rbi_vc WHERE mode = 'cached') c
	   JOIN (SELECT * FROM rbi_vc WHERE mode = 'nocache') n USING (idx)
 ORDER BY idx;
DELETE FROM rbi_vc;

/*
 * Serializable isolation.  A cache hit calls neither
 * HeapCheckForSerializableConflictOut() nor PredicateLockTID(), so the page
 * whose answer was resolved is predicate-locked once, the way an index-only
 * scan locks a page it skips.  The result must be the same and the locks must
 * be there (SSI may coarsen page locks into a relation lock, which is why
 * only their presence is asserted).
 */
BEGIN ISOLATION LEVEL SERIALIZABLE;
INSERT INTO rbi_vc
SELECT 'hot_g', 'serializable', * FROM roaring_index_count_group_stats('rbi_cnt_hot_g', true);
SELECT cnt = (SELECT count(*) FROM rbi_cnt_hot) AS rows_ok,
	   cache_hits > 0 AS cache_used
  FROM rbi_vc WHERE mode = 'serializable';
SELECT count(*) > 0 AS took_predicate_locks
  FROM pg_locks
 WHERE mode = 'SIReadLock'
   AND relation = 'rbi_cnt_hot'::regclass;
COMMIT;

/*
 * A cache that fills up.  The budget is work_mem (in entries), and when it is
 * reached nothing more is inserted: the blocks that did not get in are
 * fetched per batch, exactly as they were before the cache existed, and
 * cache_full counts those visits.  rbi_cnt is 200k rows over ~1600 heap
 * pages, every one of them dirtied by the DELETE above, which is well past
 * what 64kB of entries holds.
 */
SET work_mem = '64kB';
INSERT INTO rbi_vc
SELECT 'c10', 'full', * FROM roaring_index_count_group_stats('rbi_cnt_c10', true);
RESET work_mem;
INSERT INTO rbi_vc
SELECT 'c10', 'cached', * FROM roaring_index_count_group_stats('rbi_cnt_c10', true);
SELECT count(DISTINCT (groups, cnt, blocks_skipped, tids_rechecked)) AS answers,
	   count(DISTINCT blocks_rechecked + cache_hits) AS block_visits
  FROM rbi_vc WHERE idx = 'c10';
SELECT cnt = (SELECT count(*) FROM rbi_cnt) AS rows_ok,
	   groups = 10 AS groups_ok
  FROM rbi_vc WHERE idx = 'c10' AND mode = 'full';
SELECT (SELECT cache_full FROM rbi_vc WHERE idx='c10' AND mode='full') > 0
			AS ran_out_of_budget,
	   (SELECT cache_full FROM rbi_vc WHERE idx='c10' AND mode='cached') = 0
			AS budget_was_enough,
	   (SELECT cache_hits FROM rbi_vc WHERE idx='c10' AND mode='full') > 0
			AS still_cached_something,
	   (SELECT blocks_rechecked FROM rbi_vc WHERE idx='c10' AND mode='full') >
	   (SELECT blocks_rechecked FROM rbi_vc WHERE idx='c10' AND mode='cached')
			AS a_small_cache_fetches_more;
DROP TABLE rbi_vc;

-- the cache is on by default, and the function is STRICT like the others
SELECT count = (SELECT count(*) FROM rbi_cnt) AS rows_ok, cache_hits > 0 AS cache_used
  FROM roaring_index_count_group_stats('rbi_cnt_c10');
SELECT roaring_index_count_group_stats(NULL) IS NULL AS null_index;

/* Error cases. */
-- wrong key type
SELECT roaring_index_count('rbi_cnt_c10', 'nope'::text);
SELECT roaring_index_count('rbi_cnt_t1000', 1);
-- not a roaring index
CREATE INDEX rbi_cnt_btree ON rbi_cnt (c10);
SELECT roaring_index_count('rbi_cnt_btree', 0);
-- not an index at all
SELECT roaring_index_count('rbi_cnt', 0);
-- two indexes on different tables
CREATE TABLE rbi_cnt_other (k int4);
CREATE INDEX rbi_cnt_other_k ON rbi_cnt_other USING roaring (k);
SELECT roaring_index_count('rbi_cnt_c10', 0, 'rbi_cnt_other_k', 0);
/*
 * Indexes that exist but that this transaction may not use (2026-09-20
 * review, finding 2).  A direct SQL count was handed an index nobody vetted,
 * so it makes the checks get_relation_info() makes for a query; the
 * cross-session indcheckxmin case, which is the one that produced a wrong
 * answer, is in test/isolation/count_checkxmin.spec.
 */
CREATE TABLE rbi_cnt_elig (k int4);
INSERT INTO rbi_cnt_elig SELECT i % 3 FROM generate_series(1, 300) i;
CREATE INDEX rbi_cnt_elig_k ON rbi_cnt_elig USING roaring (k);
SELECT roaring_index_count('rbi_cnt_elig_k', 1);
UPDATE pg_index SET indisvalid = false
 WHERE indexrelid = 'rbi_cnt_elig_k'::regclass;
SELECT roaring_index_count('rbi_cnt_elig_k', 1);
SELECT roaring_index_count('rbi_cnt_elig_k', 1, 'rbi_cnt_elig_k', 1);
-- the verifier shares the check
SELECT roaring_index_verify('rbi_cnt_elig_k', true);
UPDATE pg_index SET indisvalid = true, indisready = false
 WHERE indexrelid = 'rbi_cnt_elig_k'::regclass;
SELECT roaring_index_count('rbi_cnt_elig_k', 1);
UPDATE pg_index SET indisready = true
 WHERE indexrelid = 'rbi_cnt_elig_k'::regclass;
SELECT roaring_index_count('rbi_cnt_elig_k', 1);
SELECT roaring_index_verify('rbi_cnt_elig_k', true);
DROP TABLE rbi_cnt_elig;

-- NULL arguments: STRICT
SELECT roaring_index_count('rbi_cnt_c10', NULL::int4) IS NULL AS null_key;
SELECT roaring_index_count(NULL, 0) IS NULL AS null_index;

DROP TABLE rbi_cnt_other;
DROP TABLE rbi_cnt_hot;
DROP TABLE rbi_cnt_tiny;
DROP TABLE rbi_cnt;
/*
 * DROP EXTENSION + CREATE EXTENSION in one backend (2026-09-20 review,
 * finding 7).  CASCADE takes every roaring index with it, so recreating the
 * extension legitimately gives the access method a NEW pg_am Oid.  The count
 * functions used to cache the old one in a process-local static and then
 * reject every index with "is not a roaring index"; the Oid now comes from
 * the AMNAME syscache, which is invalidated properly.  (This is the last
 * thing this file does: the CASCADE drops the indexes of every roaring index
 * in the database.)
 */
CREATE TABLE rbi_cnt_re (k int4);
INSERT INTO rbi_cnt_re SELECT i % 10 FROM generate_series(1, 1000) i;
CREATE INDEX rbi_cnt_re_k ON rbi_cnt_re USING roaring (k);
SELECT roaring_index_count('rbi_cnt_re_k', 3);
DROP EXTENSION roaring_index CASCADE;
CREATE EXTENSION roaring_index;
SELECT to_regclass('rbi_cnt_re_k') IS NULL AS index_dropped_by_cascade;
CREATE INDEX rbi_cnt_re_k ON rbi_cnt_re USING roaring (k);
SELECT roaring_index_count('rbi_cnt_re_k', 3);
SELECT roaring_index_verify('rbi_cnt_re_k', true);
SELECT count(*) FROM rbi_cnt_re WHERE k = 3;
DROP TABLE rbi_cnt_re;

DROP FUNCTION rbi_ccmp(text, text, text, text);
DROP FUNCTION rbi_ccmp2(text, text, text, text, text, text, text);
