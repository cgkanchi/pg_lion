-- Parallel walks of a B-tree filtered through lion's set (DESIGN.md §40.9).
--
-- LionBtreeScan and LionOrdered are offered as partial paths: below a Gather
-- or a Gather Merge, core's parallel index scan hands each participant whole
-- leaf pages of the B-tree, and lion's set is built once, by the first
-- participant that needs it, and read by all of them from the query's shared
-- memory - or built by each for itself when it is too large to share.  Every
-- query below is run through the node (every core scan but the index-only
-- one disabled, and no B-tree here holds a lion filter's column) and through
-- the ordinary plan (lion's walks off, sequential scans only, serially), and
-- the answers are compared: in order under an ORDER BY, as multisets
-- otherwise.  The counters of a parallel walk are the sums of its
-- participants', and are compared with the serial walk's.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET work_mem = '64MB';
-- the walks, not the count pushdown, answer the aggregates (but in section 10)
SET pg_lion.enable_count_pushdown = off;
-- parallel plans at any size
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;

CREATE TABLE bp (
	id int PRIMARY KEY,
	k int NOT NULL,		-- a permutation of the ids
	grp int,			-- 50 groups
	x int,
	c2 int,				-- lion-filtered: half the rows each
	c20 int,			-- lion-filtered: twenty values
	tags text[],
	tsv tsvector,
	note text,			-- indexed by nothing: updating it is HOT
	payload text		-- in no B-tree, and wide: the heap is several
						-- times the B-trees
) WITH (autovacuum_enabled = off, fillfactor = 80, parallel_workers = 2);
INSERT INTO bp
SELECT i, (i::bigint * 7919 % 40009)::int, i % 50, i % 1000,
	   ((hashint8extended(i::bigint, 2) & 9223372036854775807) % 2)::int,
	   ((hashint8extended(i::bigint, 20) & 9223372036854775807) % 20)::int,
	   ARRAY['t' || (i % 10), 'u' || (i % 37)],
	   to_tsvector('simple', 'w' || (i % 50) || ' x' || (i % 7)),
	   'n', repeat(md5(i::text), 2)
  FROM generate_series(1, 40000) i;
CREATE INDEX bp_k ON bp (k) INCLUDE (grp, x);
CREATE INDEX bp_c2 ON bp USING lion (c2);
CREATE INDEX bp_c20 ON bp USING lion (c20);
CREATE INDEX bp_tags ON bp USING lion (tags);
CREATE INDEX bp_tsv ON bp USING lion (tsv);
VACUUM (FREEZE, ANALYZE) bp;

/*
 * bp_rows() runs q - as CREATE TABLE AS, which may run in parallel where a
 * plpgsql FOR loop's cursor may not - and returns its rows in the order they
 * came.  bp_wrapped() is the query it runs.
 */
CREATE FUNCTION bp_wrapped(q text) RETURNS text
LANGUAGE sql AS $$
	SELECT 'SELECT row_number() OVER () AS n, s::text AS r FROM (' || q || ') s'
$$;
CREATE FUNCTION bp_rows(q text) RETURNS text[]
LANGUAGE plpgsql AS $$
DECLARE
	a text[];
BEGIN
	EXECUTE 'CREATE TEMP TABLE bp_out AS ' || bp_wrapped(q);
	SELECT coalesce(array_agg(r ORDER BY n), '{}') INTO a FROM bp_out;
	DROP TABLE bp_out;
	RETURN a;
END $$;

/*
 * bp_node() runs q through the node, every core scan but the index-only one
 * disabled, and names the plan's lion walks in plan order - parallel or not,
 * and their mode - what gathers them, and whether a parallel hash join is
 * among them.  bp_ordinary() runs q through the ordinary plan.  Their
 * settings end with them, inside a transaction block too.
 */
CREATE FUNCTION bp_node(q text, OUT walks text, OUT rows text[])
LANGUAGE plpgsql
SET enable_seqscan = off SET enable_bitmapscan = off
SET enable_indexscan = off SET enable_tidscan = off
AS $$
DECLARE
	ln text;
	nodes text[] := '{}';
	what text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || bp_wrapped(q) LOOP
		IF ln ~ 'Custom Scan \(Lion(Ordered|BtreeScan)\)' THEN
			nodes := nodes || (CASE WHEN ln ~ 'Parallel Custom Scan' THEN 'parallel ' ELSE '' END ||
							   substring(ln from 'Custom Scan \((Lion\w+)\)'));
		ELSIF ln ~ '^\s*(Ordered By|Index): .*index only' THEN
			nodes[array_length(nodes, 1)] := nodes[array_length(nodes, 1)] || ' index only';
		ELSIF ln ~ 'Gather Merge' THEN
			what := what || ' under Gather Merge';
		ELSIF ln ~ '\mGather\M' THEN
			what := what || ' under Gather';
		ELSIF ln ~ 'Parallel Hash .*Join' THEN
			what := what || ', a parallel hash join';
		END IF;
	END LOOP;
	walks := CASE WHEN array_length(nodes, 1) IS NULL THEN 'no node'
				  ELSE array_to_string(nodes, ', ') END || what;
	rows := bp_rows(q);
END $$;

CREATE FUNCTION bp_ordinary(q text) RETURNS text[]
LANGUAGE plpgsql
SET enable_seqscan = on SET enable_bitmapscan = off
SET enable_indexscan = off SET enable_indexonlyscan = off
SET pg_lion.enable_ordered_scan = off SET pg_lion.enable_btree_scan = off
SET pg_lion.enable_count_pushdown = off SET max_parallel_workers_per_gather = 0
SET enable_hashjoin = on SET enable_mergejoin = on SET enable_nestloop = on
AS $$
BEGIN
	RETURN bp_rows(q);
END $$;

/*
 * bp_cmp() compares q's answers through the node and through the ordinary
 * plan - in order when `ordered`, as multisets otherwise - and names the
 * node's walks.
 */
CREATE FUNCTION bp_cmp(q text, ordered boolean DEFAULT false) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	n record;
	a text[];
	b text[];
BEGIN
	n := bp_node(q);
	a := n.rows;
	b := bp_ordinary(q);
	IF NOT ordered THEN
		SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO a FROM unnest(a) e;
		SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO b FROM unnest(b) e;
	END IF;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows', n.walks, coalesce(array_length(a, 1), 0));
END $$;

/*
 * bp_plan() prints q's plan, every core scan but the index-only one disabled,
 * without the lines whose contents vary between majors - and with the lion
 * indexes, and the clauses of an AND of their accesses, sorted: core orders
 * a BitmapAnd's arms by their costs, which tie differently on 16 and 17.
 */
CREATE FUNCTION bp_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql
SET enable_seqscan = off SET enable_bitmapscan = off
SET enable_indexscan = off SET enable_tidscan = off
AS $$
DECLARE
	ln text;
	head text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers)' THEN
			CONTINUE;
		END IF;
		head := substring(ln from '^(\s*Lion Indexes: )');
		IF head IS NOT NULL THEN
			ln := head || (SELECT string_agg(x, ', ' ORDER BY x)
							 FROM unnest(string_to_array(substr(ln, length(head) + 1), ', ')) x);
		END IF;
		head := substring(ln from '^(\s*Lion Cond: )\(\(.*\) AND \(.*\)\)$');
		IF head IS NOT NULL AND ln !~ ' OR ' THEN
			ln := head || '((' ||
				(SELECT string_agg(x, ') AND (' ORDER BY x)
				   FROM unnest(string_to_array(substr(ln, length(head) + 3,
													  length(ln) - length(head) - 4),
											   ') AND (')) x) || '))';
		END IF;
		RETURN NEXT ln;
	END LOOP;
END $$;

/*
 * bp_counters() is the counters of q's lion walk under EXPLAIN ANALYZE,
 * through the node - the sums of the participants' when it is parallel - as
 * a jsonb object: whether it ran in parallel, the members it met, its heap
 * fetches and the rows the recheck removed, and its set, without the
 * numbers (a set built by each participant for itself counts the builds of
 * the participants that ran).  bp_serial_counters() is the serial walk's,
 * with no lazy set and no switch to fetch and sort, which a parallel walk
 * does not have and which count their members differently.
 */
CREATE FUNCTION bp_counters(q text) RETURNS jsonb
LANGUAGE plpgsql
SET enable_seqscan = off SET enable_bitmapscan = off
SET enable_indexscan = off SET enable_tidscan = off
AS $$
DECLARE
	ln text;
	c jsonb := '{"parallel": false, "removed": 0}';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Parallel Custom Scan \(Lion' THEN
			c := c || '{"parallel": true}';
		ELSIF ln ~ '^\s*Lion Set Hits:' THEN
			c := c || jsonb_build_object('hits', split_part(ln, ': ', 2)::bigint);
		ELSIF ln ~ '^\s*Heap Fetches:' THEN
			c := c || jsonb_build_object('fetched', split_part(ln, ': ', 2)::bigint);
		ELSIF ln ~ '^\s*Rows Removed by Lion Recheck:' THEN
			c := c || jsonb_build_object('removed', split_part(ln, ': ', 2)::bigint);
		ELSIF ln ~ '^\s*Lion Set:' THEN
			c := c || jsonb_build_object('set',
				regexp_replace(regexp_replace(split_part(ln, ': ', 2),
											  '^\d+ containers, ', ''),
							   '(spilled) \d+ kB|, \d+ builds', '\1', 'g'));
		END IF;
	END LOOP;
	RETURN c;
END $$;

CREATE FUNCTION bp_serial_counters(q text) RETURNS jsonb
LANGUAGE sql
SET max_parallel_workers_per_gather = 0 SET pg_lion.enable_lazy_set = off
SET pg_lion.ordered_switch_ratio = 1e6
AS $$ SELECT bp_counters(q) $$;

/*
 * bp_same() compares q's parallel walk's counters with the serial walk's:
 * the members met, the heap fetches and the rows removed by the recheck are
 * the same however the leaf pages were divided.  A first run of the serial
 * walk comes before both: a walk that fetches a dead row's heap tuple marks
 * its B-tree entry dead, and the walks after it do not meet the entry.  It
 * prints the parallel walk's counters, its heap fetches and removed rows as
 * none or some (their number depends on where the rows landed).
 */
CREATE FUNCTION bp_same(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	s jsonb;
BEGIN
	PERFORM bp_serial_counters(q);
	p := bp_counters(q);
	s := bp_serial_counters(q);
	IF NOT (p->>'parallel')::boolean THEN
		RETURN 'not parallel: ' || p::text;
	END IF;
	IF p->'hits' IS DISTINCT FROM s->'hits' OR
	   p->'fetched' IS DISTINCT FROM s->'fetched' OR
	   p->'removed' IS DISTINCT FROM s->'removed' THEN
		RETURN format('MISMATCH: parallel %s, serial %s', p, s);
	END IF;
	RETURN format('hits %s, heap fetches %s, removed %s, set %s; the same serially',
				  p->>'hits',
				  CASE WHEN (p->>'fetched')::bigint = 0 THEN 'none' ELSE 'some' END,
				  CASE WHEN (p->>'removed')::bigint = 0 THEN 'none' ELSE 'some' END,
				  p->>'set');
END $$;

-- 1. The plans: a partial LionBtreeScan below a Gather, under a Partial
--    Aggregate; a partial LionOrdered below a Gather Merge, forward and
--    backward; heap mode, when a column is in no B-tree.
SELECT * FROM bp_plan('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT * FROM bp_plan('SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id');
SELECT * FROM bp_plan('SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id DESC');
SELECT * FROM bp_plan('SELECT id, length(payload) FROM bp WHERE c20 = 3 ORDER BY id');

-- 2. The answers against the ordinary plan: ANDs, ORs and IN lists of lion
--    filters, multi-key ones, a B-tree qual beside them, aggregates and a
--    GROUP BY, the orders of a Gather Merge both ways, a LIMIT, an empty
--    answer, heap mode.  `c2 = 1` beside `c20 IN (3, 4)` keeps half the rows
--    and core's AND leaves it out; the walk's set answers it too (DESIGN.md
--    §40.2, "A clause core's AND leaves out"), so the walk is index only.
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_cmp('SELECT k, x FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c2 = 1');
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c20 = 3');
SELECT bp_cmp('SELECT id FROM bp WHERE c20 IN (1, 5, 9) AND tags @> ARRAY[''u5'']');
SELECT bp_cmp('SELECT id FROM bp WHERE c20 = 3 OR tags @> ARRAY[''u5'']');
SELECT bp_cmp('SELECT k, grp FROM bp WHERE k BETWEEN 1000 AND 30000 AND c20 = 3');
SELECT bp_cmp('SELECT grp, count(*), sum(x) FROM bp WHERE tags @> ARRAY[''t3''] GROUP BY grp');
SELECT bp_cmp('SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id', true);
SELECT bp_cmp('SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id DESC', true);
SELECT bp_cmp('SELECT k, x FROM bp WHERE c2 = 1 AND c20 IN (3, 4) ORDER BY k', true);
SELECT bp_cmp('SELECT id FROM bp WHERE c20 = 3 ORDER BY id LIMIT 7', true);
SELECT bp_cmp('SELECT id FROM bp WHERE c20 = -1');
SELECT bp_cmp('SELECT id, length(payload) FROM bp WHERE c20 = 3 ORDER BY id', true);

-- 3. The counters on a table VACUUM has left all-visible: the members met,
--    no heap fetch, the set built once and shared.
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c2 = 1');
SELECT bp_same('SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id');
SELECT bp_same('SELECT id, length(payload) FROM bp WHERE c20 = 3 ORDER BY id');

-- 4. Pages not all-visible: rows inserted in this transaction, then updated
--    (HOT and not) and deleted rows before VACUUM.  Every participant tests
--    the visibility map and fetches the heap tuple of a member on a page it
--    does not call all-visible, as the serial walk does: the same answers,
--    the same heap fetches.  After VACUUM, none.
BEGIN;
INSERT INTO bp
SELECT 40000 + i, 40009 + i, i % 50, i, i % 2, 3, ARRAY['t3'],
	   to_tsvector('simple', 'w3 x1'), 'n', 'p'
  FROM generate_series(1, 300) i;
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_cmp('SELECT k FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY k', true);
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
COMMIT;
UPDATE bp SET note = 'h' WHERE id % 97 = 0;					-- HOT
UPDATE bp SET x = x + 1 WHERE id % 89 = 0;					-- an INCLUDE column
UPDATE bp SET c20 = (c20 + 1) % 20 WHERE id % 83 = 0;		-- a lion column
UPDATE bp SET k = k + 100000 WHERE id % 79 = 0;				-- a B-tree key
DELETE FROM bp WHERE id % 73 = 0;
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c2 = 1');
SELECT bp_cmp('SELECT k, x FROM bp WHERE c20 IN (3, 4) AND tags @> ARRAY[''t3''] ORDER BY k', true);
SELECT bp_cmp('SELECT grp, count(*), sum(x) FROM bp WHERE tags @> ARRAY[''u5''] GROUP BY grp');
SELECT bp_cmp('SELECT id, length(payload) FROM bp WHERE c20 = 3 ORDER BY id', true);
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c2 = 1');
SELECT bp_same('SELECT k, x FROM bp WHERE c20 IN (3, 4) AND tags @> ARRAY[''t3''] ORDER BY k');
SELECT bp_same('SELECT id, length(payload) FROM bp WHERE c20 = 3 ORDER BY id');
VACUUM (FREEZE, ANALYZE) bp;
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_same('SELECT k, x FROM bp WHERE c20 IN (3, 4) AND tags @> ARRAY[''t3''] ORDER BY k');

-- 5. A set inexact at run time - a tsquery prefix or NOT widens it to a
--    superset - over a B-tree without its column: every participant rechecks
--    its members on their heap tuples, fetched for that, as the serial walk
--    does; an AND of lexemes is exact.  Then rows not all-visible on an
--    inexact set, fetched once for both.
SELECT bp_cmp($$SELECT id FROM bp WHERE tsv @@ 'w3 & x5'::tsquery$$);
SELECT bp_cmp($$SELECT id FROM bp WHERE tsv @@ '!w3'::tsquery$$);
SELECT bp_cmp($$SELECT k, x FROM bp WHERE tsv @@ '!w3 & !x2'::tsquery ORDER BY k$$, true);
SELECT bp_same($$SELECT id FROM bp WHERE tsv @@ 'w3 & x5'::tsquery$$);
SELECT bp_same($$SELECT id FROM bp WHERE tsv @@ '!w3'::tsquery$$);
SELECT bp_same($$SELECT k, x FROM bp WHERE tsv @@ '!w3 & !x2'::tsquery ORDER BY k$$);
BEGIN;
UPDATE bp SET note = 'r' WHERE id % 61 = 0;
SELECT bp_cmp($$SELECT id FROM bp WHERE tsv @@ '!w3 & !x2'::tsquery$$);
SELECT bp_same($$SELECT id FROM bp WHERE tsv @@ '!w3 & !x2'::tsquery$$);
ROLLBACK;
VACUUM (FREEZE) bp;

-- 6. A set each participant builds for itself, as one too large to share
--    is (pg_lion.enable_shared_set off): the same answers and counters.
SET pg_lion.enable_shared_set = off;
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c2 = 1');
SELECT bp_cmp('SELECT k, x FROM bp WHERE c2 = 1 OR tags @> ARRAY[''t3''] ORDER BY k', true);
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c2 = 1');
SELECT bp_same($$SELECT id FROM bp WHERE tsv @@ '!w3'::tsquery$$);
RESET pg_lion.enable_shared_set;
-- at hash_mem's least, the set still fits the room of three
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT bp_cmp('SELECT k, x FROM bp WHERE c2 = 1 OR tags @> ARRAY[''t3''] ORDER BY k', true);
SELECT bp_same('SELECT k, x FROM bp WHERE c2 = 1 OR tags @> ARRAY[''t3''] ORDER BY k');
RESET hash_mem_multiplier;
RESET work_mem;

-- 7. Rescans: a Gather on the inner side of a nested loop, run again for
--    each outer row - the B-tree's parallel scan started over and the set
--    built again; a Gather Merge under a LIMIT, shut down before its end
--    and then rescanned; a generic plan's parameter and an initplan's in
--    the lion filter, which every participant evaluates.
SET enable_material = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT * FROM bp_plan('SELECT * FROM (SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']) ss
					   RIGHT JOIN (VALUES (1), (2), (3)) v(i) ON true');
SELECT * FROM bp_plan('SELECT * FROM (SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id LIMIT 4) ss
					   RIGHT JOIN (VALUES (1), (2), (3)) v(i) ON true');
SELECT bp_cmp('SELECT * FROM (SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']) ss
			   RIGHT JOIN (VALUES (1), (2), (3)) v(i) ON true');
SELECT bp_cmp('SELECT * FROM (SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id LIMIT 4) ss
			   RIGHT JOIN (VALUES (1), (2), (3)) v(i) ON true');
SELECT bp_cmp('SELECT * FROM (SELECT k, x FROM bp WHERE c20 = 7 ORDER BY k DESC LIMIT 3) ss
			   RIGHT JOIN (VALUES (1), (2)) v(i) ON true');
RESET enable_material;
RESET enable_hashjoin;
RESET enable_mergejoin;
SET plan_cache_mode = force_generic_plan;
PREPARE bp_p(int) AS SELECT count(*), sum(x) FROM bp WHERE c20 = $1 AND tags @> ARRAY['t3'];
SELECT * FROM bp_plan('EXECUTE bp_p(3)');
EXECUTE bp_p(3);
EXECUTE bp_p(4);
SET max_parallel_workers_per_gather = 0;
SET pg_lion.enable_btree_scan = off;
SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY['t3'];
SELECT count(*), sum(x) FROM bp WHERE c20 = 4 AND tags @> ARRAY['t3'];
RESET pg_lion.enable_btree_scan;
SET max_parallel_workers_per_gather = 2;
DEALLOCATE bp_p;
RESET plan_cache_mode;
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c20 = (SELECT 3) AND tags @> ARRAY[''t3'']');

-- 8. The leader taking no part: the workers walk, and EXPLAIN reports the
--    set as they had it.
SET parallel_leader_participation = off;
SELECT bp_cmp('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_cmp('SELECT id FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3''] ORDER BY id', true);
SELECT bp_same('SELECT count(*), sum(x) FROM bp WHERE c20 = 3 AND tags @> ARRAY[''t3'']');
SELECT bp_same($$SELECT id FROM bp WHERE tsv @@ '!w3'::tsquery$$);
RESET parallel_leader_participation;

-- 9. The partial paths in joins: both sides of a parallel hash join, and
--    an anti join.
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT bp_cmp('SELECT count(*), sum(a.x) FROM bp a JOIN bp b ON a.k = b.id WHERE a.c20 = 3 AND b.c2 = 1');
SELECT bp_cmp('SELECT count(*) FROM bp a WHERE a.c20 = 3 AND NOT EXISTS (SELECT 1 FROM bp b WHERE b.id = a.k AND b.c2 = 1)');
RESET enable_mergejoin;
RESET enable_nestloop;

-- 10. The FK-side join (DESIGN.md §27) over a B-tree walk of its dimension:
--     the dimension rows a lion filter keeps, read from the B-tree that
--     holds the columns the join needs - its key, and a GROUP BY column when
--     there is one - and not from its heap, serially and as a partial path
--     below a parallel LionCount; for the inner join, the semi join and the
--     anti join.  bp_fk() runs q with the joins disabled, so that the count
--     pushdown is the plan, names its node and the dimension's path below
--     it, and compares the answers with the ordinary plan's; bp_fk_heap()
--     shows that walk reads none of the dimension's heap while the
--     visibility map calls its pages all-visible.  The dimension's heap is
--     twenty times its primary key, and the walk is chosen over a bitmap
--     scan of the heap with nothing else disabled: at random_page_cost 4,
--     and at 1, a heap page costing what an index page does.
SET pg_lion.enable_count_pushdown = on;
SET pg_lion.enable_semijoin = off;
CREATE TABLE bpd (
	id int PRIMARY KEY,
	attr int NOT NULL,
	region int NOT NULL,		-- lion-filtered: four values
	pad text
) WITH (autovacuum_enabled = off, parallel_workers = 2);
INSERT INTO bpd
SELECT i, i % 7, ((hashint8extended(i::bigint, 4) & 9223372036854775807) % 4)::int,
	   repeat(md5(i::text), 12)
  FROM generate_series(1, 12000) i;
CREATE INDEX bpd_region ON bpd USING lion (region);
CREATE INDEX bpd_id_attr ON bpd (id) INCLUDE (attr);
CREATE TABLE bpf (
	id int NOT NULL,
	fk int,
	x int NOT NULL
) WITH (autovacuum_enabled = off, parallel_workers = 2);
INSERT INTO bpf
SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE abs(hashint4(i)) % 13000 + 1 END,
	   abs(hashint4(i + 99)) % 10
  FROM generate_series(1, 60000) i;
CREATE INDEX bpf_fk ON bpf USING lion (fk);
CREATE INDEX bpf_x ON bpf USING lion (x);
VACUUM (FREEZE, ANALYZE) bpd;
VACUUM (FREEZE, ANALYZE) bpf;

CREATE FUNCTION bp_fk(q text) RETURNS text
LANGUAGE plpgsql
SET enable_hashjoin = off SET enable_mergejoin = off SET enable_nestloop = off
AS $$
DECLARE
	ln text;
	node text := 'not pushed down';
	child text := '';
	below boolean := false;
	a text[];
	b text[];
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || bp_wrapped(q) LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			node := CASE WHEN ln ~ 'Parallel Custom Scan' THEN 'parallel ' ELSE '' END || 'LionCount';
			below := true;
		ELSIF below AND child = '' AND ln ~ '->' THEN
			child := ' over ' || btrim(regexp_replace(ln, '^\s*->\s*(.*) on .*$', '\1'));
		ELSIF below AND child <> '' AND ln ~ '^\s*(Ordered By|Index): ' THEN
			child := child || ' (' || btrim(split_part(ln, ':', 2)) || ')';
			below := false;
		END IF;
	END LOOP;
	a := bp_rows(q);
	b := bp_ordinary(q);
	SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO a FROM unnest(a) e;
	SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO b FROM unnest(b) e;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows pushed down, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s%s, %s rows', node, child, coalesce(array_length(a, 1), 0));
END $$;

/*
 * bp_fk_heap() runs q under EXPLAIN ANALYZE with the joins disabled and names
 * the walk of the dimension below the count and its heap fetches: none when
 * the visibility map calls the dimension's pages all-visible, the key and the
 * grouped column read from the B-tree alone.
 */
CREATE FUNCTION bp_fk_heap(q text) RETURNS text
LANGUAGE plpgsql
SET enable_hashjoin = off SET enable_mergejoin = off SET enable_nestloop = off
AS $$
DECLARE
	ln text;
	walk text := 'no walk';
	fetched text := 'not counted';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(Lion(Ordered|BtreeScan)\)' THEN
			walk := btrim(regexp_replace(ln, '^\s*->\s*(.*) on .*$', '\1'));
		ELSIF walk <> 'no walk' AND ln ~ '^\s*Heap Fetches:' THEN
			fetched := CASE WHEN ln ~ ': 0$' THEN 'none' ELSE 'some' END;
		END IF;
	END LOOP;
	RETURN format('%s, heap fetches %s', walk, fetched);
END $$;

SET max_parallel_workers_per_gather = 0;
SELECT bp_fk('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SELECT bp_fk('SELECT count(*) FROM bpd d WHERE d.region = 1 AND EXISTS (SELECT 1 FROM bpf f WHERE f.fk = d.id AND f.x = 3)');
SELECT bp_fk('SELECT count(*) FROM bpd d WHERE d.region = 1 AND NOT EXISTS (SELECT 1 FROM bpf f WHERE f.fk = d.id AND f.x = 3)');
SELECT bp_fk('SELECT d.attr, count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 GROUP BY d.attr');
SELECT bp_fk_heap('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SELECT bp_fk_heap('SELECT d.attr, count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 GROUP BY d.attr');
SET max_parallel_workers_per_gather = 2;
SELECT bp_fk('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SELECT bp_fk('SELECT count(*) FROM bpd d WHERE d.region = 1 AND EXISTS (SELECT 1 FROM bpf f WHERE f.fk = d.id AND f.x = 3)');
SELECT bp_fk('SELECT count(*) FROM bpd d WHERE d.region = 1 AND NOT EXISTS (SELECT 1 FROM bpf f WHERE f.fk = d.id AND f.x = 3)');
SELECT bp_fk('SELECT d.attr, count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 GROUP BY d.attr');
SELECT bp_fk_heap('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SELECT bp_fk_heap('SELECT count(*) FROM bpd d WHERE d.region = 1 AND NOT EXISTS (SELECT 1 FROM bpf f WHERE f.fk = d.id AND f.x = 3)');
SET random_page_cost = 1;
SELECT bp_fk('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SET max_parallel_workers_per_gather = 0;
SELECT bp_fk('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
RESET random_page_cost;
-- the dimension's rows not all-visible: the walk fetches their heap tuples
-- for visibility, serially and in every participant
UPDATE bpd SET pad = 'u' WHERE id % 17 = 0;
DELETE FROM bpd WHERE id % 19 = 0;
SELECT bp_fk('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SELECT bp_fk_heap('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SET max_parallel_workers_per_gather = 2;
SELECT bp_fk('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
SELECT bp_fk('SELECT count(*) FROM bpd d WHERE d.region = 1 AND NOT EXISTS (SELECT 1 FROM bpf f WHERE f.fk = d.id AND f.x = 3)');
SELECT bp_fk_heap('SELECT count(*) FROM bpf f JOIN bpd d ON d.id = f.fk WHERE d.region = 1 AND f.x = 3');
RESET pg_lion.enable_semijoin;
SET pg_lion.enable_count_pushdown = off;
DROP TABLE bpf, bpd;
DROP TABLE bp;
DROP FUNCTION bp_fk_heap(text);
DROP FUNCTION bp_fk(text);
DROP FUNCTION bp_same(text);
DROP FUNCTION bp_serial_counters(text);
DROP FUNCTION bp_counters(text);
DROP FUNCTION bp_plan(text);
DROP FUNCTION bp_cmp(text, boolean);
DROP FUNCTION bp_ordinary(text);
DROP FUNCTION bp_node(text);
DROP FUNCTION bp_rows(text);
DROP FUNCTION bp_wrapped(text);
