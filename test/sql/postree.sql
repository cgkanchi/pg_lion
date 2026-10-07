-- Position trees (DESIGN.md §17, src/lion_postree.c).
--
-- A key's word positions, once they outgrow its entry, live in a tree of
-- position chunks that reuses the posting tree's code (§22): descent, root
-- push-down, leaf and internal splits.  Its separators are heap blocks and
-- repeat when one block's members fill several leaves, so readers and writers
-- descend to the last separator below their block and walk right.
--
-- lion_debug_postree_stress() is a test function, not part of the extension:
-- it builds a tree of its own inside the index, puts members into it (in
-- hot blocks, block 0 among them; scattered; appended; replacing), and checks
-- every leaf and every member against a reference, by a walk and by lookups,
-- halfway through and at the end.  Any difference is an ERROR.  It returns
-- the shape of the tree it built.
--
-- The third case grows a three-level tree, which needs internal pages to
-- split: posting_tree.sql cannot reach that (a posting leaf covers at least
-- 64 heap blocks), and position trees share the code that does it.

\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE TABLE postree_t (k int);
INSERT INTO postree_t SELECT g FROM generate_series(1, 100) g;
CREATE INDEX postree_i ON postree_t USING lion (k);

CREATE FUNCTION lion_debug_postree_stress(idx regclass, nops int,
										  maxnpos int, seed bigint,
										  hotpct int)
	RETURNS text AS '$libdir/pg_lion', 'lion_debug_postree_stress'
	LANGUAGE C STRICT;

-- runs of one block across leaves, ckey 0's first block among them
SELECT lion_debug_postree_stress('postree_i', 8000, 100, 4, 40);

-- members of up to 256 positions: few to a chunk, chunks split often
SELECT lion_debug_postree_stress('postree_i', 3000, 256, 5, 40);

-- three levels: internal pages split and the internal root is pushed down
SELECT lion_debug_postree_stress('postree_i', 40000, 256, 6, 0);

-- an empty tree, and one member
SELECT lion_debug_postree_stress('postree_i', 0, 1, 7, 0);
SELECT lion_debug_postree_stress('postree_i', 1, 1, 8, 0);

-- bad arguments
SELECT lion_debug_postree_stress('postree_i', 10, 0, 9, 0);
SELECT lion_debug_postree_stress('postree_t', 10, 1, 9, 0);

DROP FUNCTION lion_debug_postree_stress(regclass, int, int, bigint, int);
DROP TABLE postree_t;
