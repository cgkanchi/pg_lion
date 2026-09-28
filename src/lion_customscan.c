/*-------------------------------------------------------------------------
 *
 * lion_customscan.c
 *		A CustomScan that answers
 *
 *			SELECT count(*) [, k] FROM t WHERE <clause> [AND <clause> ...]
 *			[GROUP BY k]
 *
 *		out of roaring posting sets, visiting the heap only for pages the
 *		visibility map does not vouch for.  DESIGN.md section 10 is the
 *		specification; the counting itself lives in lion_count.c and follows
 *		the pin/visibility-map rule of DESIGN.md section 9.
 *
 * A clause is `k = const` (§10), `k = ANY (const array)` (§15), `k IS NULL`
 * or `k IS NOT NULL` (§14), each on a column with a usable lion index.
 * The first three select rows and are intersected; `IS NOT NULL` subtracts
 * the index's NULL entry from the result, and when it is the only clause the
 * node sums the counts of every entry of that index instead.  The "const" is
 * anything the node can evaluate once per scan - a literal, a parameter, a
 * stable expression such as `now() - interval '1 day'` - and a boolean
 * column tested by itself (`flag`, `NOT flag`, `flag IS TRUE`) is the
 * equality `flag = true` or `flag = false` it stands for.
 *
 * A top-level `OR` of such clauses (DESIGN.md §19) is one source as well: its
 * arms are positive clauses, or ANDs of them, on columns of the same relation
 * with usable indexes of their own, and the source is the UNION of the arms.
 * Its leaves are ordinary members of the clause array - so that an index is
 * matched for each of them per partition, a Param among them reaches
 * custom_exprs, and the cost model prices every lookup - and the OR structure
 * over them travels separately, in LION_PRIV_ORS.
 *
 * The node is planted at UPPERREL_GROUP_AGG by create_upper_paths_hook, so
 * it replaces the whole Agg-over-scan subtree rather than part of it.  Its
 * scan.scanrelid is 0 (it is an upper node with no scan relation of its own)
 * and custom_scan_tlist describes the tuples it produces: the group key Var,
 * if the query asks for it, followed by the count aggregates.
 *
 * The relation may also be a PARTITIONED table (DESIGN.md §16).  Then the
 * node counts one live leaf partition at a time - each with its own heap and
 * its own indexes, found through the planner's already-pruned part_rels and
 * with the column numbers translated per partition.  Without GROUP BY the
 * partition counts are added up into the one row.  With GROUP BY the node
 * emits PARTIAL aggregates instead - one (group key, int8 partial count) per
 * group per partition, streamed as each partition is counted - and the
 * planner puts core's Finalize HashAggregate on top to combine them, which
 * is what lets a grouping larger than hash_mem spill to disk instead of
 * being refused.  Everything below that says "the relation" therefore means
 * "the relation the node is currently counting": one table, or one partition
 * of many.
 *
 * `count(DISTINCT k)` (DESIGN.md §26) is answered by the same machinery with
 * EXISTENCE tests in place of counts: over the WHERE, one test per entry of
 * k's index, walked like a GROUP BY k whose groups are summed into one row;
 * per `GROUP BY g`, one test per (g, k) pair of the §20 nested loop, summed
 * into one row per g.  k is a driving column there and never a grouping one.
 *
 * One JOIN shape is answered too (DESIGN.md §27): a fact table joined to a
 * dimension table on a lion-indexed fk, `SELECT d.attr, count(*) FROM f JOIN
 * d ON f.fk = d.pk ... GROUP BY d.attr`.  The dimension side is the node's
 * child plan; for each of its rows the node counts the fk posting set of that
 * row's key ANDed with the fact's WHERE clauses and emits a PARTIAL count
 * beside the dimension columns, and core's Finalize Agg groups them.  "The
 * relation" is then the fact table, and the join key is one more equality
 * clause whose value comes from the child's current row.  A semi or anti join
 * whose inner side is the fact (`WHERE [NOT] EXISTS (SELECT 1 FROM f WHERE
 * f.fk = d.pk ...)`) is the same walk with an existence test per dimension
 * row, and the fact's WHERE clauses may be collected once into a private copy
 * that every count reads instead of the clauses' own posting sets.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
#include "access/parallel.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/predicate.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"
#include "utils/tuplesort.h"

#include "lion.h"
#include "lion_count.h"
#include "lion_fkjoin.h"

/* GUC and the previous hook, both owned here and installed by _PG_init. */
bool		lion_enable_count_pushdown = true;
create_upper_paths_hook_type lion_prev_create_upper_paths_hook = NULL;

/* Kinds of column in custom_scan_tlist. */
#define LION_TL_GROUPKEY		0
#define LION_TL_GROUPKEY2	1	/* the second GROUP BY column (DESIGN.md §20) */
#define LION_TL_COUNT		2
#define LION_TL_COUNT_GROUPCOL	3	/* count(group column): 0 for the NULL
									 * group, the count otherwise (§14) */
#define LION_TL_COUNT_GROUPCOL2	4	/* the same for the second group column */
#define LION_TL_COUNT_ZERO	5	/* count(col) where a clause pins col to NULL */
#define LION_TL_COUNT_DISTINCT	6	/* count(DISTINCT k), DESIGN.md §26 */
#define LION_TL_COUNT_DISTCOL	7	/* count(k) of that same k: its non-NULL
									 * rows, which a nullable k allows nowhere
									 * else */
/* LION_TL_WHEREKEY + i: the key stored in the i'th clause's entry */
#define LION_TL_WHEREKEY		8

/*
 * A column of the FK-side join's child plan (DESIGN.md §27): the dimension
 * column at position `resno` of the child's target list, read from the child's
 * current row.  Negative, so that it can never collide with the kinds above.
 */
#define LION_TL_CHILDCOL(resno)		(-(resno))
#define LION_TL_IS_CHILDCOL(kind)	((kind) < 0)
#define LION_TL_CHILDRESNO(kind)		((AttrNumber) -(kind))

/*
 * What the FK-side join counts per child row (DESIGN.md §27): the fact rows
 * the row joins (an inner join), or 1 when it joins at least one (a semi
 * join: the dimension row is a row of the result) or none (an anti join).
 * The values travel in the plan, so they are fixed.
 */
#define LION_JOIN_INNER		0
#define LION_JOIN_SEMI		1
#define LION_JOIN_ANTI		2

/* Flag bits of the join member of custom_private (LION_PRIV_JOIN). */
#define LION_JOINFLAG_COLLECT	0x01	/* the fact filters are collected once */
#define LION_JOINFLAG_ROWS		0x02	/* one row per dimension row that joins
										 * (or, for an anti join, that does
										 * not), for a core Agg above to
										 * aggregate: count(DISTINCT) */
#define LION_JOINFLAG_UNIQUE	0x04	/* the child's keys are made distinct
										 * before any is looked up, by sorting
										 * them: the forward semi join over a
										 * non-unique key, an inner join over
										 * the distinct keys */
#define LION_JOINFLAG_WALK		0x08	/* the child's rows are looked up a
										 * batch at a time, sorted into the fk
										 * index's key order and located by one
										 * walk of its leaves ("Lookups in key
										 * order") */
#define LION_JOINFLAG_SUM		0x10	/* the target list is counts alone:
										 * one partial row per run, the sum of
										 * the dimension rows' counts ("The
										 * per-key path, end to end") */

/*
 * Where an FK-side join's time goes (DESIGN.md §27, "Where a key's time
 * goes"): the phases EXPLAIN ANALYZE times when its TIMING option is on, each
 * summed over the participants of a parallel plan.
 */
#define LION_JT_CHILD		0	/* the child plan, producing its rows */
#define LION_JT_LOOKUP		1	/* locating each key's fk set */
#define LION_JT_COUNT		2	/* counting it, or testing it, and letting it go */
#define LION_JT_COLLECT		3	/* collecting the fact filters, once a run */
#define LION_JT_N			4

/*
 * How many distinct keys of a forward semi join over a non-unique key
 * (DESIGN.md §27, "Forward semi joins over a non-unique key") a participant of
 * a parallel plan claims at a time.  Every participant sorts all of the keys,
 * and they divide the sorted, distinct sequence between them in runs of this
 * many, each run counted by the one participant that claimed it: a count is 3
 * to 10 us, so a run is a fraction of a millisecond, and a participant that
 * finishes early is never more than one run behind the others.
 */
#define LION_FKJOIN_UNIQUE_CHUNK	64

/*
 * The fixed cost of one count of the FK-side join (DESIGN.md §27) - one per
 * dimension row - over and above the containers it reads and the lookup that
 * locates its set: a merge set up and torn down.  Derived from what the count
 * does (DESIGN.md §27, "The cost of a key, after the per-key path"): once it
 * works in the node's scratch memory and keeps the node's map page pinned,
 * what is left is some twenty allocations from memory already there, two
 * cursors built and the plan's decisions, about half a microsecond - this.
 * §10 and §26 measured 1.0 us a count or test that also made and deleted a
 * memory context, pinned a map page and, when it merged two sources, malloc'd
 * and freed two blocks every time: what the per-key fixes took off.
 */
#define LION_FKJOIN_COUNT_COST	(25.0 * cpu_tuple_cost)

/*
 * ... and each row the node hands up: a partial count per dimension row that
 * joins, or the row itself for count(DISTINCT) - the per-tuple context reset,
 * a virtual tuple, the projection, the return through the executor, and the
 * pause that lets go of its index pins (the leaf a walk in key order then
 * reads again for the next key is that key's leaf visit, which
 * lion_cost_fkjoin_walk() charges).  About 0.2 us from the operations (the
 * same derivation), where one cpu_tuple_cost used to be charged.  A summed
 * join (LION_JOINFLAG_SUM) hands up one row per participant and run.  What
 * core does with the rows above the node - its Finalize Agg's transitions, a
 * Gather's tuple queue - core charges.
 */
#define LION_FKJOIN_ROW_COST	(10.0 * cpu_tuple_cost)

/*
 * ... and one PROBE of such a count into a fact filter's set: a seek of the
 * filter's (materialized, §9) set to one of the fk set's container keys and
 * the AND of the two containers there.  Measured on the assert build at two
 * million fact rows and a thousand dimension rows (DESIGN.md §27): the
 * thousand counts ANDed with a 10% filter read 708,160 containers in 157 ms
 * against 354,076 in 54 ms without it, about 0.3 us per probe.  §10's two
 * cpu_operator_cost per container understate that tenfold, and with them the
 * node was chosen for that query at 157 ms against the ordinary plan's 63.
 * 20 still chose it (23,378 against 28,057); 30 refuses it (about 32,000) and
 * keeps the same query over a quarter of the dimension, 39 ms against 67,
 * chosen (about 8,000).
 */
#define LION_FKJOIN_PROBE_COST	(80.0 * cpu_operator_cost)

/*
 * ... and, per count, each SET of a fact filter that is a union (an IN list,
 * an OR across columns): the count builds its k-way union again (§15), and
 * each sub-cursor is set up and positioned whether or not the fk set's keys
 * find anything in it.  Measured on the assert build, 1.5M fact rows and 300
 * dimension rows: `t IN (n values)` took 57 ms at n = 30, 199 at 100, 599 at
 * 300 and 1195 at 1000 - 4 to 6 us per set per count.  It is the rebuild a
 * GROUP BY's counts make of a union source too, measured on the release build
 * at 2.7 to 6 us a set (LION_UNION_SET_COST), and priced as that.
 */
#define LION_FKJOIN_SET_COST	LION_UNION_SET_COST

/*
 * ... and, when the fact filters are COLLECTED once into a private copy
 * (lion_sources_collect(), DESIGN.md §27 "The fact filters, collected once"):
 *
 *	COPY_COUNT		one count of an fk set against the copy - the merge of two
 *					sources set up and torn down - in place of COUNT_COST;
 *	COPY_PROBE		per fk container, the copy sought by a binary search in
 *					memory and the two containers ANDed, in place of a probe
 *					into each filter's posting tree;
 *	COPY_MEMBER		... and per member of that fk container, which the AND
 *					walks and the visibility map is asked about;
 *	COPY_CONTAINER	once per scan, one container of the copy made.
 *
 * Measured on the assert build (DESIGN.md §27, "Cost, revisited"): a count
 * against the copy costs about 3 us plus 0.2 us per fk container when the fk
 * sets hold a member or two per container (the 2026-09-27 benchmark's five
 * million rows, 25 and 2.75 rows a key), and 0.47 us per container when they
 * hold six (§27's two million rows, 2,000 a key over 322 containers) - where,
 * without the member term, `x = 3 GROUP BY d.attr` over a thousand dimension
 * rows was chosen at 152 ms against the hash join's 125.  A copy of 1,500
 * containers takes 0.8 ms when the filter is one set.
 */
#define LION_FKJOIN_COPY_COUNT_COST	(25.0 * cpu_tuple_cost)
#define LION_FKJOIN_COPY_PROBE_COST	(15.0 * cpu_operator_cost)
#define LION_FKJOIN_COPY_MEMBER_COST	(3.0 * cpu_operator_cost)
#define LION_FKJOIN_COPY_CONTAINER_COST	(20.0 * cpu_operator_cost)

/*
 * Making the dimension's keys distinct for a forward semi join over a
 * non-unique key (DESIGN.md §27, "Forward semi joins over a non-unique key"),
 * which the node does itself with a datum sort of the child's keys:
 *
 *	SORT_COMPARE	one comparison of two keys in the sort, N log2 N of them
 *					for N keys;
 *	SORT_KEY		per key, the key read from the child's row and put into
 *					the sort, taken out of it again and compared with the
 *					previous distinct key.
 *
 * Measured on the assert build with int4 keys in random order: 0.108 us a key
 * to put 40,000 or 199,000 of them in and sort them, and 0.023 us to take
 * them out and compare them - as fast as core's Sort node over the same
 * column, and about 13 cpu_operator_cost a key at the 250 units a millisecond
 * the node's other terms run at (§27, "Cost, revisited").  cost_sort() is not
 * used: it charges two cpu_operator_cost a comparison, 36 a key at 200,000,
 * three times what the sort takes here (while cost_agg() charges core's own
 * hashed unique-ification one a row), and it answers enable_sort, which this
 * sort, no Sort node, does not.  A text key compares more slowly than these.
 * A sort larger than work_mem also writes its keys out and reads them back,
 * once each, as core's cost_tuplesort() charges one merge pass.
 */
#define LION_FKJOIN_SORT_COMPARE_COST	(0.25 * cpu_operator_cost)
#define LION_FKJOIN_SORT_KEY_COST	(6.0 * cpu_operator_cost)

/*
 * Looking the child's rows up in the fk index's key order (DESIGN.md §27,
 * "Lookups in key order"), which the node does a batch at a time:
 *
 *	BATCH_ROW		per row, its place in the batch - the row copied in, its
 *					share of the sort (log2 of the batch comparisons through the
 *					opclass's comparison function) and the slot it is handed
 *					back to the target list in.  Priced as one directory page
 *					visit (LION_DESCENT_COST), which is on the high side:
 *					putting a key into the datum sort of the distinct keys
 *					above, sorting it and taking it out again measured at a
 *					fraction of what a page visit is fitted at, and a row's
 *					copy and its slot are of the same order.  High on purpose:
 *					the walk replaces a descent's internal levels with this, so
 *					it is taken only where it saves a page a key or more -
 *					never over a directory of height 1, whose descent is the
 *					root and the leaf.
 *	BATCH_ENT_BYTES	per row, what a batch holds besides the row's own columns:
 *					the entry that sorts it, the MinimalTuple's header and the
 *					allocator's chunk headers of the row and of a key copied
 *					by reference.
 */
#define LION_FKJOIN_BATCH_ROW_COST	LION_DESCENT_COST
#define LION_FKJOIN_BATCH_ENT_BYTES	(sizeof(LionJoinEnt) + 48)

/*
 * How many GROUP BY columns the node understands (DESIGN.md §20).  One is
 * driven by that index's entry scan; two are the nested loop of
 * lion_next_group2(), whose cost is the product of the two entry counts.
 */
#define LION_MAX_GROUPCOLS	2

/*
 * Kinds of WHERE clause the pushdown understands.  EQ, ARRAY and NULL select
 * rows (they are positive sources of the count); NOTNULL removes them
 * (DESIGN.md §14: the NULL rows are exactly the members of the index's
 * reserved NULL entry, so `IS NOT NULL` is their complement).
 */
#define LION_CLAUSE_EQ		0	/* col = const */
#define LION_CLAUSE_ARRAY	1	/* col = ANY (const array), DESIGN.md §15 */
#define LION_CLAUSE_NULL		2	/* col IS NULL */
#define LION_CLAUSE_NOTNULL	3	/* col IS NOT NULL */
#define LION_CLAUSE_MULTI	4	/* col @> / && / @@ const, DESIGN.md §17 */
#define LION_CLAUSE_RANGE	5	/* col < / <= / >= / > const, DESIGN.md §28 */
#define LION_CLAUSE_RANGESRC	6	/* the same on a column that does not drive
									 * the count: a source, DESIGN.md §32 */

/*
 * A RANGE clause is neither: it is not a source of the count at all, but a
 * bound on the entry walk that drives it (DESIGN.md §28), so it is never
 * located, never merged and never priced as a lookup.  A RANGESRC clause is a
 * bound of a range on ANOTHER column, and the range is a positive source: the
 * rows whose key lies in it (DESIGN.md §32, "A range as a source").
 */
#define LION_CLAUSE_IS_POSITIVE(k) \
	((k) != LION_CLAUSE_NOTNULL && (k) != LION_CLAUSE_RANGE)

/*
 * Which clause kinds pin their column to ONE value, so that a target list
 * asking for that column can be answered with the key the entry stored.  A
 * multi-key clause pins nothing: `tags @> '{a}'` says what the array
 * contains, not what it is.
 */
#define LION_CLAUSE_PINS_VALUE(k) \
	((k) == LION_CLAUSE_EQ || (k) == LION_CLAUSE_NULL)

/*
 * An `IN` list longer than this is not pushed down: every listed value needs
 * its own posting set, and each of those may hold a buffer pin for as long as
 * the node runs (DESIGN.md §9).  A long list is also exactly the case where
 * the ordinary bitmap plan does well.
 */
#define LION_MAX_ARRAY_ELEMS		1000

/*
 * What the tests of a count(DISTINCT k) walk are (DESIGN.md §26), as far as
 * the cost model is concerned: none at all, existence tests that stop at the
 * first visible row, or full counts because the target list wants rows too.
 */
#define LION_DISTINCT_NONE		0
#define LION_DISTINCT_EXISTS	1
#define LION_DISTINCT_COUNT		2

/*
 * The fixed cost of one test of a count(DISTINCT k) walk - one entry of k, or
 * one (g, k) pair - over and above the containers it reads: a merge set up
 * and torn down (a memory context, the cursors, the visibility-map state) and,
 * for a pair, the inner set's lookup.  Measured on the assert build at 100k
 * rows: about 2 us per test with nothing to intersect and 3.4 us with a WHERE
 * set to seek, against 0.2 us per row for the sorting aggregate core builds
 * for a distinct count (whose model charges about 0.1 per row), which makes a
 * test worth some ten of that aggregate's rows.  It is what refuses a walk
 * over many entries when the WHERE leaves few rows to sort - 1000 entries of
 * k against 500 matching rows measured 3.4 ms against 0.56 (DESIGN.md §26).
 */
#define LION_DISTINCT_TEST_COST	(50.0 * cpu_tuple_cost)

/*
 * ... and of each count a GROUP BY makes, one per entry of its column or pair
 * of two (DESIGN.md §10, §20): the same set-up and tear-down, and the entry
 * decoded, over and above its merge (lion_merge_cpu_cost()).  Measured on the
 * release build over a 5M-row table: 1.0 us a test of a count(DISTINCT) walk
 * of 20,000 one-container entries, 1.3 us a group of 200 whose sets are six
 * RUN containers each.  A group of an IN list that drives the groups (§15)
 * was located with the list and decodes nothing: 0.35 us a group, measured
 * over 1,000 of them.
 */
#define LION_ENTRY_COUNT_COST	(50.0 * cpu_tuple_cost)
#define LION_LIST_GROUP_COST	(18.0 * cpu_tuple_cost)

/*
 * ... and each set of a WHERE source that is a union (an IN list, an OR
 * across columns), which every count of a GROUP BY builds again: its cursor
 * set up and positioned over the set's copy, and its merge (DESIGN.md §15,
 * §19).  Measured on the release build: 2 us a set for 4 groups ANDed with a
 * list of 1,000 values of 100 rows each, and 2.7 and 6 us a set, a few us of
 * which are the union's own set-up, for 200 groups of 250 rows ANDed with
 * `a = 17 OR b = 3` and with `b IN (3, 4)`.
 */
#define LION_UNION_SET_COST		(100.0 * cpu_tuple_cost)

/*
 * The fixed cost of one ENTRY of a range-bounded walk (DESIGN.md §28) - the
 * sum over a range, and a GROUP BY over one - over and above the containers
 * it reads: the walk resumes at a key (a leaf read and a binary search), the
 * entry is copied out, and its count sets up and tears down a merge.
 * Measured on the assert build at 100k rows over a unique timestamp: 3601
 * entries in 6.7 ms summed and 5.2 ms grouped, 1.4 to 1.9 us each, against
 * the btree index-only scan's 0.64 ms for the same 3601 rows at 171 cost
 * units.  Without it both were chosen, at about a tenth of that estimate;
 * with it a near-unique column goes to btree, and a range over a column of
 * 200 values (21 entries, 0.18 ms against btree's 0.9) stays with the node.
 * The unbounded group walk of §10 keeps its own calibration.
 */
#define LION_RANGE_ENTRY_COST	(40.0 * cpu_tuple_cost)

/*
 * ... and of one SMALL entry of a summed range, counted with the rest of its
 * leaf as one union (DESIGN.md §28, "Counting a walk"): the leaf is read once
 * for all of them, and what is left per entry is its copy, its cursor and its
 * share of the union.  Measured on the assert build over the 2M-row repro
 * table: 0.3 to 0.5 us an entry of one row, against 1.5 before.
 */
#define LION_RANGE_UNION_ENTRY_COST	(12.0 * cpu_tuple_cost)

/*
 * The descents a summed walk over a summarized column makes on top of the
 * first (DESIGN.md §32): the walk of a side is up to three phases - the keys
 * below its first whole bucket, the whole buckets' summaries, the keys above
 * its last - and each phase after the first starts with a descent of its own
 * to where it begins, a root-to-leaf read each.
 */
#define LION_SUMMARY_PHASE_DESCENTS	2.0

/*
 * One container of a set a summed walk PROBES at the other sources' rows
 * (DESIGN.md §32, "Summed ranges: dense and probed"): the cursor's step to it
 * or past it - a sparse segment's pair skipped, an item fetched - and the
 * search for its key among the probe's.  No visibility map, no merge and no
 * count set up per set, which is what LION_CONTAINER_COST and the per-entry
 * constants above are mostly made of: a quarter of LION_CONTAINER_COST,
 * derived from what the step leaves out rather than measured.  The members a
 * probed container marks are charged LION_AND_MEMBER_COST each, the probe's
 * as a set of LION_RANGE_UNION_ENTRY_COST.
 */
#define LION_PROBE_STEP_COST	(2.0 * cpu_operator_cost)

/*
 * ... and when a walk turns to probing: once it has handed out this many
 * times the rows of the other sources' smallest positive one
 * (lion_sum_probe_due()), which the cost model predicts the same way.
 */
#define LION_PROBE_SWITCH	2.0

/*
 * THE MERGE'S CPU (DESIGN.md §10, "The units"), which lion_merge_cpu_cost()
 * charges wherever posting sets are counted or intersected - the count
 * pushdown's AND, each group of a GROUP BY and each pair of two, and the AND a
 * lion index scan makes of several columns' sets (lioncostestimate()).
 * Fitted on the release build (-O2, no assertions; 4-core VM, warm shared
 * buffers) to the backend CPU time of 35 counts over a 5M-row table whose sets
 * hold 1 to 4,500 rows a container - eleven sets alone, and pairs, triples
 * and a quadruple of them ANDed - at 500 cost units a millisecond, the rate
 * core's own scans of the same tables run at there (sequential scans 400 to
 * 700, index-only scans 415 to 535):
 *
 *	CONTAINER	each container the driver reads and counts: 32 to 39 ns;
 *	MEMBER		... and each of its members, which the visibility-map mask is
 *				built from: 0.74 ns, up to MEMBER_CAP of them - a bitset costs
 *				what an array of about a thousand members does;
 *	PROBE		each seek of another source's posting tree to a container key
 *				of the driver's, and the container it finds: 210 ns - with the
 *				leaves the seeks move to fitted at 0.7 us a page, which the
 *				model charges as I/O instead (lion_probed_pages());
 *	AND_MEMBER	each member of the running intersection ANDed with what a
 *				probe finds: 4 to 5 ns.
 *
 * Median residual 27%.  The model it replaces - two cpu_operator_cost a
 * container, members and probes free - priced these counts at 87 to 46,000
 * units a millisecond.
 *
 * MEMORY_PROBE is a probe of a set the count has copied into memory - the
 * WHERE sets a GROUP BY intersects with every group, after their first use
 * (DESIGN.md §9), and the outer entry's set of a two-column GROUP BY (§20): a
 * binary search of the copy's containers, 100 to 200 ns, measured over
 * 20,000 groups of 219 containers each and 1,000 of 38.
 */
#define LION_CONTAINER_COST		(8.0 * cpu_operator_cost)
#define LION_MEMBER_COST		(0.15 * cpu_operator_cost)
#define LION_MEMBER_CAP			1024.0
#define LION_PROBE_COST			(40.0 * cpu_operator_cost)
#define LION_MEMORY_PROBE_COST	(30.0 * cpu_operator_cost)
#define LION_AND_MEMBER_COST	(0.8 * cpu_operator_cost)

/*
 * One level of an entry directory descended - a page pinned and locked, a
 * binary search, the entry decoded - for a count's lookup of a key, an IN
 * list's of each of its values, and the FK-side join's of each dimension row's
 * key (DESIGN.md §21, §27).  Fitted on the release build to the FK-side join's
 * 23 shapes (DESIGN.md §27, "Cost, revisited"): 0.6 us a directory page read,
 * median residual 9%.  The page itself is charged separately, as I/O.
 */
#define LION_DESCENT_COST		(120.0 * cpu_operator_cost)

/*
 * One candidate TID of a heap recheck (DESIGN.md §9): the visibility check of
 * its tuple, on a page the recheck has pinned (the page itself is charged as
 * I/O).  Fitted on the release build to counts of 99 to 1M candidates on a
 * 2M-row table whose every page had been made dirty, and to 2,100 on a
 * clustered value whose map had gone stale: 24 to 29 ns a TID where a page
 * holds 68 to 150 of them, up to 70 ns where it holds 34 scattered ones - one
 * and a half of the rows cpu_tuple_cost stands for in a sequential scan
 * (DESIGN.md §10, "The units").
 *
 * A GROUP BY rechecks each group's candidates on its own, and answers a page
 * it has fetched out of the visibility cache from its second visit on: 130 ns
 * a candidate over 200 groups of a whole dirty table (2M candidates, 1.6M
 * answered from the cache), which one cpu_tuple_cost priced at 53,000 for
 * 267 ms.
 */
#define LION_RECHECK_TID_COST	(1.5 * cpu_tuple_cost)
#define LION_RECHECK_GROUP_TID_COST	(6.0 * cpu_tuple_cost)

/* Which walk a range bounds, for the cost model (DESIGN.md §28). */
#define LION_RANGED_NONE	0
#define LION_RANGED_WALK	1	/* a GROUP BY or count(DISTINCT) walk */
#define LION_RANGED_SUM		2	/* the sum over the range's entries */
#define LION_RANGED_SUMALL	3	/* the sum over every entry, `k IS NOT NULL`
								 * alone (§14): priced as a range over all of
								 * k when k has summaries (§32) */

/*
 * A sum over a walk's entries counts them a LEAF at a time (DESIGN.md §28,
 * "Counting a walk"): the entries the walk copied out of one directory leaf
 * are counted together, as the union of disjoint sets, when they are small -
 * at most LION_SUM_UNION_MAX_ITEMS containers each on average - and one by
 * one otherwise.  The union saves the set-up of a count per entry, which is
 * the whole cost of an entry of a row or two; for entries of many containers
 * that set-up is noise, and the pairwise container unions below the merge's
 * bitset image (LION_OR_BITSET_MIN) cost more than it saves.  Measured on the
 * repro table of DESIGN.md §28 (assert build), beside a selective equality:
 * 86,399 one-row entries 148 ms one by one and 40 ms a leaf at a time; 1,000
 * entries of 230 containers each 39 ms one by one and 165 ms as unions of
 * eight leaves' worth.
 */
#define LION_SUM_UNION_MAX_ITEMS	4

/*
 * How a range bounding a sum is evaluated (DESIGN.md §28, "The complement"):
 * the entries it selects, or the rows with a value minus the entries it does
 * not select, or - when it selects every entry - the rows with a value alone.
 */
#define LION_RANGE_EVAL_INSIDE		0
#define LION_RANGE_EVAL_COMPLEMENT	1
#define LION_RANGE_EVAL_FULL		2

/* Flag bits of the third integer of LION_PRIV_INTS. */
#define LION_FLAG_SINGLEGROUP	0x01
#define LION_FLAG_SUMALL			0x02
#define LION_FLAG_GROUPIDX		0x04	/* an index drives the entry scan */
#define LION_FLAG_RANGE			0x08	/* its walk is bounded by the RANGE
										 * clauses (DESIGN.md §28) */

/*
 * What the planner decided, in a form the executor can be handed through
 * custom_private.  Everything in there has to be a copyable/serialisable
 * node, so it is six plain lists plus one filled in at plan time, behind a
 * shape marker:
 *
 *	0	IntList: LION_PRIV_MAGIC and LION_PRIV_NMEMBERS.  The list is
 *		positional, so the executor checks this before reading anything else:
 *		a plan made by a differently shaped build of this library (a cached
 *		plan across an upgrade, a hand-built node) is then an error and not a
 *		list silently read at the wrong offsets.  Bump LION_PRIV_MAGIC whenever
 *		the meaning of a member changes without its position doing so.
 *	1	OidList: heap Oid, the outer and inner group index Oids (InvalidOid if
 *		none; the inner one only for a two-column GROUP BY, DESIGN.md §20),
 *		then one Oid per WHERE clause, in the same order as the other lists.
 *		An index may be a MULTICOLUMN one (DESIGN.md §24); which of its key
 *		columns each of these is read for is NOT carried here but derived at
 *		execution time from the opened index, the attnum in member 2 and
 *		the kind of opclass the use needs - multi-key for a multi-key clause
 *		(member 4), scalar for everything else (lion_index_col_for()).
 *		For a partitioned table the heap Oid is the PARENT's (EXPLAIN resolves
 *		column names against it) and every index Oid is InvalidOid: the real
 *		ones are per partition, in LION_PRIV_PARTS.
 *	2	IntList: base RT index, the outer and inner group attnums (0 if none),
 *		the LION_FLAG_* bits, then one attnum per WHERE clause.  Attnums are
 *		the PARENT's throughout; each partition's own numbering lives in its
 *		index Oids.
 *	3	List of Expr, one per WHERE clause: the compared value, the array of
 *		an IN list, or a NULL Const placeholder for a null test.  It is a
 *		Const for a literal query and a Param - or an ArrayExpr over Consts
 *		and Params - for a prepared one, or any other stable expression over
 *		those (DESIGN.md §10).  The PATH carries
 *		them here; lion_plan_custom_path() moves them into the CustomScan's
 *		custom_exprs and leaves this member empty, because that is the field
 *		setrefs.c fixes up and SS_finalize_plan() collects Param ids from -
 *		without which a changed exec Param would not rescan the node
 *	4	IntList: LION_CLAUSE_* for each WHERE clause
 *	5	List of OidList, one per live leaf partition and empty for a plain
 *		table (DESIGN.md §16): heap Oid, the outer and inner group index Oids
 *		(InvalidOid if none), then one index Oid per WHERE clause
 *	6	OidList: the operator of each WHERE clause (InvalidOid for a null
 *		test), which is what EXPLAIN prints a multi-key clause with
 *	7	List of IntList, one per OR restriction (DESIGN.md §19):
 *		{first clause index, number of arms, then the number of leaves in
 *		each arm}.  Its leaves are the clauses [first, first + sum) of the
 *		lists above, contiguous and in arm order; they are not sources of
 *		their own and pin no value the target list may print
 *	8	List of Expr: the HAVING clause as an implicit-AND list, the same
 *		columns and count aggregates as the target list, applied by the node
 *		to each finished group (DESIGN.md §10).  The PATH carries it here;
 *		lion_plan_custom_path() moves it into the CustomScan's plan.qual -
 *		where setrefs.c rewrites its aggregates into references to the
 *		node's own count columns - and leaves this member empty.  Empty for
 *		a partitioned table, whose HAVING is applied by the Finalize Agg
 *		above the node (§16)
 *	9	IntList: the attnum of the column a count(DISTINCT k) counts, or
 *		empty (DESIGN.md §26).  Its index is in member 1: in the outer group
 *		slot when there is no GROUP BY (k's entries drive the scan), in the
 *		inner slot beside a GROUP BY g (the (g, k) nested loop).  k is not a
 *		grouping column, so the inner group attnum of member 2 stays 0
 *	10	IntList: the FK-side join (DESIGN.md §27), or empty: the number of
 *		the clause that is the join key - an equality on the fact's fk whose
 *		value expression is the dimension's column - then the LION_JOIN_*
 *		kind of join and the LION_JOINFLAG_* bits, the sort operator and
 *		collation the keys are made distinct by (LION_JOINFLAG_UNIQUE; 0
 *		otherwise), and, added at plan time once the child plan exists, the
 *		position of that column in the child's target list.  The join key
 *		clause is not a source: the node looks it up once per child row (per
 *		distinct key), in source slot 0
 *	11	List of three OidLists: the functions whose evaluation the node
 *		replaces, for the EXECUTE checks the executor would have made on the
 *		plan it stands for (DESIGN.md §9, "Privileges"; checked at executor
 *		startup by lion_check_replaced_execute(), never at plan time): the
 *		aggregates of the target list and the HAVING; the functions of the
 *		WHERE clauses and of the FK-side join clause; and the equality
 *		functions of the GROUP BY
 *	12	List: GROUP BY coalesce(col, c) (DESIGN.md §10), or empty: the Const
 *		c, then an OidList of the grouping equality operator and collation,
 *		with which the entry walk recognises c's own entry.  The group
 *		column in member 2 is col
 *	13	IntList: LION_TL_* for each custom_scan_tlist column (added at plan
 *		time, when the target list is known)
 */
#define LION_PRIV_VERSION	0
#define LION_PRIV_OIDS		1
#define LION_PRIV_INTS		2
#define LION_PRIV_CONSTS		3
#define LION_PRIV_CLAUSEKINDS 4
#define LION_PRIV_PARTS		5
#define LION_PRIV_CLAUSEOPS	6
#define LION_PRIV_ORS		7
#define LION_PRIV_HAVING		8
#define LION_PRIV_DISTINCT	9
#define LION_PRIV_JOIN		10
#define LION_PRIV_EXECUTE	11
#define LION_PRIV_COALESCE	12
#define LION_PRIV_TLKINDS	13

/*
 * Shape of the list above: "RBI" and a shape version, and its length.  Shape
 * 2 dropped the group column member, which only the cross-partition hash
 * merge needed (DESIGN.md §16: the node emits partial aggregates now).  Shape
 * 3 moved the clause values out of member 3 and into custom_exprs, so that a
 * Param among them reaches setrefs.c and SS_finalize_plan() (DESIGN.md §10).
 * Shape 4 added the OR structure of DESIGN.md §19, and shape 5 the second
 * GROUP BY column of DESIGN.md §20.
 *
 * Shape 7 added the HAVING member (8) in front of the target-list kinds, and
 * shape 8 the DISTINCT member (9) there, with two new target-list kinds that
 * moved LION_TL_WHEREKEY (DESIGN.md §26).
 *
 * Shape 9 added the JOIN member (10) of DESIGN.md §27 in front of the
 * target-list kinds, and the negative target-list kinds that name a column of
 * the child plan.
 *
 * Shape 10 added the EXECUTE member (11) in front of the target-list kinds
 * (the 2026-09-23 review: a pushed-down count ran after EXECUTE on count() or
 * on the clause's operator had been revoked).
 *
 * Shape 11 moved no member but gave two of them a new meaning: the RANGE
 * clause kind of DESIGN.md §28, whose clauses are bounds on the driving entry
 * walk and not sources, and the LION_FLAG_RANGE bit that says so.  A build
 * that knows neither would take such a clause for a source.
 *
 * Shape 12 gave the JOIN member (10) the kind of join and its flags between
 * the clause number and the child's column (DESIGN.md §27's semi and anti
 * joins, and the fact filters collected once), and shape 13 the sort operator
 * and collation after them (the forward semi join over a non-unique key).
 *
 * Shape 14 added the COALESCE member (12) in front of the target-list kinds
 * (GROUP BY coalesce(col, c), DESIGN.md §10).
 *
 * Shape 6 changed no member's POSITION, which is exactly what the marker is
 * for: since DESIGN.md §24 an index Oid here may name a MULTICOLUMN index, and
 * the key column it is read for is not in the list at all - the executor
 * derives it from the index it really opened, the clause's heap attnum and
 * its kind (lion_index_col_for()), because a partition's index may put the
 * same column at a different position from the parent's.  A plan built before
 * that would have been made by a planner that never chose a multicolumn
 * index, so it would still decode correctly; saying so is cheaper than having
 * to know that.  Matching the kind as well changed no member either: the
 * planner has always chosen the column by it, so a plan of any shape-13 build
 * names the column the executor now derives.
 */
#define LION_PRIV_MAGIC		0x5242490e
#define LION_PRIV_NMEMBERS	14

/*
 * One WHERE clause of the pushdown, as the executor sees it.
 *
 * storedkey is the key the clause's entry holds, which is what a target list
 * that prints the pinned column has to report (see lion_emit_tuple()).  It is
 * remembered here rather than read out of the posting set, because with
 * partitions the set is released before the row is emitted; the first
 * partition that has the key wins, and any other partition's key compares
 * equal to it by the opclass equality.
 */
typedef struct LionClauseState
{
	int			kind;			/* LION_CLAUSE_* */
	Oid			idxoid;
	Oid			opno;			/* the clause's operator (0 for a null test) */
	AttrNumber	attno;			/* the HEAP column (the parent's, with §16's
								 * partitions) */
	AttrNumber	idxcol;			/* and its KEY COLUMN in `idx` (DESIGN.md
								 * §24), derived from the index that was really
								 * opened - which for a partition is that
								 * partition's own numbering */

	/*
	 * The compared value: its expression (from custom_exprs), the expression
	 * itself when it is a plain Const, an initialised ExprState when it is
	 * not, and the value once it has been evaluated.  A literal query has its
	 * value ready at plan time; a prepared one evaluates its Param - and any
	 * query its stable expression, `now() - interval '1 day'` - through the
	 * node's ExprContext at the start of every scan and after every ReScan,
	 * because a nested loop changes an exec Param between them (DESIGN.md
	 * §10).
	 */
	Expr	   *valexpr;
	Const	   *con;			/* valexpr, when it is a Const; else NULL */
	ExprState  *valstate;		/* set up when valexpr is not a Const */
	Oid			valtype;		/* type valexpr produces */
	Datum		val;
	bool		valisnull;

	StrategyNumber strategy;	/* LION_CLAUSE_MULTI: 2, 3 or 5 */

	/*
	 * LION_CLAUSE_MULTI: how this scan's query was answered.  A literal is
	 * KEYS, always (the planner only takes an exact one); a value the node
	 * evaluates is extracted as a superset (lion_locate_multikey(), DESIGN.md
	 * §17, "A query known only at run time"), and LOSSY and ALL then need
	 * every candidate rechecked in the heap - ALL with no posting set at all.
	 */
	LionQueryMode qmode;
	Relation	idx;
	Datum		storedkey;
	bool		hasstoredkey;
	bool		keyisnull;
} LionClauseState;

/*
 * One OR restriction (DESIGN.md §19), as a structure over the flattened
 * clause array: clauses [first, first + nleaves) are its leaves, grouped into
 * narms arms of armlen[] leaves each, in arm order.  An arm of more than one
 * leaf is their AND; the source is the OR of the arms.
 */
typedef struct LionOrState
{
	int			first;
	int			nleaves;
	int			narms;
	int		   *armlen;
} LionOrState;

/*
 * One input of the merge, after slot 0 (the group).  A plain clause is one
 * item; an OR restriction is one item over several clauses (DESIGN.md §19).
 */
typedef struct LionSourceItem
{
	int			clauseno;		/* the clause, or the OR's first leaf */
	int			orno;			/* -1, or the OR this item stands for */
	bool		rangesrc;		/* a range as a source (DESIGN.md §32): the
								 * first of its bounds, standing for all of
								 * them */
} LionSourceItem;

/*
 * A range taken as a source that was too large to collect into memory
 * (DESIGN.md §32, "A range as a source"): what LionCountSource.rangewalk
 * points at, and what lion_node_count() expands into the sets a walk of the
 * range hands out.
 */
typedef struct LionRangeSource
{
	Relation	index;
	AttrNumber	col;			/* its KEY COLUMN in index */
	LionRange	range;
} LionRangeSource;

/*
 * One relation the executor counts: a plain table, or one live leaf
 * partition (DESIGN.md §16).  The index Oids are that relation's own.
 */
typedef struct LionPartState
{
	Oid			heapoid;
	Oid			groupidxoid;	/* InvalidOid when no index drives the scan */
	Oid			groupidxoid2;	/* the inner one of a two-column GROUP BY */
	Oid		   *clauseidxoid;	/* one per WHERE clause */
} LionPartState;

/*
 * One child row of a batch the FK-side join looks up in key order (DESIGN.md
 * §27, "Lookups in key order"): its key, copied into the batch, with the hash
 * the directory order uses after the comparison; the row itself for the
 * target list, or NULL where the key is all the row there is (the distinct
 * keys of a forward semi join); and its place in the child's order, which
 * breaks the sort's ties so that a batch always sorts the same way.
 */
typedef struct LionJoinEnt
{
	Datum		key;
	uint32		hash;
	bool		isnull;			/* joins nothing: an anti join's row only */
	int32		seq;
	MinimalTuple tuple;
} LionJoinEnt;

typedef struct LionCountScanState
{
	CustomScanState css;

	/* decoded from custom_private */
	Oid			heapoid;
	Index		scanrelid;
	Oid			groupidxoid;
	Oid			groupidxoid2;	/* the inner index of a two-column GROUP BY */
	AttrNumber	groupattno;
	AttrNumber	groupattno2;

	/*
	 * The HEAP column whose entries drive the scan: groupattno, or - for the
	 * sum-over-all of DESIGN.md §14, which has no group column at all - the
	 * column of the `IS NOT NULL` clause the planner chose as its driver.  It
	 * is what the driving index's KEY COLUMN is derived from (§24), and it is
	 * NOT groupattno: the target list, EXPLAIN and the partitioned dispatch
	 * all ask `groupattno != 0` to mean "there is a GROUP BY".
	 */
	AttrNumber	driveattno;
	AttrNumber	groupidxcol;	/* key column of groupidx (§24) */
	AttrNumber	groupidxcol2;	/* ... and of groupidx2 */

	/*
	 * count(DISTINCT k) (DESIGN.md §26).  distattno is k, or 0.  Without a
	 * GROUP BY k's index is groupidx and drives the scan (driveattno is k);
	 * beside a GROUP BY g it is groupidx2, the INNER side of the nested loop.
	 * innerattno is the heap column of groupidx2 in either use - the second
	 * grouping column of §20, or k - and is what everything that reads the
	 * inner index asks, while groupattno2 keeps meaning "a second GROUP BY
	 * column".
	 *
	 * distfull says a test has to COUNT rather than stop at the first visible
	 * row, because the target list wants rows as well: without a GROUP BY any
	 * other count does (the total is the sum over k's entries), with one only
	 * count(k) does - count(*) and count(g) need the GROUP's count, which is
	 * distgroupcount.  distcount and distcolcount are the finished group's
	 * count(DISTINCT k) and count(k), read by lion_emit_tuple(), and
	 * disttests is what EXPLAIN ANALYZE reports as "Distinct Keys Tested".
	 */
	AttrNumber	distattno;
	AttrNumber	innerattno;
	bool		distfull;
	bool		distgroupcount;
	int64		distcount;
	int64		distcolcount;
	int64		disttests;

	/*
	 * GROUP BY coalesce(g, c) (DESIGN.md §10), when hascoal: c, and the
	 * grouping equality and collation the entry walk recognises c's own entry
	 * by.  The walk adds the NULL entry's count and that entry's into
	 * coalcount instead of emitting either, and emits their one group - as c
	 * - after the last entry of the relation; coalwalked says that has
	 * happened, so the next call returns nothing more.
	 */
	bool		hascoal;
	Const	   *coalconst;
	Oid			coaleqop;
	Oid			coalcoll;
	FmgrInfo	coaleqfn;
	int64		coalcount;
	bool		coalwalked;
	bool		singlegroup;	/* GROUP BY over constant columns only */
	bool		sumall;			/* no GROUP BY, but every entry of the group
								 * index is counted and summed (DESIGN.md §14,
								 * `col IS NOT NULL` with nothing else) */
	bool		hasgroupidx;	/* an index's entries drive the count */

	/*
	 * The RANGE clauses of DESIGN.md §28: they bound the driving index's
	 * entry walk instead of being sources, all of them on its column and
	 * ANDed into `range`, which lion_locate_where() resolves against the
	 * relation being counted (a partition's own index, §16) and every begin
	 * of the driver's walk hands to lion_entry_scan_begin_range().
	 */
	bool		hasrange;
	LionRange	range;

	/*
	 * How each relation's range-bounded sum was evaluated (DESIGN.md §28, "The
	 * complement"), indexed by LION_RANGE_EVAL_*: what EXPLAIN ANALYZE prints
	 * as "Range Evaluation".
	 */
	int64		rangeeval[3];

	/*
	 * Summary entries the sums added up in place of the keys they cover
	 * (DESIGN.md §32): what EXPLAIN ANALYZE prints as "Summaries Summed".
	 */
	int64		summaries;

	/*
	 * Summed walks over a summarized column that were PROBED at the rows of
	 * the other sources instead of counted set by set (DESIGN.md §32, "Summed
	 * ranges: dense and probed"): what EXPLAIN ANALYZE prints as "Range Walks
	 * Probed".
	 */
	int64		rangeprobed;

	/*
	 * The ranges taken as sources (DESIGN.md §32, "A range as a source"): how
	 * many were collected into memory, how many were too large and were
	 * summed over their walk at every count instead, and what the collected
	 * ones hold - all of them within one hash table's memory.
	 */
	int64		rangesrc_collected;
	int64		rangesrc_walked;
	int64		rangesrc_spilled;	/* of the collected: in a temporary file */
	Size		rangesrc_held;
	int			nclause;
	LionClauseState *clause;
	int			ntlist;
	int		   *tlkind;

	/*
	 * The OR restrictions (DESIGN.md §19) and the sources they and the plain
	 * clauses make up.  item[k] describes source slot k + 1; a clause that is
	 * an OR leaf has no source of its own, which is what inor[] says.
	 */
	int			nor;
	LionOrState *ors;
	bool	   *inor;			/* one per clause */
	int			nitem;
	LionSourceItem *item;

	/*
	 * The relations to count.  npart is 0 for a plain table, whose heap and
	 * indexes are opened once for the life of the node; a partitioned one
	 * (DESIGN.md §16) has one LionPartState per live leaf partition and opens
	 * them one partition at a time, so that no partition's buffer pin ever
	 * outlives that partition's processing.
	 */
	int			npart;
	LionPartState *part;

	/* runtime: the relation currently being counted */
	Relation	heap;
	Relation	groupidx;
	Relation	groupidx2;

	/*
	 * The inputs of the count: slot 0 is the group (or the driving index of
	 * a sumall), slots 1 .. nitem the WHERE items - one per plain clause and
	 * one per OR restriction (DESIGN.md §19) - and, for a two-column GROUP BY
	 * (DESIGN.md §20), slot nitem + 1 is the inner group.  The clause sources
	 * are located once per node execution and kept until the node is done,
	 * reset or closed - their pins (DESIGN.md section 9) only until the first
	 * row goes up, which leaves them NOPIN copies (lion_pause_run()); the
	 * groups' sets are located, counted and released one group (one pair) at
	 * a time.
	 */
	LionCountSource *sources;
	int			nsource;		/* nitem + 1, or nitem + 2 with two group cols */
	LionPostingSet groupset;
	LionPostingSet groupset2;
	bool		located;
	bool		valsdone;		/* the clause values have been evaluated */
	bool		wheremissing;	/* a positive clause selects nothing at all */
	bool		scanning;
	bool		done;
	bool		filtered;		/* the last group failed HAVING, fetch the next */
	LionEntryScan escan;

	/*
	 * The nested loop of a two-column GROUP BY (DESIGN.md §20).  The outer
	 * index's entries drive the scan exactly as a single group column's do;
	 * the inner index's KEYS are read once per relation into innercxt and
	 * each pair's inner posting set is located afresh, because a located set
	 * holds a buffer pin and there must be no pin per distinct inner value
	 * (DESIGN.md §9).  When the keys do not fit the work_mem budget innerkey
	 * is NULL and the inner index's entry scan is walked once per outer group
	 * instead, which needs no memory at all.
	 */
	Datum	   *innerkey;
	bool	   *innerisnull;
	int			ninnerkey;
	int			inneridx;		/* next inner key of the current outer group */
	bool		outeropen;		/* groupset holds the current outer group */
	bool		wherepinned;	/* WHERE sets may hold pins (since the last
								 * lion_locate_where()) */
	Datum		outerkey;
	bool		outerisnull;
	LionEntryScan escan2;		/* the innerkey == NULL fallback */
	bool		scanning2;

	/*
	 * A WHERE item that the DRIVER makes redundant, because the entries of a
	 * scalar lion index are disjoint (DESIGN.md §15).  There are two shapes of
	 * it and they are mutually exclusive; in both, dsources is st->sources with
	 * that item left out and slot 0 pointed at the driver's set, and ndsource
	 * is how many of it are in use.
	 *
	 *	ingroupitem	an IN list on the very column a GROUP BY drives.  The
	 *				groups are then exactly the listed values and each one's
	 *				rows are that value's own entry, so the node walks the
	 *				clause's already-located posting sets instead of every entry
	 *				of the index - a thousand sets instead of twenty thousand on
	 *				`c20k` - and the clause is not intersected with them,
	 *				because `entry ∩ (entry ∪ the rest of the list)` is the
	 *				entry.  ingroupset is how far that walk has got.
	 *	sumallitem	the `col IS NOT NULL` of a sum-over-all (DESIGN.md §14) on
	 *				the very index that drives it.  That clause is a NEGATED
	 *				source - the column's NULL entry, subtracted - and
	 *				subtracting it from another entry of the same index removes
	 *				nothing, while subtracting it from ITSELF leaves nothing.
	 *				So the driver skips the NULL entry and drops the source,
	 *				which takes an andnot against a dense posting set off every
	 *				container key of every entry.
	 *
	 * Both are -1 when the shape does not apply.
	 */
	int			ingroupitem;
	int			ingroupset;		/* next set of that item */
	int			ingroupleft;	/* ... and how many of its sets that have an
								 * entry are still to come */
	int			sumallitem;
	LionCountSource *dsources;
	int			ndsource;

	/*
	 * The WHERE items of a GROUP BY, COLLECTED once per relation into one
	 * private posting set that every later group - or pair of groups (§20) -
	 * is counted against instead of the items themselves (DESIGN.md §10, "The
	 * WHERE sets, collected once"; lion_group_count()).  Without it each
	 * count intersects the items again and reads the pages of every set it
	 * cannot keep a copy of once per group.
	 *
	 *	wtried		the collection has been made, or refused for good, for
	 *				the relation being counted
	 *	wcollected	... and made: wherecoll holds it, in wherecxt
	 *	wknown		wreach, wcompound and wsingle describe the WHERE items
	 *	wreach		what collecting them reads, in container keys: the reach
	 *				of the positive item with the fewest rows, which drives
	 *				the collection's merge
	 *	wcompound	more than one item to combine, or one union - worth
	 *				collecting whatever the sets are
	 *	wsingle		the one set of a lone item, which is worth collecting
	 *				only while every count walks its pages
	 *				(lion_posting_set_rewalked()), or NULL
	 *	wspent		what the counts so far read of the items, in the same
	 *				units, and wcounts how many there were
	 *	wckeys		the relation's container keys, which cap every reach
	 *
	 * wsources is where a count's sources are put together with wherecoll:
	 * the driver's slot, the collected set, the items it does not hold, and a
	 * second group's slot.  wherecollected and wherespilled are what EXPLAIN
	 * ANALYZE reports.
	 */
	bool		wtried;
	bool		wcollected;
	bool		wknown;
	bool		wcompound;
	LionPostingSet *wsingle;
	LionPostingSet wherecoll;
	double		wreach;
	double		wspent;
	int64		wcounts;
	double		wckeys;
	LionCountSource *wsources;
	int64		wherecollected;
	int64		wherespilled;

	/*
	 * An IN list too long to locate at once (DESIGN.md §15, "A list too long
	 * to locate at once"): the WHERE item it is, -1 for none, and its non-NULL
	 * values - of type batchtype - sorted into the order a lookup takes them
	 * in, with their hashes, which lion_count_batched() locates and counts a
	 * batch at a time.  Only a count of one row per relation takes it; the
	 * values live in wherecxt.  listbatches is how many batches were counted,
	 * for EXPLAIN ANALYZE.
	 */
	int			batchitem;
	Oid			batchtype;
	int			nbatchval;
	Datum	   *batchval;
	uint32	   *batchhash;
	int64		listbatches;

	/*
	 * GROUP BY over a partitioned table (DESIGN.md §16): the partitions are
	 * walked one at a time and each one's groups are emitted as PARTIAL
	 * aggregates as they are counted, so the node's only state between rows
	 * is which partition is open and how far its entry scan has got.  curpart
	 * is the partition being scanned and partopen says whether it is open;
	 * core's Finalize HashAggregate above combines the partial counts.
	 */
	int			curpart;
	bool		partopen;

	MemoryContext pergroup;		/* reset before each group is counted */
	MemoryContext outercxt;		/* §20: the outer group's set and key */
	MemoryContext innercxt;		/* §20: the inner index's cached keys */
	MemoryContext wherecxt;		/* the located WHERE payload copies */
	MemoryContext keycxt;		/* the clause keys a target list may print */
	MemoryContext valcxt;		/* the evaluated Param values */

	/*
	 * One visibility cache for the whole node execution (DESIGN.md §9).
	 * Every group of every partition counts through it, so a heap block the
	 * visibility map cannot vouch for is fetched once per query however many
	 * groups come back to it - which is what the cost model above is allowed
	 * to assume.  It is emptied on ReScan and whenever the relation or the
	 * snapshot changes under it (lion_count_sources_cached() does the latter).
	 */
	LionVisCache *viscache;
	LionCountStats stats;

	/*
	 * The heap recheck of the multi-key clauses whose run-time query the
	 * posting sets could not answer exactly (DESIGN.md §17, "A query known
	 * only at run time"), or NULL.  Built by lion_locate_where() for the
	 * relation being counted, in wherecxt, and set on viscache, which is how
	 * every count of this execution finds it; lion_release_where() takes it
	 * away again.
	 */
	LionRowFilter *filter;

	/*
	 * Whether the statement leaves the relation being counted alone
	 * (DESIGN.md §11, "On-access pruning sets the visibility map too").  On
	 * PostgreSQL 19 and later that is what lets the heap recheck's on-access
	 * pruning mark the pages it cleans all-visible.  writtenrels is every
	 * relation the statement modifies or row-locks, collected once when the
	 * node starts; rel_read_only is decided from it whenever a relation (a
	 * table, or one partition) is opened.
	 */
	List	   *writtenrels;
	bool		rel_read_only;

	/*
	 * Directory pages this node's execution has read (DESIGN.md §21), as the
	 * difference of the process-wide counter across each ExecCustomScan call.
	 * It is what EXPLAIN ANALYZE prints as "Directory Pages Read", and what
	 * test/sql/directory.sql uses to prove that a sorted IN list costs one
	 * pass over the leaves it crosses instead of a descent per value.
	 */
	int64		dirpages;

	/*
	 * The FK-side join (DESIGN.md §27).  joinclause is the clause that is the
	 * join key, or -1 for every other shape; its value is column joinkeyresno
	 * of the child plan's current row, childslot, which is also where the
	 * target list's dimension columns are read from.  The key's posting set is
	 * located into groupset and counted as source slot 0, once per child row.
	 * joinlookups and joinmissing are what EXPLAIN ANALYZE reports.
	 */
	int			joinclause;
	AttrNumber	joinkeyresno;
	PlanState  *child;
	TupleTableSlot *childslot;
	int64		joinlookups;
	int64		joinmissing;

	/*
	 * What each child row's count is (DESIGN.md §27): the number of fact rows
	 * it joins for an inner join, and for a semi or anti join whether it
	 * joins any - LION_JOIN_*.  joincollect says the plan wants the fact
	 * filters collected once into joinfilter (lion_sources_collect(), in
	 * outercxt) and each fk set counted against that copy instead of against
	 * the filters themselves; joincollected says the attempt has been made
	 * for this run, and joinfiltered that it worked - joinsources is then
	 * what each count reads.  joinfilterrows is the copy's size, for EXPLAIN.
	 */
	int			jointype;
	bool		joincollect;
	bool		joinrows;
	bool		joinsum;		/* LION_JOINFLAG_SUM: one row, the sum */
	bool		joincollected;
	bool		joinfiltered;
	LionPostingSet joinfilter;
	LionCountSource joinsources[2];
	int64		joinfilterrows;
	int64		joinspilled;	/* copies that went to a temporary file */

	/*
	 * The forward semi join over a non-unique key (DESIGN.md §27, "Forward
	 * semi joins over a non-unique key"): joinunique says each key is counted
	 * once however many child rows carry it.  The keys of the child's rows go
	 * into joinsort first, a datum sort by joinsortop under joinsortcoll, and
	 * a key joineqfn finds equal to the previous distinct one, joinprevkey
	 * (in joinkeycxt), is skipped; each distinct key is handed on as the key
	 * column of joinsortslot, a row of the child's shape whose other columns
	 * nothing reads.  joinkeypos numbers the distinct keys, and in a parallel
	 * plan joinchunk is the run of them this participant claimed last, -1
	 * before the first.  joinsorted is the keys sorted, and joinsortstats
	 * what the sort did, for EXPLAIN ANALYZE.
	 */
	bool		joinunique;
	Oid			joinsortop;
	Oid			joinsortcoll;
	FmgrInfo	joineqfn;
	Oid			joinkeytype;
	bool		joinkeybyval;
	int16		joinkeylen;
	Tuplesortstate *joinsort;
	bool		joinsortdone;
	TupleTableSlot *joinsortslot;
	MemoryContext joinkeycxt;
	Datum		joinprevkey;
	bool		joinhaveprev;
	int64		joinkeypos;
	int64		joinchunk;
	int64		joinsorted;
	bool		joinhavesortstats;
	TuplesortInstrumentation joinsortstats;

	/*
	 * Lookups in key order (DESIGN.md §27, "Lookups in key order"): joinwalk
	 * says the plan reads the child's rows - or the distinct keys - a batch
	 * at a time, as many as work_mem holds, into joinbatch in joinbatchcxt,
	 * sorts them into the fk index's directory order and locates their keys
	 * with one walk of its leaves, joinwalker, begun at the first batch
	 * (joinwalkbegun).  joinbatchpos is the next row of the batch to look up,
	 * joinchilddone that the child has no rows left, joinbatchslot the slot a
	 * batched row is handed to the target list in, and joinbatches how many
	 * batches there were, for EXPLAIN ANALYZE.  The last key looked up and
	 * what it found (joinlast*) answer the next row as well when it has the
	 * same key: a duplicated dimension key is looked up once a batch.  The
	 * key's type (joinkeytype, joinkeybyval, joinkeylen) is set for this as
	 * for joinunique.
	 */
	bool		joinwalk;
	bool		joinwalkbegun;
	LionLookupWalk joinwalker;
	MemoryContext joinbatchcxt;
	LionJoinEnt *joinbatch;
	int			joinbatchn;
	int			joinbatchcap;
	int			joinbatchpos;
	bool		joinchilddone;
	TupleTableSlot *joinbatchslot;
	int64		joinbatches;
	bool		joinlasthave;
	Datum		joinlastkey;
	bool		joinlastfound;
	int64		joinlastcount;

	/*
	 * A parallel FK-side join (DESIGN.md §27, "Parallel"): every participant
	 * counts the dimension rows its share of the child returns and adds what
	 * EXPLAIN ANALYZE reports into joinshared, in the dynamic shared memory
	 * of the Gather above it, when it shuts down; the leader copies the sums
	 * into the joinworker fields before that memory goes.  They are NULL,
	 * zero and -1 outside a parallel plan.
	 */
	struct LionJoinShared *joinshared;
	bool		joinreported;
	LionCountStats joinworkerstats;
	int64		joinworkerlookups;
	int64		joinworkermissing;
	int64		joinworkerdirpages;
	int64		joinworkerfilterrows;
	int64		joinworkersorted;
	int64		joinworkerspilled;
	int64		joinworkerbatches;

	/*
	 * Where a key's time goes (DESIGN.md §27): the rows the child returned,
	 * the posting-tree pages the keys' counts read (lion_posting_pages_read
	 * over each count) and - only under EXPLAIN ANALYZE with its TIMING
	 * option, which is what jointiming says, as core times a node only then -
	 * the time spent in each phase (LION_JT_*).  The count's own counters are
	 * in stats (LionCountStats: key_containers, copy_containers, ...).  The
	 * joinworker* copies are the workers' sums, as for the counters above.
	 */
	int64		joinchildrows;
	int64		joinposting;
	bool		jointiming;
	instr_time	jointime[LION_JT_N];
	int64		joinworkerchildrows;
	int64		joinworkerposting;
	instr_time	joinworkertime[LION_JT_N];
} LionCountScanState;

/*
 * What the participants of a parallel FK-side join add up for EXPLAIN
 * ANALYZE, in the Gather's dynamic shared memory (lion_shutdown_custom_scan()).
 * filterrows is the largest copy any participant made of the fact filters,
 * -1 when none did.  sorted is the keys a participant of this run sorted -
 * every participant sorts all of them - and sortedruns the same summed over
 * the runs before it (lion_reinitialize_dsm()).
 *
 * nextchunk is the one thing the participants share while they run: the next
 * run of LION_FKJOIN_UNIQUE_CHUNK distinct keys of a forward semi join over a
 * non-unique key that nobody has claimed yet.  participants is how many the
 * leader started the plan for, itself included, which is how many ways each
 * of them divides the list pin budget (lion_list_pin_participants()).
 * batches is the batches of keys looked up in key order, summed, and
 * childrows, posting and time the rows, the posting pages and the phases of
 * "Where a key's time goes" (DESIGN.md §27), summed.
 */
typedef struct LionJoinShared
{
	slock_t		mutex;
	LionCountStats stats;
	int64		lookups;
	int64		missing;
	int64		dirpages;
	int64		filterrows;
	int64		spilled;
	int64		sorted;
	int64		sortedruns;
	int64		batches;
	int64		childrows;
	int64		posting;
	instr_time	time[LION_JT_N];
	pg_atomic_uint32 nextchunk;
	int			participants;
} LionJoinShared;

static Plan *lion_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
								  CustomPath *best_path, List *tlist,
								  List *clauses, List *custom_plans);
static Node *lion_create_custom_scan_state(CustomScan *cscan);
static void lion_begin_custom_scan(CustomScanState *node, EState *estate,
								  int eflags);
static TupleTableSlot *lion_exec_custom_scan(CustomScanState *node);
static TupleTableSlot *lion_exec_custom_scan_internal(CustomScanState *node);
static void lion_end_custom_scan(CustomScanState *node);
static void lion_rescan_custom_scan(CustomScanState *node);
static void lion_explain_custom_scan(CustomScanState *node, List *ancestors,
									ExplainState *es);
static Size lion_estimate_dsm(CustomScanState *node, ParallelContext *pcxt);
static void lion_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
								void *coordinate);
static void lion_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
								  void *coordinate);
static void lion_initialize_worker(CustomScanState *node, shm_toc *toc,
								   void *coordinate);
static void lion_shutdown_custom_scan(CustomScanState *node);

static const CustomPathMethods lion_count_path_methods = {
	.CustomName = "LionCount",
	.PlanCustomPath = lion_plan_custom_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods lion_count_scan_methods = {
	.CustomName = "LionCount",
	.CreateCustomScanState = lion_create_custom_scan_state,
};

static const CustomExecMethods lion_count_exec_methods = {
	.CustomName = "LionCount",
	.BeginCustomScan = lion_begin_custom_scan,
	.ExecCustomScan = lion_exec_custom_scan,
	.EndCustomScan = lion_end_custom_scan,
	.ReScanCustomScan = lion_rescan_custom_scan,
	.EstimateDSMCustomScan = lion_estimate_dsm,
	.InitializeDSMCustomScan = lion_initialize_dsm,
	.ReInitializeDSMCustomScan = lion_reinitialize_dsm,
	.InitializeWorkerCustomScan = lion_initialize_worker,
	.ShutdownCustomScan = lion_shutdown_custom_scan,
	.ExplainCustomScan = lion_explain_custom_scan,
};


/* =====================================================================
 * Planner
 * ===================================================================== */

/*
 * Peel binary-coercion relabels off an expression.  A varchar column
 * compared with a text constant arrives as RelabelType(Var) = Const, and the
 * lion index on that column is a text_ops index, so the relabelled form is
 * exactly what we want to match.
 */
static Node *
lion_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * Does this opfamily extract many keys from one value (DESIGN.md §17)?  The
 * presence of support function 2 is the same test lion_fill_state() makes.
 */
static bool
lion_opfamily_is_multikey(Oid opfamily, Oid opcintype)
{
	return OidIsValid(get_opfamily_proc(opfamily, opcintype, opcintype,
										LION_EXTRACTVALUE_PROC));
}

/*
 * A usable lion index on one plain column of rel, or NULL.  Only indexes
 * the planner put in rel->indexlist are considered, which already excludes
 * invalid ones (get_relation_info() skips !indisvalid).
 *
 * multikey selects between the two shapes of opclass, and the caller always
 * knows which one it needs: a multi-key index's ENTRIES are keys and not
 * column values, so it can answer `tags @> '{a}'` but can neither drive a
 * GROUP BY (the entries would be lexemes, not arrays) nor be summed over
 * (a row appears under each of its keys, so the sum of the entries is not
 * the number of rows - which is what DESIGN.md §14's sum-over-all rests on).
 *
 * *colp receives the INDEX COLUMN (1-based) that indexes `attno` (DESIGN.md
 * §24).  A multicolumn lion index holds each column's keys as an independent
 * set of entries, so ANY of its columns will do and the rest of this file
 * carries that number beside the index; colp may be NULL for a caller that
 * only asks whether such an index exists.
 */
static IndexOptInfo *
lion_find_roaring_index(RelOptInfo *rel, AttrNumber attno, bool multikey,
					   AttrNumber *colp)
{
	Oid			amoid = lion_get_am_oid();
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		int			i;

		if (idx->relam != amoid)
			continue;
		if (idx->hypothetical)
			continue;
		if (idx->indpred != NIL || idx->indexprs != NIL)
			continue;

		/*
		 * ANY key column, not just the first (DESIGN.md §24).  INCLUDE columns
		 * (ncolumns > nkeycolumns) cannot happen - amcaninclude is false - but
		 * the loop is bounded by nkeycolumns anyway, because an INCLUDE column
		 * has no opclass to ask about.
		 *
		 * `indexprs`: an expression column has indexkeys[i] == 0 and can never
		 * match a heap attno, so the skip above could in principle be relaxed
		 * to "skip the expression COLUMNS".  It is left as it is: the count
		 * pushdown has no way to evaluate the expression for its output.
		 * `indpred` likewise - a partial index would need its predicate
		 * applied, which this node does not do.
		 */
		for (i = 0; i < idx->nkeycolumns; i++)
		{
			if (idx->indexkeys[i] != attno)
				continue;
			if (lion_opfamily_is_multikey(idx->opfamily[i],
										 idx->opcintype[i]) != multikey)
				continue;

			if (colp != NULL)
				*colp = (AttrNumber) (i + 1);
			return idx;
		}
	}

	return NULL;
}

/*
 * The equality operator an index's opclass defines on its own key type: the
 * relation whose classes its entries are.  A scalar roaring opclass always
 * has it (the AM requires strategy 1), but an opfamily that only declares
 * cross-type members for (opcintype, opcintype) would not, and then nothing
 * below can be proved about the index.
 *
 * col is the index's KEY COLUMN (DESIGN.md §24): every column of a
 * multicolumn index has an opclass of its own, so every question about an
 * opclass has to name one.
 */
static Oid
lion_index_equality_op(IndexOptInfo *idx, AttrNumber col)
{
	int			i = col - 1;

	return get_opfamily_member(idx->opfamily[i], idx->opcintype[i],
							   idx->opcintype[i], LION_STRAT_EQUAL);
}

/*
 * Does equality on this type imply that equal values have the same binary
 * representation?
 *
 * This is the question btree deduplication asks before it may replace one
 * tuple with another that compares equal (_bt_allequalimage() in
 * src/backend/access/nbtree/nbtutils.c), and it is exactly the question the
 * count pushdown has to ask before it prints a key an index stored instead of
 * a value a visible row holds: a posting set keeps ONE representative per
 * equality class, and if the type allows two equal values to look different
 * (citext 'Bob'/'BOB', numeric 1.0/1.00, a nondeterministic collation) that
 * representative may be a spelling no visible row contains (the 2026-09-20
 * review, finding 4).
 *
 * The test is the type's DEFAULT btree opclass (lookup_type_cache with
 * TYPECACHE_BTREE_OPFAMILY, the same family SortGroupClause.eqop comes from),
 * its BTEQUALIMAGE_PROC support function, called under the collation the
 * index compared its keys with - which is how btequalimage/btvarstrequalimage
 * decide determinism.  No support function means no (that is btree's rule as
 * well).
 *
 * That answer is necessary but not sufficient.  equalimage promises that
 * equal values are "interchangeable without loss of semantic information",
 * which is what deduplication needs, and bpchar - whose trailing blanks carry
 * no meaning to it - registers btvarstrequalimage although 'a   ' = 'a' and
 * bpcharout prints the blanks: an entry indexed as 'a   ' then printed a
 * deleted row's spelling for a visible 'a' (the 2026-09-23 review).  An
 * extension's function is only its author's word, on the same weaker
 * promise.  So the type also has to be one of the core types below, each of
 * whose equality compares every byte its output function prints: fixed-width
 * integers and the date/time types (timetz compares the zone as well as the
 * instant), uuid, bytea, bit strings (their lengths too), MAC addresses,
 * inet (family, prefix length and the whole address; cidr is indexed as inet),
 * enums, and text and name, whose equality under a deterministic collation -
 * which the support function still decides - is a byte comparison.  bpchar
 * is left out on purpose; numeric, the floats (-0 and 0), interval ('1 day'
 * and '24 hours'), jsonb, arrays and ranges have no support function and are
 * refused either way.  A domain is indexed under its base type's opclass.
 */
static bool
lion_type_equalimage(Oid typid, Oid collation)
{
	TypeCacheEntry *typentry;
	Oid			proc;

	switch (typid)
	{
		case BOOLOID:
		case CHAROID:
		case NAMEOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case OIDVECTOROID:
		case XID8OID:
		case MONEYOID:
		case PG_LSNOID:
		case TEXTOID:
		case BYTEAOID:
		case BITOID:
		case VARBITOID:
		case DATEOID:
		case TIMEOID:
		case TIMETZOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case UUIDOID:
		case INETOID:
		case MACADDROID:
		case MACADDR8OID:
		case ANYENUMOID:
			break;
		default:
			return false;
	}

	typentry = lookup_type_cache(typid, TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(typentry->btree_opf) || !OidIsValid(typentry->btree_opintype))
		return false;

	proc = get_opfamily_proc(typentry->btree_opf, typentry->btree_opintype,
							 typentry->btree_opintype, BTEQUALIMAGE_PROC);
	if (!OidIsValid(proc))
		return false;

	/*
	 * A collatable type's support function insists on being told a collation
	 * (check_collation_set() in btvarstrequalimage()), so a key type that has
	 * one but an index that does not is simply refused here.
	 */
	if (OidIsValid(get_typcollation(typentry->btree_opintype)) &&
		!OidIsValid(collation))
		return false;

	return DatumGetBool(OidFunctionCall1Coll(proc, collation,
											 ObjectIdGetDatum(typentry->btree_opintype)));
}

/*
 * May the node print a value taken from this index's stored keys?
 *
 * Two things have to hold, and both are properties of the index rather than
 * of the query, so they are checked once per relation (per partition: nothing
 * stops two partitions from using different opclasses):
 *
 *	- the index's own equality has to BE the type's equality, so that "in the
 *	  same entry" implies "equal" in the sense the next test is about.  The
 *	  index groups rows by strategy 1 of its opfamily, which is free to be a
 *	  coarser relation than the type's default equality (the review's
 *	  lower()-based text opclass is a valid opclass and a coarser one);
 *	- and equality has to imply an identical representation, or the stored
 *	  representative may be a spelling no visible row has.
 *
 * When either fails the query may still be pushed down as a COUNT: counting
 * an equality class needs no representative.  Only value-producing pushdowns
 * - the GROUP BY column in the output, or a column a WHERE clause pins whose
 * value the target list prints - come through here.
 */
static bool
lion_index_can_emit_value(IndexOptInfo *idx, AttrNumber col)
{
	Oid			typid = idx->opcintype[col - 1];
	Oid			idxeq = lion_index_equality_op(idx, col);
	TypeCacheEntry *typentry;
	Oid			typeeq;

	if (!OidIsValid(idxeq))
		return false;

	typentry = lookup_type_cache(typid, TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(typentry->btree_opf) || !OidIsValid(typentry->btree_opintype))
		return false;
	typeeq = get_opfamily_member(typentry->btree_opf, typentry->btree_opintype,
								 typentry->btree_opintype,
								 BTEqualStrategyNumber);
	if (!OidIsValid(typeeq) || typeeq != idxeq)
		return false;

	return lion_type_equalimage(typid, idx->indexcollations[col - 1]);
}

/*
 * The same, but for a column a particular clause is applied to.
 *
 * For a scalar clause (`=`, `= ANY`, a null test) opno has to be strategy 1
 * of the index's opfamily (the index's opfamily and the operator Oid, so
 * cross-type integer equality is fine), and cmptype - the type the column is
 * compared with, InvalidOid for a null test - has to be one the opfamily can
 * compare with the indexed type and can hash, or the lookup in lion_count.c
 * would fail at run time.
 *
 * For a multi-key clause (DESIGN.md §17) the index must be a multi-key one
 * whose opfamily gives opno the strategy the planner decided on, and whose
 * extractQuery function is the very one the plan-time extraction used: the
 * plan was only made because that function called the query exact, and a
 * different function might not.
 *
 * Every partition is checked separately, because nothing stops one of them
 * from carrying a lion index built with a different opclass.
 */
static IndexOptInfo *
lion_match_index(RelOptInfo *rel, AttrNumber attno, int kind, Oid opno,
				Oid cmptype, StrategyNumber strategy, Oid extractquery,
				Oid exprcoll, AttrNumber *colp)
{
	bool		multikey = (kind == LION_CLAUSE_MULTI);
	AttrNumber	col = 1;
	IndexOptInfo *idx = lion_find_roaring_index(rel, attno, multikey, &col);
	int			i;

	if (idx == NULL)
		return NULL;
	i = col - 1;				/* the KEY COLUMN's opclass (DESIGN.md §24) */

	/*
	 * The planner's own rule, IndexCollMatchesExprColl(): a collation-
	 * sensitive clause may only use an index built under that collation.
	 * The index hashed and compared its keys with its own collation and the
	 * count never rechecks the predicate, so a mismatch (say a case-
	 * insensitive index under a case-sensitive query) would count rows the
	 * query does not select.
	 */
	if (OidIsValid(exprcoll) && idx->indexcollations[i] != exprcoll)
		return NULL;

	if (multikey)
	{
		if (get_op_opfamily_strategy(opno, idx->opfamily[i]) != strategy)
			return NULL;
		if (get_opfamily_proc(idx->opfamily[i], idx->opcintype[i],
							  idx->opcintype[i],
							  LION_EXTRACTQUERY_PROC) != extractquery)
			return NULL;
		if (colp != NULL)
			*colp = col;
		return idx;
	}

	/*
	 * A class declared on a polymorphic type (enum_ops is FOR TYPE anyenum)
	 * names its members on that type - (anyenum, anyenum), and the hash proc
	 * for it - while the value a clause compares the column with is of the
	 * column's own enum: the constant of `k = 'x'`, the elements of `k IN
	 * (...)`, a range bound.  That is one of the class's own values and not a
	 * cross-type search, so it is resolved to the class's type before any
	 * member is looked up, for EVERY kind of clause.  Only the range
	 * comparison of DESIGN.md §28 used to do this, and `k = 'x'` and `k IN
	 * (...)` on an enum column asked for an (anyenum, mood) member that no
	 * family has: the index was declined and the node never reached, on the
	 * very columns it is best at.  The test is the one lion_probe_init()
	 * makes at run time (lion_type_is_column(), lion_count.c): the same BASE
	 * type as the key column's own - a domain over the enum is its enum, and
	 * a different enum, whose OIDs mean nothing to this column, is not.
	 */
	if (OidIsValid(cmptype) && IsPolymorphicType(idx->opcintype[i]))
	{
		Oid			coltype = get_atttype(idx->indexoid, col);

		if (OidIsValid(coltype) &&
			getBaseType(cmptype) == getBaseType(coltype))
			cmptype = idx->opcintype[i];
	}

	/*
	 * A range comparison (DESIGN.md §28) has to be one of the index's range
	 * strategies, with the ordering its walk needs - proc 4 for the pair - in
	 * the same family; lionvalidate() insists on both together, and this is
	 * where a catalogue that disagrees is declined rather than walked
	 * linearly.  The equality and hash checks below apply to it as well: the
	 * executor resolves the bound's comparison through lion_probe_init(),
	 * which needs them.
	 */
	if (kind == LION_CLAUSE_RANGE || kind == LION_CLAUSE_RANGESRC)
	{
		if (!LION_STRAT_IS_RANGE(get_op_opfamily_strategy(opno,
														  idx->opfamily[i])))
			return NULL;
		if (!OidIsValid(get_opfamily_proc(idx->opfamily[i], idx->opcintype[i],
										  cmptype, LION_CMP_PROC)))
			return NULL;
	}
	else if (OidIsValid(opno) &&
			 get_op_opfamily_strategy(opno, idx->opfamily[i]) != LION_STRAT_EQUAL)
		return NULL;

	if (OidIsValid(cmptype))
	{
		if (!OidIsValid(get_opfamily_member(idx->opfamily[i],
											idx->opcintype[i], cmptype,
											LION_STRAT_EQUAL)))
			return NULL;
		if (!OidIsValid(get_opfamily_proc(idx->opfamily[i], cmptype, cmptype,
										  LION_HASH_PROC)))
			return NULL;
	}

	if (colp != NULL)
		*colp = col;
	return idx;
}

/*
 * The strategy number an operator has in some roaring opfamily, with that
 * family and the type its members are declared on.  Returns 0 when no roaring
 * family knows the operator.
 *
 * The clause analysis has to tell a multi-key clause from an equality one
 * BEFORE any index has been matched, because the parent of a partitioned
 * table has no index list of its own (DESIGN.md §16) and the answer decides
 * what the clause even means.  Taking it from the operator rather than from
 * an index is safe because lion_match_index() checks the strategy again
 * against the index that will really answer the clause, per partition.
 */
static StrategyNumber
lion_op_roaring_strategy(Oid opno, Oid *opfamily, Oid *lefttype)
{
	Oid			amoid = lion_get_am_oid();
	CatCList   *catlist;
	StrategyNumber result = 0;
	int			i;

	*opfamily = InvalidOid;
	*lefttype = InvalidOid;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));
	for (i = 0; i < catlist->n_members; i++)
	{
		Form_pg_amop amop =
			(Form_pg_amop) GETSTRUCT(&catlist->members[i]->tuple);

		if (amop->amopmethod != amoid || amop->amoppurpose != AMOP_SEARCH)
			continue;

		result = amop->amopstrategy;
		*opfamily = amop->amopfamily;
		*lefttype = amop->amoplefttype;
		break;
	}
	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * How extractquery answers the query in con, as the executor would ask it:
 * lion_extract_query()'s mode for a literal, and with superset
 * lion_extract_query_superset()'s, which is what it asks of a query it only
 * has at run time (DESIGN.md §17, "A query known only at run time").
 */
static LionQueryMode
lion_multikey_query_mode(Oid extractquery, StrategyNumber strategy,
						 Const *con, bool superset)
{
	FmgrInfo	flinfo;
	LionQuery	q;
	LionState	state;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionQueryMode mode;

	if (con->constisnull)
		return LION_QMODE_NONE; /* every operator involved is strict */

	/*
	 * lion_extract_query() wants an LionState, but only for the extractQuery
	 * FmgrInfo and the collation; nothing here touches an index.  The
	 * collation of a query is the clause's own, which for the collatable key
	 * types the multi-key classes use (text lexemes, text array elements) is
	 * what the extraction functions ignore anyway - they take the query
	 * apart, they do not compare it.
	 */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring count query extract",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&state, 0, sizeof(state));
	state.multikey = true;
	state.collation = con->constcollid;
	fmgr_info(extractquery, &flinfo);
	state.extractquery = flinfo;

	if (superset)
		lion_extract_query_superset(&state, con->constvalue, strategy, &q);
	else
		lion_extract_query(&state, con->constvalue, strategy, &q);
	mode = q.mode;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return mode;
}

/*
 * Extract a multi-key query at plan time and say whether the posting sets can
 * answer it exactly (DESIGN.md §17).  Only then is a LITERAL query pushed
 * down: an ALL-mode query would need every row rechecked against the heap,
 * which is what the ordinary bitmap plan already does and does better.
 *
 * *extractquery receives the support function used, which lion_match_index()
 * then insists on finding on every index that will answer the clause, so that
 * the run-time extraction cannot come out differently from this one.
 */
static bool
lion_multikey_query_is_exact(Oid opfamily, Oid lefttype,
							StrategyNumber strategy, Const *con,
							Oid *extractquery)
{
	*extractquery = get_opfamily_proc(opfamily, lefttype, lefttype,
									  LION_EXTRACTQUERY_PROC);
	if (!OidIsValid(*extractquery))
		return false;

	return lion_multikey_query_mode(*extractquery, strategy, con,
									false) == LION_QMODE_KEYS;
}

/*
 * One relation the executor will count, with the indexes it will use: a
 * plain table, or one live leaf partition (DESIGN.md §16).
 */
typedef struct LionCountTarget
{
	RelOptInfo *rel;			/* for the per-relation cost */
	Oid			heapoid;
	IndexOptInfo *driveidx[LION_MAX_GROUPCOLS];	/* the indexes whose entries
												 * are scanned; [0] is the
												 * outer one, [1] the inner
												 * one of a two-column GROUP
												 * BY (DESIGN.md §20) */
	AttrNumber	drivecol[LION_MAX_GROUPCOLS];	/* and which KEY COLUMN of
												 * each of them (§24): the two
												 * may be columns of ONE
												 * multicolumn index */
	List	   *whereidx;		/* IndexOptInfo *, one per WHERE clause */
	List	   *wherecol;		/* int list, that index's key column (§24),
								 * one per WHERE clause */
	Var		   *drivevar[LION_MAX_GROUPCOLS];	/* the driving columns in THIS
												 * relation's own numbering,
												 * which is what a per-relation
												 * estimate_num_groups() needs;
												 * NULL when nothing drives the
												 * scan */
} LionCountTarget;

/*
 * The Var a parent column becomes in one child, or NULL when the child does
 * not have it (a column dropped in that partition).  Partitions may number
 * their columns differently, so every attnum the pushdown carries across a
 * partition boundary goes through here (DESIGN.md §16); the Var itself is
 * what a per-partition group estimate has to be made against, because the
 * statistics live on the child.
 */
static Var *
lion_child_var(PlannerInfo *root, Index childrelid, AttrNumber parentattno)
{
	AppendRelInfo *appinfo;
	Var		   *cvar;

	if (parentattno <= 0)
		return NULL;
	if (childrelid == 0 || childrelid >= (Index) root->simple_rel_array_size)
		return NULL;
	if (root->append_rel_array == NULL)
		return NULL;
	appinfo = root->append_rel_array[childrelid];
	if (appinfo == NULL)
		return NULL;
	if ((int) parentattno > list_length(appinfo->translated_vars))
		return NULL;

	cvar = (Var *) list_nth(appinfo->translated_vars, parentattno - 1);
	if (cvar == NULL || !IsA(cvar, Var) || cvar->varattno <= 0)
		return NULL;

	return cvar;
}

/*
 * The same, reduced to the attribute number; 0 when the child lacks it.
 */
static AttrNumber
lion_child_attno(PlannerInfo *root, Index childrelid, AttrNumber parentattno)
{
	Var		   *cvar = lion_child_var(root, childrelid, parentattno);

	return (cvar != NULL) ? cvar->varattno : 0;
}

/*
 * Collect one LionCountTarget per relation the node will count: just rel when
 * it is an ordinary table, or one per live leaf partition when it is a
 * partitioned parent, recursing through sub-partitioned children
 * (DESIGN.md §16).
 *
 * The attribute numbers are rel's own and are translated for every child.
 * The partition set is the planner's already-pruned one: part_rels entries
 * that are non-NULL and in live_parts, minus the ones the planner has since
 * proved empty.
 *
 * Returns false when the pushdown is impossible - a child that is not a plain
 * table (a foreign table, say), a column dropped in some partition, or a leaf
 * without a usable lion index on one of the columns.  An empty *targets
 * means everything was pruned away; the caller leaves that to the planner's
 * own dummy-rel handling.
 */
/*
 * Everything lion_match_index() needs about one clause, gathered once by the
 * clause analysis and reused for every relation.
 */
typedef struct LionClauseInfo
{
	AttrNumber	attno;			/* in the PARENT's numbering */
	int			kind;			/* LION_CLAUSE_* */
	Oid			opno;			/* 0 for a null test */
	Oid			cmptype;		/* the type the column is compared with */
	StrategyNumber strategy;	/* multi-key clauses only */
	Oid			extractquery;	/* multi-key clauses only */
	Oid			collation;		/* clause input collation; InvalidOid if the operator ignores it */
	bool		valueout;		/* the target list prints this column's value,
								 * so the index has to be able to produce it
								 * (lion_index_can_emit_value()) */
} LionClauseInfo;

/*
 * Everything the driving index of one relation has to satisfy, gathered once
 * by lion_try_count_path() and applied to every partition's own index.
 */
typedef struct LionDriveInfo
{
	AttrNumber	attno;			/* in the PARENT's numbering, 0 for none */
	Var		   *var;			/* the column as THIS relation numbers it, for
								 * a per-relation group estimate; NULL for
								 * none */
	Oid			collation;		/* the grouping column's collation, or none */
	Oid			eqop;			/* GROUP BY: the equality the index must have
								 * as strategy 1 of its opfamily; InvalidOid
								 * when nothing groups (the sum-over-all of
								 * DESIGN.md §14 does not care how the entries
								 * partition the rows) */
	bool		valueout;		/* the group key appears in the output */
} LionDriveInfo;

static bool
lion_collect_targets(PlannerInfo *root, RelOptInfo *rel,
					const LionDriveInfo *drive, int ndrive, List *whereattnos,
					List *clauseinfos, List **targets)
{
	RangeTblEntry *rte;
	LionCountTarget *t;
	ListCell   *l1;
	ListCell   *l2;
	int			d;

	/* Sub-partitioning nests, exactly as expand_partitioned_rtentry() does. */
	check_stack_depth();

	if (rel == NULL || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return false;
	if (rte->securityQuals != NIL || rte->tablesample != NULL)
		return false;

	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
	{
		int			i;

		/* Pruned down to nothing: no targets, but no reason to bail either. */
		if (IS_DUMMY_REL(rel))
			return true;
		if (!IS_PARTITIONED_REL(rel))
			return false;

		for (i = 0; i < rel->nparts; i++)
		{
			RelOptInfo *child = rel->part_rels[i];
			LionDriveInfo cdrive[LION_MAX_GROUPCOLS];
			List	   *cattnos = NIL;

			if (child == NULL || !bms_is_member(i, rel->live_parts))
				continue;		/* pruned at plan time */
			if (IS_DUMMY_REL(child))
				continue;		/* provably empty: it counts nothing */

			for (d = 0; d < ndrive; d++)
			{
				cdrive[d] = drive[d];
				if (drive[d].attno == 0)
					continue;
				cdrive[d].var = lion_child_var(root, child->relid,
											  drive[d].attno);
				if (cdrive[d].var == NULL)
					return false;
				cdrive[d].attno = cdrive[d].var->varattno;
			}
			foreach(l1, whereattnos)
			{
				AttrNumber	ca = lion_child_attno(root, child->relid,
												 (AttrNumber) lfirst_int(l1));

				if (ca == 0)
					return false;
				cattnos = lappend_int(cattnos, (int) ca);
			}

			if (!lion_collect_targets(root, child, cdrive, ndrive, cattnos,
									 clauseinfos, targets))
				return false;
		}
		return true;
	}

	/*
	 * A leaf.  Anything whose rows do not live in a local heap this backend
	 * can read - a foreign table above all - is out.  A materialized view
	 * cannot be a partition; it is accepted here because the single-table
	 * path goes through this function too.
	 */
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return false;
	if (rel->indexlist == NIL)
		return false;

	/*
	 * Nor a table of another table AM (lion_table_am_supported()): the count
	 * reads the heap's visibility map.  ambuild refuses to put a lion index
	 * there, so this only declines what got past it; partitions may each have
	 * their own table AM, and one such leaf declines the whole parent.
	 */
	{
		Relation	relation = table_open(rte->relid, NoLock);
		bool		supported = lion_table_am_supported(relation);

		table_close(relation, NoLock);
		if (!supported)
			return false;
	}

	t = (LionCountTarget *) palloc0(sizeof(LionCountTarget));
	t->rel = rel;
	t->heapoid = rte->relid;

	/*
	 * The driving indexes - a GROUP BY column's (two of them for the nested
	 * loop of DESIGN.md §20), or the one DESIGN.md §14 sums over - must be
	 * scalar ones: their entries have to be the column's values, one per row.
	 */
	for (d = 0; d < ndrive; d++)
	{
		t->drivevar[d] = drive[d].var;
		if (drive[d].attno == 0)
			continue;

		t->drivecol[d] = 1;
		t->driveidx[d] = lion_find_roaring_index(rel, drive[d].attno, false,
											   &t->drivecol[d]);
		if (t->driveidx[d] == NULL)
			return false;
		/* Grouping under one collation, index built under another: no. */
		if (OidIsValid(drive[d].collation) &&
			t->driveidx[d]->indexcollations[t->drivecol[d] - 1] !=
			drive[d].collation)
			return false;

		/*
		 * Grouping asks for the groups of ONE equality relation, and the
		 * index's entries are the classes of its own opclass equality.  A
		 * matching collation does not make those the same relation: an
		 * opclass may define a coarser equality on the same type (a text
		 * opclass over lower(), say), and then its entries are already
		 * merged groups that no aggregation above the node can take
		 * apart.  So strategy 1 of this index's opfamily, on its own key
		 * type, has to be the very operator the planner chose for the
		 * grouping column (the 2026-09-20 review, finding 3).  This also
		 * covers the count(col) cases of DESIGN.md §14 that read the group
		 * column's entries (a real group is count(*), the NULL group is 0),
		 * because they are only reached through a grouping index.
		 */
		if (OidIsValid(drive[d].eqop) &&
			lion_index_equality_op(t->driveidx[d], t->drivecol[d]) !=
			drive[d].eqop)
			return false;

		/*
		 * Printing the group key means printing a key this index stored, so
		 * it has to be a representation the rows really have (finding 4).
		 */
		if (drive[d].valueout &&
			!lion_index_can_emit_value(t->driveidx[d], t->drivecol[d]))
			return false;
	}

	forboth(l1, whereattnos, l2, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(l2);
		AttrNumber	col = 1;
		IndexOptInfo *idx = lion_match_index(rel, (AttrNumber) lfirst_int(l1),
											ci->kind, ci->opno, ci->cmptype,
											ci->strategy, ci->extractquery,
											ci->collation, &col);

		if (idx == NULL)
			return false;

		/* Same rule for a pinned column whose value the output prints. */
		if (ci->valueout && !lion_index_can_emit_value(idx, col))
			return false;

		t->whereidx = lappend(t->whereidx, idx);
		t->wherecol = lappend_int(t->wherecol, (int) col);
	}

	*targets = lappend(*targets, t);
	return true;
}

/*
 * The columns of rel declared NOT NULL: RelOptInfo.notnullattnums, which 17
 * added.  On 16 it is read from the relation the same way 17's
 * get_relation_info() fills it in, including leaving it empty for an
 * inheritance parent that is not partitioned, whose children may disagree.
 */
static Bitmapset *
lion_notnullattnums(PlannerInfo *root, RelOptInfo *rel)
{
#if PG_VERSION_NUM >= 170000
	return rel->notnullattnums;
#else
	RangeTblEntry *rte;
	Relation	relation;
	Bitmapset  *result = NULL;

	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return NULL;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION)
		return NULL;

	relation = table_open(rte->relid, NoLock);
	if (!rte->inh || relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		for (int i = 0; i < relation->rd_att->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(relation->rd_att, i);

			if (attr->attnotnull && !attr->attisdropped)
				result = bms_add_member(result, attr->attnum);
		}
	}
	table_close(relation, NoLock);

	return result;
#endif
}

static bool lion_agg_distinct_var(Aggref *agg, Index rti, Var **var,
								  Oid *eqop, Oid *collation);

/*
 * Is the aggregate a count(DISTINCT col) whose column is provably unique over
 * rel (DESIGN.md §26, "A unique column")?  Then no two rows the node counts
 * share a non-NULL value of col, so count(DISTINCT col) IS count(col) over the
 * same rows - of every group, under any WHERE - and lion_agg_is_count() takes
 * it as such: count(*), where col is also known non-NULL.
 *
 * The proof is the FK-side join's (lion_column_is_unique()): a single-column
 * btree index on col that is unique, immediate, not partial and not an
 * expression, whose opfamily has the DISTINCT's own equality as its equality
 * strategy and whose collation is the DISTINCT's - the equality and collation
 * nodeAgg would deduplicate the column under (lion_agg_distinct_var()).  For a
 * partitioned parent rel is the parent, whose unique index is a global proof;
 * a unique index on each partition is not one.
 */
static bool
lion_agg_distinct_unique(Aggref *agg, Index rti, RelOptInfo *rel)
{
	Var		   *var;
	Oid			eqop;
	Oid			collation;

	if (!lion_agg_distinct_var(agg, rti, &var, &eqop, &collation))
		return false;
	return lion_column_is_unique(rel, var->varattno, eqop, collation);
}

/*
 * Can the aggregate be answered by counting a posting set?
 *
 * count(*) always can.  count(col) can when every row the node counts is
 * known to have col non-null, or known to have it NULL - in which case the
 * answer is 0 - which DESIGN.md §14 makes true in more cases than the
 * attnotnull of DESIGN.md §10:
 *
 *	- col is declared NOT NULL;
 *	- a clause pins col with a strict operator (`col = c`, `col = ANY (...)`)
 *	  or with `col IS NOT NULL`;
 *	- col is the group column: the count of a non-NULL group is count(*), and
 *	  the NULL group's is 0;
 *	- a clause says `col IS NULL`: then it is 0 for every group.
 *
 * count(DISTINCT col) is count(col) when col is unique over rel
 * (lion_agg_distinct_unique()), and then takes the same rules.
 */
static bool
lion_agg_is_count(PlannerInfo *root, Aggref *agg, Index rti, RelOptInfo *rel,
				 const AttrNumber *groupattno, int ngroup,
				 const List *nonnullattnos, const List *nullattnos)
{
	TargetEntry *tle;
	Node	   *arg;
	Var		   *var;
	int			i;

	if (agg->aggfnoid != F_COUNT_ && agg->aggfnoid != F_COUNT_ANY)
		return false;
	if (agg->aggorder != NIL || agg->aggfilter != NULL || agg->aggvariadic)
		return false;
	if (agg->aggdistinct != NIL && !lion_agg_distinct_unique(agg, rti, rel))
		return false;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE)
		return false;
	if (agg->aggkind != AGGKIND_NORMAL)
		return false;

	if (agg->aggfnoid == F_COUNT_)
		return agg->aggstar && agg->args == NIL;

	/* count(col) */
	if (list_length(agg->args) != 1)
		return false;
	tle = (TargetEntry *) linitial(agg->args);
	if (!IsA(tle, TargetEntry))
		return false;
	arg = lion_strip((Node *) tle->expr);
	if (arg == NULL || !IsA(arg, Var))
		return false;
	var = (Var *) arg;
	if (var->varno != (int) rti || var->varattno <= 0 ||
			var->varlevelsup != 0)
		return false;

	for (i = 0; i < ngroup; i++)
	{
		if (groupattno[i] == var->varattno)
			return true;
	}
	if (list_member_int((List *) nullattnos, (int) var->varattno))
		return true;
	if (list_member_int((List *) nonnullattnos, (int) var->varattno))
		return true;

	return bms_is_member(var->varattno, lion_notnullattnums(root, rel));
}

/*
 * Is the aggregate a count(DISTINCT col) of a plain column of rti (DESIGN.md
 * §26)?  Then *var is the column, *eqop the equality the DISTINCT compares
 * with and *collation the collation it compares under, which is what the
 * column's index has to agree with - checked per relation in
 * lion_collect_targets(), exactly as for a grouping column.
 *
 * The equality is the aggregate's own SortGroupClause's, the type's default
 * btree equality that nodeAgg deduplicates with, and the collation is the
 * ARGUMENT's (exprCollation(), before any relabel is stripped): that is what
 * nodeAgg sorts and compares the inputs under, so `count(DISTINCT t COLLATE
 * "C")` asks for "C" whatever the column itself says.
 */
static bool
lion_agg_distinct_var(Aggref *agg, Index rti, Var **var, Oid *eqop,
					  Oid *collation)
{
	TargetEntry *tle;
	SortGroupClause *sgc;
	Node	   *arg;

	if (agg->aggfnoid != F_COUNT_ANY || agg->aggdistinct == NIL)
		return false;
	if (agg->aggorder != NIL || agg->aggfilter != NULL || agg->aggvariadic ||
		agg->aggstar)
		return false;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE ||
		agg->aggkind != AGGKIND_NORMAL)
		return false;
	if (list_length(agg->args) != 1 || list_length(agg->aggdistinct) != 1)
		return false;

	tle = (TargetEntry *) linitial(agg->args);
	sgc = (SortGroupClause *) linitial(agg->aggdistinct);
	if (!IsA(tle, TargetEntry) || !IsA(sgc, SortGroupClause))
		return false;
	if (sgc->tleSortGroupRef != tle->ressortgroupref ||
		!OidIsValid(sgc->eqop))
		return false;

	arg = lion_strip((Node *) tle->expr);
	if (arg == NULL || !IsA(arg, Var))
		return false;
	*var = (Var *) arg;
	if ((*var)->varno != (int) rti || (*var)->varattno <= 0 ||
		(*var)->varlevelsup != 0)
		return false;

	*eqop = sgc->eqop;
	*collation = exprCollation((Node *) tle->expr);
	return true;
}

/*
 * Is a GROUP BY expression `coalesce(col, c)` one the entry walk can group by
 * exactly (DESIGN.md §10, "coalesce")?  Its groups are col's, except that the
 * rows where col is NULL belong to the group of c - which is c's own group
 * when some row has col = c, and a group of their own printed as c when none
 * does.  lion_next_group() holds both back and emits them as one row after
 * the last entry, so all this has to prove is that "the entry whose key is c"
 * means exactly what the GROUP BY means by it:
 *
 *	- two arguments, a plain column of the relation and a non-NULL Const.
 *	  eval_const_expressions() has already dropped NULL arguments and
 *	  everything after the first non-NULL constant, and folded a constant
 *	  expression into its value; anything else - a Param, a stable function,
 *	  a second column - declines;
 *	- no coercion anywhere: the column, the Const and the expression all of
 *	  the column's own type, so that the grouping equality - the planner's
 *	  SortGroupClause.eqop, which lion_collect_targets() requires to be the
 *	  driving index's strategy 1, as for a bare column - compares c with the
 *	  stored keys as it compares the rows.  A cast of the column (int4 to
 *	  int8, say) makes the first argument a function call and declines, even
 *	  where the cast would preserve equality, and so does a relabelled one;
 *	- and the column's own collation for the expression: the index's
 *	  collation is checked against the column's, per relation, and so the
 *	  lookup of c is made under the very collation the GROUP BY compares
 *	  under (`coalesce(t, 'x' COLLATE "C")` declines).
 *
 * The value printed for the group of c is the Const - the constant as the
 * query wrote it, as core prints it for a group of NULL rows; for c's own
 * group that is the value the rows hold too, because a key the target list
 * prints is only ever produced by an index whose equality implies an
 * identical representation (the value gate of lion_collect_targets()).
 */
static bool
lion_group_coalesce(CoalesceExpr *ce, Index rti, Var **var, Const **con)
{
	Node	   *a0;
	Node	   *a1;
	Var		   *v;
	Const	   *c;

	if (list_length(ce->args) != 2)
		return false;
	a0 = (Node *) linitial(ce->args);
	a1 = (Node *) lsecond(ce->args);
	if (!IsA(a0, Var) || !IsA(a1, Const))
		return false;
	v = (Var *) a0;
	c = (Const *) a1;
	if (v->varno != (int) rti || v->varattno <= 0 || v->varlevelsup != 0)
		return false;
	if (c->constisnull)
		return false;
	if (ce->coalescetype != v->vartype || c->consttype != v->vartype)
		return false;
	if (ce->coalescecollid != v->varcollid)
		return false;

	*var = v;
	*con = c;
	return true;
}

/*
 * Cost the pushdown (DESIGN.md section 10).
 *
 * Proportional rather than exact.  What a count actually reads:
 *
 *	- for each WHERE key: ONE DESCENT of the entry directory (height + 1
 *	  pages, DESIGN.md §21), then that key's own container chain, which was
 *	  written sequentially and is a fraction of the index's container pages
 *	  proportional to the clause selectivity.  An IN list is one lookup per
 *	  element (DESIGN.md §15), but its values are SORTED first and located in
 *	  one left-to-right walk, so a long list pays for the leaves it crosses
 *	  and not for a descent each;
 *	- for a GROUP BY: every page of the group index;
 *	- one O(1) step per container per participating source (the visibility map
 *	  is read per container);
 *	- the heap the visibility map cannot vouch for: the TIDs on blocks that
 *	  are not all-visible (pg_class.relallvisible via RelOptInfo.allvisfrac),
 *	  each resolved against the snapshot, on as many distinct blocks as there
 *	  can be - each of them fetched once per query.
 *
 * The directory pages are NOT charged wholesale: they would price a
 * single-key count on a small table above a sequential scan of the whole
 * table.
 *
 * It has to beat Agg-over-BitmapHeapScan when the pushdown really is cheaper
 * and lose when it is not; it is not meant to be comparable with core cost
 * estimates to the last decimal.
 *
 * A partitioned table is priced as the sum of its live leaf partitions, each
 * with its own pages / allvisfrac / rows and its own indexes (DESIGN.md §16).
 * numgroups is the parent's estimate throughout: a partition may hold rows of
 * every group.
 */
/*
 * The pages of the entry directory, and how deep it is (DESIGN.md §21).  Both
 * come off the meta page, which lion_get_state() has cached in rd_amcache, as
 * btcostestimate reads the tree height from btree's metapage; the directory
 * page count is maintained exactly by ambuild and by every split.
 */
static double
lion_index_dir_pages(IndexOptInfo *idx, double *height)
{
	Relation	indexrel = index_open(idx->indexoid, AccessShareLock);
	LionMetaPageData meta;
	double		dirpages;

	lion_read_meta(indexrel, &meta);
	dirpages = (double) meta.dirpages;
	if (height != NULL)
		*height = (double) meta.height;

	index_close(indexrel, AccessShareLock);
	return Max(dirpages, 1.0);
}

/* A key column's n_distinct, or -1 for an expression column. */
static double
lion_index_column_nd(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					 int i)
{
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	AttrNumber	attno = idx->indexkeys[i];
	VariableStatData vardata;
	Var		   *var;
	double		nd;
	bool		isdefault;

	/* An expression column has no heap attribute to ask about. */
	if (attno <= 0)
		return -1.0;

	var = makeVar(rel->relid, attno, get_atttype(rte->relid, attno), -1,
				  get_typcollation(get_atttype(rte->relid, attno)), 0);
	examine_variable(root, (Node *) var, 0, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	pfree(var);

	return Max(nd, 1.0);
}

/* Is the relation one whose statistics the shares below can ask about? */
static bool
lion_index_shares_known(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx)
{
	RangeTblEntry *rte;

	if (idx->nkeycolumns <= 1)
		return false;
	if (rel->relid == 0 || rel->relid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[rel->relid];
	return rte != NULL && rte->rtekind == RTE_RELATION;
}

/*
 * ONE KEY COLUMN's share of a multicolumn lion index (DESIGN.md §24).
 *
 * Everything the model prices an index by - `idx->pages` and
 * lion_index_dir_pages() - is PER RELATION, and a multicolumn index is one
 * relation holding n independent sets of entries.  Charging a column the whole
 * directory and the whole page count would price `WHERE b = 1` on `(a, b, c)`
 * as three times what the same query on a single-column index of `b` costs,
 * and the node would be refused for a query it answers exactly as fast.  §24
 * says a column's set should be priced "as a single-column index of that
 * column", so the page terms are scaled by this column's share of the
 * relation's entries.
 *
 * The planner cannot count a column's entries: the meta page carries the
 * directory's shape for the whole relation and nothing per column.  What it
 * does have is the HEAP column's n_distinct, which is the same order of
 * magnitude as that column's entry count (a scalar opclass makes one entry per
 * distinct value, plus the reserved ones), so the share is
 *
 *		n_distinct(this column) / sum of n_distinct over the index's columns
 *
 * taken through examine_variable()/get_variable_numdistinct(), the same pair
 * estimate_num_groups() uses, against a Var built from this relation's own
 * attribute numbers - which for a partition are the partition's (§16).
 *
 * THE LIMITATION, and it is a real one: n_distinct is not entries.  A
 * multi-key column (§17) has one entry per LEXEME and not per row value, so
 * its share is understated - usually far - and the scalar columns beside it
 * are charged for its directory.  A column with no statistics at all falls
 * back to DEFAULT_NUM_DISTINCT for that column alone, which makes the split
 * equal when NO column has statistics and biased when only some do.  Both
 * errors are bounded by the number of columns, which is why this correction is
 * worth making at all: without it the error is exactly that factor, always,
 * and always against the node.
 */
static double
lion_index_column_share(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					   AttrNumber col)
{
	double		total = 0;
	double		mine = 0;
	int			i;

	if (!lion_index_shares_known(root, rel, idx))
		return 1.0;

	for (i = 0; i < idx->nkeycolumns; i++)
	{
		double		nd = lion_index_column_nd(root, rel, idx, i);

		if (nd < 0.0)
			return 1.0 / (double) idx->nkeycolumns;
		total += nd;
		if (i == col - 1)
			mine = nd;
	}

	if (total <= 0.0 || mine <= 0.0)
		return 1.0 / (double) idx->nkeycolumns;

	return Min(mine / total, 1.0);
}

/*
 * ONE KEY COLUMN's share of a multicolumn lion index's CONTAINER pages
 * (DESIGN.md §24).  lion_index_column_share() splits the relation by entries,
 * which is right for the directory - a column has an entry per distinct
 * value - and wrong for the posting sets: every row is under one entry of
 * each scalar column, so a column's container pages follow its ROWS and what
 * they cost to store, not its entries.  Split by n_distinct, the three values
 * of a status column beside a column of 2,000 got 0.14% of a 110 MB index's
 * pages where they hold a tenth of it, and `status = 'val2'` - a million
 * rows over every container key - was priced at 16 for a 1 ms count.
 *
 * So each column is weighed by the bytes its sets take: nd entries of N / nd
 * rows each, which lie in Min(container keys, N / nd) containers of a header
 * each (16 bytes with its line pointer) and two bytes a member up to a
 * bitset's 4 kB, or about six bytes a row where the rows are too few a
 * container for one (a sparse segment, §13).  On that 110 MB index the
 * estimate comes to 114 MB, the 2,000-value column 44% of it.
 */
double
lion_index_column_posting_share(PlannerInfo *root, RelOptInfo *rel,
								IndexOptInfo *idx, AttrNumber col)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		ckeys = Max((double) rel->pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		total = 0;
	double		mine = 0;
	int			i;

	if (!lion_index_shares_known(root, rel, idx))
		return 1.0;

	for (i = 0; i < idx->nkeycolumns; i++)
	{
		double		nd = lion_index_column_nd(root, rel, idx, i);
		double		rows;
		double		conts;
		double		bytes;

		if (nd < 0.0)
			return 1.0 / (double) idx->nkeycolumns;
		rows = tuples / nd;
		conts = Min(ckeys, rows);
		if (rows / conts <= 2.0)
			bytes = nd * rows * 6.0;
		else
			bytes = nd * (conts * 16.0 +
						  Min(2.0 * rows, conts * (double) LION_BITSET_BYTES));
		total += bytes;
		if (i == col - 1)
			mine = bytes;
	}

	if (total <= 0.0 || mine <= 0.0)
		return 1.0 / (double) idx->nkeycolumns;

	return Min(mine / total, 1.0);
}

/*
 * Does this index order its entries by the KEY TYPE's own order (DESIGN.md
 * §21)?  Two things have to hold, and both are about what the planner is
 * allowed to conclude from the entry scan coming out in directory order:
 *
 *	- the index is ordered at all, i.e. its opclass has support function 4 (or
 *	  its key type has a default btree opclass to borrow one from);
 *	- and that ordering IS the key type's default btree ordering, because that
 *	  is the order an `ORDER BY col` asks for.  An opclass free to define its
 *	  own comparison is free to define a different one.
 *
 * The collation is not checked here: §10 already requires the index's
 * collation to equal the grouping column's, which is the same rule the
 * planner's IndexCollMatchesExprColl() applies to an index scan.
 */
static bool
lion_index_orders_naturally(IndexOptInfo *idx, AttrNumber col)
{
	Relation	indexrel = index_open(idx->indexoid, AccessShareLock);
	LionState  *state = lion_index_column_state(indexrel, col);
	bool		ok = false;

	if (state->ordered)
	{
		TypeCacheEntry *typentry = lookup_type_cache(state->typid,
													 TYPECACHE_CMP_PROC);

		ok = OidIsValid(typentry->cmp_proc) &&
			typentry->cmp_proc == state->cmpproc.fn_oid;
	}

	index_close(indexrel, AccessShareLock);
	return ok;
}

/*
 * `var`'s n_distinct over the WHOLE table - a walk visits an entry whatever
 * the other clauses leave of it - which is how many entries a scalar column's
 * index has.  It is taken as examine_variable() gives it rather than through
 * estimate_num_groups(), which would scale it down by every clause of the
 * relation, a range included, and count the range twice.
 */
static double
lion_var_ndistinct(PlannerInfo *root, RelOptInfo *rel, Var *var)
{
	VariableStatData vardata;
	double		ndistinct;
	bool		isdefault;

	examine_variable(root, (Node *) var, rel->relid, &vardata);
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);

	return Max(1.0, ndistinct);
}

/*
 * How many entries of `var`'s index a range walk visits (DESIGN.md §28): its
 * n_distinct times the range's own selectivity, at least one.
 */
static double
lion_range_entries(PlannerInfo *root, RelOptInfo *rel, Var *var,
				  Selectivity sel)
{
	return Max(1.0, lion_var_ndistinct(root, rel, var) * sel);
}

/*
 * The correlation between `var`'s values and the heap's physical order, as
 * lioncostestimate() reads it for a plain scan (lion_var_heap_correlation(),
 * DESIGN.md §29.11): ANALYZE's number less the sum of the values' squared
 * frequencies, which is what it comes out at for values placed at random;
 * 0 when there is none.  Read raw, a column of a few values placed at random
 * looked as though each value's rows were packed on a share of the heap, and
 * the recheck of a count over one was priced on too few pages.
 */
static double
lion_var_correlation(PlannerInfo *root, RelOptInfo *rel, Var *var)
{
	return lion_var_heap_correlation(root, rel->relid, var);
}

/*
 * How many containers a posting set of `members` members can span: one per
 * LION_BLOCKS_PER_CONTAINER heap pages, and never more than one per member.
 */
double
lion_containers_for(double heap_pages, double members)
{
	return Max(1.0, Min(heap_pages / LION_BLOCKS_PER_CONTAINER, members));
}

/*
 * Does key column col of idx have summary posting sets (DESIGN.md §32)?  What
 * the index's meta page says, read through its cached state as a count reads
 * it; a column built without them has none until the next REINDEX.
 */
static bool
lion_index_col_summarized(IndexOptInfo *idx, AttrNumber col)
{
	Relation	indexrel;
	bool		summarized;

	indexrel = index_open(idx->indexoid, AccessShareLock);
	summarized = lion_index_column_state(indexrel, col)->summarized;
	index_close(indexrel, AccessShareLock);
	return summarized;
}

/*
 * ... and how many the rows of ONE KEY of index column `col` do lie in: as
 * many as they can when the column's values are placed at random, their
 * share of the heap's container keys when it is stored in value order, and
 * between the two by the correlation's square, as cost_index() interpolates
 * pages (lion_var_heap_correlation(); lion_cost_range_sum() does the same for
 * a range's entries).  A day of a table loaded in time order is one or two
 * containers, not one at every container key: priced as scattered, a GROUP
 * BY over 365 such days cost 4,650 for 0.6 ms.  An expression or a multi-key
 * column has no correlation to go by and is taken as scattered.
 */
static double
lion_key_containers(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					AttrNumber col, double members)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		scattered = lion_containers_for(heap_pages, members);
	double		inorder;
	double		corr;
	AttrNumber	attno;
	RangeTblEntry *rte;
	Var		   *var;

	if (col < 1 || col > idx->nkeycolumns || scattered <= 1.0)
		return scattered;
	attno = idx->indexkeys[col - 1];
	if (attno <= 0 || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return scattered;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION ||
		lion_opfamily_is_multikey(idx->opfamily[col - 1],
								  idx->opcintype[col - 1]))
		return scattered;

	var = makeVar(rel->relid, attno, get_atttype(rte->relid, attno), -1,
				  idx->indexcollations[col - 1], 0);
	corr = lion_var_correlation(root, rel, var);
	pfree(var);

	inorder = Max(1.0, members * (heap_pages / LION_BLOCKS_PER_CONTAINER) /
				  Max(rel->tuples, 1.0));
	return Max(1.0, scattered + (inorder - scattered) * corr * corr);
}

/*
 * The share of an intersection's work an EXISTENCE test does (DESIGN.md §26).
 *
 * The test stops at the first container that shows a visible row.  An
 * intersection expected to hold `survivors` rows spread over `containers`
 * containers has a row in about one container in containers/survivors, so the
 * test reads about containers/survivors + 1 of them - the whole thing when it
 * is expected to be empty, which is also when it has to be read to the end to
 * say so.  The same share applies to the recheck candidates the test queues,
 * because it rechecks one container at a time and stops with them.
 */
static double
lion_exists_fraction(double containers, double survivors)
{
	containers = Max(containers, 1.0);
	if (survivors < 1.0)
		return 1.0;
	return Min(1.0, (containers / survivors + 1.0) / containers);
}

/*
 * How tall the posting tree of a set that occupies `leaves` container pages is
 * (DESIGN.md §22): 0 while it fits on one page, and one level for every
 * LION_POSTING_FANOUT pages above that.
 *
 * The planner cannot read it anywhere: the meta page carries the height of the
 * entry DIRECTORY, not of any one key's posting tree, and asking a key's own
 * root for it would be a page read per estimate.  So it is derived from the
 * shape the tree is built with - an internal page holds
 * LION_PAGE_CAPACITY / (MAXALIGN(sizeof(LionPostingPivot)) + sizeof(ItemIdData))
 * = 679 downlinks (lion_posting.c) - which is exact for a bulk-built tree and
 * an underestimate of at most one level for a tree grown by splits.
 */
#define LION_POSTING_FANOUT \
	((double) (LION_PAGE_CAPACITY / (MAXALIGN(LION_POSTING_PIVOT_SIZE) + \
									 sizeof(ItemIdData))))

static double
lion_posting_height(double leaves)
{
	double		height = 0;

	for (leaves = Max(leaves, 1.0); leaves > 1.0; leaves /= LION_POSTING_FANOUT)
		height += 1.0;

	return height;
}

/*
 * How many pages of one posting tree that occupies `leaves` container pages
 * `probes` seeks into it read (DESIGN.md §22).
 *
 * A source that does not drive the leapfrog join is never walked: it is sought
 * to the container keys the driver produces, and one seek is a descent - one
 * internal page per level and the leaf the key lives on - or, when the key is
 * a page or two ahead, a step right, which the seek takes only while it is no
 * dearer than the descent it saves (`LION_POSTING_SEEK_STEPS`).  So a probe
 * costs `height + 1` pages and the source as a whole costs that many times the
 * probes, never more than its whole chain, which is what a walk reads.
 *
 * Measured on the benchmark's one-million-row `fact` (release build,
 * 2026-09-22), counting the node's buffer accesses: `c2 = 1` alone walks its
 * 151 container pages and touches 154 buffers; probed by `c20k = 77`, which
 * has a container at about 40 of the heap's 301 container keys, it touches 118
 * - a descent's worth per probe and a page or so of stepping - against the 100
 * this charges and the 151 a walk would.  Probed by `c1m = 12345`, which has
 * one container key, it touches 2.
 */
static double
lion_probed_pages(double leaves, double probes, double height)
{
	return Min(Max(leaves, 1.0), Max(probes, 0.0) * (height + 1.0));
}

/*
 * What the k-way union of `nkeys` posting sets holding `members` rows between
 * them costs, in cpu_operator_cost units (DESIGN.md §15).
 *
 * Two terms, because lion_ecursor_build() does two different things:
 *
 *	- a MIN-HEAP sift per container, log2(k) deep.  The heap holds the
 *	  sub-cursors, not the members, so this is counted per container and not
 *	  per row: a set of `members / nkeys` rows has that many containers at most,
 *	  and never more than the heap has container keys;
 *	- and the UNION of the containers standing at one key, which is a bitset
 *	  image once there are LION_OR_BITSET_MIN of them - a fixed pass over the
 *	  key's range, plus one bit set per member - and a pairwise fold below
 *	  that, which touches the members once per fold.
 *
 * Charging `members x log2(k)` for all of it, as if every row were compared
 * its way through the heap, asked 24750 of the 26481 cost units for `c200 IN
 * (1000 values)` at one million rows - a query the node answers in 6.3 ms
 * against the B-tree index-only scan's 68 - and refused it.
 */
static double
lion_merge_ops(double heap_pages, double members, double nkeys)
{
	double		containers = nkeys * lion_containers_for(heap_pages,
														members / nkeys);
	double		sifts = containers * log2(nkeys);

	if (nkeys >= (double) LION_OR_BITSET_MIN)
		return sifts + lion_containers_for(heap_pages, members) *
			LION_BITSET_WORDS + members;

	return sifts + members * log2(nkeys);
}

/*
 * What a merge that counts the AND of `nsrc` sources costs in CPU (DESIGN.md
 * §10, "The units"; §22, §25): members[i] rows lying in containers[i]
 * containers (a union's are its sets' together), of a table of `tuples` rows.
 *
 * The source with the fewest members drives: each of its containers is read
 * and counted (LION_CONTAINER_COST, and LION_MEMBER_COST a member, up to a
 * bitset's worth).  At each of its container keys the others are sought in
 * ascending order of members, a probe each (LION_PROBE_COST, or
 * LION_MEMORY_PROBE_COST for a source inmem[] says is a copy in memory) that
 * ANDs the running intersection's members with what it finds
 * (LION_AND_MEMBER_COST).
 * A key is abandoned as soon as the intersection empties (§25), so the k'th
 * source is sought only at the keys where the ones before it left a row: at
 * 1 - exp(-lambda) of them, lambda the rows the intersection is expected to
 * hold at a key when the columns are independent.  Without that the third
 * source of `c20k = 77 AND c200 = 17 AND c2 = 1` was charged a probe at each
 * of 219 keys, when c200 empties all but two of them, and the node was refused
 * at 777 against a BitmapAnd's 237 for 0.03 ms against 2.8.
 *
 * probes[i], when probes is not NULL, is set to how often source i is sought
 * (0 for the driver), for the pages those probes read.
 */
double
lion_merge_cpu_cost(int nsrc, const double *members, const double *containers,
					const bool *inmem, double tuples, double *probes)
{
	int		   *order;
	int			i;
	int			k;
	double		keys;
	double		lambda;
	double		alive = 1.0;
	double		cost;

	if (nsrc <= 0)
		return 0.0;

	order = (int *) palloc(sizeof(int) * nsrc);
	for (i = 0; i < nsrc; i++)
	{
		double		m = members[i];

		/* insertion sort by members, ascending: the order the merge seeks in */
		for (k = i; k > 0 && members[order[k - 1]] > m; k--)
			order[k] = order[k - 1];
		order[k] = i;
	}

	keys = Max(containers[order[0]], 1.0);
	lambda = Max(members[order[0]], 0.0) / keys;
	cost = keys * (LION_CONTAINER_COST +
				   LION_MEMBER_COST * Min(lambda, LION_MEMBER_CAP));
	if (probes != NULL)
		probes[order[0]] = 0.0;

	for (k = 1; k < nsrc; k++)
	{
		int			j = order[k];
		double		sought = keys * alive;

		cost += sought * ((inmem != NULL && inmem[j]) ?
						  LION_MEMORY_PROBE_COST : LION_PROBE_COST);
		cost += sought * LION_AND_MEMBER_COST * Min(lambda, LION_MEMBER_CAP);
		if (probes != NULL)
			probes[j] = sought;

		lambda *= Min(Max(members[j], 0.0) / Max(tuples, 1.0), 1.0);
		alive = 1.0 - exp(-lambda);
	}

	pfree(order);
	return cost;
}

/*
 * The cost of ONE fetch of each of `pages` distinct heap pages of a relation
 * that has `heap_pages` pages altogether, per page.
 *
 * A recheck pass is not a sequence of random disk reads when the pages it
 * touches are in memory, and two things say that they are:
 *
 *	- the working set's share of the cache.  effective_cache_size is what the
 *	  planner is told about the memory available for caching, and
 *	  index_pages_fetched() already prorates it over the pages of the query's
 *	  relations; a dirty working set that fits in this relation's share of it
 *	  is read from memory rather than from the device, which is what makes a
 *	  count that visits each dirty page at most once per query (DESIGN.md §9)
 *	  cheap even when it returns to those pages for every group;
 *	- and the set's density.  A set that covers most of the relation is read
 *	  in physical order whatever the cache holds, which is the interpolation
 *	  cost_bitmap_heap_scan() makes between the two page costs.
 *
 * Whichever of the two argues for sequential access more strongly decides,
 * and the answer moves between seq_page_cost and random_page_cost - so a
 * dirty working set far larger than the cache is still charged as random
 * I/O, which is the case a blanket preference for this node would get wrong.
 */
static Cost
lion_heap_page_cost(PlannerInfo *root, RelOptInfo *rel, double pages,
				   double heap_pages)
{
	double		spc_random_page_cost;
	double		spc_seq_page_cost;
	double		total_pages;
	double		cache_pages;
	double		resident;
	double		density;
	double		seqness;

	if (pages <= 0.0)
		return 0.0;

	get_tablespace_page_costs(rel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/* This relation's prorated share of the cache, as index_pages_fetched(). */
	total_pages = Max(root->total_table_pages, heap_pages);
	cache_pages = Max((double) effective_cache_size * heap_pages / total_pages,
					  1.0);

	resident = Min(cache_pages / pages, 1.0);
	density = sqrt(Min(pages / heap_pages, 1.0));
	seqness = Max(resident, density);

	return spc_random_page_cost -
		(spc_random_page_cost - spc_seq_page_cost) * seqness;
}

static bool *lion_or_leaf_map(List *ors, int nclause);
static int *lion_or_group_map(List *ors, int nclause);
static Cost lion_cost_recheck(PlannerInfo *root, RelOptInfo *rel,
							  List *whereidx, List *wherecol,
							  List *whereclauses, List *wherekinds, List *ors,
							  double matched, double counts, double ceiling);

/*
 * Does an IN list's source take the disjoint-sum short-circuit of DESIGN.md
 * §15, and does it drive the groups?  Both questions are about the SHAPE of
 * the query and are answered here so that the price matches what the executor
 * will do (lion_count_sources_cached(), lion_next_group_inlist()).
 *
 *	*sumshort	the list is the only positive source and nothing else drives
 *				the count, so its union MAY never be built: each entry is
 *				counted on its own and the counts are added up.  Whether that is
 *				also the cheaper way depends on the entries' density and is
 *				decided by the caller, which has the estimates.
 *	*groupdrive	the list is on the very column the GROUP BY drives, so the
 *				listed values ARE the groups: the index's entry scan does not
 *				happen, there are at most as many groups as listed values, and
 *				the list is not a source (a group intersected with the union of
 *				a disjoint list is the group).
 *
 * eqdrives says that an EQUALITY on the driving column drives the entries as
 * a list of one would, which is what the count(DISTINCT k) walk of DESIGN.md
 * §26 does with `k = c` (a GROUP BY never sees one: the planner folds a
 * grouping column an equality pins).
 *
 * Returns the clause index of the list, or -1 when neither applies.
 *
 * *groupdrive has to be the executor's answer and not an approximation of
 * it, because it is also what decides whether the path may claim pathkeys:
 * the entry walk emits its groups in directory order, the list's sets in
 * whatever order they were located in.  lion_locate_where() takes as the
 * driver the FIRST clause - in clause order, OR leaves skipped - that is a
 * list (or, with eqdrives, an equality) on the driving index's own key
 * column, whatever else the WHERE holds, and so does this function.  It used
 * to give up at the second list anywhere in the WHERE ("neither is THE one"),
 * which with `g IN (...) AND h IN (...) GROUP BY g` priced a walk of every
 * entry of g and promised `ORDER BY g` a sorted output the executor never
 * built: it drove the groups from g's list all the same (the 2026-09-25
 * review).  The listed sets come out in key order for every opfamily this
 * extension ships, which is why no test caught it; a family whose lookup
 * falls back to an unsorted probe emits them in hash order.
 */
static int
lion_inlist_shape(IndexOptInfo *groupidx, AttrNumber groupcol,
				 IndexOptInfo *groupidx2,
				 List *whereidx, List *wherecol, List *whereclauses,
				 List *wherekinds,
				 List *ors, bool eqdrives, bool *sumshort, bool *groupdrive)
{
	int			nclause = list_length(whereclauses);
	bool	   *inor = lion_or_leaf_map(ors, nclause);
	int			firstlist = -1; /* the first list anywhere */
	int			nlist = 0;
	int			groupci = -1;	/* the first one on the driving column */
	int			npos = 0;
	int			ci = 0;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	*sumshort = false;
	*groupdrive = false;

	forfour(lc1, whereidx, lc2, whereclauses, lc3, wherekinds, lc4, wherecol)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		bool		ondriver = (groupidx != NULL &&
								idx->indexoid == groupidx->indexoid &&
								(AttrNumber) lfirst_int(lc4) == groupcol);

		if (!inor[ci] && LION_CLAUSE_IS_POSITIVE(lfirst_int(lc3)))
		{
			npos++;
			if (IsA((Node *) lfirst(lc2), ScalarArrayOpExpr) ||
				(eqdrives && lfirst_int(lc3) == LION_CLAUSE_EQ && ondriver))
			{
				nlist++;
				if (firstlist < 0)
					firstlist = ci;
				if (groupci < 0 && ondriver)
					groupci = ci;
			}
		}
		ci++;
	}
	pfree(inor);

	/*
	 * The list drives the groups only when its entries ARE the groups: the
	 * same index AND the same key column (DESIGN.md §24).  Two columns of one
	 * multicolumn index are two independent sets of entries, so a list on `b`
	 * says nothing about the groups of `a` - which is also how the executor
	 * decides it (lion_locate_where(), by index and by heap attno).  Any other
	 * list, or a second one on the same column, is an ordinary source.
	 *
	 * The sum of a list's entries stands for its union only when nothing else
	 * is in the AND, so that needs the list to be the one positive clause.
	 */
	if (groupidx == NULL && groupidx2 == NULL && ors == NIL && npos == 1 &&
		nlist == 1)
	{
		*sumshort = true;
		return firstlist;
	}
	if (groupidx != NULL && groupidx2 == NULL && groupci >= 0)
	{
		*groupdrive = true;
		return groupci;
	}
	return -1;
}

/*
 * The Var of the WHERE clause when it is exactly one positive clause and that
 * clause is a plain equality `col = value` on a column of rel; NULL otherwise
 * (a partition's clauses name the parent, and are left alone).
 */
static Var *
lion_single_eq_var(RelOptInfo *rel, List *whereclauses, List *wherekinds)
{
	Node	   *eq = NULL;
	int			npos = 0;
	ListCell   *lc1;
	ListCell   *lc2;
	Node	   *arg;

	forboth(lc1, whereclauses, lc2, wherekinds)
	{
		if (!LION_CLAUSE_IS_POSITIVE(lfirst_int(lc2)))
			continue;
		npos++;
		if (lfirst_int(lc2) == LION_CLAUSE_EQ)
			eq = (Node *) lfirst(lc1);
	}
	if (npos != 1 || eq == NULL)
		return NULL;
	if (IsA(eq, RestrictInfo))
		eq = (Node *) ((RestrictInfo *) eq)->clause;
	if (!IsA(eq, OpExpr) || list_length(((OpExpr *) eq)->args) != 2)
		return NULL;

	arg = (Node *) linitial(((OpExpr *) eq)->args);
	while (arg != NULL && IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;
	if (arg == NULL || !IsA(arg, Var) || (Index) ((Var *) arg)->varno != rel->relid)
	{
		arg = (Node *) lsecond(((OpExpr *) eq)->args);
		while (arg != NULL && IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
	}
	if (arg == NULL || !IsA(arg, Var) || (Index) ((Var *) arg)->varno != rel->relid ||
		((Var *) arg)->varattno <= 0)
		return NULL;
	return (Var *) arg;
}

/*
 * THE SUM OVER A RANGE (DESIGN.md §28, "The cost of a summed range"):
 * `count(*) WHERE <range on k> [AND F]`, priced as lion_sumall_relation()
 * runs it.
 *
 *	- ONE side of the range is walked: the entries it selects, or - when F
 *	  has a positive source and k's directory is ordered - the entries below
 *	  and above it, plus one count of F minus k's NULL entry (the complement).
 *	  The executor takes the side with fewer leaves, and so does this.
 *	- Each entry walked costs a fixed amount (LION_RANGE_ENTRY_COST for one
 *	  counted on its own, LION_RANGE_UNION_ENTRY_COST for a small one counted
 *	  with the rest of its leaf) plus its OWN containers - the rows of one key
 *	  of k, not the rows that survive F: the entry drives its count when it is
 *	  the smaller, and F is then probed at each of its container keys, so the
 *	  key steps are the smaller of the two containers' worth, once for every
 *	  source.  How many containers one key's rows lie in follows the column's
 *	  correlation with the heap, as cost_index() interpolates pages.
 *	- When k has SUMMARIES (DESIGN.md §32), a side that covers whole buckets
 *	  walks only the keys of the buckets at its ends, entry by entry, and
 *	  counts each whole bucket's summary once instead of its keys
 *	  (lion_cost_range_side()).
 *	- The leaves under the entries walked are read once each; with a
 *	  complement to choose, both sides are first stepped a leaf at a time up
 *	  to the smaller one's end, counting only (lion_range_choose()).
 *	- The heap the visibility map cannot vouch for: F's candidates and the
 *	  entries' for the complement, the entries' for the range, each page
 *	  fetched at most twice (the per-entry revisits of DESIGN.md §28).
 *
 * Before this the walk was charged the range's entries at LION_RANGE_ENTRY_COST
 * and ONE container each - the rows of the whole count spread over them - so a
 * range over 2,000 keys of a thousand rows each beside a selective equality
 * was priced at 1,344 for 920,000 containers, and a range over 1.9 million keys
 * at 813,000 whether or not the keys outside it were a handful.
 */
static Cost lion_range_recheck(PlannerInfo *root, RelOptInfo *rel,
							   double tids, double entries, double corr);
static int *lion_rangesrc_leaders(List *whereidx, List *wherecol,
								  List *wherekinds, List *ors, int nclause);

/*
 * A summarized column (DESIGN.md §32), as the cost of a walk over it sees it:
 * how many keys one bucket holds, and what one bucket's summary costs to
 * count and to read.
 */
typedef struct LionSumModel
{
	double		keysper;		/* keys of k in one bucket */
	double		persum;			/* one summary's count: fixed + containers */
	double		sumpages;		/* ... and the pages it reads */
} LionSumModel;

/*
 * What one side of a summed range walk costs (DESIGN.md §28, §32): nkeys
 * entries of k in one run, each `perentry` and `pageper` pages - or, when k is
 * summarized (sum != NULL), the keys of the partial buckets at the run's
 * `nedges` ends one by one and each bucket it covers whole as ONE count of
 * its summary.  A run of n keys over buckets of `keysper` keys covers about
 * n / keysper - 1 of them whole when both its ends fall inside a bucket: half
 * a bucket is left over at each end on average.  With no whole bucket the
 * executor walks the keys as it always did (lion_entry_scan_plan_sum()), and
 * so does this.
 *
 * *counts is how many counts the side makes - entries and summaries - which
 * is what the heap recheck is spread over; *pages is what it reads.
 */
static Cost
lion_cost_range_side(double nkeys, double nedges, const LionSumModel *sum,
					 double perentry, double pageper, double *counts,
					 double *pages)
{
	double		whole = 0.0;
	double		edgekeys;
	Cost		cost;

	if (sum != NULL)
		whole = Max(0.0, nkeys / sum->keysper - 0.5 * nedges);
	if (whole < 1.0)
	{
		*counts = nkeys;
		*pages = nkeys * pageper;
		return nkeys * (perentry + pageper * seq_page_cost);
	}

	edgekeys = Max(0.0, nkeys - whole * sum->keysper);
	*counts = edgekeys + whole;
	*pages = edgekeys * pageper + whole * sum->sumpages;
	cost = edgekeys * (perentry + pageper * seq_page_cost) +
		whole * (sum->persum + sum->sumpages * seq_page_cost);

	/* the phases after the first each start with a descent of their own */
	return cost + LION_SUMMARY_PHASE_DESCENTS * random_page_cost;
}

/*
 * The same side beside the other sources F when the executor may PROBE it
 * (DESIGN.md §32, "Summed ranges: dense and probed"): `probe` and
 * `probeentry` are what a summary and an entry cost probed rather than
 * counted, siderows the rows the side holds and frows F's.  A walk that uses
 * summaries turns to probing once it has handed out LION_PROBE_SWITCH times
 * F's rows (lion_sum_probe_due()); the sets before that are counted as they
 * always were, those after it probed, and F is counted twice more - once
 * collected, once ANDed with what the probe marked - which is `fcount` each.
 * Without a whole bucket, or when the side never gets that far, it is
 * lion_cost_range_side()'s.  *counts is the counts the heap recheck is spread
 * over: the ones before the switch, and the probe's one.
 */
static Cost
lion_cost_range_side_probed(double nkeys, double nedges,
							const LionSumModel *sum, const LionSumModel *probe,
							double perentry, double probeentry,
							double pageper, double siderows, double frows,
							Cost fcount, double *counts, double *pages)
{
	Cost		counted = lion_cost_range_side(nkeys, nedges, sum, perentry,
											   pageper, counts, pages);
	Cost		probed;
	double		pcounts;
	double		ppages;
	double		before;

	if (sum == NULL || probe == NULL ||
		nkeys / sum->keysper - 0.5 * nedges < 1.0 ||
		siderows < LION_PROBE_SWITCH * frows)
		return counted;

	probed = lion_cost_range_side(nkeys, nedges, probe, probeentry, pageper,
								  &pcounts, &ppages);
	before = Min(1.0, LION_PROBE_SWITCH * frows / Max(siderows, 1.0));
	*counts = before * *counts + 1.0;
	return before * counted + (1.0 - before) * probed + 2.0 * fcount;
}

static Cost
lion_cost_range_sum(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *groupidx,
					AttrNumber groupcol, Var *rangevar, Selectivity rangesel,
					List *whereclauses, List *wherekinds, List *ors,
					bool collect)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		matching = Max(lion_probe_rel_rows(root, rel), 1.0);
	double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		sel = Min(Max(rangesel, 1e-10), 1.0);
	double		nd = lion_var_ndistinct(root, rel, rangevar);
	double		nin = Max(1.0, nd * sel);
	double		nout = Max(0.0, nd - nin);
	double		rowsper = Max(tuples * sel / nin, 1.0);
	double		corr = lion_var_correlation(root, rel, rangevar);
	double		scattered = lion_containers_for(heap_pages, rowsper);
	double		inorder = Max(1.0, rowsper * ckeys / tuples);
	double		percont = scattered + (inorder - scattered) * corr * corr;
	double		pageper;
	double		frows = Min(tuples, matching / sel);
	double		fcont = lion_containers_for(heap_pages, frows);
	double		fm = Min(frows / Max(fcont, 1.0), LION_MEMBER_CAP);
	double		m = Min(rowsper / Max(percont, 1.0), LION_MEMBER_CAP);
	double		steps;
	double		perentry;
	double		probeentry = 0.0;
	Cost		fcount;
	int			nsrc = list_length(ors);
	int			nplain = list_length(ors);
	int		   *orgrp = lion_or_group_map(ors, list_length(whereclauses));
	int			ci = 0;
	LionState  *colstate;
	bool		ordered;
	bool		summarized;
	uint32		bucket_tids;
	Relation	indexrel;
	ListCell   *lc;
	LionSumModel summodel;
	LionSumModel *sum = NULL;
	LionSumModel probemodel;
	LionSumModel *probe = NULL;
	LionSumShape shape;
	double		incounts;
	double		inpages;
	double		outcounts;
	double		outpages;
	Cost		inside;
	Cost		outside;

	/* The sources of F: one per OR restriction, one per positive clause. */
	foreach(lc, wherekinds)
	{
		if (LION_CLAUSE_IS_POSITIVE(lfirst_int(lc)) && orgrp[ci] < 0)
		{
			nsrc++;
			/* a source that is a range carries no pin (§32) */
			if (lfirst_int(lc) != LION_CLAUSE_RANGESRC)
				nplain++;
		}
		ci++;
	}
	pfree(orgrp);

	/* One count of F: its containers, each of its sources probed at them. */
	fcount = fcont * (LION_CONTAINER_COST + LION_MEMBER_COST * fm +
					  nsrc * LION_PROBE_COST);

	indexrel = index_open(groupidx->indexoid, AccessShareLock);
	colstate = lion_index_column_state(indexrel, groupcol);
	ordered = colstate->ordered;
	summarized = colstate->summarized;
	bucket_tids = lion_get_index_state(indexrel)->meta.summary_tids;
	memset(&shape, 0, sizeof(shape));
	if (summarized && ordered && bucket_tids > 0)
		lion_summary_shape(indexrel, colstate, &shape);
	index_close(indexrel, AccessShareLock);

	/*
	 * One entry: its fixed cost, and its containers' key steps against every
	 * source of F.  Small ones are counted a leaf at a time.
	 */
	{
		double		keys = (nsrc > 0) ? Min(percont, fcont) : percont;

		/*
		 * the driver's containers, and each of F's sources probed at them -
		 * in memory, where a walk copies them on their second use (§9)
		 */
		steps = keys * (LION_CONTAINER_COST + LION_MEMBER_COST * m) +
			keys * nsrc * (LION_MEMORY_PROBE_COST + LION_AND_MEMBER_COST * m);
	}
	perentry = ((percont <= (double) LION_SUM_UNION_MAX_ITEMS) ?
				LION_RANGE_UNION_ENTRY_COST : LION_RANGE_ENTRY_COST) + steps;

	/* The leaves under one entry: its share of the column's pages. */
	pageper = Max(1.0, (double) groupidx->pages *
				  lion_index_column_share(root, rel, groupidx, groupcol)) / nd;

	/*
	 * The summaries (DESIGN.md §32).  A bucket closes at the first key that
	 * brings it to summary_tids rows, so it holds that many rows - or one
	 * key's, when a key alone has more - and bucketrows / rowsper keys.  Its
	 * summary is ONE set of those rows, which lies in as many containers as
	 * that many rows spread over the heap as the column's order says, and
	 * takes about the bytes of its keys' sets with the containers they share
	 * merged: their pages, scaled by the containers the union saves.  It is
	 * counted on its own (LION_RANGE_ENTRY_COST), as a large entry is.
	 *
	 * That is a column whose keys arrive in order.  Keys that arrive in
	 * descending order all go into the first bucket (the open one, on an
	 * index built empty), and keys in no order into the middle buckets, which
	 * never split: the buckets are then far larger than summary_tids, and a
	 * range walks the keys of the ones at its ends.  The rows the column's
	 * summaries hold per summary, read off the index (lion_summary_shape()),
	 * say how large they really are, and once that is more than twice what
	 * the reloption says it is what the model takes - so that one giant
	 * bucket makes a range that covers a bucket whole the rare case it is.
	 * The model used to count whole buckets the data did not have, and priced
	 * such a range as a sum of summaries that the executor then walked key by
	 * key.  A column with no summary at all - `on` over an empty table - is
	 * walked, as the executor walks it.
	 */
	if (summarized && ordered && bucket_tids > 0 && shape.nsummaries > 0)
	{
		double		bucketrows = Max((double) bucket_tids, rowsper);
		double		realrows = shape.rows / shape.nsummaries;
		double		sscattered;
		double		sinorder;
		double		sc;
		double		sm;
		double		skeys;

		if (realrows > 2.0 * bucketrows)
			bucketrows = realrows;
		sscattered = lion_containers_for(heap_pages, bucketrows);
		sinorder = Max(1.0, bucketrows * ckeys / tuples);
		sc = sscattered + (sinorder - sscattered) * corr * corr;
		sm = Min(bucketrows / Max(sc, 1.0), LION_MEMBER_CAP);
		skeys = (nsrc > 0) ? Min(sc, fcont) : sc;

		summodel.keysper = Max(1.0, bucketrows / rowsper);

		/* the summary's containers, and F's sources probed at them, as above */
		summodel.persum = LION_RANGE_ENTRY_COST +
			skeys * (LION_CONTAINER_COST + LION_MEMBER_COST * sm) +
			skeys * nsrc * (LION_MEMORY_PROBE_COST + LION_AND_MEMBER_COST * sm);
		summodel.sumpages = Max(1.0, summodel.keysper * pageper *
								Min(1.0, sc / (summodel.keysper * percont)));
		sum = &summodel;

		/*
		 * ... and PROBED at F's rows instead (DESIGN.md §32, "Summed ranges:
		 * dense and probed"), which the executor turns to part of the way
		 * through a side that holds more rows than F, when F has a source
		 * that carries a pin and a copy of it fits work_mem
		 * (lion_sum_probe_due()): each summary and each entry a set with no
		 * count of its own, its containers stepped over at
		 * LION_PROBE_STEP_COST and the ones at F's keys marked against F's
		 * members, the fewer of the two sides' per container.
		 */
		if (!collect && nplain > 0 &&
			frows * 2.0 * sizeof(uint16) <= (double) work_mem * 1024.0)
		{
			probemodel = summodel;
			probemodel.persum = LION_RANGE_UNION_ENTRY_COST +
				sc * LION_PROBE_STEP_COST +
				Min(sc, fcont) * LION_AND_MEMBER_COST * Min(sm, fm);
			probeentry = LION_RANGE_UNION_ENTRY_COST +
				percont * LION_PROBE_STEP_COST +
				Min(percont, fcont) * LION_AND_MEMBER_COST * Min(m, fm);
			probe = &probemodel;
		}
	}

	inside = lion_cost_range_side_probed(nin, 2.0, sum, probe, perentry,
										 probeentry, pageper, tuples * sel,
										 frows, fcount, &incounts, &inpages);

	/*
	 * A range collected as a source (DESIGN.md §32) reads the same walk and
	 * counts nothing, so it rechecks nothing either.
	 */
	if (collect)
		return inside;
	inside += lion_range_recheck(root, rel, matching * dirtyfrac, incounts,
								 corr);
	if (nsrc == 0 || !ordered)
		return inside;

	/*
	 * The complement: the entries outside - a run at each end of the column,
	 * with a partial bucket at the range's side of each - the count of F minus
	 * the NULL entry (F's containers once more, and a descent), and F's
	 * candidates on top of the outside entries' for the recheck.
	 */
	outside = lion_cost_range_side_probed(nout, 2.0, sum, probe, perentry,
										  probeentry, pageper, nout * rowsper,
										  frows, fcount, &outcounts,
										  &outpages) +
		fcount + random_page_cost +
		lion_range_recheck(root, rel, frows * (2.0 - sel) * dirtyfrac,
						   Max(outcounts, 1.0), corr);

	/* ... and the leaf-by-leaf race that picks the side, counting only. */
	return Min(inside, outside) +
		2.0 * Min(inpages, outpages) * seq_page_cost;
}

/*
 * The column of idx's key column col, as a Var of rel: what the statistics
 * of a range on it are looked up by.  NULL for an expression column.
 */
static Var *
lion_index_col_var(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
				   AttrNumber col)
{
	RangeTblEntry *rte;
	AttrNumber	attno = idx->indexkeys[col - 1];
	Oid			type;

	if (attno <= 0 || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return NULL;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return NULL;
	type = get_atttype(rte->relid, attno);
	return makeVar(rel->relid, attno, type, -1, get_typcollation(type), 0);
}

/*
 * A RANGE TAKEN AS A SOURCE (DESIGN.md §32, "A range as a source"): the rows
 * whose key lies in it, which the executor collects into memory once per
 * relation - the walk of the range, summaries and all, priced as the walk of
 * a summed range with nothing to AND and nothing to recheck - and then reads
 * as any other set.  What it takes is at most a container per container key
 * of the heap, and about two bytes a row until those fill up; one that would
 * not fit in a hash table's memory (get_hash_memory_limit(), which the
 * executor shares among the relation's ranges) is walked instead, at every
 * count it is part of - `counts` of them - and an OR's leaf, which cannot be
 * walked, is priced out of the plan.
 */
static Cost
lion_cost_range_source(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					   AttrNumber col, Selectivity sel, double counts,
					   bool inor)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		rows = Max(rel->tuples, 1.0) * sel;
	double		conts = lion_containers_for(heap_pages, rows);
	double		bytes = Min(rows * sizeof(uint16) +
							conts * LION_CONTAINER_HDRSZ,
							conts * LION_CONTAINER_MAX_SIZE);
	Var		   *var = lion_index_col_var(root, rel, idx, col);
	Cost		walk;

	if (var == NULL)
		return disable_cost;
	walk = lion_cost_range_sum(root, rel, idx, col, var, sel, NIL, NIL, NIL,
							   true);
	if (bytes <= (double) get_hash_memory_limit())
		return walk + conts * cpu_operator_cost;
	if (inor)
		return disable_cost;
	return walk * Max(counts, 1.0);
}

/*
 * The heap recheck of a range walk (DESIGN.md §28): `tids` candidates on the
 * pages the visibility map cannot vouch for, counted `entries` at a time.  The
 * visibility cache fetches a dirty page once per query - but it answers a page
 * only from its second visit on, and each count flushes its own recheck batch,
 * so a page that holds candidates of several entries is fetched for each of
 * them until the cache has it: never more than twice, and never more often
 * than the entries' candidates lie on pages.  How many pages one entry's
 * candidates lie on follows the column's order, as cost_index() interpolates
 * it with the correlation's square: one per row scattered, their share of the
 * heap in order.
 */
static Cost
lion_range_recheck(PlannerInfo *root, RelOptInfo *rel, double tids,
				   double entries, double corr)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		dirty_pages = Min(heap_pages * dirtyfrac, heap_pages);
	double		rowsper;
	double		scattered;
	double		inorder;
	double		perentry;
	double		pages;

	if (tids <= 0.0 || dirtyfrac <= 0.0)
		return 0.0;

	/* the candidates of one entry, dirty pages or not */
	rowsper = tids / dirtyfrac / Max(entries, 1.0);
	scattered = Min(rowsper, heap_pages);
	inorder = Max(1.0, rowsper * heap_pages / tuples);
	perentry = scattered + (inorder - scattered) * corr * corr;

	pages = Min(tids, dirty_pages);
	pages = Min(Min(tids, entries * perentry * dirtyfrac), 2.0 * pages);

	return pages * lion_heap_page_cost(root, rel, pages, heap_pages) +
		tids * LION_RECHECK_TID_COST;
}

static Cost
lion_cost_count_rel(PlannerInfo *root, RelOptInfo *rel,
				   IndexOptInfo *groupidx, AttrNumber groupcol,
				   IndexOptInfo *groupidx2, AttrNumber groupcol2,
				   List *whereidx, List *wherecol, List *whereclauses,
				   List *wherekinds,
				   List *ors, double numgroups,
				   double outer_entries, double inner_entries, int distinct,
				   double drivefrac, Var *rangevar, bool rangesum)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		dirty_pages;
	double		matching = Max(lion_probe_rel_rows(root, rel), 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		random_pages = 0;	/* directory leaves, one per lookup */
	Cost		descent_cost = 0;	/* comparisons on the way down (§21) */
	double		seq_pages = 0;	/* container chains, read in order */
	Cost		lookup_cost = 0;	/* an IN list's bucket pages, in order */
	Cost		probe_cost = 0; /* what the SOUGHT sources read (§22) */
	Cost		read_cpu = 0;	/* containers read whole: unions, lists */
	Cost		merge_cpu = 0;	/* the AND's driver and probes (§10) */
	int		   *orgrp;			/* each clause's OR restriction, or -1 */
	double		merge_ops = 0;	/* comparisons a union of k sets makes */
	double		recheck_tids;
	double		recheck_pages;
	double	   *clausesel;		/* each clause's own selectivity, for §19 */
	double	   *clausepages;	/* what a WALK of its sets would read */
	double	   *clauseleaves;	/* leaves of ONE of its sets */
	double	   *clauseheight;	/* how tall that set's posting tree is */
	double	   *clausekeys;		/* how many sets it looks up */
	double	   *clauseidx;		/* the pages of the index it reads */
	int		   *clausesrc;		/* the AND source it is part of, or -1 */
	double	   *srcmembers;		/* members of each AND source */
	double	   *srccontainers;	/* and the containers they lie in */
	bool	   *srcunion;		/* is it a union of several sets? */
	double	   *srcsets;		/* ... of how many */
	double	   *srcprobes;		/* how often each is sought */
	int			nsrc;
	int			driver = -1;	/* the source that drives the leapfrog */
	int			nclause = list_length(whereclauses);
	int			ci = 0;
	int			i;
	Cost		pair_cost = 0;	/* §20: the (outer, inner) group pairs */
	Cost		run;
	bool		sumshort;		/* §15: the IN list is summed, not merged */
	bool		groupdrive;		/* §15: the IN list is the GROUP BY driver */
	int			inlistci;
	double		ingroups = numgroups;	/* groups the node really emits */
	double		recheckshare = 1.0; /* §26: what the existence tests recheck */
	Cost		rangecost = 0;	/* §28: the walk of a summed range */
	Cost		rangesrc_cost = 0;	/* §32: collecting the ranges taken as
									 * sources */
	int		   *rangelead;		/* each clause's range source, or -1 */
	double		listrows = 0;	/* rows of one entry of a group-driving list */
	double		walked = 1.0;	/* counts the merge runs: groups, pairs */
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	/*
	 * A sum over a range (DESIGN.md §28) prices its walk, its entries and its
	 * heap recheck itself (lion_cost_range_sum()); what is left here is the
	 * WHERE sources, located once and read once - materialized after their
	 * first use - and the one row.
	 */
	if (rangesum)
	{
		rangecost = lion_cost_range_sum(root, rel, groupidx, groupcol,
										rangevar, drivefrac, whereclauses,
										wherekinds, ors, false);
		ingroups = 1.0;
	}

	/*
	 * A count(DISTINCT k) without a GROUP BY walks k's entries, and a WHERE
	 * equality on k itself drives that walk as a list of one would (DESIGN.md
	 * §26).
	 */
	inlistci = lion_inlist_shape(groupidx, groupcol, groupidx2,
								whereidx, wherecol, whereclauses,
								wherekinds, ors,
								distinct != LION_DISTINCT_NONE && groupidx2 == NULL,
								&sumshort, &groupdrive);

	clausesel = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	clausepages = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	clauseleaves = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	clauseheight = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	clausekeys = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	clauseidx = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	clausesrc = (int *) palloc0(sizeof(int) * Max(nclause, 1));
	srcmembers = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	srccontainers = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	srcunion = (bool *) palloc0(sizeof(bool) * Max(nclause, 1));
	srcsets = (double *) palloc0(sizeof(double) * Max(nclause, 1));
	srcprobes = (double *) palloc0(sizeof(double) * (Max(nclause, 1) + 2));
	orgrp = lion_or_group_map(ors, nclause);

	/*
	 * The sources of the AND: one per OR restriction (a union is one source,
	 * DESIGN.md §19) and one per positive clause outside them.  Which of them
	 * drives the leapfrog join decides what the others read, so they are
	 * numbered here and the page terms are charged once the driver is known.
	 */
	nsrc = list_length(ors);
	for (ci = 0; ci < nclause; ci++)
		clausesrc[ci] = -1;
	ci = 0;
	rangelead = lion_rangesrc_leaders(whereidx, wherecol, wherekinds, ors,
									  nclause);

	forfour(lc1, whereidx, lc2, whereclauses, lc3, wherekinds, lc4, wherecol)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Node	   *clause = (Node *) lfirst(lc2);
		Selectivity sel;
		double		share;
		double		dirpages;
		double		height = 0;
		double		container_pages;
		double		nkeys = 1.0;

		if (!LION_CLAUSE_IS_POSITIVE(lfirst_int(lc3)))
		{
			/*
			 * `IS NOT NULL` selects no rows of its own; it has been left out
			 * of this estimate since before partitions existed.
			 */
			ci++;
			continue;
		}

		/*
		 * A range taken as a source (DESIGN.md §32): one source for all the
		 * bounds of its conjunction, priced once, by the first of them.  It
		 * is read from memory, so it is sought at no page cost; what it costs
		 * is collecting it (lion_cost_range_source()).
		 */
		if (lfirst_int(lc3) == LION_CLAUSE_RANGESRC)
		{
			List	   *bounds = NIL;
			double		clc;
			int			j;

			if (rangelead[ci] != ci)
			{
				ci++;
				continue;
			}
			for (j = ci; j < nclause; j++)
				if (rangelead[j] == ci)
					bounds = lappend(bounds, list_nth(whereclauses, j));
			sel = clauselist_selectivity(root, bounds, 0, JOIN_INNER, NULL);
			list_free(bounds);
			clausesel[ci++] = sel;

			rangesrc_cost += lion_cost_range_source(root, rel, idx,
													(AttrNumber) lfirst_int(lc4),
													sel, Max(ingroups, 1.0),
													orgrp[ci - 1] >= 0);
			clausekeys[ci - 1] = 1.0;
			clauseleaves[ci - 1] = 1.0;
			clauseidx[ci - 1] = Max((double) idx->pages, 1.0);

			/*
			 * As one set of the AND, it drives the merge or is probed at the
			 * driver's keys (lion_merge_cpu_cost()); as an OR's leaf, its
			 * containers are all read into the union.
			 */
			clc = lion_key_containers(root, rel, idx,
									  (AttrNumber) lfirst_int(lc4), tuples * sel);
			if (orgrp[ci - 1] >= 0)
				read_cpu += clc * (LION_CONTAINER_COST + LION_MEMBER_COST *
								   Min(tuples * sel / clc, LION_MEMBER_CAP));
			clausesrc[ci - 1] = (orgrp[ci - 1] >= 0) ? orgrp[ci - 1] : nsrc++;
			srcmembers[clausesrc[ci - 1]] += tuples * sel;
			srccontainers[clausesrc[ci - 1]] += clc;
			srcsets[clausesrc[ci - 1]] += 1.0;
			if (orgrp[ci - 1] >= 0)
				srcunion[clausesrc[ci - 1]] = true;
			continue;
		}

		sel = clause_selectivity(root, clause, 0, JOIN_INNER, NULL);
		clausesel[ci++] = sel;

		/*
		 * The page terms belong to ONE KEY COLUMN of this index (DESIGN.md
		 * §24): the directory it descends is its own share of the relation's,
		 * and so are the container pages its chains lie on.  The DEPTH is not
		 * scaled - the descent passes through the upper levels the columns
		 * share - and neither is the index's own size below, which is what the
		 * caching argument of lion_heap_page_cost() is about and is a property
		 * of the relation.
		 */
		share = lion_index_column_share(root, rel, idx,
										(AttrNumber) lfirst_int(lc4));
		dirpages = lion_index_dir_pages(idx, &height);

		container_pages = ((double) idx->pages - 1.0 - dirpages) *
			lion_index_column_posting_share(root, rel, idx,
											(AttrNumber) lfirst_int(lc4));
		container_pages = Max(container_pages, 0.0);
		dirpages = Max(dirpages * share, 1.0);

		/*
		 * An IN list costs one lookup per element (DESIGN.md §15).  Each of
		 * them hashes to a bucket page of its own - at most one per bucket,
		 * so a list longer than the index has buckets shares them - walks a
		 * chain of its own, whose pages are its share of the container pages
		 * but never fewer than one, and contributes a sub-cursor of its own
		 * to the union the merge evaluates.  A union of k sets merges the
		 * members of all of them, which costs log2(k) comparisons per member
		 * however the merge is organised, and that is the term that makes a
		 * long list lose: at one million rows a thousand-element list took
		 * 15 ms against the B-tree index-only scan's 3.1 ms and was chosen
		 * anyway, because every element was priced as one bucket page (the
		 * 2026-09-21 follow-up review).  A single-key clause has k = 1 and
		 * pays nothing for a merge it does not make.
		 *
		 * ... unless there is no union to build.  When the list is the only
		 * positive source, or when it drives the groups, the entries are
		 * counted one at a time and added up (the disjoint-sum short-circuit,
		 * DESIGN.md §15): what is left is the per-element lookup and the
		 * per-element container work, both already priced above, and nothing
		 * at all for a merge that does not happen.  Dropping the term is what
		 * lets a thousand-value list on a high-cardinality column be chosen
		 * again, which it should be: 1.5 ms against the B-tree's 4.5 at one
		 * million rows.
		 */
		if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

#if PG_VERSION_NUM >= 170000
			nkeys = Max(estimate_array_length(root,
											  (Node *) lsecond(saop->args)),
						1.0);
#else
			nkeys = Max(estimate_array_length((Node *) lsecond(saop->args)),
						1.0);
#endif
		}

		if (nkeys > 1.0)
		{
			/*
			 * A list's lookups are NOT a sequence of random reads.
			 * lion_posting_set_lookup_many() sorts the values into the
			 * directory order and walks the leaves left to right (DESIGN.md
			 * §21), so the same argument lion_heap_page_cost() makes about a
			 * recheck's heap pages applies to them, and more strongly than it
			 * did to the hash directory this replaced: a set of pages that is
			 * dense in the index, or that fits in the cache, is read at
			 * something near seq_page_cost.  Charging a thousand-element list
			 * five hundred RANDOM reads of a one-megabyte index is what kept
			 * the node from being chosen for a query it answers in 2.9 ms
			 * against the B-tree index-only scan's 3.7.
			 *
			 * The chain is capped at the container pages the index has: a
			 * list cannot read more of them than exist, and an index whose
			 * entries are all INLINE (which is what a high-cardinality column
			 * looks like since DESIGN.md §13) has none to read at all - its
			 * payloads are on the leaves already charged.
			 */
			double		lookups = Min(nkeys, dirpages);
			double		idx_pages = Max((double) idx->pages, 1.0);

			lookup_cost += lookups *
				lion_heap_page_cost(root, rel, lookups, idx_pages);

			/*
			 * ... and each value's search: a binary search of its leaf, and
			 * the levels above it too where the values are too sparse for the
			 * walk to step from one to the next (a descent each, §21).  Free
			 * before 2026-09-27, which chose a thousand-value list over a
			 * near-unique column at 7.6 ms against the btree index-only scan's
			 * 1.8.
			 */
			descent_cost += nkeys * LION_DESCENT_COST *
				(1.0 + height * Min(1.0, dirpages / nkeys));
			clausepages[ci - 1] = Min(Max(nkeys, container_pages * sel),
									  container_pages);
		}
		else
		{
			/*
			 * One descent.  Only the LEAF is charged as a page read: the root
			 * and the internal pages above it are a handful of blocks that
			 * every lookup touches, so they stay in cache, which is exactly
			 * the argument btcostestimate() makes about a btree's upper
			 * levels.  What the descent does cost is the comparisons, one
			 * page's worth per level.
			 */
			random_pages += 1.0;
			descent_cost += (height + 1.0) * LION_DESCENT_COST;
			clausepages[ci - 1] = Max(1.0, container_pages * sel);
		}

		/*
		 * What a walk of this clause's sets would read is on the books; how
		 * much of it is really read depends on whether its source drives the
		 * leapfrog join, which is not known until every clause has been seen
		 * (the page terms are charged below).  One of its sets - a list has
		 * nkeys of them - occupies this many container pages, and its posting
		 * tree is that tall.
		 */
		clauseleaves[ci - 1] = Max(clausepages[ci - 1] / nkeys, 1.0);
		clauseheight[ci - 1] = lion_posting_height(clauseleaves[ci - 1]);
		clausekeys[ci - 1] = nkeys;
		clauseidx[ci - 1] = Max((double) idx->pages, 1.0);

		{
			double		clc = nkeys * lion_key_containers(root, rel, idx,
														  (AttrNumber) lfirst_int(lc4),
														  tuples * sel / nkeys);

			/*
			 * A single set ANDed with the others is read only where it drives
			 * the merge, and PROBED at the driver's container keys elsewhere
			 * (DESIGN.md §22), which lion_merge_cpu_cost() prices below.  The
			 * containers of an OR leaf and of an IN list's sets are all READ:
			 * a union is built of them (§15, §19) or they are summed.  A list
			 * that drives the groups is read one entry a group, below.
			 */
			if (!(groupdrive && ci - 1 == inlistci) &&
				(orgrp[ci - 1] >= 0 || nkeys > 1.0))
				read_cpu += clc * (LION_CONTAINER_COST + LION_MEMBER_COST *
								   Min(tuples * sel / clc, LION_MEMBER_CAP));

			/*
			 * Which source of the AND this clause belongs to: the union of its
			 * OR restriction, or one of its own.  A list that drives the groups
			 * is not a source at all - each group IS one of its entries - so it
			 * neither drives the leapfrog nor is sought by it.
			 */
			if (groupdrive && ci - 1 == inlistci)
				clausesrc[ci - 1] = -1;
			else
			{
				clausesrc[ci - 1] = (orgrp[ci - 1] >= 0) ? orgrp[ci - 1] : nsrc++;
				srcmembers[clausesrc[ci - 1]] += tuples * sel;
				srccontainers[clausesrc[ci - 1]] += clc;
				srcsets[clausesrc[ci - 1]] += nkeys;
				if (orgrp[ci - 1] >= 0 || nkeys > 1.0)
					srcunion[clausesrc[ci - 1]] = true;
			}
		}

		/*
		 * Does the merge build this clause's union?  A list that drives the
		 * groups is not a source at all, and a list that is the only positive
		 * source is SUMMED instead - but only while the sum is the cheaper of
		 * the two, which is lion_sum_is_cheaper() in the executor and the same
		 * test from estimates here: dense entries, enough of them for the
		 * merge's bitset image, and the merge runs after all.
		 */
		if (nkeys > 1.0)
		{
			double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
			bool		merged = true;

			if (ci - 1 == inlistci)
				merged = (!groupdrive &&
						  nkeys >= (double) LION_OR_BITSET_MIN &&
						  (tuples * sel / nkeys) / ckeys >
						  (double) LION_SUM_MAX_DENSITY);

			if (merged)
				merge_ops += lion_merge_ops(heap_pages, tuples * sel, nkeys);
		}

		/*
		 * A list that drives the groups is not a source: each group is one of
		 * its entries, and the rest of the list has nothing to say about that
		 * group's rows.  So it is not intersected with anything, and the
		 * per-group work below is over the listed values rather than over
		 * every entry of the index.
		 */
		if (groupdrive && ci - 1 == inlistci)
		{
			ingroups = Min(nkeys, ingroups);
			listrows = tuples * sel / nkeys;
		}
	}

	/*
	 * An OR across columns (DESIGN.md §19) is the union of its arms, and a
	 * union of k sub-cursors costs what §15's IN list does: log2(k)
	 * comparisons per member, over the members of all of them together.  The
	 * lookups and the chains of its leaves have already been charged above,
	 * one per leaf, exactly as if they had been separate clauses; the term
	 * here is the merge they take part in and nothing else.
	 */
	foreach(lc1, ors)
	{
		List	   *one = (List *) lfirst(lc1);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		double		members = 0;
		int			nleaves = 0;

		for (i = 0; i < narms; i++)
			nleaves += list_nth_int(one, 2 + i);
		for (i = first; i < first + nleaves && i < nclause; i++)
			members += tuples * clausesel[i];

		if (nleaves > 1)
			merge_ops += members * log2((double) nleaves);
	}
	pfree(clausesel);
	pfree(orgrp);
	pfree(rangelead);


	/*
	 * WHAT THE SOURCES READ (DESIGN.md §22).  The same leapfrog decides it: the
	 * DRIVER - the source with the fewest members - is the only one walked end
	 * to end, and it pays for its whole share of the index's container pages,
	 * as every source did before the posting tree existed.  Every other source
	 * is SOUGHT to the container keys the driver produces, so it pays for the
	 * pages those probes touch (lion_probed_pages()) and never for more than a
	 * walk of it would have cost.
	 *
	 * That bound is the whole of this section's planner change.  `c20k = 77 AND
	 * c200 = 17 AND c2 = 1` at one million rows probes `c2` at about 50 of the
	 * heap's 300 container keys, a descent each, and the count really does
	 * touch 118 of that index's buffers rather than the 154 a walk of the set
	 * takes; charging it the whole 152-page walk asked 168.8 cost units for a
	 * count the node answers in 0.25 ms, against 117.6 now.  The BitmapAnd it
	 * still loses to is priced at 62.1 and takes 0.88 ms - what remains between
	 * them is the unit and not the count of pages, which DESIGN.md §22 records
	 * as the open item.
	 *
	 * With a GROUP BY the probing happens once per group - the group's own
	 * posting set is a source like any other, and the smaller one of it and the
	 * WHERE sources drives - so the probes are counted over all the groups
	 * together.  That is more probes than a WHERE set has pages many times
	 * over, which is exactly why a grouped count is priced as it was: one read
	 * of each WHERE set, which is also what the materialized copy of it costs
	 * (DESIGN.md §9 - the sets a GROUP BY intersects with every group are
	 * copied out on their second use and are probed in memory after that).
	 */
	/*
	 * Which one drives is decided from the MEMBERS, as lion_run_merge() decides
	 * it from the entries' `ntids` - not from the containers, which for a union
	 * of k sets are counted k times over and would hand the merge to whichever
	 * source happens to lie in the fewest of them.  What the driver then costs
	 * the others is its CONTAINER KEYS, of which there are no more than the
	 * heap has.
	 */
	if (nsrc > 0)
	{
		double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
		int			s;

		for (s = 0; s < nsrc; s++)
		{
			if (driver < 0 || srcmembers[s] < srcmembers[driver])
				driver = s;
			srccontainers[s] = Min(srccontainers[s], ckeys);
		}
	}

	/*
	 * THE MERGE (DESIGN.md §10, "The units"; §22).  An ungrouped count is one
	 * merge of the AND's sources, driven by the smallest and the others sought
	 * at its keys (lion_merge_cpu_cost()) - unless its one source is a union,
	 * whose containers are all read above and are the whole of the work.  A
	 * grouped count runs one merge per GROUP, in which the group's own set is a
	 * source like any other - it drives whenever it is smaller than the WHERE's
	 * sets - and one per (outer, inner) PAIR of two group columns (§20).  Every
	 * entry of the grouping column is counted, whatever the WHERE leaves of it
	 * (lion_next_group() skips the empty ones only after counting them): so the
	 * merges are its n_distinct, not the groups the planner expects out.  The
	 * model before 2026-09-27 charged each group the containers of its share of
	 * the WHERE's rows, which for `c20k, count(*) ... WHERE c20 = 3 GROUP BY
	 * c20k` is 12 of the 219 each entry's own set lies in: 6,550 units for
	 * 1,091 ms, where the sequential aggregate is 142,000 for 905.
	 * A summed range prices its entries' merges itself (lion_cost_range_sum()).
	 */
	if (groupidx == NULL && !rangesum && nsrc > 0 &&
		(nsrc > 1 || !srcunion[0]))
		merge_cpu = lion_merge_cpu_cost(nsrc, srcmembers, srccontainers,
										NULL, tuples, srcprobes);
	else if (groupidx != NULL && !rangesum)
	{
		double	   *mem = (double *) palloc(sizeof(double) * (nsrc + 2));
		double	   *keys = (double *) palloc(sizeof(double) * (nsrc + 2));
		bool	   *inmem = (bool *) palloc0(sizeof(bool) * (nsrc + 2));
		double		unionsets = 0;
		double		gnd = -1.0;
		double		gm;
		double		per;
		double		share = 1.0;
		int			s;

		/*
		 * The WHERE's sets are copied into memory on their second use and
		 * probed there by every group after that (DESIGN.md §9); a group's
		 * own set is located from the index for its one count.
		 */
		for (s = 0; s < nsrc; s++)
		{
			mem[s] = srcmembers[s];
			keys[s] = srccontainers[s];
			inmem[s] = true;
		}

		/* the rows of one group's own set, and how many groups are counted */
		if (groupdrive)
		{
			gm = Max(listrows, 1.0);
			walked = Max(ingroups, 1.0);
		}
		else
		{
			if (groupcol >= 1 && groupcol <= groupidx->nkeycolumns &&
				rel->relid > 0 && rel->relid < (Index) root->simple_rel_array_size &&
				root->simple_rte_array[rel->relid] != NULL &&
				root->simple_rte_array[rel->relid]->rtekind == RTE_RELATION)
				gnd = lion_index_column_nd(root, rel, groupidx, groupcol - 1);
			if (gnd > 0.0)
			{
				gm = tuples / gnd;
				walked = Max(gnd * drivefrac, 1.0);
			}
			else
			{
				gm = tuples / Max(numgroups, 1.0);
				walked = Max(ingroups, 1.0);
			}
		}
		mem[nsrc] = gm;
		keys[nsrc] = lion_key_containers(root, rel, groupidx, groupcol, gm);

		/*
		 * A WHERE source that is a union - an IN list, an OR - is built again
		 * by every count, each of its sets' cursors set up and positioned
		 * over the copy in memory (LION_UNION_SET_COST a set).
		 */
		for (s = 0; s < nsrc; s++)
			if (srcunion[s])
				unionsets += srcsets[s];

		if (groupidx2 == NULL)
		{
			/*
			 * The count(DISTINCT k) walk over k's entries (DESIGN.md §26)
			 * tests each entry for ONE visible row and stops there, so it
			 * reads the share of each entry's intersection
			 * lion_exists_fraction() expects - one container or two when the
			 * entries are dense, all of it when the WHERE leaves most of them
			 * empty - and rechecks that share of the candidates.
			 */
			if (distinct == LION_DISTINCT_EXISTS)
			{
				double		drvkeys = keys[nsrc];

				/* the test walks the merge's driver: the smallest source */
				for (s = 0; s < nsrc; s++)
					if (mem[s] < mem[nsrc])
						drvkeys = Min(drvkeys, keys[s]);
				share = lion_exists_fraction(drvkeys, matching / walked);
				recheckshare = share;
			}
			per = lion_merge_cpu_cost(nsrc + 1, mem, keys, inmem, tuples,
									  srcprobes) + unionsets * LION_UNION_SET_COST;
			merge_cpu = walked * per * share;

			/*
			 * ... and each count's own set-up and tear-down (a memory context,
			 * the sources' cursors, the visibility-map state): a distinct
			 * walk's test prices it (LION_DISTINCT_TEST_COST, below), a range's
			 * walk its entry (LION_RANGE_ENTRY_COST).
			 */
			if (distinct == LION_DISTINCT_NONE && rangevar == NULL)
				merge_cpu += walked * (groupdrive ? LION_LIST_GROUP_COST :
									   LION_ENTRY_COUNT_COST);
		}
		else
		{
			/*
			 * A second GROUP BY column (DESIGN.md §20): every (outer, inner)
			 * PAIR of the two indexes' entries is a count of its own, the
			 * two groups' sets and the WHERE's merged, whether or not the
			 * intersection comes out empty.  Two groups whose rows are
			 * scattered over the whole heap have a container at nearly every
			 * container key, which is what makes a pair expensive even when
			 * their intersection is empty.
			 */
			double		oe = Max(outer_entries, 1.0);
			double		ie = Max(inner_entries, 1.0);

			mem[nsrc] = tuples / oe;
			keys[nsrc] = lion_key_containers(root, rel, groupidx, groupcol,
											 tuples / oe);
			mem[nsrc + 1] = tuples / ie;
			keys[nsrc + 1] = lion_key_containers(root, rel, groupidx2,
												 groupcol2, tuples / ie);
			/*
			 * The early exit of a count(DISTINCT k)'s pair test is NOT
			 * discounted: a pair's time is its lookup and its merge's set-up
			 * far more than its members.  Discounted, 200 x 50 pairs over 100k
			 * rows were chosen at 10,566 for 45 ms against the sorting
			 * aggregate's 10,694 for 27.
			 */
			per = lion_merge_cpu_cost(nsrc + 2, mem, keys, inmem, tuples,
									  srcprobes) + unionsets * LION_UNION_SET_COST;
			walked = oe * ie;
			merge_cpu = walked * per;
		}
		pfree(mem);
		pfree(keys);
		pfree(inmem);
	}

	for (i = 0; i < nclause; i++)
	{
		double		pages = clausepages[i];

		if (pages <= 0.0)
			continue;			/* a negated clause, or nothing to read */

		if (clausesrc[i] < 0 || clausesrc[i] == driver || rangesum)
			seq_pages += pages; /* walked: the driver, a group's own list,
								 * and what a range's entries are ANDed with */
		else
		{
			pages = Min(pages,
						clausekeys[i] * lion_probed_pages(clauseleaves[i],
														  srcprobes[clausesrc[i]] * walked,
														  clauseheight[i]));
			probe_cost += pages *
				lion_heap_page_cost(root, rel, pages, clauseidx[i]);
		}
	}
	pfree(clausepages);
	pfree(clauseleaves);
	pfree(clauseheight);
	pfree(clausekeys);
	pfree(clauseidx);
	pfree(clausesrc);
	pfree(srcmembers);
	pfree(srccontainers);
	pfree(srcunion);
	pfree(srcsets);
	pfree(srcprobes);

	/*
	 * The entry scan of the driving index - unless an IN list on that very
	 * column drives the groups instead (DESIGN.md §15), in which case its
	 * elements' lookups and containers, charged above, ARE the per-group work
	 * and the index's entries are never walked.  A summed range has priced its
	 * walk already.
	 */
	if (groupidx != NULL && !groupdrive && !rangesum)
	{
		/*
		 * The entry scan walks ONE key column's entries and stops at the first
		 * entry of the next (DESIGN.md §24), so what it reads of a multicolumn
		 * index is that column's share of it and not the whole relation - and
		 * a range bounds the walk to drivefrac of those (§28), the share of
		 * the column's entries it selects.
		 */
		seq_pages += Max(1.0, (double) groupidx->pages *
						 lion_index_column_share(root, rel, groupidx,
												 groupcol) * drivefrac);

		/*
		 * Beside a second GROUP BY column a count(DISTINCT k) tests each
		 * group of the first for a row as well, which the pairs' tests come
		 * on top of (DESIGN.md §26).
		 */
		if (distinct != LION_DISTINCT_NONE && groupidx2 != NULL)
			recheckshare = lion_exists_fraction(
												lion_containers_for(heap_pages,
																	matching / Max(numgroups, 1.0)),
												matching / Max(numgroups, 1.0));
	}

	/*
	 * A second GROUP BY column (DESIGN.md §20) is a nested loop over the two
	 * indexes' entries: the outer index's entries drive the scan and the
	 * inner index is read ONCE - its keys are kept in memory - but every
	 * (outer, inner) PAIR costs a lookup of the inner posting set and an
	 * attempt at the intersection, whether or not that comes out empty.
	 * Three terms, and the second is the one that decides:
	 *
	 *	- one cpu_tuple_cost per pair, for the lookup and the per-pair
	 *	  bookkeeping;
	 *	- the INTERSECTION itself.  ANDing two containers costs about the
	 *	  members of the smaller of them, so one pair costs about
	 *	  Min(rows/outer_entries, rows/inner_entries) member steps, and summed
	 *	  over all outer_entries x inner_entries pairs that is exactly
	 *	  `Min(outer_entries, inner_entries) x rows` - independent of which of
	 *	  the two drives the scan, which is why the choice of outer is about
	 *	  the entry scans and the memory and not about this;
	 *	- and the container bookkeeping of both sides at every pair, which is
	 *	  what makes a pair of WIDELY SPREAD groups expensive even when their
	 *	  intersection is empty: two groups whose rows are scattered over the
	 *	  whole heap have a container at nearly every container key, so the
	 *	  merge steps through all of them.
	 *
	 * Measured on 200k rows of a 100-byte-wide table, uncorrelated columns
	 * (2026-09-21, assert build): 20 x 2 groups is 5.6 ms against the
	 * sequential aggregate's 37.3 ms and is chosen; 200 x 20 is 60.7 ms
	 * against 36.8 ms and must NOT be, which the member term above is what
	 * says; 20000 x 200 is four million pairs and is refused by a wide
	 * margin.
	 */
	if (groupidx2 != NULL)
	{
		double		oe = Max(outer_entries, 1.0);
		double		ie = Max(inner_entries, 1.0);

		double		co = lion_containers_for(heap_pages, tuples / oe);
		double		cinner = lion_containers_for(heap_pages, tuples / ie);

		seq_pages += Max(1.0, (double) groupidx2->pages *
						 lion_index_column_share(root, rel, groupidx2,
												 groupcol2));
		pair_cost = Max(oe * ie, numgroups) *
			((distinct == LION_DISTINCT_NONE) ? LION_ENTRY_COUNT_COST :
			 cpu_tuple_cost);

		/*
		 * The (g, k) pairs of a count(DISTINCT k) per group (DESIGN.md §26)
		 * are these same pairs and are charged exactly as above - that is
		 * what makes a large |G| x |K| lose to the sorting aggregate core
		 * builds for a distinct count - plus the fixed cost of a test on
		 * each.  The early exit is NOT discounted from the pair terms: a
		 * pair's time is its lookup and its merge's setup far more than its
		 * members, and measured per pair an existence test was 6.5 us
		 * against 7.7 for a count (100k rows, 200 x 50 pairs, assert build).
		 * What it does save is heap rechecks, one container's worth per test
		 * instead of all of them, and that is charged as such below.
		 */
		if (distinct != LION_DISTINCT_NONE)
		{
			pair_cost += oe * ie * LION_DISTINCT_TEST_COST;
			recheckshare += (distinct == LION_DISTINCT_EXISTS) ?
				lion_exists_fraction(Min(co, cinner), matching / (oe * ie)) :
				1.0;
		}
	}
	/*
	 * ... and the fixed cost of the tests of a count(DISTINCT k) that are not
	 * pairs (DESIGN.md §26): one per entry of k the walk visits - or per
	 * listed value, when a list on k drives it - and beside a GROUP BY one
	 * per group, the group's own test.
	 */
	if (distinct != LION_DISTINCT_NONE)
		pair_cost += ingroups * LION_DISTINCT_TEST_COST;

	/*
	 * Rechecking is what makes the pushdown expensive, and the estimate has
	 * to say so: every TID whose heap block the visibility map cannot vouch
	 * for is resolved against the snapshot.
	 *
	 * The blocks that can be touched are only the ones the visibility map
	 * cannot vouch for - heap_pages * dirtyfrac, from the same
	 * relallvisible/relpages the TID estimate comes from - and each of them
	 * is FETCHED AT MOST ONCE per query, whatever brings the count back to
	 * it: the per-query visibility cache resolves every root line pointer of
	 * a dirty page on its first visit and answers every later visit out of
	 * memory (DESIGN.md §9).  Charging random reads across the whole heap
	 * instead made the model refuse the pushdown on freshly vacuumed tables,
	 * where it is at its best (the 2026-09-20 review, finding 5).
	 *
	 * A GROUP BY therefore pays for the same working set as a single count,
	 * once, and what it repeats per group is CPU: one visibility-bit lookup
	 * per candidate TID - and the candidates of all the groups together are
	 * the same matching * dirtyfrac - plus the per-group container
	 * bookkeeping already in ncontainers above.  Charging
	 * numgroups * dirty_pages RANDOM reads instead asked 1.8M cost units for
	 * a 200-group count of five million rows with 9% of the heap pages
	 * dirty, against the sequential aggregate's 175k, for a node that ran in
	 * 148 ms against 1174 ms over a resident 71 MiB working set with zero
	 * physical reads (the 2026-09-21 follow-up review).
	 */
	dirty_pages = Min(heap_pages * dirtyfrac, heap_pages);
	recheck_tids = matching * dirtyfrac * recheckshare;
	recheck_pages = Min(recheck_tids, dirty_pages);

	/* A summed range has priced its own (lion_cost_range_sum()). */
	if (rangesum)
		recheck_tids = recheck_pages = 0.0;

	/*
	 * ... except that a RANGE-bounded walk (DESIGN.md §28) does not visit a
	 * dirty page once per query: it counts entry by entry, each count flushes
	 * its own recheck batch, and the visibility cache only answers a page from
	 * its second visit on - so a page that holds rows of several entries is
	 * fetched for each of them.  How many pages one entry's candidates lie on
	 * depends on how the column follows the heap order, which is what
	 * cost_index() interpolates with the correlation's square: at most one per
	 * row when the values are scattered, and the rows' share of the heap when
	 * they are stored in order.  Measured at five million rows with every heap
	 * page dirty: a 30-day range over randomly placed days rechecked 68,384
	 * block visits for 37,133 pages (166 ms, against the bitmap heap scan's
	 * 46), and the same range over days stored in order 581 (3.8 ms, against
	 * the btree's 11.5).  Priced as one visit per dirty page the first was
	 * chosen and the second refused.  The cache does bound it: a page is
	 * resolved on its second visit and answered from memory after that, so
	 * while the cache has room no page is fetched more than twice - 2,454
	 * block visits for 1,148 pages when 21 entries of a 200-value column share
	 * every page of a small table.
	 */
	if (rangevar != NULL && recheck_tids > 0.0)
	{
		double		entries = lion_range_entries(root, rel, rangevar,
												 drivefrac);
		double		rowsper = matching / entries;
		double		corr = lion_var_correlation(root, rel, rangevar);
		double		scattered = Min(rowsper, heap_pages);
		double		inorder = Max(1.0, rowsper * heap_pages / tuples);
		double		perentry = scattered + (inorder - scattered) * corr * corr;

		recheck_pages = Min(Min(recheck_tids, entries * perentry * dirtyfrac),
							2.0 * recheck_pages);
	}

	/*
	 * ... and the rows of ONE value (a plain equality, the commonest count of
	 * all) lie on as many heap pages as the column's order says, which is
	 * what cost_index() prices a plain index scan's heap side with (§29.11):
	 * one per row when the values are scattered, the rows' share of the heap
	 * when they are stored in order, interpolated by the correlation's
	 * square.  The recheck visits each of those pages once, in block order,
	 * so charging one page per candidate TID made a count over a clustered
	 * value whose visibility map had gone stale (relallvisible is only
	 * refreshed by VACUUM, while updates and on-access pruning change the
	 * map) cost more than a plain index scan that fetches every row: 5036
	 * against 3252 units for 5000 rows on 32 pages at a million rows, and the
	 * node ran 2.5x faster than the scan that was chosen.
	 */
	if (rangevar == NULL && ors == NIL && recheck_tids > 0.0)
	{
		Var		   *eqvar = lion_single_eq_var(rel, whereclauses, wherekinds);

		if (eqvar != NULL)
		{
			double		corr = lion_var_correlation(root, rel, eqvar);
			double		scattered = Min(matching, heap_pages);
			double		inorder = Max(1.0, matching * heap_pages / tuples);
			double		spanned = scattered + (inorder - scattered) * corr * corr;

			recheck_pages = Min(recheck_pages, Max(1.0, spanned * dirtyfrac));
		}
	}

	run = random_pages * random_page_cost;
	run += descent_cost;
	run += lookup_cost;
	run += seq_pages * seq_page_cost;
	run += probe_cost;
	run += read_cpu + merge_cpu;	/* containers, members, probes (§10) */
	run += merge_ops * cpu_operator_cost;
	run += recheck_pages * lion_heap_page_cost(root, rel, recheck_pages,
											  heap_pages);
	run += recheck_tids * ((groupidx != NULL && !rangesum) ?
						   LION_RECHECK_GROUP_TID_COST : LION_RECHECK_TID_COST);
	run += ingroups * cpu_tuple_cost;
	run += pair_cost;
	run += rangecost;
	run += rangesrc_cost;

	return run;
}

/*
 * Sum the per-relation costs over every relation the node will count and put
 * the result on the path.  The WHERE clauses that do not select rows
 * (`IS NOT NULL`, DESIGN.md §14) are left out of the per-relation estimate,
 * as they were before partitions existed - by lion_cost_count_rel() itself
 * rather than by filtering the lists here, because the OR structure of
 * DESIGN.md §19 names its leaves by their position in them.
 */
static void
lion_cost_count_path(PlannerInfo *root, CustomPath *cpath, List *targets,
					List *whereclauses, List *wherekinds, List *ors,
					double numgroups, double outer_entries,
					double inner_entries, double outrows, int distinct,
					int ranged, double drivefrac)
{
	Cost		run = 0;
	ListCell   *lc;

	foreach(lc, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		int			tranged = ranged;
		double		tfrac = drivefrac;
		Var		   *rangevar;

		/*
		 * The sum over every entry of a summarized column (DESIGN.md §32) is
		 * the sum over its summaries: a range over all of it, and priced as
		 * one.  Without summaries - in this relation: partitions differ - it
		 * is the walk over every entry it always was.
		 */
		if (tranged == LION_RANGED_SUMALL)
		{
			if (t->driveidx[0] != NULL &&
				lion_index_col_summarized(t->driveidx[0], t->drivecol[0]))
			{
				tranged = LION_RANGED_SUM;
				tfrac = 1.0;
			}
			else
				tranged = LION_RANGED_NONE;
		}
		rangevar = (tranged != LION_RANGED_NONE) ? t->drivevar[0] : NULL;

		run += lion_cost_count_rel(root, t->rel,
								  t->driveidx[0], t->drivecol[0],
								  t->driveidx[1], t->drivecol[1],
								  t->whereidx, t->wherecol,
								  whereclauses, wherekinds, ors, numgroups,
								  outer_entries, inner_entries, distinct,
								  tfrac, rangevar,
								  tranged == LION_RANGED_SUM && rangevar != NULL &&
								  t->driveidx[0] != NULL);

		/*
		 * A multi-key query the node only has at run time may need its
		 * candidates rechecked in the heap, one count - one group, one test -
		 * at a time (DESIGN.md §17, "A query known only at run time").
		 */
		run += lion_cost_recheck(root, t->rel, t->whereidx, t->wherecol,
								 whereclauses, wherekinds, ors,
								 lion_probe_rel_rows(root, t->rel), numgroups,
								 t->rel->tuples);

		/*
		 * A range-bounded GROUP BY walk (DESIGN.md §28) pays a fixed cost per
		 * entry it visits, in each relation it walks - a partition's entries
		 * are its own.  A count(DISTINCT) walk already pays its per-test cost
		 * for each of them (§26), which is the same work, and a summed range
		 * has priced its entries in lion_cost_range_sum().
		 */
		if (tranged == LION_RANGED_WALK && distinct == LION_DISTINCT_NONE &&
			rangevar != NULL)
			run += lion_range_entries(root, t->rel, rangevar,
									  tfrac) * LION_RANGE_ENTRY_COST;
	}

	cpath->path.rows = outrows;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = 0;
#endif

	/*
	 * Every form of the node streams its rows as it counts them - one per
	 * group, and with partitions one per group per partition (DESIGN.md §16:
	 * the partials go to a Finalize Agg above, which is costed by core) - so
	 * only the single-row forms have to do the whole scan before the first
	 * row comes out.
	 */
	cpath->path.startup_cost = (outrows <= 1.0) ? run : 0.0;
	cpath->path.total_cost = run;
}

/*
 * How a multi-key clause will be answered (DESIGN.md §17), and how many
 * posting sets its query is made of: the keys its extraction yields, which is
 * what a count merges at every container key it asks the clause about.
 *
 * A literal is extracted again the way the executor extracts it, exactly -
 * only an exact extraction of a literal is pushed down
 * (lion_multikey_query_is_exact()).  A value the executor only has at run
 * time comes here as its plan-time estimate when there is one (the clause
 * lion_analyze_leaf() gave the cost model carries it), extracted as the
 * executor will extract the run-time value: exactly, as a superset to be
 * rechecked, or as every row.  With no estimate at all - a generic plan's
 * parameter - the query's shape is unknown, and it is taken to be the
 * expensive one, every row, as lioncostestimate() takes it for a bitmap scan.
 * Anything unexpected answers one set, the old price.
 */
static LionQueryMode
lion_multikey_cost_mode(IndexOptInfo *idx, AttrNumber col, Node *clause,
						double *nkeys)
{
	OpExpr	   *op;
	Node	   *arg;
	Const	   *con;
	Oid			opfamily;
	Oid			lefttype;
	Oid			proc;
	int			strategy;
	FmgrInfo	flinfo;
	LionQuery	q;
	LionState	state;
	MemoryContext cxt;
	MemoryContext oldcxt;

	*nkeys = 1.0;
	if (clause == NULL || !IsA(clause, OpExpr) || col < 1 ||
		col > idx->nkeycolumns || list_length(((OpExpr *) clause)->args) != 2)
		return LION_QMODE_KEYS;
	op = (OpExpr *) clause;

	/* the column is the left operand of a multi-key clause, the query the right */
	arg = lion_strip((Node *) lsecond(op->args));
	if (arg == NULL || !IsA(arg, Const))
		return LION_QMODE_ALL;
	con = (Const *) arg;
	if (con->constisnull)
		return LION_QMODE_NONE;

	opfamily = idx->opfamily[col - 1];
	lefttype = idx->opcintype[col - 1];
	strategy = get_op_opfamily_strategy(op->opno, opfamily);
	proc = get_opfamily_proc(opfamily, lefttype, lefttype,
							 LION_EXTRACTQUERY_PROC);
	if (strategy == 0 || !OidIsValid(proc))
		return LION_QMODE_KEYS;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring count query keys",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&state, 0, sizeof(state));
	state.multikey = true;
	state.collation = con->constcollid;
	fmgr_info(proc, &flinfo);
	state.extractquery = flinfo;

	/* a query the exact extraction answers comes out the same either way */
	lion_extract_query_superset(&state, con->constvalue,
								(StrategyNumber) strategy, &q);
	if (q.mode == LION_QMODE_KEYS || q.mode == LION_QMODE_LOSSY)
		*nkeys = Max((double) q.nkeys, 1.0);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return q.mode;
}

static double
lion_multikey_nkeys(IndexOptInfo *idx, AttrNumber col, Node *clause)
{
	double		nkeys;

	(void) lion_multikey_cost_mode(idx, col, clause, &nkeys);
	return nkeys;
}

/*
 * The heap recheck of the multi-key clauses whose query the node only has at
 * run time (DESIGN.md §17, "A query known only at run time"), over `matched`
 * rows of `rel` - the rows every clause selects - counted `counts` times over
 * (once per group, per test of a count(DISTINCT) walk, or per dimension row
 * of the FK-side join), and never more than `ceiling` candidates.
 *
 * How each clause will be answered is asked of its plan-time estimate
 * (lion_multikey_cost_mode()): exactly, which needs no recheck and costs
 * nothing here; as a superset, whose candidates are taken to be the rows the
 * clause selects - a floor, since the estimate says the superset is wider but
 * not by how much; or as every row, whose candidates are then every row the
 * OTHER clauses select - with no other clause, every row of the relation,
 * read by a sequential scan (lion_count_heap_filtered()).  A value with no
 * estimate at all is that last case.
 *
 * Each candidate is fetched, whatever the visibility map says - the map
 * vouches for visibility and not for the filter - on the pages each count
 * reads for itself (a filtered recheck keeps no visibility cache), and tested
 * at the clauses' own evaluation cost.  §10's recheck of the dirty pages is
 * still charged beside it by the caller; it is the smaller of the two.
 */
static Cost
lion_cost_recheck(PlannerInfo *root, RelOptInfo *rel, List *whereidx,
				  List *wherecol, List *whereclauses, List *wherekinds,
				  List *ors, double matched, double counts, double ceiling)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		cand = Max(matched, 1.0);
	int		   *orgrp = lion_or_group_map(ors, list_length(whereclauses));
	Cost		perrow = cpu_tuple_cost;
	bool		any = false;
	double		pages;
	int			ci = 0;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	forfour(lc1, whereidx, lc2, whereclauses, lc3, wherekinds, lc4, wherecol)
	{
		Node	   *clause = (Node *) lfirst(lc2);
		bool		inor = (orgrp[ci++] >= 0);
		LionQueryMode mode;
		double		nkeys;
		QualCost	qual_cost;

		/* an OR leaf's query is always a literal (lion_analyze_leaf()) */
		if (lfirst_int(lc3) != LION_CLAUSE_MULTI || inor)
			continue;
		mode = lion_multikey_cost_mode((IndexOptInfo *) lfirst(lc1),
									   (AttrNumber) lfirst_int(lc4), clause,
									   &nkeys);
		if (mode != LION_QMODE_LOSSY && mode != LION_QMODE_ALL)
			continue;

		any = true;
		cost_qual_eval_node(&qual_cost, clause, root);
		perrow += qual_cost.per_tuple;
		if (mode == LION_QMODE_ALL)
			cand /= Max(clause_selectivity(root, clause, 0, JOIN_INNER, NULL),
						1e-10);
	}
	pfree(orgrp);
	if (!any)
		return 0.0;

	cand = Min(cand, Max(ceiling, 1.0));
	pages = Min(cand, heap_pages * Max(counts, 1.0));

	return pages * lion_heap_page_cost(root, rel, pages, heap_pages) +
		cand * perrow;
}

/*
 * The fk column's distinct keys (DESIGN.md §27).  A key's posting set holds
 * every row of the table with that key, not the ones the fact filters leave,
 * so this is the column's n_distinct over the whole table, taken as
 * lion_range_entries() takes it.  estimate_num_groups() scales it down to the
 * rows the relation's own clauses leave, which for an fk of a few rows per key
 * under a filter of a few percent made every key eight times as large as it
 * is (the 2026-09-27 benchmark: 21.9 rows a key where there are 2.75).
 */
static double
lion_fkjoin_fk_ndistinct(PlannerInfo *root, RelOptInfo *rel, Var *fkvar)
{
	VariableStatData vardata;
	bool		isdefault;
	double		nd;

	examine_variable(root, (Node *) fkvar, rel->relid, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	return Max(Min(nd, Max(rel->tuples, 1.0)), 1.0);
}

/*
 * How many participants a parallel plan's rows are divided among: core's
 * get_parallel_divisor() (costsize.c), which is static there.  The leader
 * takes part less the more workers it has to serve.
 */
static double
lion_parallel_divisor(int workers)
{
	double		divisor = workers;

	if (parallel_leader_participation)
	{
		double		leader = 1.0 - (0.3 * workers);

		if (leader > 0)
			divisor += leader;
	}
	return divisor;
}

/*
 * The directory pages of the FK-side join's lookups when the child's rows are
 * looked up in key order (DESIGN.md §27, "Lookups in key order"): `rows` of
 * them, over a key column of `leaves` leaves in a directory `height` levels
 * above them, each row taking `rowbytes` of a batch.  From what the walk does
 * (lion_lookup_walk_find()):
 *
 *	- the rows are sorted in batches of what work_mem holds, and the first key
 *	  of each batch is a descent, height + 1 pages;
 *	- every row is the visit of its key's leaf, one page, and its place in the
 *	  batch, LION_FKJOIN_BATCH_ROW_COST;
 *	- the keys of a batch lie `leaves / keys` leaves apart on average.  While
 *	  that is at most the height, the walk steps over them, a page each; past
 *	  it a key descends, which costs the height of the directory over and
 *	  above its leaf.
 *
 * Against a descent per row, height + 1 pages, the walk is cheaper only where
 * the height is at least two and a batch holds more keys than there are
 * leaves between them: a directory of height 1 descends through the root
 * alone, which a row's place in the batch costs as much as.
 */
static Cost
lion_cost_fkjoin_walk(double rows, double leaves, double height,
					  double rowbytes)
{
	double		perbatch = Max(floor((double) work_mem * 1024.0 /
									 Max(rowbytes, 1.0)), 1.0);
	double		batches = ceil(rows / perbatch);
	double		keys = rows / Max(batches, 1.0);
	double		apart = leaves / Max(keys, 1.0);

	return batches * (height + 1.0) * LION_DESCENT_COST +
		rows * ((1.0 + Min(apart, height)) * LION_DESCENT_COST +
				LION_FKJOIN_BATCH_ROW_COST);
}

/*
 * Cost the FK-side join (DESIGN.md §27) over the fact relation `rel`, for
 * `dimrows` dimension rows, `found` of which have an entry in the fk index;
 * the child plan's own cost is the caller's to add.  A key without an entry
 * costs its descent and nothing more.  Every caller but the forward semi join
 * over a non-unique key takes every row to find one (found = dimrows), which
 * is what an fk into a dimension key is expected to do.
 *
 * What the node does per dimension row: one lookup of the key in the fk
 * index - a directory descent, whose leaf is charged at lion_heap_page_cost()'s
 * interpolated cost over as many distinct leaves as the lookups can touch, or
 * a step of a walk of the leaves in key order where that is cheaper (*walk,
 * lion_cost_fkjoin_walk(); `rowbytes` is what one child row takes of a batch)
 * - then one count of that key's set ANDed with the fact filters: a merge set up
 * and torn down (LION_FKJOIN_COUNT_COST), the set's containers at §10's two
 * cpu_operator_cost each, and the set's chain pages, which are none at all
 * when the fk entries are INLINE.  The rows the node hands up are the
 * caller's to charge (LION_FKJOIN_ROW_COST), which knows how many there are
 * (lion_add_fkjoin_paths()).  A semi or anti join's
 * count is an existence test (`exists`), which reads the share of the set's
 * containers lion_exists_fraction() expects before it finds a visible row.
 *
 * The fact filters are read one of two ways, and *collect says which one is
 * cheaper:
 *
 *	- PROBED per count: every count seeks each filter source at the fk set's
 *	  container keys (LION_FKJOIN_PROBE_COST a probe).  A source that is a
 *	  UNION - an IN list, an OR across columns, a multi-key clause whose query
 *	  is several keys - builds its merge again at every count: each of its
 *	  sets is set up (LION_FKJOIN_SET_COST) and the containers standing at the
 *	  probed keys are merged, lion_merge_ops() of the whole source prorated to
 *	  those keys.  That was the 2026-09-23 review's finding for a thousand-value
 *	  IN list, and the 2026-09-27 benchmark's for a tsquery of four lexemes,
 *	  priced as one set and chosen at half the hash join's cost for a plan
 *	  sixteen times slower;
 *	- or COLLECTED once (lion_sources_collect()): one merge of the filters
 *	  over all of their containers, as a single count of them would make, and
 *	  a private copy of what survives, which every count then seeks with a
 *	  binary search (LION_FKJOIN_COPY_PROBE_COST a probe).  Only when the copy
 *	  is expected to fit in a hash join's memory (get_hash_memory_limit(),
 *	  which is also what the executor gives it) - a container's members at two
 *	  bytes each, a bitset's 4 kB at most - because past it the executor gives
 *	  up and probes.
 *
 * Either way the filters are located once for the whole scan, each one lookup
 * and one walk of its chain, as a single count prices them.
 *
 * And the heap the visibility map cannot vouch for, exactly as §10 prices it:
 * the candidates are the fact rows the dimension rows reach, which is their
 * keys' rows - `rows per fk value` each, and never more than the table - times
 * what the fact filters leave of them, on dirty pages fetched once per query.
 * Nothing here charges the whole fact heap: that is what the node exists not
 * to read.
 */
static Cost
lion_cost_fkjoin_rel(PlannerInfo *root, RelOptInfo *rel, LionCountTarget *t,
					 Var *fkvar, int joinclause, List *whereclauses,
					 List *wherekinds, List *ors, double dimrows,
					 double found, bool exists, double rowbytes,
					 bool *collect, bool *walk)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		tuples = Max(rel->tuples, 1.0);
	double		wheresel = Min(Max(rel->rows, 1.0) / tuples, 1.0);
	IndexOptInfo *fkidx = (IndexOptInfo *) list_nth(t->whereidx, joinclause);
	AttrNumber	fkcol = (AttrNumber) list_nth_int(t->wherecol, joinclause);
	int			nclause = list_length(whereclauses);
	int		   *orgrp = lion_or_group_map(ors, nclause);
	int			nsrc = list_length(ors);
	double	   *srcsets = (double *) palloc0(sizeof(double) * (nclause + nsrc + 1));
	double	   *srcprobesets = (double *) palloc0(sizeof(double) * (nclause + nsrc + 1));
	double	   *srcmembers = (double *) palloc0(sizeof(double) * (nclause + nsrc + 1));
	double		probesets;
	double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		nd;
	double		perkey;
	double		share;
	double		height = 0;
	double		dirpages;
	double		container_pages;
	double		lookups;
	Cost		descents;
	Cost		walked;
	double		cfk;
	double		readshare = 1.0;
	double		probes = 0;
	double		drive = -1.0;
	double		matched;
	double		recheck_tids;
	double		recheck_pages;
	double		filtered;
	double		copyckeys;
	double		copybytes;
	int			npositive = 0;
	Cost		run = 0;
	Cost		probed = 0;
	Cost		collected = 0;
	int			ci = 0;
	int			sno;
	int		   *rangelead;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	dimrows = Max(dimrows, 1.0);
	found = Min(Max(found, 1.0), dimrows);
	*collect = false;
	*walk = false;

	/* How many rows one key of the fk column has, and in how many containers. */
	nd = lion_fkjoin_fk_ndistinct(root, rel, fkvar);
	perkey = tuples / nd;
	cfk = lion_containers_for(heap_pages, perkey);

	/*
	 * An existence test stops at the first container that shows a row the
	 * snapshot sees (§26), and a key's set holds `perkey x wheresel` of those
	 * among its cfk containers.
	 */
	if (exists)
		readshare = lion_exists_fraction(cfk, perkey * wheresel);

	/* ---- one lookup and one count per dimension row ---- */
	share = lion_index_column_share(root, rel, fkidx, fkcol);
	dirpages = lion_index_dir_pages(fkidx, &height);
	container_pages = Max(((double) fkidx->pages - 1.0 - dirpages) *
						  lion_index_column_posting_share(root, rel, fkidx,
														  fkcol), 0.0);
	dirpages = Max(dirpages * share, 1.0);

	lookups = Min(dimrows, dirpages);
	run += lookups * lion_heap_page_cost(root, rel, lookups,
										 Max((double) fkidx->pages, 1.0));

	/*
	 * A descent per row, or the walk in key order when it reads fewer pages
	 * (DESIGN.md §27, "Lookups in key order").  The leaves themselves are
	 * charged above either way, each once: that is what the walk reads, and
	 * what the descents read too while the directory stays in the cache.
	 */
	descents = dimrows * (height + 1.0) * LION_DESCENT_COST;
	walked = lion_cost_fkjoin_walk(dimrows, dirpages, height, rowbytes);
	if (walked < descents)
	{
		*walk = true;
		run += walked;
	}
	else
		run += descents;
	run += Min(found * container_pages / nd, container_pages) * seq_page_cost;

	/* ---- the fact filters: located once, each a source of every count ---- */
	rangelead = lion_rangesrc_leaders(t->whereidx, t->wherecol, wherekinds,
									  ors, nclause);
	forfour(lc1, t->whereidx, lc2, whereclauses, lc3, wherekinds,
			lc4, t->wherecol)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Node	   *clause = (Node *) lfirst(lc2);
		int			kind = lfirst_int(lc3);
		Selectivity sel;
		double		nkeys = 1.0;
		double		cshare;
		double		cdir;
		double		cheight = 0;
		double		cpages;

		if (ci == joinclause || !LION_CLAUSE_IS_POSITIVE(kind))
		{
			ci++;
			continue;
		}

		/*
		 * A range taken as a source (DESIGN.md §32): collected once, by the
		 * first of its bounds, and read from memory by every count - or walked
		 * by each, when it is too large to collect (lion_cost_range_source()).
		 */
		if (kind == LION_CLAUSE_RANGESRC)
		{
			List	   *bounds = NIL;
			int			j;

			if (rangelead[ci] != ci)
			{
				ci++;
				continue;
			}
			for (j = ci; j < nclause; j++)
				if (rangelead[j] == ci)
					bounds = lappend(bounds, list_nth(whereclauses, j));
			sel = clauselist_selectivity(root, bounds, 0, JOIN_INNER, NULL);
			list_free(bounds);

			sno = (orgrp[ci] >= 0) ? orgrp[ci] : nsrc + ci;
			srcsets[sno] += 1.0;
			srcmembers[sno] += tuples * sel;
			run += lion_cost_range_source(root, rel, idx,
										  (AttrNumber) lfirst_int(lc4), sel,
										  dimrows, orgrp[ci] >= 0);
			ci++;
			continue;
		}

		sel = clause_selectivity(root, clause, 0, JOIN_INNER, NULL);
		probesets = 0.0;
		if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			Node	   *array = lion_strip((Node *) lsecond(saop->args));

#if PG_VERSION_NUM >= 170000
			nkeys = Max(estimate_array_length(root,
											  (Node *) lsecond(saop->args)),
						1.0);
#else
			nkeys = Max(estimate_array_length((Node *) lsecond(saop->args)),
						1.0);
#endif

			/*
			 * An array whose length the planner cannot see - a parameter; an
			 * expression comes here as its plan-time estimate
			 * (lion_analyze_leaf()) - is located and collected once for
			 * whatever it turns out to hold, and priced once at
			 * estimate_array_length()'s guess.  But a count that PROBES the
			 * filters builds its union again per dimension row, so there it
			 * is priced as the longest list a literal may be: underpricing a
			 * scan's one-off work costs a little, underpricing a dimension
			 * row's costs that many times over (the 2026-09-23 review's
			 * 43,000 values over 20,000 dimension rows ran for minutes).
			 */
			if (array == NULL ||
				(!IsA(array, Const) && !IsA(array, ArrayExpr)))
				probesets = Max(nkeys, (double) LION_MAX_ARRAY_ELEMS);
		}
		else if (kind == LION_CLAUSE_MULTI)
			nkeys = lion_multikey_nkeys(idx, (AttrNumber) lfirst_int(lc4),
										clause);
		if (probesets <= 0.0)
			probesets = nkeys;

		/*
		 * Which source of the AND the clause's sets belong to: its OR's, or
		 * one of its own (numbered after the ORs).  A multi-key clause's sets
		 * are the lexemes or elements its query combines, each of them at
		 * least as frequent as the clause itself where they are ORed: they
		 * are priced as nkeys sets holding the clause's rows nkeys times
		 * over, which is a floor.
		 */
		sno = (orgrp[ci] >= 0) ? orgrp[ci] : nsrc + ci;
		srcsets[sno] += nkeys;
		srcprobesets[sno] += probesets;
		srcmembers[sno] += tuples * ((kind == LION_CLAUSE_MULTI) ?
									 Min(sel * nkeys, 1.0) : sel);
		ci++;

		/* Located once: a lookup per set, and the walk of their chains. */
		cshare = lion_index_column_share(root, rel, idx,
										 (AttrNumber) lfirst_int(lc4));
		cdir = lion_index_dir_pages(idx, &cheight);
		cpages = Max(((double) idx->pages - 1.0 - cdir) *
					 lion_index_column_posting_share(root, rel, idx,
													 (AttrNumber) lfirst_int(lc4)),
					 0.0);

		run += Min(nkeys, Max(cdir * cshare, 1.0)) * random_page_cost +
			nkeys * (cheight + 1.0) * LION_DESCENT_COST;
		run += Max(Min(nkeys, cpages), cpages * sel) * seq_page_cost;
	}

	/*
	 * PROBED: every count seeks each source at the container keys the fk set
	 * has, and a seek finds a container only where the source has one.  A
	 * union source builds its k-way union again at every count.
	 *
	 * COLLECTED: the same sources merged once over all of their containers -
	 * driven by the sparsest, the others probed at its keys, a union's merge
	 * built in full once - and the survivors copied.
	 */
	for (sno = 0; sno < nclause + nsrc; sno++)
	{
		double		call;
		double		cs;

		if (srcsets[sno] <= 0.0)
			continue;
		npositive++;
		call = lion_containers_for(heap_pages, srcmembers[sno]);
		cs = Min(cfk, call);
		probes += cs;
		if (srcprobesets[sno] > 1.0)
			probed += found *
				(srcprobesets[sno] * LION_FKJOIN_SET_COST +
				 lion_merge_ops(heap_pages, srcmembers[sno], srcprobesets[sno]) *
				 Min(cs / ckeys, 1.0) * readshare * cpu_operator_cost);
		if (srcsets[sno] > 1.0)
			collected += srcsets[sno] * LION_FKJOIN_SET_COST +
				lion_merge_ops(heap_pages, srcmembers[sno], srcsets[sno]) *
				cpu_operator_cost;
		if (drive < 0.0 || call < drive)
			drive = call;
	}
	pfree(orgrp);
	pfree(rangelead);
	pfree(srcsets);
	pfree(srcprobesets);
	pfree(srcmembers);

	probed += found * probes * readshare * LION_FKJOIN_PROBE_COST;

	/*
	 * What the collected copy holds and how large it is: the filters' rows, in
	 * as many containers as those rows can occupy, each at most a bitset.
	 */
	filtered = tuples * wheresel;
	copyckeys = lion_containers_for(heap_pages, filtered);
	copybytes = copyckeys * (LION_CONTAINER_HDRSZ + sizeof(LionContainer *) +
							 Min(2.0 * filtered / copyckeys,
								 (double) LION_BITSET_BYTES));
	probed += found * LION_FKJOIN_COUNT_COST;
	if (npositive > 0)
	{
		collected += drive * 2.0 * cpu_operator_cost +
			drive * (npositive - 1) * LION_FKJOIN_PROBE_COST +
			copyckeys * LION_FKJOIN_COPY_CONTAINER_COST;
		collected += found * (LION_FKJOIN_COPY_COUNT_COST +
							  Min(cfk, copyckeys) * readshare *
							  (LION_FKJOIN_COPY_PROBE_COST +
							   perkey / cfk * LION_FKJOIN_COPY_MEMBER_COST));

		/*
		 * A hot standby never makes the copy (lion_join_collect()), so there
		 * every count probes, and is priced so (2026-09-28 review): a plan
		 * made there as if the copy would be made chose the node where each
		 * dimension row then built the filters' unions again.
		 */
		if (copybytes <= (double) get_hash_memory_limit() &&
			collected < probed && !RecoveryInProgress())
			*collect = true;
	}
	run += *collect ? collected : probed;

	/*
	 * The fk set's containers at every count, read and counted as any count's
	 * are (lion_merge_cpu_cost(): LION_CONTAINER_COST, and LION_MEMBER_COST a
	 * member of them).
	 */
	run += found * cfk * readshare *
		(LION_CONTAINER_COST + LION_MEMBER_COST * Min(perkey / cfk,
													   LION_MEMBER_CAP));

	/* ---- the heap the visibility map cannot vouch for ---- */
	matched = Min(found * perkey, tuples) * wheresel * readshare;
	recheck_tids = matched * dirtyfrac;
	recheck_pages = Min(recheck_tids, heap_pages * dirtyfrac);
	run += recheck_pages * lion_heap_page_cost(root, rel, recheck_pages,
											   heap_pages);
	run += recheck_tids * LION_RECHECK_TID_COST;

	return run;
}

/*
 * The walker of lion_is_value_expr(): true at anything that makes an
 * expression something other than one value per scan - a column of this
 * query level or of any other, a subquery, an aggregate or a window function
 * - or that the node's ExprContext has nothing to evaluate with (a Param
 * other than a statement's own or an exec one).
 */
static bool
lion_not_value_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) || IsA(node, PlaceHolderVar))
		return true;
	if (IsA(node, Param))
	{
		Param	   *p = (Param *) node;

		return (p->paramkind != PARAM_EXTERN && p->paramkind != PARAM_EXEC);
	}
	if (IsA(node, SubLink) || IsA(node, SubPlan) ||
		IsA(node, AlternativeSubPlan))
		return true;
	if (IsA(node, Aggref) || IsA(node, WindowFunc) ||
		IsA(node, GroupingFunc))
		return true;
	if (IsA(node, CurrentOfExpr))
		return true;
	return expression_tree_walker(node, lion_not_value_walker, context);
}

/*
 * Is this expression a value the node can compare a column with - a literal,
 * or an expression it evaluates once at the start of the scan (DESIGN.md
 * §10)?
 *
 * A Param is accepted wherever a Const is, which is what lets a prepared
 * statement's GENERIC plan reach the pushdown: the planner leaves `k = $1` as
 * a Param, the cost model uses its default selectivity, and the executor
 * evaluates it through the node's own ExprContext.  Both parameter kinds
 * qualify: PARAM_EXTERN for a prepared statement's own parameters and
 * PARAM_EXEC for the ones a nested loop or a LATERAL reference supplies,
 * which change between rescans.
 *
 * So is any other expression that the executor would take as an index scan's
 * RUN-TIME key (ExecIndexBuildScanKeys()): no Var of any level, no volatile
 * function, no subquery, no aggregate or window function.  That is what a
 * time window is written as - `ts >= now() - interval '90 days'`, `d >=
 * current_date - 30`, `d = $1::date + 1` in a generic plan - and none of it
 * is a Const or a Param: eval_const_expressions() folds only immutable
 * functions, and now(), current_date and timestamptz arithmetic (which
 * depends on the time zone) are stable.  A stable expression has one value
 * throughout a statement, so evaluating it once per scan, beside the Params
 * (lion_eval_clause_values()), means what evaluating it per row means; a
 * volatile one does not (`k = random()`), and is declined.  At plan time its
 * value is only estimated, which clause_selectivity() does itself through
 * estimate_expression_value(); a cached generic plan evaluates it anew at
 * every execution.
 *
 * An ArrayExpr is accepted for the array of an IN list, because that is the
 * shape `k IN ($1, $2)` keeps in a generic plan, when each of its elements is
 * a value by the same rule: the node evaluates the array ONCE per scan.
 */
static bool
lion_is_value_expr(Node *node, bool allow_array_expr)
{
	if (node == NULL)
		return false;
	if (IsA(node, Const))
		return true;
	if (IsA(node, ArrayExpr))
	{
		ArrayExpr  *a = (ArrayExpr *) node;
		ListCell   *lc;

		if (!allow_array_expr || a->multidims || a->elements == NIL)
			return false;
		foreach(lc, a->elements)
		{
			if (!lion_is_value_expr(lion_strip((Node *) lfirst(lc)), false))
				return false;
		}
		return true;
	}
	if (expression_returns_set(node) || lion_not_value_walker(node, NULL))
		return false;
	return !contain_volatile_functions(node);
}

/*
 * Number of elements of a Const array, or -1 when it is not a plain array.
 */
static int
lion_array_const_nelems(Const *con)
{
	ArrayType  *arr;
	int			nelems;

	if (con->constisnull)
		return -1;
	if (!OidIsValid(get_element_type(con->consttype)))
		return -1;

	arr = DatumGetArrayTypeP(con->constvalue);
	if (ARR_NDIM(arr) == 0)
		nelems = 0;
	else if (ARR_NDIM(arr) != 1)
		nelems = -1;
	else
		nelems = ARR_DIMS(arr)[0];

	if ((Pointer) arr != DatumGetPointer(con->constvalue))
		pfree(arr);

	return nelems;
}

/*
 * Is this clause a test of a boolean column by itself, which core's
 * match_boolean_index_clause() hands an index scan as `col = true` or `col =
 * false`?  `col`, `NOT col`, `col IS TRUE` and `col IS FALSE` are, and set
 * *var and *value; eval_const_expressions() has already folded `col = true`
 * into the first and `col = false` and `col <> true` into the second, so no
 * boolean equality ever reaches the pushdown as an OpExpr.  All four are
 * false for NULL, as `=` is.  A domain over boolean is its base type here as
 * everywhere: the WHERE clause relabels it to boolean, and bool_ops answers.
 */
static bool
lion_boolean_eq_test(Node *clause, Var **var, bool *value)
{
	Node	   *arg = clause;
	bool		val = true;

	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) clause;

		if (b->boolop != NOT_EXPR || list_length(b->args) != 1)
			return false;
		arg = (Node *) linitial(b->args);
		val = false;
	}
	else if (IsA(clause, BooleanTest))
	{
		BooleanTest *bt = (BooleanTest *) clause;

		if (bt->booltesttype == IS_TRUE)
			val = true;
		else if (bt->booltesttype == IS_FALSE)
			val = false;
		else
			return false;
		arg = (Node *) bt->arg;
	}

	arg = lion_strip(arg);
	if (arg == NULL || !IsA(arg, Var) ||
		getBaseType(((Var *) arg)->vartype) != BOOLOID)
		return false;

	*var = (Var *) arg;
	*value = val;
	return true;
}

/*
 * `col IS NOT TRUE` and `col IS NOT FALSE` of a boolean column hold for the
 * NULL rows as well, so they are no equality: they are the OR of two clauses
 * the posting sets answer, `col = false OR col IS NULL` and `col = true OR
 * col IS NULL`, and that OR is returned for the machinery of DESIGN.md §19 to
 * take apart like any other.  NULL for every other clause.
 */
static BoolExpr *
lion_boolean_not_test(Node *clause)
{
	BooleanTest *bt;
	Node	   *arg;
	NullTest   *nt;
	Expr	   *eq;

	if (clause == NULL || !IsA(clause, BooleanTest))
		return NULL;
	bt = (BooleanTest *) clause;
	if (bt->booltesttype != IS_NOT_TRUE && bt->booltesttype != IS_NOT_FALSE)
		return NULL;
	arg = lion_strip((Node *) bt->arg);
	if (arg == NULL || !IsA(arg, Var) ||
		getBaseType(((Var *) arg)->vartype) != BOOLOID)
		return NULL;

	eq = make_opclause(BooleanEqualOperator, BOOLOID, false, (Expr *) arg,
					   (Expr *) makeBoolConst(bt->booltesttype == IS_NOT_FALSE,
											  false),
					   InvalidOid, InvalidOid);
	nt = makeNode(NullTest);
	nt->arg = (Expr *) arg;
	nt->nulltesttype = IS_NULL;
	nt->argisrow = false;
	nt->location = -1;

	return (BoolExpr *) makeBoolExpr(OR_EXPR, list_make2(eq, nt), -1);
}

/*
 * The walker of lion_or_arms(): node in disjunctive normal form, as a list of
 * arms that are each the list of their leaves, with the number of leaves of
 * them all in *nleaves; NIL once that number would pass LION_MAX_ARRAY_ELEMS.
 */
static List *
lion_or_dnf(Node *node, int *nleaves)
{
	BoolExpr   *orform = lion_boolean_not_test(node);
	List	   *result;
	double		total;
	ListCell   *lc;

	check_stack_depth();

	if (orform != NULL)
		node = (Node *) orform;

	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == OR_EXPR)
	{
		result = NIL;
		total = 0;
		foreach(lc, ((BoolExpr *) node)->args)
		{
			int			n;
			List	   *sub = lion_or_dnf((Node *) lfirst(lc), &n);

			if (sub == NIL)
				return NIL;
			total += n;
			if (total > LION_MAX_ARRAY_ELEMS)
				return NIL;
			result = list_concat(result, sub);
		}
		*nleaves = (int) total;
		return result;
	}

	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == AND_EXPR)
	{
		/* one empty arm, which the first term's arms extend */
		result = list_make1(NIL);
		total = 0;
		foreach(lc, ((BoolExpr *) node)->args)
		{
			int			n;
			List	   *sub = lion_or_dnf((Node *) lfirst(lc), &n);
			List	   *product = NIL;
			ListCell   *la;
			ListCell   *lb;

			if (sub == NIL)
				return NIL;

			/* every arm so far ANDed with every arm of this term */
			total = total * list_length(sub) + (double) n * list_length(result);
			if (total > LION_MAX_ARRAY_ELEMS)
				return NIL;
			foreach(la, result)
			{
				foreach(lb, sub)
					product = lappend(product,
									  list_concat_copy((List *) lfirst(la),
													   (List *) lfirst(lb)));
			}
			result = product;
		}
		*nleaves = (int) total;
		return result;
	}

	/* a leaf, for lion_analyze_leaf() to accept or decline */
	*nleaves = 1;
	return list_make1(list_make1(node));
}

/*
 * An OR restriction as the arms of one union (DESIGN.md §19): a list of arms,
 * each the list of the clauses it ANDs, or NIL when it is too big to count.
 *
 * The OR machinery takes an OR of ANDs of leaves, one level deep, because
 * that is what a source's tree is built from (lion_locate_or()).  Anything
 * nested deeper is DISTRIBUTED into that shape: `(a AND (b OR c)) OR d` is
 * `(a AND b) OR (a AND c) OR d`, and `(a AND flag IS NOT TRUE) OR d` - whose
 * `flag IS NOT TRUE` is `flag = false OR flag IS NULL` (lion_boolean_not_test())
 * and used to decline the whole query - is `(a AND flag = false) OR (a AND
 * flag IS NULL) OR d`.  The union of the arms is the same set of rows.
 *
 * Distributing repeats a term in every arm it is distributed into, and each
 * repetition is a leaf of its own: its own lookup, priced as such, and its own
 * posting set in the union.  An AND of k two-way ORs is 2^k arms of k leaves.
 * So the leaves of the result are bounded by the number an IN list's sets are
 * (LION_MAX_ARRAY_ELEMS, DESIGN.md §15), for the same reason: every set of the
 * union may hold a buffer pin for as long as the node runs (§9), and an OR's
 * leaves are never materialized (§19).  Past it the query is declined.
 */
static List *
lion_or_arms(Node *clause)
{
	int			nleaves;

	return lion_or_dnf(clause, &nleaves);
}

/*
 * One WHERE clause as the analysis below understands it: which column it
 * constrains, with what, and everything lion_match_index() will need in order
 * to find an index for it on each relation.
 */
typedef struct LionLeafInfo
{
	Var		   *var;
	Node	   *val;			/* the value expression, or a NULL placeholder */
	Node	   *costclause;		/* the clause as the cost model is to see it,
								 * or NULL for the query's own: `col = true`
								 * for a bare boolean column, an IN list with
								 * its array estimated */
	Oid			opno;			/* 0 for a null test */
	Oid			cmptype;		/* the type the column is compared with */
	StrategyNumber strategy;	/* multi-key clauses only */
	Oid			extractquery;	/* multi-key clauses only */
	Oid			collation;		/* clause input collation, or none */
	int			kind;			/* LION_CLAUSE_* */
} LionLeafInfo;

/*
 * Is this clause one the posting sets can answer, and on a plain column of
 * rti?  Fills *out and returns true, or returns false and leaves the caller
 * to decline the whole query.
 *
 * allow_negated says whether `col IS NOT NULL` - the one clause kind that
 * subtracts rather than selects (DESIGN.md §14) - is acceptable here.  Under
 * an OR it is not: the union of the arms would have to be the union of one
 * arm's complement with the others', and the complement of a posting set is
 * not a posting set (DESIGN.md §19).
 *
 * allow_range says the same of a range comparison (DESIGN.md §28), which
 * bounds the entry walk that drives the count or, on another column, is a
 * range taken as a source (§32).  Every caller allows it now - an OR's arm
 * takes a range as a source, the rows the union of its sets holds - and it is
 * the caller that decides which of the two a range is.
 *
 * allow_recheck says the same of a multi-key clause whose query the node only
 * has at run time (DESIGN.md §17, "A query known only at run time"), which may
 * turn out to need every candidate rechecked in the heap.  The recheck tests
 * the clause alone, which is right for a clause the other sources are ANDed
 * with; a leaf of an OR is not tested alone - the row passes when ANY arm
 * holds, and the other arms' leaves are answered from posting sets and never
 * evaluated - so under an OR the query has to be a literal the posting sets
 * answer exactly, as before.
 */
static bool
lion_analyze_leaf(PlannerInfo *root, Node *clause, Index rti,
				 bool allow_negated, bool allow_range, bool allow_recheck,
				 LionLeafInfo *out)
{
	Var		   *boolvar;
	bool		boolval;

	memset(out, 0, sizeof(LionLeafInfo));
	out->opno = InvalidOid;
	out->cmptype = InvalidOid;
	out->extractquery = InvalidOid;
	out->collation = InvalidOid;

	if (clause == NULL)
		return false;

	if (lion_boolean_eq_test(clause, &boolvar, &boolval))
	{
		/*
		 * A boolean column by itself (lion_boolean_eq_test()): the equality
		 * `col = true` or `col = false` it stands for, with bool_ops' own
		 * strategy 1.  The Const is the clause's value like any literal's, so
		 * EXPLAIN prints `flag = true` as core prints the index condition,
		 * and the cost model is handed that OpExpr rather than the bare
		 * column, so that everything it asks of an equality is asked of
		 * this one.
		 */
		out->var = boolvar;
		out->val = (Node *) makeBoolConst(boolval, false);
		out->opno = BooleanEqualOperator;
		out->cmptype = BOOLOID;
		out->strategy = LION_STRAT_EQUAL;
		out->kind = LION_CLAUSE_EQ;
		out->costclause = (Node *) make_opclause(BooleanEqualOperator, BOOLOID,
												 false, (Expr *) boolvar,
												 (Expr *) out->val,
												 InvalidOid, InvalidOid);
	}
	else if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *left;
		Node	   *right;
		Oid			opfamily;
		Oid			lefttype;

		if (list_length(op->args) != 2)
			return false;
		if (!op_strict(op->opno))
			return false;

		left = lion_strip((Node *) linitial(op->args));
		right = lion_strip((Node *) lsecond(op->args));
		if (left == NULL || right == NULL)
			return false;

		out->strategy = lion_op_roaring_strategy(op->opno, &opfamily, &lefttype);

		if (out->strategy == LION_STRAT_EQUAL)
		{
			/* Equality commutes, so either side may hold the column. */
			if (IsA(left, Var) && lion_is_value_expr(right, false))
			{
				out->var = (Var *) left;
				out->val = right;
			}
			else if (lion_is_value_expr(left, false) && IsA(right, Var))
			{
				out->var = (Var *) right;
				out->val = left;
			}
			else
				return false;

			/*
			 * A literal NULL equals nothing.  A parameter that turns out to be
			 * NULL is the same answer, but only the executor can see it, so it
			 * selects no rows there instead.
			 */
			if (IsA(out->val, Const) && ((Const *) out->val)->constisnull)
				return false;

			out->opno = op->opno;
			out->cmptype = exprType(out->val);
			out->kind = LION_CLAUSE_EQ;
		}
		else if (LION_STRAT_IS_RANGE(out->strategy))
		{
			Oid			rangeop = op->opno;

			/*
			 * A range comparison (DESIGN.md §28).  The column may be on either
			 * side: `5 < k` is `k > 5`, and the commutator is what the index's
			 * opfamily has to know - a family that has `<(int8, int4)` but not
			 * `>(int4, int8)` cannot answer it, and is not asked to.
			 */
			if (!allow_range)
				return false;
			if (IsA(left, Var) && lion_is_value_expr(right, false))
			{
				out->var = (Var *) left;
				out->val = right;
			}
			else if (IsA(right, Var) && lion_is_value_expr(left, false))
			{
				rangeop = get_commutator(op->opno);
				if (!OidIsValid(rangeop))
					return false;
				out->var = (Var *) right;
				out->val = left;
				out->strategy = lion_op_roaring_strategy(rangeop, &opfamily,
														 &lefttype);
			}
			else
				return false;
			if (!LION_STRAT_IS_RANGE(out->strategy) || !op_strict(rangeop))
				return false;

			/* A literal NULL bound compares with nothing, as for equality. */
			if (IsA(out->val, Const) && ((Const *) out->val)->constisnull)
				return false;

			out->opno = rangeop;
			out->cmptype = exprType(out->val);
			out->kind = LION_CLAUSE_RANGE;
		}
		else if (out->strategy == LION_STRAT_CONTAINS ||
				 out->strategy == LION_STRAT_OVERLAP ||
				 out->strategy == LION_STRAT_MATCH)
		{
			/*
			 * A multi-key operator (DESIGN.md §17).  Unlike equality it does
			 * not commute - `'{a}' @> tags` is a containment the other way
			 * round, which is strategy 4 and not pushed down - so the column
			 * has to be the left operand.
			 */
			if (!IsA(left, Var))
				return false;
			out->var = (Var *) left;
			out->val = right;

			if (IsA(right, Const))
			{
				if (((Const *) out->val)->constisnull)
					return false;

				/*
				 * A literal query's SHAPE is known now, so only an EXACT one
				 * is pushed down.  `tags @> '{}'`, a tsquery with
				 * NOT/phrase/prefix/weights and anything with a NULL element
				 * want rows rechecked in the heap, which is what the ordinary
				 * plan does anyway.
				 */
				if (!lion_multikey_query_is_exact(opfamily, lefttype,
												 out->strategy,
												 (Const *) out->val,
												 &out->extractquery))
					return false;
			}
			else
			{
				Node	   *est;

				/*
				 * A query the node only has at run time: a generic plan's
				 * `tags @> $1`, `tsv @@ to_tsquery(current_setting(...))`
				 * (DESIGN.md §17, "A query known only at run time").  Its
				 * shape decides whether the posting sets answer it exactly,
				 * and the shape is not known until the executor has the value
				 * - `$1 = '{}'` is every row, a phrase is its lexemes' rows
				 * and more - so the node cannot decline the ones they do not
				 * answer.  It answers them from a SUPERSET instead and
				 * rechecks every candidate in the heap
				 * (lion_extract_query_superset(), LionRowFilter), which is
				 * exact for every value the clause can take.  A value the
				 * executor evaluates once per scan qualifies, by the rule
				 * equality follows (lion_is_value_expr()), and so does the
				 * `ARRAY[$1, $2]` a generic plan keeps for an array written
				 * out of parameters.
				 */
				if (!allow_recheck || !lion_is_value_expr(right, true))
					return false;
				out->extractquery = get_opfamily_proc(opfamily, lefttype,
													  lefttype,
													  LION_EXTRACTQUERY_PROC);
				if (!OidIsValid(out->extractquery))
					return false;

				/*
				 * The cost model extracts the plan-time estimate of the value
				 * the way the executor will extract the value itself
				 * (lion_multikey_cost_mode()), so it is handed the clause
				 * over that estimate; a value without one - a Param - is
				 * priced as the expensive shape.  The executor evaluates the
				 * expression, never the estimate.
				 */
				est = estimate_expression_value(root, right);
				if (IsA(est, Const))
					out->costclause = (Node *)
						make_opclause(op->opno, op->opresulttype,
									  op->opretset,
									  (Expr *) linitial(op->args),
									  (Expr *) est,
									  op->opcollid, op->inputcollid);
			}

			out->opno = op->opno;
			out->cmptype = InvalidOid;	/* the query is not a key */
			out->kind = LION_CLAUSE_MULTI;
		}
		else
			return false;		/* strategy 4 (`<@`), or not ours at all */

		out->collation = op->inputcollid;
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *left;
		Node	   *right;
		Oid			opfamily;
		Oid			lefttype;
		int			nelems;

		/* `= ALL (...)` is not a union of keys (DESIGN.md §15). */
		if (!saop->useOr)
			return false;
		if (list_length(saop->args) != 2)
			return false;
		if (!op_strict(saop->opno))
			return false;

		left = lion_strip((Node *) linitial(saop->args));
		right = lion_strip((Node *) lsecond(saop->args));
		if (left == NULL || right == NULL)
			return false;
		if (!IsA(left, Var) || !lion_is_value_expr(right, true))
			return false;
		out->var = (Var *) left;
		out->val = right;

		/*
		 * The length cap of DESIGN.md §15 applies to the lists whose length is
		 * known now: a literal array and the ARRAY[...] a generic plan keeps
		 * for `k IN ($1, $2)`.  A parameter that IS an array has no length
		 * until the executor has it, and by then there is no plan to decline
		 * in favour of, so it is answered whatever its length.
		 *
		 * Any other array expression - a stable function's, `k = ANY
		 * (string_to_array(current_setting(...), ','))` - is estimated as
		 * core's selectivity functions estimate it, and when that gives a
		 * literal the cap is applied to it and the cost model is handed the
		 * clause over it, so that it prices the list's real length rather
		 * than estimate_array_length()'s guess for an expression.  The
		 * executor still evaluates the expression itself (the estimate is
		 * the plan-time value, which a stable function need not keep).
		 */
		if (IsA(out->val, Const))
		{
			if (((Const *) out->val)->constisnull)
				return false;
			nelems = lion_array_const_nelems((Const *) out->val);
			if (nelems < 0 || nelems > LION_MAX_ARRAY_ELEMS)
				return false;
		}
		else if (IsA(out->val, ArrayExpr))
		{
			nelems = list_length(((ArrayExpr *) out->val)->elements);
			if (nelems > LION_MAX_ARRAY_ELEMS)
				return false;
		}
		else if (!IsA(out->val, Param))
		{
			Node	   *est = estimate_expression_value(root, out->val);

			if (IsA(est, Const) && !((Const *) est)->constisnull)
			{
				ScalarArrayOpExpr *costsaop;

				nelems = lion_array_const_nelems((Const *) est);
				if (nelems > LION_MAX_ARRAY_ELEMS)
					return false;

				costsaop = (ScalarArrayOpExpr *) copyObject(saop);
				costsaop->args = list_make2(linitial(costsaop->args), est);
				out->costclause = (Node *) costsaop;
			}
		}

		/*
		 * `col op ANY (array)` is a union of single-key lookups, so the
		 * operator has to be equality; `tags @> ANY (...)` would be a union of
		 * multi-key queries, which nothing here builds.
		 */
		if (lion_op_roaring_strategy(saop->opno, &opfamily,
									&lefttype) != LION_STRAT_EQUAL)
			return false;

		out->opno = saop->opno;
		out->cmptype = get_element_type(exprType(out->val));
		if (!OidIsValid(out->cmptype))
			return false;
		out->kind = LION_CLAUSE_ARRAY;
		out->collation = saop->inputcollid;
	}
	else if (IsA(clause, NullTest) ||
			 (IsA(clause, BooleanTest) &&
			  (((BooleanTest *) clause)->booltesttype == IS_UNKNOWN ||
			   ((BooleanTest *) clause)->booltesttype == IS_NOT_UNKNOWN)))
	{
		bool		isnull;
		Node	   *arg;

		/* `flag IS UNKNOWN` is `flag IS NULL`, and its negation likewise. */
		if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;

			if (nt->argisrow)
				return false;
			isnull = (nt->nulltesttype == IS_NULL);
			arg = (Node *) nt->arg;
		}
		else
		{
			isnull = (((BooleanTest *) clause)->booltesttype == IS_UNKNOWN);
			arg = (Node *) ((BooleanTest *) clause)->arg;
		}
		if (!isnull && !allow_negated)
			return false;
		arg = lion_strip(arg);
		if (arg == NULL || !IsA(arg, Var))
			return false;
		out->var = (Var *) arg;

		out->kind = isnull ? LION_CLAUSE_NULL : LION_CLAUSE_NOTNULL;
		/* The executor needs no value; keep the lists in step. */
		out->val = (Node *) makeNullConst(out->var->vartype,
										  out->var->vartypmod,
										  out->var->varcollid);
	}
	else
		return false;

	if (out->var->varno != (int) rti || out->var->varattno <= 0 ||
		out->var->varlevelsup != 0)
		return false;

	return true;
}

/*
 * Append one analysed clause to the parallel lists the planner carries.  The
 * flattened clause array holds the leaves of an OR restriction alongside the
 * plain clauses (DESIGN.md §19), so everything that follows - matching an
 * index per relation, pricing the lookup, moving a Param into custom_exprs -
 * treats them alike; inor says which are which.  The clause the cost model
 * is given is the leaf's own costclause when the analysis made one (a bare
 * boolean column's `col = true`), the query's otherwise.
 */
static void
lion_append_clause(const LionLeafInfo *leaf, Node *clause, bool inor,
				  List **whereattnos, List **clauseinfos, List **whereclauses,
				  List **whereconsts, List **wherekinds, List **whereopnos,
				  List **whereinor)
{
	LionClauseInfo *ci = (LionClauseInfo *) palloc0(sizeof(LionClauseInfo));

	ci->attno = leaf->var->varattno;
	ci->kind = leaf->kind;
	ci->opno = leaf->opno;
	ci->cmptype = leaf->cmptype;
	ci->strategy = leaf->strategy;
	ci->extractquery = leaf->extractquery;
	ci->collation = leaf->collation;

	*whereattnos = lappend_int(*whereattnos, (int) leaf->var->varattno);
	*clauseinfos = lappend(*clauseinfos, ci);
	*whereclauses = lappend(*whereclauses,
							leaf->costclause != NULL ? leaf->costclause : clause);
	*whereconsts = lappend(*whereconsts, leaf->val);
	*wherekinds = lappend_int(*wherekinds, leaf->kind);
	*whereopnos = lappend_oid(*whereopnos, leaf->opno);
	*whereinor = lappend_int(*whereinor, inor ? 1 : 0);
}

/*
 * Which clauses are leaves of an OR restriction (DESIGN.md §19)?  Decoded
 * from LION_PRIV_ORS, whose lists name a contiguous run of clauses each.
 */
static bool *
lion_or_leaf_map(List *ors, int nclause)
{
	bool	   *map = (bool *) palloc0(sizeof(bool) * Max(nclause, 1));
	ListCell   *lc;

	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			nleaves = 0;
		int			i;

		for (i = 0; i < narms; i++)
			nleaves += list_nth_int(one, 2 + i);
		for (i = first; i < first + nleaves && i < nclause; i++)
			map[i] = true;
	}

	return map;
}

/*
 * The same map, but naming WHICH OR restriction each clause is a leaf of (-1
 * for a clause that is not one).  The cost model needs the identity and not
 * just the fact: a union is ONE source of the AND (DESIGN.md §19), and which
 * source a clause belongs to is what decides whether it is walked or sought
 * (DESIGN.md §22).
 */
static int *
lion_or_group_map(List *ors, int nclause)
{
	int		   *map = (int *) palloc(sizeof(int) * Max(nclause, 1));
	int			group = 0;
	int			i;
	ListCell   *lc;

	for (i = 0; i < Max(nclause, 1); i++)
		map[i] = -1;

	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			nleaves = 0;

		for (i = 0; i < narms; i++)
			nleaves += list_nth_int(one, 2 + i);
		for (i = first; i < first + nleaves && i < nclause; i++)
			map[i] = group;
		group++;
	}

	return map;
}

/*
 * The ARM each clause is a leaf of, numbered across every OR restriction (-1
 * for a clause that is not an OR's leaf): the bounds of one range in one arm
 * are one source (DESIGN.md §32), and the same column's bounds in two arms
 * are two.
 */
static int *
lion_or_arm_map(List *ors, int nclause)
{
	int		   *map = (int *) palloc(sizeof(int) * Max(nclause, 1));
	int			arm = 0;
	int			i;
	ListCell   *lc;

	for (i = 0; i < Max(nclause, 1); i++)
		map[i] = -1;

	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			leaf = linitial_int(one);
		int			narms = lsecond_int(one);
		int			a;

		for (a = 0; a < narms; a++, arm++)
		{
			int			len = list_nth_int(one, 2 + a);

			for (i = 0; i < len; i++, leaf++)
				if (leaf < nclause)
					map[leaf] = arm;
		}
	}

	return map;
}

/*
 * For each clause, the clause that stands for its range when it is a bound of
 * a range taken as a source (DESIGN.md §32): the first RANGESRC clause on the
 * same key column of the same index in the same conjunction - the top level,
 * or one OR arm.  -1 for every other clause.  The executor groups them the
 * same way (lion_begin_custom_scan(), lion_locate_or()).
 */
static int *
lion_rangesrc_leaders(List *whereidx, List *wherecol, List *wherekinds,
					  List *ors, int nclause)
{
	int		   *lead = (int *) palloc(sizeof(int) * Max(nclause, 1));
	int		   *armof = lion_or_arm_map(ors, nclause);
	int			i;
	int			j;

	for (i = 0; i < nclause; i++)
	{
		lead[i] = -1;
		if (list_nth_int(wherekinds, i) != LION_CLAUSE_RANGESRC)
			continue;
		for (j = 0; j <= i; j++)
		{
			if (list_nth_int(wherekinds, j) == LION_CLAUSE_RANGESRC &&
				armof[j] == armof[i] &&
				list_nth(whereidx, j) == list_nth(whereidx, i) &&
				list_nth_int(wherecol, j) == list_nth_int(wherecol, i))
			{
				lead[i] = j;
				break;
			}
		}
	}
	pfree(armof);
	return lead;
}

/*
 * The partially-grouped PathTarget a partitioned GROUP BY node produces
 * (DESIGN.md §16): the grouping column(s) unchanged, plus every aggregate
 * marked as the INITIAL phase of a split aggregate.  For count(*) and
 * count(col) the transition type is int8 and there is nothing to serialize,
 * so a partial row is just (group key, int8 partial count).
 *
 * This is make_partial_grouping_target() (src/backend/optimizer/plan/
 * planner.c) applied to our own grouped target, and it has to stay that:
 * setrefs.c re-derives the partial Aggrefs from the Finalize Agg's own ones
 * (convert_combining_aggrefs()) and matches them against the subplan's target
 * list with equal(), so an Aggref that differs in any field - the aggsplit
 * above all - would not be found.
 *
 * Our target only ever holds plain Vars and count Aggrefs at the top level
 * (the checks above refuse everything else), which is why pull_var_clause()
 * needs no SRF or window handling here.
 */
static PathTarget *
lion_make_partial_target(PlannerInfo *root, PathTarget *grouping_target,
						 List *having)
{
	PathTarget *partial_target = create_empty_pathtarget();
	List	   *non_group_cols = NIL;
	List	   *non_group_exprs;
	int			i = 0;
	ListCell   *lc;

	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
		{
			/*
			 * A grouping column, carried through as it is - sortgroupref and
			 * all, because that is how the Finalize Agg finds it again
			 * (set_upper_references() matches group items by sortgroupref
			 * first, and make_agg() numbers its grouping columns that way).
			 */
			add_column_to_pathtarget(partial_target, expr, sgref);
		}
		else
			non_group_cols = lappend(non_group_cols, expr);
		i++;
	}

	/*
	 * A count the HAVING alone mentions has to be produced as well, or the
	 * Finalize Agg would have nothing to combine for it; core's version does
	 * the same with the havingQual.
	 */
	if (having != NIL)
		non_group_cols = lappend(non_group_cols, having);
	non_group_exprs = pull_var_clause((Node *) non_group_cols,
									  PVC_INCLUDE_AGGREGATES |
									  PVC_RECURSE_WINDOWFUNCS |
									  PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(partial_target, non_group_exprs);

	foreach(lc, partial_target->exprs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);

		if (IsA(aggref, Aggref))
		{
			Aggref	   *newaggref = makeNode(Aggref);

			/* Flat-copy, as core does, so no other tree is damaged. */
			memcpy(newaggref, aggref, sizeof(Aggref));
			mark_partial_aggref(newaggref, AGGSPLIT_INITIAL_SERIAL);
			lfirst(lc) = newaggref;
		}
	}

	list_free(non_group_exprs);
	list_free(non_group_cols);

	return set_pathtarget_cost_width(root, partial_target);
}

/*
 * The functions of an expression the executor would check EXECUTE on when it
 * initialised it, found as ExecInitExprRec() finds them: a function call's,
 * an operator's (OpExpr, DistinctExpr, NullIfExpr), and a ScalarArrayOpExpr's
 * comparison - its negator's for a hashed NOT IN - plus the hash function of
 * a hashed one.  Every clause the node answers is made of these, Vars and
 * values.
 */
static bool
lion_replaced_funcs_walker(Node *node, List **funcs)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr))
		*funcs = list_append_unique_oid(*funcs, ((FuncExpr *) node)->funcid);
	else if (IsA(node, OpExpr) || IsA(node, DistinctExpr) ||
			 IsA(node, NullIfExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		*funcs = list_append_unique_oid(*funcs,
										OidIsValid(op->opfuncid) ?
										op->opfuncid : get_opcode(op->opno));
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

		if (OidIsValid(saop->negfuncid))
			*funcs = list_append_unique_oid(*funcs, saop->negfuncid);
		else
			*funcs = list_append_unique_oid(*funcs,
											OidIsValid(saop->opfuncid) ?
											saop->opfuncid :
											get_opcode(saop->opno));
		if (OidIsValid(saop->hashfuncid))
			*funcs = list_append_unique_oid(*funcs, saop->hashfuncid);
	}
	else if (IsA(node, Aggref))
		return false;			/* lion_replaced_aggs_walker()'s */
	return expression_tree_walker(node, lion_replaced_funcs_walker,
								  (void *) funcs);
}

/* The aggregates of an expression, which ExecInitAgg() would check. */
static bool
lion_replaced_aggs_walker(Node *node, List **aggs)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		*aggs = list_append_unique_oid(*aggs, ((Aggref *) node)->aggfnoid);
		return false;
	}
	return expression_tree_walker(node, lion_replaced_aggs_walker,
								  (void *) aggs);
}

/*
 * What the node stands in for, as custom_private member LION_PRIV_EXECUTE:
 * the functions the executor would have asked EXECUTE on for the plan the
 * node replaces, so that lion_check_replaced_execute() can ask the same of
 * whoever runs it (DESIGN.md §9, "Privileges"; the 2026-09-23 review found a
 * pushed-down count answering after REVOKE EXECUTE on count() or int4eq).
 * Verified against core on every supported release:
 *
 *	- the Agg checks each aggregate (tlexprs, having) for the current user,
 *	  and its transition and final functions for the aggregate's owner;
 *	- the scan initialises its quals, rel's baserestrictinfo - every one of
 *	  which the node answers, OR trees and IN lists included - and so checks
 *	  each operator's function (and a hashed IN list's hash function);
 *	- a GROUP BY's Agg checks each grouping column's equality function
 *	  (ExecBuildGroupingEqual(), hashed or sorted), but not the hash or sort
 *	  support functions;
 *	- a hash or nested-loop join initialises its join clause, so an FK-side
 *	  join (fj) adds the join operator's function.  A merge join compares
 *	  through the btree support function unchecked, so a query the planner
 *	  would have merge-joined asks one privilege more of the node; the SQL
 *	  meaning of the query - it calls `=` - is the one kept.
 *
 * Not included: count(DISTINCT k)'s equality, which nodeAgg calls through an
 * unchecked FmgrInfo for a single column, so core runs the query with that
 * function revoked and so does the node; and the projection and the HAVING
 * evaluated over the finished counts, and a non-literal clause value, which
 * are initialised with ExecInitExpr() and so checked by core as they stand.
 * (A clause value that calls functions - `now() - interval '1 day'` - has
 * them in baserestrictinfo as well, so they are in the list too.)  A boolean
 * column tested by itself (`WHERE flag`) calls nothing in core's plan either:
 * the node's `flag = true` is looked up through the index, as core's index
 * scan looks it up, without asking EXECUTE on booleq.
 */
static List *
lion_replaced_functions(RelOptInfo *rel, List *tlexprs, List *having,
						List *groupclause, const LionFkJoin *fj)
{
	List	   *aggs = NIL;
	List	   *funcs = NIL;
	List	   *groupfuncs = NIL;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
		(void) lion_replaced_funcs_walker((Node *) ((RestrictInfo *) lfirst(lc))->clause,
										  &funcs);
	if (fj != NULL)
		funcs = list_append_unique_oid(funcs, get_opcode(fj->opno));
	foreach(lc, groupclause)
		groupfuncs = list_append_unique_oid(groupfuncs,
											get_opcode(((SortGroupClause *) lfirst(lc))->eqop));
	(void) lion_replaced_aggs_walker((Node *) tlexprs, &aggs);
	(void) lion_replaced_aggs_walker((Node *) having, &aggs);

	return list_make3(aggs, funcs, groupfuncs);
}

/*
 * Is the aggregate one the FK-side join can answer (DESIGN.md §27)?  Its
 * partial value per dimension row is that row's count of joined fact rows,
 * which is count(*) - and count(x) for any x that is non-NULL in every joined
 * row: a non-NULL constant (`count(1)`), or either side of the join key, which
 * a strict equality never matches when NULL.  A semi join's rows are the
 * dimension rows with a match, so the same holds of their key; an anti join's
 * are the rows WITHOUT one, the NULL keys among them, so there only the
 * constant does.
 */
static bool
lion_fkjoin_agg_is_count(Aggref *agg, const LionFkJoin *fj)
{
	TargetEntry *tle;
	Node	   *arg;

	if (agg->aggfnoid != F_COUNT_ && agg->aggfnoid != F_COUNT_ANY)
		return false;
	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->aggvariadic)
		return false;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE ||
		agg->aggkind != AGGKIND_NORMAL)
		return false;

	if (agg->aggfnoid == F_COUNT_)
		return agg->aggstar && agg->args == NIL;

	if (list_length(agg->args) != 1)
		return false;
	tle = (TargetEntry *) linitial(agg->args);
	if (!IsA(tle, TargetEntry))
		return false;
	arg = lion_strip((Node *) tle->expr);
	if (arg == NULL)
		return false;
	if (IsA(arg, Const))
		return !((Const *) arg)->constisnull;
	if (IsA(arg, Var))
	{
		Var		   *v = (Var *) arg;

		if (v->varlevelsup != 0 || fj->jointype == JOIN_ANTI)
			return false;
		return (v->varno == fj->fkvar->varno &&
				v->varattno == fj->fkvar->varattno) ||
			(v->varno == fj->pkvar->varno &&
			 v->varattno == fj->pkvar->varattno);
	}
	return false;
}

/*
 * Is the aggregate a count(DISTINCT x) the FK-side join can hand to a core Agg
 * above rows of its own (DESIGN.md §27, "count(DISTINCT)")?  Those rows are
 * the dimension rows that join - one each, however many fact rows each joins
 * - and a DISTINCT count does not see multiplicity, so over them it is the
 * join's own for any x the rows carry:
 *
 *	- a column of the dimension: its values come out of the dimension's
 *	  tuples, as they would out of the join's;
 *	- the fact's join column, in an inner join (and in the forward semi join
 *	  over a non-unique key, an inner join over the distinct keys), which the
 *	  rows carry as the dimension's key.  Every fact value a dimension row
 *	  joins is equal to that row's key under the join's operator; if that is
 *	  the equality the DISTINCT compares with - the same operator or one of
 *	  its btree or hash family (equality_ops_are_compatible()) - under the
 *	  same collation, then the fact values of one dimension row are one
 *	  distinct value, and the key is it.  Two dimension rows with equal keys
 *	  are one value too, which the Agg's own DISTINCT sees: uniqueness is not
 *	  what makes this exact.  The key is emitted where the fact's column
 *	  stands, so the two have to be of one type.
 *
 * No FILTER, no ORDER BY, one argument; the Agg evaluates the rest.
 */
static bool
lion_fkjoin_agg_is_distinct(Aggref *agg, const LionFkJoin *fj)
{
	TargetEntry *tle;
	SortGroupClause *sgc;
	Node	   *arg;
	Var		   *v;

	if (agg->aggfnoid != F_COUNT_ANY)
		return false;
	if (list_length(agg->aggdistinct) != 1 || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->aggvariadic)
		return false;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE ||
		agg->aggkind != AGGKIND_NORMAL)
		return false;
	if (list_length(agg->args) != 1)
		return false;
	tle = (TargetEntry *) linitial(agg->args);
	if (!IsA(tle, TargetEntry))
		return false;
	arg = lion_strip((Node *) tle->expr);
	if (arg == NULL || !IsA(arg, Var))
		return false;
	v = (Var *) arg;
	if (v->varlevelsup != 0 || v->varattno <= 0)
		return false;

	if (v->varno == fj->pkvar->varno)
		return true;			/* a dimension column */

	if (v->varno != fj->fkvar->varno || v->varattno != fj->fkvar->varattno)
		return false;
	if (fj->jointype != JOIN_INNER && fj->jointype != JOIN_UNIQUE_INNER)
		return false;
	if (fj->fkvar->vartype != fj->pkvar->vartype)
		return false;
	sgc = (SortGroupClause *) linitial(agg->aggdistinct);
	if (!IsA(sgc, SortGroupClause) || !OidIsValid(sgc->eqop))
		return false;
	if (!equality_ops_are_compatible(sgc->eqop, fj->opno))
		return false;
	if (agg->inputcollid != fj->collation)
		return false;
	return true;
}

/*
 * The custom_private of an FK-side join path (DESIGN.md §27): the members
 * lion_try_fkjoin_path() built, with the join member of this path - the
 * clause, the kind of join, the flags, and the sort operator and collation of
 * a forward semi join's distinct keys - in LION_PRIV_JOIN.  The operator and
 * the collation are OIDs in an IntList, which casting to int and back keeps.
 */
static List *
lion_fkjoin_private(List *base, int joinclause, int jointype, int flags,
					Oid sortop, Oid sortcoll)
{
	List	   *priv = list_copy(base);

	lfirst(list_nth_cell(priv, LION_PRIV_JOIN)) =
		list_make5_int(joinclause, jointype, flags, (int) sortop,
					   (int) sortcoll);
	return priv;
}

/*
 * What making the keys of a forward semi join over a non-unique key distinct
 * costs (DESIGN.md §27, "Forward semi joins over a non-unique key"): the keys
 * of `rows` child rows, `width` bytes each, sorted, and each taken out of the
 * sort again and compared with the previous distinct key.  All of it comes
 * before the first key is looked up.  A datum sort holds three words a key,
 * and the key itself when it is passed by reference; past work_mem it writes
 * its keys out and reads them back once, three quarters of the pages in
 * sequence, as core's cost_tuplesort() charges one merge pass.
 */
static Cost
lion_fkjoin_sort_cost(double rows, int width)
{
	double		n = Max(rows, 2.0);
	double		bytes = n * (MAXALIGN(width) + 3 * sizeof(Datum));
	Cost		cost;

	cost = LION_FKJOIN_SORT_COMPARE_COST * n * (log(n) / log(2.0)) +
		LION_FKJOIN_SORT_KEY_COST * n;
	if (bytes > work_mem * 1024.0)
		cost += 2.0 * ceil(bytes / BLCKSZ) *
			(0.75 * seq_page_cost + 0.25 * random_page_cost);
	return cost;
}

/*
 * Is the FK-side join's partial target counts alone (LION_JOINFLAG_SUM)?  The
 * target is lion_make_partial_target()'s: grouping columns, dimension Vars and
 * partial count Aggrefs, nothing else at the top level.
 */
static bool
lion_target_counts_only(PathTarget *target)
{
	ListCell   *lc;

	if (target->exprs == NIL)
		return false;
	foreach(lc, target->exprs)
	{
		if (!IsA(lfirst(lc), Aggref))
			return false;
	}
	return true;
}

/*
 * One FK-side join path (DESIGN.md §27) over `child` - the dimension's
 * cheapest path, or, with `workers` above zero, its cheapest partial path run
 * by that many workers - and what goes above it into the grouped rel.
 *
 * A forward semi join over a non-unique key (fj->jointype JOIN_UNIQUE_INNER)
 * is different on both counts.  `dimrows` is its DISTINCT keys, which are what
 * the node looks up, `found` those of them expected to have fact rows at all,
 * and the child is always a whole path of the dimension, whose rows the node
 * sorts to make the keys distinct before it looks any up.  In a parallel plan
 * every participant runs that whole child and sorts all of it - a partial
 * child would give each participant keys the others may have too, and a key
 * counted by two participants counts its fact rows twice - and the
 * participants divide the sorted, distinct keys among themselves instead, in
 * runs of LION_FKJOIN_UNIQUE_CHUNK (lion_join_next_key()).  So the child and
 * the sort are charged to every participant in full, as the fact filters'
 * copy is, and the lookups and counts are one participant's share of the
 * keys.
 *
 * The node streams one row per dimension row that joins (a partial count, or
 * the row itself for count(DISTINCT)), so it starts when its child does, and
 * it costs the child plus what it does per dimension row.  A target of counts
 * alone is summed instead (LION_JOINFLAG_SUM): one row per participant and
 * run, which is what core's Gather and Finalize Agg above it then handle.
 * In a parallel plan the child hands each participant its share of the
 * dimension rows, the dimension rows divided as core divides a partial path's
 * (the Gather may start more workers than the child planned, "Parallel");
 * every participant locates the fact filters and makes its own copy of them,
 * so the cost of those is charged in full to each, as a hash join below a
 * Gather charges its hash table - the elapsed cost of the parallel plan is
 * one participant's.
 *
 * Above a partial path goes a Gather; above that, or above the node itself,
 * core's Finalize Agg over the partial counts - or, for emitted rows, a Sort
 * by the query's group pathkeys (the GROUP BY columns and the DISTINCT
 * arguments core may have chosen to presort) and core's plain Agg.
 */
static void
lion_add_fkjoin_paths(PlannerInfo *root, RelOptInfo *rel,
					  RelOptInfo *output_rel, const LionFkJoin *fj,
					  List *having, LionCountTarget *first, Path *child,
					  int workers, PathTarget *nodetarget, List *base,
					  int joinclause, int jointype, bool emitrows,
					  List *whereclauses, List *wherekinds, List *ors,
					  double dimrows, double found, bool parallel_safe)
{
	Query	   *parse = root->parse;
	CustomPath *cpath;
	Path	   *input;
	bool		partial = (workers > 0);
	bool		unique = (fj->jointype == JOIN_UNIQUE_INNER);
	double		divisor = partial ? lion_parallel_divisor(workers) : 1.0;
	double		childrows = (partial || unique) ?
		clamp_row_est(dimrows / divisor) : clamp_row_est(child->rows);
	double		share = Min(childrows / Max(dimrows, 1.0), 1.0);
	double		childfound = Min(clamp_row_est(found * share), childrows);
	double		rows;
	double		inrows;
	double		numgroups;
	double		rowbytes;
	bool		collect;
	bool		walk;
	bool		sum;
	Cost		run;
	Cost		startup;
	int			flags;
	AggStrategy aggstrategy;
	AggClauseCosts agg_costs;

	/*
	 * What a child row takes of a batch looked up in key order: its columns
	 * and the batch's own bookkeeping - or, for the distinct keys of a forward
	 * semi join, the key alone.
	 */
	rowbytes = (double) LION_FKJOIN_BATCH_ENT_BYTES +
		MAXALIGN(unique ?
				 get_typavgwidth(fj->pkvar->vartype, fj->pkvar->vartypmod) :
				 child->pathtarget->width);

	run = lion_cost_fkjoin_rel(root, rel, first, fj->fkvar, joinclause,
							   whereclauses, wherekinds, ors, childrows,
							   childfound,
							   jointype != LION_JOIN_INNER || emitrows,
							   rowbytes, &collect, &walk);

	/*
	 * Partial counts and nothing else - no dimension column to group by or
	 * to print - are handed up as one partial row, their sum (DESIGN.md §27,
	 * "The per-key path, end to end").
	 */
	sum = !emitrows && lion_target_counts_only(nodetarget);
	flags = (collect ? LION_JOINFLAG_COLLECT : 0) |
		(emitrows ? LION_JOINFLAG_ROWS : 0) |
		(unique ? LION_JOINFLAG_UNIQUE : 0) |
		(walk ? LION_JOINFLAG_WALK : 0) |
		(sum ? LION_JOINFLAG_SUM : 0);

	/*
	 * The node starts when its child does - or, when it sorts the child's
	 * keys first, once the whole child has run and the keys are sorted.
	 */
	startup = child->startup_cost;
	if (unique)
	{
		startup = child->total_cost +
			lion_fkjoin_sort_cost(child->rows, child->pathtarget->width);
		run += startup - child->total_cost;
	}

	/*
	 * A fact filter that is a multi-key query the node only has at run time
	 * may need the candidates of every count rechecked in the heap (DESIGN.md
	 * §17, "A query known only at run time"): the fact rows the keys it looks
	 * up reach, each count reading its own.
	 */
	{
		double		tuples = Max(rel->tuples, 1.0);
		double		reach = Min(childfound * tuples /
								lion_fkjoin_fk_ndistinct(root, rel, fj->fkvar),
								tuples);

		run += lion_cost_recheck(root, rel, first->whereidx, first->wherecol,
								 whereclauses, wherekinds, ors,
								 reach * Min(Max(rel->rows, 1.0) / tuples, 1.0),
								 childfound, reach);
	}

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = nodetarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = partial;
	cpath->path.parallel_safe = parallel_safe;
	cpath->path.parallel_workers = workers;
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = list_make1(child);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = lion_fkjoin_private(base, joinclause, jointype,
												flags, fj->uniqsortop,
												unique ? fj->collation :
												InvalidOid);
	cpath->methods = &lion_count_path_methods;

	/*
	 * At most one row per dimension row comes out (per distinct key that has
	 * fact rows, for a forward semi join over a non-unique key).  A semi or
	 * anti join's rows are exactly the join's, whose estimate the planner has
	 * made already, as is the share of them a participant sees.
	 */
	rows = unique ? childfound :
		(jointype == LION_JOIN_INNER) ? childrows :
		clamp_row_est(Min(fj->joinrel->rows * share, childrows));

	/* ... or one, their sum, when that is all the target wants */
	if (sum)
		rows = 1.0;
	run += rows * LION_FKJOIN_ROW_COST;
	cpath->path.rows = rows;
	cpath->path.startup_cost = startup;
	cpath->path.total_cost = child->total_cost + run;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = child->disabled_nodes;
#endif

	input = &cpath->path;
	inrows = rows;
	if (partial)
	{
		/* the rows of every participant */
		inrows = clamp_row_est(rows * divisor);
		input = (Path *) create_gather_path(root, output_rel, input,
											nodetarget, NULL, &inrows);
	}

	if (!emitrows)
	{
		/* ---- the Finalize Agg that groups the partial counts ---- */
		if (root->processed_groupClause != NIL)
		{
			aggstrategy = AGG_HASHED;
			numgroups = estimate_num_groups(root,
											get_sortgrouplist_exprs(root->processed_groupClause,
																	root->processed_tlist),
											inrows, NULL, NULL);
		}
		else
		{
			aggstrategy = (parse->groupClause != NIL) ? AGG_SORTED : AGG_PLAIN;
			numgroups = 1.0;
		}

		MemSet(&agg_costs, 0, sizeof(agg_costs));
		get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_costs);

		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, input,
								 output_rel->reltarget,
								 aggstrategy, AGGSPLIT_FINAL_DESERIAL,
								 root->processed_groupClause,
								 having,
								 &agg_costs,
								 numgroups));
		return;
	}

	/*
	 * ---- core's own Agg over the dimension rows that join ----
	 *
	 * Sorted by root->group_pathkeys, which is the GROUP BY followed by the
	 * DISTINCT arguments core decided to presort (the aggregates it marked
	 * aggpresorted, whose DISTINCT then only compares neighbours): the order
	 * core's own sorted Agg would be given.
	 */
	if (root->group_pathkeys != NIL)
		input = (Path *) create_sort_path(root, output_rel, input,
										  root->group_pathkeys, -1.0);
	if (root->processed_groupClause != NIL)
	{
		aggstrategy = AGG_SORTED;
		numgroups = estimate_num_groups(root,
										get_sortgrouplist_exprs(root->processed_groupClause,
																root->processed_tlist),
										inrows, NULL, NULL);
	}
	else
	{
		aggstrategy = (parse->groupClause != NIL) ? AGG_SORTED : AGG_PLAIN;
		numgroups = 1.0;
	}

	MemSet(&agg_costs, 0, sizeof(agg_costs));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &agg_costs);

	add_path(output_rel, (Path *)
			 create_agg_path(root, output_rel, input,
							 output_rel->reltarget,
							 aggstrategy, AGGSPLIT_SIMPLE,
							 root->processed_groupClause,
							 having,
							 &agg_costs,
							 numgroups));
}

/*
 * The rest of lion_try_count_path() for the FK-side join (DESIGN.md §27).
 *
 * The caller has analysed the FACT rel's WHERE clauses exactly as it does for
 * a single table and hands their lists over; `rel` is the fact rel.  What is
 * added here:
 *
 *	- the join key, as one more LION_CLAUSE_EQ clause on the fact's fk column
 *	  whose value expression is the dimension's column.  lion_collect_targets()
 *	  then matches an index for it like any clause - strategy 1 of its
 *	  opfamily for the join operator, a cross-type equality and hash for the
 *	  dimension key's type, the clause's collation - which is everything a
 *	  lookup needs to answer `f.fk = <that row's key>` exactly (§10);
 *	- the target list, which decides what the node emits.  Counts - count(*),
 *	  count(1), count of the key - are PARTIAL counts per dimension row, and
 *	  core's Finalize Agg above groups and adds them: hashed for a GROUP BY,
 *	  sorted over no columns for a GROUP BY the planner folded to constants
 *	  (an empty join has no group then), plain without one.  count(DISTINCT)
 *	  of the key or of a dimension column has no partial form; then the node
 *	  emits the dimension ROWS that join, one each, and core's own Agg - over
 *	  a Sort by the query's group pathkeys, which is what its DISTINCT and a
 *	  GROUP BY of it expect - computes the aggregates as it would over the
 *	  join (lion_fkjoin_agg_is_distinct()).  The two do not mix: a count of
 *	  pairs cannot come out of rows that stand for dimension rows.  Either
 *	  way the groups are the dimension's and core forms them, so neither the
 *	  grouping-equality rule nor the value-representation rule of §10 has
 *	  anything to check;
 *	- the paths: the node over the dimension's cheapest path, and - when the
 *	  query may run in parallel and the dimension has a partial path - the
 *	  node over that partial path under a Gather, each participant counting
 *	  the dimension rows its share of the child returns ("Parallel").
 */
static void
lion_try_fkjoin_path(PlannerInfo *root, RelOptInfo *rel,
					 RelOptInfo *output_rel, GroupPathExtraData *extra,
					 const LionFkJoin *fj, List *having,
					 List *whereattnos, List *clauseinfos, List *whereclauses,
					 List *whereconsts, List *wherekinds, List *whereopnos,
					 List *whereinor, List *ors)
{
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	List	   *exprs;
	List	   *items;
	LionLeafInfo leaf;
	LionDriveInfo nodrive[LION_MAX_GROUPCOLS];
	int			joinclause;
	List	   *targets = NIL;
	LionCountTarget *first;
	PathTarget *nodetarget;
	List	   *oids;
	List	   *ints;
	List	   *consts = NIL;
	List	   *ckinds = NIL;
	List	   *base;
	double		dimrows;
	double		found;
	int			jointype;
	bool		emitrows;
	bool		parallel;
	int			ncount = 0;
	int			ndistinct = 0;
	ListCell   *lc;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	int			i;

	/* ---- the target and the HAVING: dimension columns and counts ---- */
	exprs = list_copy(output_rel->reltarget->exprs);
	if (having != NIL)
		exprs = lappend(exprs, having);
	if (contain_subplans((Node *) exprs))
		return;

	/*
	 * A volatile expression would be evaluated once per DIMENSION row by the
	 * node's projection instead of once per join row, which is a different
	 * query.
	 */
	if (contain_volatile_functions((Node *) exprs))
		return;

	items = pull_var_clause((Node *) exprs,
							PVC_INCLUDE_AGGREGATES |
							PVC_RECURSE_WINDOWFUNCS |
							PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, items)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *v = (Var *) node;

			/*
			 * Only the dimension's own columns: they come out of the child's
			 * rows.  A fact column would have to be a group of the posting
			 * sets under each dimension row, which v1 does not build.
			 */
			if (v->varno != (int) fj->dimrel->relid || v->varattno <= 0 ||
				v->varlevelsup != 0)
				return;
		}
		else if (IsA(node, Aggref))
		{
			if (lion_fkjoin_agg_is_count((Aggref *) node, fj))
				ncount++;
			else if (lion_fkjoin_agg_is_distinct((Aggref *) node, fj))
				ndistinct++;
			else
				return;
		}
		else
			return;
	}
	if (ncount + ndistinct == 0 || (ncount > 0 && ndistinct > 0))
		return;
	emitrows = (ndistinct > 0);

	if (!emitrows)
	{
		/*
		 * The grouping is done above the node, from partial counts, so the
		 * planner has to consider the aggregates splittable and a GROUP BY
		 * has to be hashable (a sorted Finalize Agg would need a Sort over
		 * the node, which is not built for partial counts).
		 */
		if (extra == NULL || (extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0)
			return;
		if (root->processed_groupClause != NIL &&
			!grouping_is_hashable(root->processed_groupClause))
			return;
		nodetarget = lion_make_partial_target(root, output_rel->reltarget,
											  having);
	}
	else
	{
		/*
		 * The node's rows are the grouping input - the target core would
		 * have computed over the join, which the scan/join rel carries by
		 * now: the grouping expressions and the columns the aggregates read.
		 * Every column in it has to be one the rows carry: the dimension's,
		 * or the fact's join column that a DISTINCT count reads, which the
		 * node emits as the key.  A DISTINCT aggregate groups by sorting, so
		 * a GROUP BY must be sortable.
		 */
		if (root->processed_groupClause != NIL &&
			!grouping_is_sortable(root->processed_groupClause))
			return;
		nodetarget = fj->joinrel->reltarget;
		if (contain_volatile_functions((Node *) nodetarget->exprs))
			return;
		items = pull_var_clause((Node *) nodetarget->exprs,
								PVC_RECURSE_AGGREGATES |
								PVC_RECURSE_WINDOWFUNCS |
								PVC_INCLUDE_PLACEHOLDERS);
		foreach(lc, items)
		{
			Var		   *v = (Var *) lfirst(lc);

			if (!IsA(v, Var) || v->varlevelsup != 0 || v->varattno <= 0)
				return;
			if (v->varno == (int) fj->dimrel->relid)
				continue;
			if (v->varno == fj->fkvar->varno &&
				v->varattno == fj->fkvar->varattno &&
				(fj->jointype == JOIN_INNER ||
				 fj->jointype == JOIN_UNIQUE_INNER))
				continue;
			return;
		}
	}

	/*
	 * An IN list whose array is a parameter, or any other expression that is
	 * not a literal list, has no length until the executor has it (§15).  It
	 * used to be refused here, because a count that reads the fact filters
	 * rebuilds the list's union per dimension row, and a 43,000-value `=
	 * ANY ($1)` over 20,000 dimension rows ran for minutes (the 2026-09-23
	 * review).  It is taken now as a single table takes it, evaluated once
	 * per scan and rescan (lion_eval_clause_values(), in every participant of
	 * a parallel plan): the fact filters are collected once into a copy that
	 * every count reads (DESIGN.md §27) whatever the list's length - a list
	 * too long to open at once is read as a windowed union for it
	 * (lion_count_sources_run()) - and the cost model prices the per-count
	 * reading of an array it cannot see as the longest list a literal may be
	 * (lion_cost_fkjoin_rel()), so that the copy is what it chooses.  An
	 * expression's array is priced by its plan-time estimate.
	 */

	/* ---- the join key: one more equality clause on the fact rel ---- */
	memset(&leaf, 0, sizeof(leaf));
	leaf.var = fj->fkvar;
	leaf.val = (Node *) copyObject(fj->pkexpr);
	leaf.opno = fj->opno;
	leaf.cmptype = exprType(fj->pkexpr);
	leaf.strategy = LION_STRAT_EQUAL;
	leaf.extractquery = InvalidOid;
	leaf.collation = fj->collation;
	leaf.kind = LION_CLAUSE_EQ;
	joinclause = list_length(whereattnos);
	lion_append_clause(&leaf, fj->clause, false,
					  &whereattnos, &clauseinfos, &whereclauses,
					  &whereconsts, &wherekinds, &whereopnos, &whereinor);

	/*
	 * ---- the fact rel and its indexes ----
	 *
	 * Nothing drives an entry scan: the child's rows drive the counts.  A
	 * partitioned fact rel was refused by the caller, so there is one target.
	 */
	memset(nodrive, 0, sizeof(nodrive));
	if (!lion_collect_targets(root, rel, nodrive, 0, whereattnos, clauseinfos,
							 &targets))
		return;
	if (list_length(targets) != 1)
		return;
	first = (LionCountTarget *) linitial(targets);

	/*
	 * ---- what every path of it carries ----
	 *
	 * The dimension rows the node looks up: the child's, or - for a forward
	 * semi join over a non-unique key - their distinct keys, which is the
	 * inner join the node counts (JOIN_UNIQUE_INNER is LION_JOIN_INNER with
	 * LION_JOINFLAG_UNIQUE).  NULL is one of the groups estimate_num_groups()
	 * counts, and one the node never looks up.
	 */
	if (fj->jointype == JOIN_UNIQUE_INNER)
		dimrows = clamp_row_est(estimate_num_groups(root,
													list_make1(fj->pkvar),
													fj->dimpath->rows,
													NULL, NULL));
	else
		dimrows = clamp_row_est(fj->dimpath->rows);

	/*
	 * Which of them find an entry of the fk index: every dimension row, as an
	 * fk into a dimension key is expected to - except that the distinct keys
	 * of a forward semi join are those of any set of rows, which may share
	 * few values with the fact (`fk IN (SELECT k FROM ...)`).  Their number
	 * is what core's own estimate of the semi join says: the fact rows it
	 * leaves (the join rel's rows) over the rows the fact filters leave of one
	 * key's (the fact rel's rows over the fk's distinct values).
	 */
	found = dimrows;
	if (fj->jointype == JOIN_UNIQUE_INNER)
		found = Min(dimrows,
					clamp_row_est(fj->joinrel->rows *
								  lion_fkjoin_fk_ndistinct(root, rel, fj->fkvar) /
								  Max(rel->rows, 1.0)));
	jointype = (fj->jointype == JOIN_SEMI) ? LION_JOIN_SEMI :
		(fj->jointype == JOIN_ANTI) ? LION_JOIN_ANTI : LION_JOIN_INNER;

	oids = list_make3_oid(rte->relid, InvalidOid, InvalidOid);
	ints = list_make4_int((int) rel->relid, 0, 0, 0);
	i = 0;
	forthree(l1, whereattnos, l2, whereconsts, l3, wherekinds)
	{
		oids = lappend_oid(oids,
						   ((IndexOptInfo *) list_nth(first->whereidx,
													  i))->indexoid);
		ints = lappend_int(ints, lfirst_int(l1));
		consts = lappend(consts, copyObject((Node *) lfirst(l2)));
		ckinds = lappend_int(ckinds, lfirst_int(l3));
		i++;
	}

	base = list_make1(list_make2_int(LION_PRIV_MAGIC, LION_PRIV_NMEMBERS));
	base = lappend(base, oids);
	base = lappend(base, ints);
	base = lappend(base, consts);
	base = lappend(base, ckinds);
	base = lappend(base, NIL);	/* parts */
	base = lappend(base, whereopnos);
	base = lappend(base, ors);
	base = lappend(base, NIL);	/* having */
	base = lappend(base, NIL);	/* distinct */
	base = lappend(base, NIL);	/* join: lion_fkjoin_private() */
	/* the dimension's GROUP BY is the Agg's, which checks it */
	base = lappend(base, lion_replaced_functions(rel,
												 output_rel->reltarget->exprs,
												 having, NIL, fj));
	base = lappend(base, NIL);	/* coalesce: the fact side groups nothing */

	/*
	 * ---- may it run in a worker ----
	 *
	 * Every participant opens the fact table and its indexes, so the fact
	 * rel has to be one a worker may read (not a temporary table, say); the
	 * clause values are evaluated in every one of them; and the grouped rel
	 * says whether the target and the HAVING may be computed above a Gather.
	 * The serial node is then as parallel-safe as its child - a worker may
	 * run the whole of it, under debug_parallel_query or below a Gather that
	 * core puts over the plan - and with a partial path of the dimension it
	 * is also offered parallel, each participant counting its share.
	 */
	parallel = (output_rel->consider_parallel &&
				fj->joinrel->consider_parallel &&
				rel->consider_parallel &&
				fj->dimrel->consider_parallel &&
				is_parallel_safe(root, (Node *) consts));

	lion_add_fkjoin_paths(root, rel, output_rel, fj, having, first,
						  fj->dimpath, 0, nodetarget, base, joinclause,
						  jointype, emitrows, whereclauses, wherekinds, ors,
						  dimrows, found,
						  parallel && fj->dimpath->parallel_safe);
	if (parallel && fj->jointype == JOIN_UNIQUE_INNER)
	{
		IndexOptInfo *fkidx = (IndexOptInfo *) list_nth(first->whereidx,
														joinclause);
		Path	   *whole = NULL;
		double		fkpages;
		int			workers;

		/*
		 * A forward semi join over a non-unique key is parallel over its
		 * whole child, which every participant runs and sorts: the sorted,
		 * distinct keys are what they divide (lion_add_fkjoin_paths()).  So
		 * the child has to give every participant the same rows - it may run
		 * in a worker, and nothing in the dimension's quals is volatile,
		 * which could keep a row in one participant and drop it in another.
		 * It is the dimension's cheapest path that may run in a worker, which
		 * is not its cheapest path when that is a Gather.  The workers are
		 * what a parallel scan of the fk index pages the distinct keys read
		 * would get, as for the other joins; there is no partial child whose
		 * own count could be more.
		 */
		foreach(lc, fj->dimrel->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);

			if (p->parallel_safe && p->param_info == NULL &&
				(whole == NULL || compare_path_costs(p, whole, TOTAL_COST) < 0))
				whole = p;
		}
		fkpages = (double) fkidx->pages *
			Min(dimrows / lion_fkjoin_fk_ndistinct(root, rel, fj->fkvar), 1.0);
		workers = compute_parallel_worker(fj->dimrel, -1, fkpages,
										  max_parallel_workers_per_gather);

		if (whole != NULL && workers > 0 &&
			!contain_volatile_functions((Node *) fj->dimrel->baserestrictinfo))
			lion_add_fkjoin_paths(root, rel, output_rel, fj, having, first,
								  whole, workers, nodetarget, base,
								  joinclause, jointype, emitrows, whereclauses,
								  wherekinds, ors, dimrows, found, true);
	}
	else if (parallel && fj->dimrel->partial_pathlist != NIL)
	{
		Path	   *partial = (Path *) linitial(fj->dimrel->partial_pathlist);
		IndexOptInfo *fkidx = (IndexOptInfo *) list_nth(first->whereidx,
														joinclause);
		double		fkpages;
		int			workers;

		/*
		 * How many workers.  The node's work is the dimension rows' lookups
		 * and counts, not the dimension scan the child's own count was made
		 * for - a few thousand dimension rows are a small scan and a large
		 * join - so it gets the workers core would give a parallel scan of
		 * the fk index pages those lookups read (the dimension rows' share of
		 * the fk keys), or the child's, whichever is more.  A parallel_workers
		 * setting on the dimension decides alone, and
		 * max_parallel_workers_per_gather caps it, as they do core's.  The
		 * child's parallel scan hands out its rows to however many
		 * participants come.
		 */
		fkpages = (double) fkidx->pages *
			Min(dimrows / lion_fkjoin_fk_ndistinct(root, rel, fj->fkvar), 1.0);
		workers = compute_parallel_worker(fj->dimrel, -1, fkpages,
										  max_parallel_workers_per_gather);
		if (fj->dimrel->rel_parallel_workers == -1)
			workers = Max(workers, partial->parallel_workers);

		if (partial->param_info == NULL && workers > 0)
			lion_add_fkjoin_paths(root, rel, output_rel, fj, having, first,
								  partial, workers, nodetarget, base,
								  joinclause, jointype, emitrows, whereclauses,
								  wherekinds, ors, dimrows, dimrows, true);
	}
}

/*
 * Decide whether count(*) over input_rel can be answered from roaring
 * posting sets and, if so, add a CustomPath to output_rel.  Every failed
 * check simply returns: the normal plan is always available.
 *
 * fj is the FK-side join of DESIGN.md §27, or NULL.  With it, input_rel is
 * the JOIN rel and everything below about "the relation" - its WHERE clauses
 * and their indexes - is asked of the FACT rel, fj->factrel, unchanged; what
 * differs (the join key, the target list, the path) is lion_try_fkjoin_path().
 */
/*
 * Turn the RANGE clauses of every range column but `keep` (an index into
 * rangepos, or -1 for none) into bounds of ranges taken as sources
 * (DESIGN.md §32): LION_CLAUSE_RANGESRC in the clause lists, which is all the
 * executor goes by.
 */
static void
lion_ranges_to_sources(List *rangepos, int keep, List *wherekinds,
					   List *clauseinfos)
{
	int			r = 0;
	ListCell   *lc;

	foreach(lc, rangepos)
	{
		ListCell   *lp;

		if (r++ == keep)
			continue;
		foreach(lp, (List *) lfirst(lc))
		{
			int			pos = lfirst_int(lp);

			lfirst_int(list_nth_cell(wherekinds, pos)) = LION_CLAUSE_RANGESRC;
			((LionClauseInfo *) list_nth(clauseinfos, pos))->kind =
				LION_CLAUSE_RANGESRC;
		}
	}
}

static void
lion_try_count_path(PlannerInfo *root, RelOptInfo *input_rel,
				   RelOptInfo *output_rel, GroupPathExtraData *extra,
				   const LionFkJoin *fj)
{
	Query	   *parse = root->parse;
	RangeTblEntry *rte;
	Index		rti;
	Var		   *groupvar[LION_MAX_GROUPCOLS] = {NULL, NULL};
	AttrNumber	groupattno[LION_MAX_GROUPCOLS] = {0, 0};
	Oid			groupeqop[LION_MAX_GROUPCOLS] = {InvalidOid, InvalidOid};
	bool		groupvalueout[LION_MAX_GROUPCOLS] = {false, false};
	double		groupest[LION_MAX_GROUPCOLS] = {1.0, 1.0};
	int			ngroup = 0;
	int			g;
	AttrNumber	driveattno = 0; /* column whose index drives the entry scan */
	bool		singlegroup = false;
	bool		sumall = false;
	bool		partitioned = false;
	List	   *valueattnos = NIL;	/* pinned columns the output prints */
	LionDriveInfo drive[LION_MAX_GROUPCOLS];
	int			ndrive = 0;
	PathTarget *partialtarget = NULL;	/* set for a partitioned GROUP BY */
	List	   *whereattnos = NIL;	/* its column, in the PARENT's numbering */
	List	   *clauseinfos = NIL;	/* LionClauseInfo, one per clause */
	List	   *whereclauses = NIL; /* the clause, for selectivity */
	List	   *whereconsts = NIL;	/* its value expression - a Const, a Param,
									 * or a NULL placeholder for a null test */
	List	   *wherekinds = NIL;	/* LION_CLAUSE_* */
	List	   *whereopnos = NIL;	/* the clause's operator (0 for a null test) */
	List	   *whereinor = NIL;	/* 1 when the clause is a leaf of an OR */
	List	   *ors = NIL;		/* one IntList per OR restriction (§19) */
	List	   *posattnos = NIL;	/* columns with a positive clause */
	List	   *eqattnos = NIL;		/* columns pinned to one value */
	List	   *nonnullattnos = NIL;	/* columns a clause proves non-null */
	List	   *nullattnos = NIL;	/* columns a clause pins to NULL */
	List	   *targets = NIL;		/* LionCountTarget, one per counted relation */
	LionCountTarget *first;
	Var		   *notnullvar = NULL;	/* the first `IS NOT NULL` column */
	Var		   *rangevar = NULL;	/* the column the RANGE clauses bound (§28) */
	List	   *rangeclauses = NIL; /* ... and those clauses' RestrictInfos */
	List	   *rangevars = NIL;	/* every column a range bounds (§28, §32) */
	List	   *rangecls = NIL;		/* ... its clauses' RestrictInfos, a List
									 * per column */
	List	   *rangepos = NIL;		/* ... and their positions in the clause
									 * lists, an IntList per column */
	bool		hasrangesrc = false;	/* a range is a source (§32) */
	bool		pinnedsrc = false;	/* a source outside ranges that holds the
									 * §9 pin: a clause, or an OR of them */
	Selectivity rangesel = 1.0; /* the share of its entries they select */
	List	   *oids;
	List	   *ints;
	List	   *consts = NIL;
	List	   *ckinds = NIL;
	List	   *parts = NIL;
	CustomPath *cpath;
	double		numgroups;
	double		outrows;
	bool		haveagg = false;
	bool		havepositive = false;
	List	   *having;
	List	   *checkexprs;
	Var		   *distvar = NULL;	/* the column count(DISTINCT) counts (§26) */
	Oid			disteqop = InvalidOid;
	Oid			distcoll = InvalidOid;
	bool		distcounts = false; /* its walk must count, not test (§26) */
	double		distest = 0;
	List	   *uniqattnos = NIL;	/* columns of a count(DISTINCT) proved
									 * unique: a count(col) each (§26) */
	Const	   *groupcoal = NULL;	/* GROUP BY coalesce(col, c): c (§10) */
	Node	   *groupexpr = NULL;	/* ... and the whole expression */
	ListCell   *lc;

	/* ---- the query as a whole ---- */
	if (parse->commandType != CMD_SELECT)
		return;
	if (!parse->hasAggs)
		return;
	if (parse->groupingSets != NIL)
		return;
	if (parse->hasWindowFuncs || parse->hasTargetSRFs ||
		parse->hasDistinctOn || parse->distinctClause != NIL)
		return;
	if (parse->rowMarks != NIL || root->rowMarks != NIL)
		return;

	/*
	 * HAVING (DESIGN.md §10).  By now it is an implicit-AND list of the
	 * clauses that mention an aggregate (or are volatile, or contain a
	 * subquery): the planner has already moved every other clause into WHERE
	 * (subquery_planner()).  The node knows each group's count before it
	 * emits the group, so a HAVING over the counts it computes and the
	 * columns it can print is a filter on its output - checked below against
	 * the same rules as the target list, and applied by the node itself or,
	 * for a partitioned table, by the Finalize Agg above it.
	 */
	having = (extra != NULL) ? (List *) extra->havingQual :
		(List *) parse->havingQual;

	/* The FK-side join counts the fact rel's posting sets (DESIGN.md §27). */
	if (fj != NULL)
		input_rel = fj->factrel;

	/* ---- a single base relation: one table, or one partitioned parent ---- */
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;
	if (bms_membership(input_rel->relids) != BMS_SINGLETON)
		return;
	rti = input_rel->relid;
	if (rti == 0 || rti >= (Index) root->simple_rel_array_size)
		return;
	rte = root->simple_rte_array[rti];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return;
	if (rte->securityQuals != NIL || rte->tablesample != NULL)
		return;
	if (IS_DUMMY_REL(input_rel))
		return;					/* the planner has already proved it empty */

	if (rte->inh)
	{
		/*
		 * A partitioned parent (DESIGN.md §16).  The parent itself has no
		 * storage and no index list; every live leaf partition is counted in
		 * turn, with its own heap and its own indexes.  Old-style inheritance
		 * parents are not handled: their children are not required to have
		 * the parent's columns at all.
		 */
		if (rte->relkind != RELKIND_PARTITIONED_TABLE)
			return;
		if (input_rel->part_scheme == NULL || !IS_PARTITIONED_REL(input_rel))
			return;
		/* ... but not as the fact side of a join, in v1 (DESIGN.md §27) */
		if (fj != NULL)
			return;

		/*
		 * With partitionwise aggregation the planner builds its own per-child
		 * grouping paths; ours would only compete on cost, and the interaction
		 * is untested, so stay out of the way.  This applies to partitioned
		 * parents only: grouping_planner() sets patype whenever the GUC is on,
		 * before it knows whether the input is partitioned at all, so testing
		 * it earlier would switch the pushdown off for plain tables too.
		 */
		if (extra != NULL && extra->patype != PARTITIONWISE_AGGREGATE_NONE)
			return;
		partitioned = true;
	}
	else
	{
		if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
			return;
		if (input_rel->indexlist == NIL)
			return;
	}

	/*
	 * ---- GROUP BY: nothing, one indexed column, or two (DESIGN.md §20) ----
	 *
	 * Two columns are a nested loop over the two indexes' entries, so every
	 * rule below is made per column: its own index, its own collation, its
	 * own grouping equality, its own value-representation gate.
	 *
	 * None of it applies to the FK-side join (DESIGN.md §27), whose groups are
	 * the dimension's and are formed by the Finalize Agg above the node.
	 */
	if (fj == NULL &&
		list_length(root->processed_groupClause) > LION_MAX_GROUPCOLS)
		return;

	if (root->processed_groupClause == NIL)
	{
		/*
		 * Every GROUP BY column was proved constant by the planner (it does
		 * that for a column with an equality qual against anything that is
		 * not a Var - a literal or a parameter), so the query has one group - but, unlike a plain aggregate, it
		 * must produce no row at all when nothing matches.
		 */
		singlegroup = (parse->groupClause != NIL);
	}
	else if (fj == NULL)
	{
		foreach(lc, root->processed_groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   root->processed_tlist);
			Node	   *expr;
			int			i;

			if (tle == NULL)
				return;

			/*
			 * `GROUP BY coalesce(col, c)` is col's groups with the NULL group
			 * relabelled c - and merged into c's own group when col has that
			 * value (DESIGN.md §10, "coalesce").  Only that one grouping
			 * column, and only in the plain form, where the node's entry walk
			 * can do the merge exactly (lion_group_coalesce()).
			 */
			if (IsA(tle->expr, CoalesceExpr))
			{
				if (list_length(root->processed_groupClause) != 1 ||
					!lion_group_coalesce((CoalesceExpr *) tle->expr, rti,
										 &groupvar[ngroup], &groupcoal))
					return;
				groupexpr = (Node *) tle->expr;
				expr = (Node *) groupvar[ngroup];
			}
			else
				expr = lion_strip((Node *) tle->expr);
			if (expr == NULL || !IsA(expr, Var))
				return;
			groupvar[ngroup] = (Var *) expr;
			if (groupvar[ngroup]->varno != (int) rti ||
				groupvar[ngroup]->varattno <= 0 ||
				groupvar[ngroup]->varlevelsup != 0)
				return;

			/*
			 * A nullable group column is fine now: NULL keys have an entry of
			 * their own, so the NULL group is produced like any other
			 * (DESIGN.md §14).
			 */
			groupattno[ngroup] = groupvar[ngroup]->varattno;

			/* The same column twice is not a grouping this node can drive. */
			for (i = 0; i < ngroup; i++)
			{
				if (groupattno[i] == groupattno[ngroup])
					return;
			}

			/*
			 * The equality the planner chose for this column is what the index
			 * that drives the scan has to implement, whether there is one
			 * relation or many (lion_collect_targets(), finding 3 of the
			 * 2026-09-20 review), so a grouping clause without one is of no use
			 * here.
			 */
			groupeqop[ngroup] = sgc->eqop;
			if (!OidIsValid(groupeqop[ngroup]))
				return;

			/*
			 * A partitioned GROUP BY emits one PARTIAL aggregate per group per
			 * partition and lets core's Finalize HashAggregate combine them
			 * (DESIGN.md §16), so the column has to be hashable and the planner
			 * has to consider the aggregates splittable at all.  count(*) and
			 * count(col) always are, but the answer is the planner's to give.
			 * One table needs neither: it streams its finished groups.
			 */
			if (partitioned &&
				(!sgc->hashable || extra == NULL ||
				 (extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0))
				return;

			ngroup++;
		}
	}

	/* ---- every WHERE clause must be one the posting sets can answer ---- */
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *clause;
		LionLeafInfo leaf;

		if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant)
			return;

		clause = (Node *) rinfo->clause;

		/*
		 * `flag IS NOT TRUE` of a boolean column is `flag = false OR flag IS
		 * NULL`, and is an OR like any other from here on
		 * (lion_boolean_not_test()).
		 */
		{
			BoolExpr   *orform = lion_boolean_not_test(clause);

			if (orform != NULL)
				clause = (Node *) orform;
		}

		/*
		 * ---- an OR across columns (DESIGN.md §19) ----
		 *
		 * Every arm has to be a positive clause the posting sets can answer,
		 * or an AND of such clauses, each on a column of this relation.  The
		 * whole restriction then becomes ONE source - the union of the arms -
		 * which is ANDed with the other sources and with the GROUP BY driver
		 * like any other.  A negated arm (`IS NOT NULL`, a NOT of anything
		 * but a boolean column) is declined: the complement of a posting set
		 * is not a posting set, and under a union there is nothing to
		 * subtract it from.  An arm `flag IS NOT TRUE` is two arms of the
		 * same union, `flag = false` and `flag IS NULL`, and an AND arm with
		 * an OR inside - `flag IS NOT TRUE` among its terms, or any other -
		 * is distributed into the arms it stands for (lion_or_arms()).
		 *
		 * The leaves are appended to the clause array like plain clauses, so
		 * that an index is matched for each of them per partition and a Param
		 * among them reaches custom_exprs; what marks them out is that they
		 * are contiguous and named by an entry in `ors`.  They constrain no
		 * column of the RESULT, so none of the attnum bookkeeping below
		 * (posattnos, eqattnos, nonnullattnos, nullattnos) takes them: the
		 * other arm may hold rows where this arm's column is NULL, or is
		 * anything at all.
		 */
		if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == OR_EXPR)
		{
			List	   *arms;
			List	   *armlens = NIL;
			int			orfirst = list_length(whereattnos);
			bool		thisrange = false;
			ListCell   *la;

			if (list_length(((BoolExpr *) clause)->args) < 2)
				return;

			arms = lion_or_arms(clause);
			if (arms == NIL)
				return;

			foreach(la, arms)
			{
				List	   *arm = (List *) lfirst(la);
				ListCell   *lb;

				foreach(lb, arm)
				{
					if (!lion_analyze_leaf(root, (Node *) lfirst(lb), rti,
										  false, true, false, &leaf))
						return;

					/*
					 * A range in an arm is a leaf of the union like any other:
					 * the rows whose key lies in it, collected (DESIGN.md §32).
					 */
					if (leaf.kind == LION_CLAUSE_RANGE)
					{
						leaf.kind = LION_CLAUSE_RANGESRC;
						thisrange = true;
					}
					lion_append_clause(&leaf, (Node *) lfirst(lb), true,
									  &whereattnos, &clauseinfos,
									  &whereclauses, &whereconsts,
									  &wherekinds, &whereopnos, &whereinor);
				}
				armlens = lappend_int(armlens, list_length(arm));
			}

			ors = lappend(ors,
						  list_concat(list_make2_int(orfirst,
													 list_length(armlens)),
									  armlens));
			havepositive = true;

			/*
			 * A union holds the §9 pin at every container key only when every
			 * leaf does, which a range collected into memory does not
			 * (DESIGN.md §32).
			 */
			if (thisrange)
				hasrangesrc = true;
			else
				pinnedsrc = true;
			continue;
		}

		if (!lion_analyze_leaf(root, clause, rti, true, true, true, &leaf))
			return;

		/*
		 * A range comparison (DESIGN.md §28) bounds the entry walk that DRIVES
		 * the count - or, on a column that does not drive it, is a bound of a
		 * range taken as a source (§32) - which is decided once everything
		 * else is known.  Any number of them may name one column: they are
		 * ANDed into one range, and they are gathered per column here.  A
		 * strict comparison is never true of NULL, so the column is non-NULL
		 * in every row counted.
		 */
		if (leaf.kind == LION_CLAUSE_RANGE)
		{
			int			pos = list_length(whereattnos);
			int			r = 0;
			ListCell   *lr;

			foreach(lr, rangevars)
			{
				if (((Var *) lfirst(lr))->varattno == leaf.var->varattno)
					break;
				r++;
			}
			if (lr == NULL)
			{
				rangevars = lappend(rangevars, leaf.var);
				rangecls = lappend(rangecls, list_make1(rinfo));
				rangepos = lappend(rangepos, list_make1_int(pos));
			}
			else
			{
				ListCell   *c1 = list_nth_cell(rangecls, r);
				ListCell   *c2 = list_nth_cell(rangepos, r);

				lfirst(c1) = lappend((List *) lfirst(c1), rinfo);
				lfirst(c2) = lappend_int((List *) lfirst(c2), pos);
			}
			nonnullattnos = lappend_int(nonnullattnos,
										(int) leaf.var->varattno);
			lion_append_clause(&leaf, clause, false,
							  &whereattnos, &clauseinfos, &whereclauses,
							  &whereconsts, &wherekinds, &whereopnos,
							  &whereinor);
			continue;
		}

		/*
		 * Which index answers the clause, whether its opfamily has the
		 * operator as strategy 1, and whether it can hash and compare the
		 * constant's type is settled per relation, in lion_match_index():
		 * with partitions there is one index per partition and they need not
		 * share an opclass (DESIGN.md §16).
		 */

		if (LION_CLAUSE_IS_POSITIVE(leaf.kind))
		{
			havepositive = true;
			pinnedsrc = true;
		}

		if (LION_CLAUSE_IS_POSITIVE(leaf.kind) && leaf.kind != LION_CLAUSE_MULTI)
		{
			/*
			 * A second positive clause on a column is a source of its own,
			 * ANDed with the first exactly as a clause on another column is:
			 * it is matched to an index that answers ITS operator under ITS
			 * collation (lion_match_index(), per relation), and the AND of
			 * two exact sources is exact.  Only the very same clause again -
			 * the same kind, the same operator, the same input collation and
			 * an equal() value - is dropped, because the lookup the first one
			 * makes is then its answer too.
			 *
			 * Anything less than all four is not "the same clause", and a
			 * clause dropped here is one no index is ever asked about (the
			 * 2026-09-25 review).  Comparing the kind and the value alone
			 * called `v === 'A' AND v = 'A'` a duplicate over an index whose
			 * case-insensitive opclass answers `===` - and nothing at all
			 * answered `=` - so both spellings were counted, 10000 rows
			 * where the query selects 5000.  A nondeterministic collation
			 * does it with one operator: `s COLLATE ci = 'a' AND s = 'a'`
			 * differ in nothing but their input collation.  Now the second
			 * clause needs an index of its own and the query is declined
			 * when there is none.
			 *
			 * Two clauses that differ only in value used to be declined too,
			 * on the grounds that they select little or nothing.  They are
			 * answered now, because nothing in the AND is specific to one
			 * clause per column: the entry a GROUP BY or an IN list drives
			 * is the FIRST suitable clause on its index (lion_inlist_shape()
			 * and lion_locate_where() agree on that), the key a target list
			 * prints is the first pinning clause's, and every clause on the
			 * column must then be one whose index may print it.  Under a
			 * coarse equality they need not even disagree: `v === 'a' AND
			 * v === 'A'` is one entry, looked up twice.
			 *
			 * `IS NOT NULL` is not subject to any of this - it constrains
			 * nothing by itself and is simply subtracted - and neither is a
			 * multi-key clause, whose sources intersect exactly as two
			 * clauses on different columns do (`tags @> '{a}' AND
			 * tags && '{b,c}'` is one AND of three key sets).  Nor is a leaf
			 * of an OR, which says nothing about the rows the OTHER arms
			 * select and so cannot stand in for a clause that does.
			 */
			if (list_member_int(posattnos, (int) leaf.var->varattno))
			{
				ListCell   *l1;
				ListCell   *l2;
				ListCell   *l3;
				bool		same = false;

				forthree(l1, clauseinfos, l2, whereconsts, l3, whereinor)
				{
					LionClauseInfo *prev = (LionClauseInfo *) lfirst(l1);

					if (lfirst_int(l3) != 0 ||
						prev->attno != leaf.var->varattno)
						continue;
					if (prev->kind == leaf.kind &&
						prev->opno == leaf.opno &&
						prev->collation == leaf.collation &&
						equal(lfirst(l2), leaf.val))
					{
						same = true;
						break;
					}
				}
				if (same)
					continue;
			}
			else
				posattnos = lappend_int(posattnos, (int) leaf.var->varattno);
		}

		switch (leaf.kind)
		{
			case LION_CLAUSE_EQ:
				eqattnos = lappend_int(eqattnos, (int) leaf.var->varattno);
				nonnullattnos = lappend_int(nonnullattnos,
											(int) leaf.var->varattno);
				break;
			case LION_CLAUSE_NOTNULL:
				if (notnullvar == NULL)
					notnullvar = leaf.var;
				nonnullattnos = lappend_int(nonnullattnos,
											(int) leaf.var->varattno);
				break;
			case LION_CLAUSE_ARRAY:
			case LION_CLAUSE_MULTI:
				/* a strict operator with a non-NULL constant */
				nonnullattnos = lappend_int(nonnullattnos,
											(int) leaf.var->varattno);
				break;
			case LION_CLAUSE_NULL:
				nullattnos = lappend_int(nullattnos, (int) leaf.var->varattno);
				break;
		}

		lion_append_clause(&leaf, clause, false,
						  &whereattnos, &clauseinfos, &whereclauses,
						  &whereconsts, &wherekinds, &whereopnos, &whereinor);
	}

	/*
	 * The FK-side join's fact filters are all sources, ANDed with every fk
	 * set, so every range among them is a range taken as a source (DESIGN.md
	 * §32); the fk set of each dimension row is what carries the §9 pin.
	 */
	if (fj != NULL && rangevars != NIL)
	{
		lion_ranges_to_sources(rangepos, -1, wherekinds, clauseinfos);
		hasrangesrc = true;
		havepositive = true;
	}

	/*
	 * ---- the FK-side join (DESIGN.md §27) ----
	 *
	 * The fact rel's clauses are all ones the posting sets answer; the join
	 * key, the target list and the path are the join's own business.
	 */
	if (fj != NULL)
	{
		lion_try_fkjoin_path(root, input_rel, output_rel, extra, fj, having,
							 whereattnos, clauseinfos, whereclauses,
							 whereconsts, wherekinds, whereopnos, whereinor,
							 ors);
		return;
	}

	/* ---- the grouped relation's target, and the HAVING that filters it ---- */
	checkexprs = list_copy(output_rel->reltarget->exprs);
	if (having != NIL)
	{
		/*
		 * A SubPlan would need the node to run a subquery per group; an
		 * uncorrelated one is an InitPlan by now and arrives as a Param,
		 * which is a plain value here.
		 */
		if (contain_subplans((Node *) having))
			return;
		checkexprs = list_concat(checkexprs,
								 pull_var_clause((Node *) having,
												 PVC_INCLUDE_AGGREGATES |
												 PVC_RECURSE_WINDOWFUNCS |
												 PVC_INCLUDE_PLACEHOLDERS));
	}

	/*
	 * ---- count(DISTINCT k) (DESIGN.md §26) ----
	 *
	 * Found first, because it decides what drives the scan: k's own entries
	 * when there is no GROUP BY, and the inner side of the (g, k) nested loop
	 * when there is one.  Every distinct aggregate has to be over the same
	 * plain column, with the same equality and collation - one k, one walk -
	 * and k may be neither a grouping column (its entries would be groups and
	 * distinct values at once) nor the third driving column of a two-column
	 * GROUP BY.  A partitioned table declines: distinct counts do not add up
	 * across partitions, and the partial aggregates of §16 would add them.
	 */
	foreach(lc, checkexprs)
	{
		Aggref	   *agg = (Aggref *) lfirst(lc);
		Var		   *dv;
		Oid			deq;
		Oid			dcoll;

		if (!IsA(agg, Aggref) || agg->aggdistinct == NIL)
			continue;
		if (!lion_agg_distinct_var(agg, rti, &dv, &deq, &dcoll))
			return;

		/*
		 * A column the relation proves unique - a primary key, say - is not
		 * k at all: its distinct count is the count(col) of the same rows
		 * (lion_agg_distinct_unique()), answered wherever that count(col) is
		 * - which is every row count when col is known non-NULL, and the
		 * other cases of DESIGN.md §14.  Nothing is walked for it, so neither
		 * partitions nor a second distinct column stand in its way.  One
		 * whose count(col) the node cannot answer (a nullable unique column
		 * nothing proves non-NULL) is left to be walked as k, as before.
		 */
		if (lion_column_is_unique(input_rel, dv->varattno, deq, dcoll) &&
			lion_agg_is_count(root, agg, rti, input_rel, groupattno, ngroup,
							  nonnullattnos, nullattnos))
		{
			uniqattnos = list_append_unique_int(uniqattnos,
												(int) dv->varattno);
			continue;
		}
		if (distvar == NULL)
		{
			distvar = dv;
			disteqop = deq;
			distcoll = dcoll;
		}
		else if (dv->varattno != distvar->varattno || deq != disteqop ||
				 dcoll != distcoll)
			return;
	}
	if (distvar != NULL)
	{
		/*
		 * The plan tells the two kinds apart by their column alone
		 * (lion_plan_custom_path()), so one column is not both: a distinct
		 * count proved unique under one collation beside a walked one under
		 * another is left to the ordinary plan.
		 */
		if (list_member_int(uniqattnos, (int) distvar->varattno))
			return;
		if (partitioned || ngroup > 1)
			return;
		/* a coalesce group is §10's walk, not the (g, k) loop of §26 */
		if (groupcoal != NULL)
			return;
		for (g = 0; g < ngroup; g++)
		{
			if (groupattno[g] == distvar->varattno)
				return;
		}
	}

	foreach(lc, checkexprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *v = (Var *) node;

			if (v->varno != (int) rti || v->varattno <= 0 ||
				v->varlevelsup != 0)
				return;

			/*
			 * Under GROUP BY coalesce(g, c) the group's value is the whole
			 * expression, which the node prints, and never g by itself: a
			 * bare g can only reach here out of a HAVING that takes the
			 * expression apart, and the key the node holds for the merged
			 * group is not g's value in all of its rows.
			 */
			if (groupcoal != NULL && v->varattno == groupattno[0])
				return;

			/*
			 * The column has to be either the group key or one a clause pins
			 * to a single value.  In the second case the value we output is
			 * the key the index stored, not the constant from the query: a
			 * cross-type or otherwise non-identical constant that compares
			 * equal must not change what the query prints.  A column pinned
			 * to NULL by `IS NULL` prints NULL, which the stored key of the
			 * reserved entry says as well.
			 *
			 * Either way the value comes out of an index entry, so the index
			 * has to be one whose entries can produce it - which is decided
			 * per relation, once the indexes are known.  `IS NULL` is exempt:
			 * NULL has one representation.
			 */
			for (g = 0; g < ngroup; g++)
			{
				if (v->varattno == groupattno[g])
					break;
			}
			if (g < ngroup)
				groupvalueout[g] = true;
			else if (list_member_int(eqattnos, (int) v->varattno))
			{
				if (!list_member_int(valueattnos, (int) v->varattno))
					valueattnos = lappend_int(valueattnos, (int) v->varattno);
			}
			else if (!list_member_int(nullattnos, (int) v->varattno))
				return;
		}
		else if (groupcoal != NULL && equal(node, groupexpr))
		{
			/* the coalesce group's value, printed as the node emits it */
			groupvalueout[0] = true;
		}
		else if (IsA(node, Aggref))
		{
			Aggref	   *agg = (Aggref *) node;
			AttrNumber	countcols[LION_MAX_GROUPCOLS + 1];
			int			ncountcols = ngroup;
			Node	   *arg = (agg->args != NIL) ?
				lion_strip((Node *) ((TargetEntry *) linitial(agg->args))->expr) :
				NULL;
			AttrNumber	argattno = (arg != NULL && IsA(arg, Var)) ?
				((Var *) arg)->varattno : 0;

			if (agg->aggdistinct != NIL && distvar != NULL &&
				argattno == distvar->varattno)
			{
				/* checked above; a distinct count needs no other test */
				haveagg = true;
				continue;
			}

			/*
			 * GROUP BY coalesce(g, c) counts g's NULL rows in c's group
			 * (DESIGN.md §10), where count(g) - or a count(DISTINCT g) proved
			 * to be it - counts only the rest, a number the node does not
			 * keep apart.  So it is answered only where no row it counts has
			 * g NULL: g declared NOT NULL, or a clause of the WHERE that says
			 * so, and then there is nothing to merge and it is the group's
			 * count.
			 */
			if (groupcoal != NULL && argattno != 0 &&
				argattno == groupattno[0] &&
				!list_member_int(nonnullattnos, (int) argattno) &&
				!bms_is_member(argattno, lion_notnullattnums(root, input_rel)))
				return;

			/*
			 * count(k) of the distinct column is answered too, whatever k's
			 * nullability: it is the rows of k's non-NULL entries, which the
			 * walk visits anyway (DESIGN.md §26) - so for this purpose k is
			 * one more column whose entries are known, like a group column.
			 */
			memcpy(countcols, groupattno, sizeof(AttrNumber) * ngroup);
			if (distvar != NULL)
				countcols[ncountcols++] = distvar->varattno;
			if (!lion_agg_is_count(root, agg, rti, input_rel,
								  countcols, ncountcols, nonnullattnos,
								  nullattnos))
				return;
			haveagg = true;

			/*
			 * Does the walk have to COUNT rather than test for existence?
			 * Without a GROUP BY every other count is a sum over k's entries;
			 * beside one, only count(k) is a sum over the pairs, and count(*)
			 * and count(g) are the group's own count.
			 */
			if (distvar != NULL)
			{
				bool		ofk = (argattno == distvar->varattno);

				if (ngroup == 0 || ofk)
					distcounts = true;
			}
		}
		else
			return;
	}
	if (!haveagg)
		return;

	/* Something has to drive the count. */
	driveattno = groupattno[0];
	if (distvar != NULL && ngroup == 0)
	{
		/* k's entries drive it, whatever the WHERE says (DESIGN.md §26). */
		driveattno = distvar->varattno;
	}

	/*
	 * Which range bounds the walk that DRIVES the count (DESIGN.md §28): the
	 * one on the column the GROUP BY or the count(DISTINCT) walks, or - with
	 * neither - the WIDEST range, whose sum is then the count: it walks the
	 * fewer sets of the two sides (the complement, and the summaries of §32,
	 * make a wide walk short) where a range taken as a source is collected
	 * row by row.  A column with an equality, a list or a null test of its
	 * own does not drive: those are positive clauses on it, which §10 leaves
	 * to the ordinary merge.  Every other range is a source (§32).
	 */
	if (rangevars != NIL)
	{
		int			best = -1;
		double		bestsel = -1.0;
		int			r = 0;
		ListCell   *l1;
		ListCell   *l2;

		forboth(l1, rangevars, l2, rangecls)
		{
			Var		   *v = (Var *) lfirst(l1);

			if (!list_member_int(posattnos, (int) v->varattno) && ngroup <= 1)
			{
				if (ngroup == 1 || distvar != NULL)
				{
					if (v->varattno == driveattno)
						best = r;
				}
				else
				{
					Selectivity sel = clauselist_selectivity(root,
															 (List *) lfirst(l2),
															 rti, JOIN_INNER,
															 NULL);

					if (sel > bestsel)
					{
						bestsel = sel;
						best = r;
					}
				}
			}
			r++;
		}
		if (best >= 0)
		{
			rangevar = (Var *) list_nth(rangevars, best);
			rangeclauses = (List *) list_nth(rangecls, best);
		}
		if (list_length(rangevars) > (best >= 0 ? 1 : 0))
		{
			lion_ranges_to_sources(rangepos, best, wherekinds, clauseinfos);
			hasrangesrc = true;
			havepositive = true;
		}
	}

	if (distvar != NULL && ngroup == 0)
	{
		/* driven by k's entries, above */
	}
	else if (ngroup == 0 && rangevar != NULL)
	{
		/*
		 * A range on k with no GROUP BY (DESIGN.md §28): the sum over k's
		 * entries in the range, each intersected with the other clauses -
		 * §14's sum-over-all with the walk bounded.  The entries of one
		 * scalar index are disjoint, so the sum is the count of their union,
		 * which is exactly the rows whose k is in the range.
		 */
		sumall = true;
		driveattno = rangevar->varattno;
	}
	else if (ngroup == 0 && !havepositive)
	{
		/*
		 * Only `IS NOT NULL` clauses: that column's index knows every row of
		 * the table, so the count is the sum over all of its entries with the
		 * NULL one subtracted (DESIGN.md §14).  The entries of one index are
		 * disjoint - a row has one value per column - so summing them is the
		 * count of their union.
		 */
		if (notnullvar == NULL)
			return;
		sumall = true;
		driveattno = notnullvar->varattno;
	}
	/* A group folded to a constant can only have come from a WHERE key. */
	if (singlegroup && !havepositive)
		return;

	/*
	 * A range taken as a source (DESIGN.md §32) is collected into memory and
	 * holds no pin, so every count it takes part in needs another source that
	 * carries the §9 interlock, or it would recheck every row in the heap: the
	 * walk that drives the count, or a clause - or an OR of clauses - outside
	 * the ranges.  Without one the query is left to the ordinary plan.
	 */
	if (hasrangesrc &&
		!(ngroup > 0 || sumall || distvar != NULL || pinnedsrc))
		return;

	/*
	 * The range left bounding a walk has to bound the one that DRIVES the
	 * count (DESIGN.md §28): k's entries under a GROUP BY k, a count(DISTINCT
	 * k) or the sum above, or g's under `g, count(DISTINCT k) ... GROUP BY g`.
	 * Every other range was made a source above (§32), so this only guards
	 * against the two halves of this function drifting apart.
	 */
	if (rangevar != NULL &&
		(ngroup > 1 || driveattno != rangevar->varattno))
		return;

	/*
	 * Which clauses have to produce a value, rather than just select rows: a
	 * value-producing pushdown needs an index whose stored keys are a
	 * representation the rows themselves have (finding 4 of the 2026-09-20
	 * review), and that is checked per relation below.
	 */
	foreach(lc, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);

		ci->valueout = (ci->kind == LION_CLAUSE_EQ &&
						list_member_int(valueattnos, (int) ci->attno));
	}

	/*
	 * ---- the relations to count, and the indexes on each of them ----
	 *
	 * One table, or one live leaf partition at a time (DESIGN.md §16).  The
	 * column numbers above are the parent's; lion_collect_targets() maps them
	 * onto each partition through its AppendRelInfo before looking an index
	 * up, because partitions may number their columns differently.
	 */
	memset(drive, 0, sizeof(drive));

	/*
	 * How many entries each grouping column's index has, which is what
	 * decides the outer/inner roles of the nested loop (DESIGN.md §20) and
	 * what the cost model multiplies: the column with FEWER distinct values
	 * drives the scan, so the other index's keys - which are read once and
	 * probed per pair - are the ones whose bucket lookups are repeated.
	 */
	for (g = 0; g < ngroup; g++)
		groupest[g] = estimate_num_groups(root, list_make1(groupvar[g]),
										  input_rel->rows, NULL, NULL);
	if (ngroup == 2 && groupest[1] < groupest[0])
	{
		Var		   *tv = groupvar[0];
		AttrNumber	ta = groupattno[0];
		Oid			te = groupeqop[0];
		bool		tvo = groupvalueout[0];
		double		tn = groupest[0];

		groupvar[0] = groupvar[1];
		groupattno[0] = groupattno[1];
		groupeqop[0] = groupeqop[1];
		groupvalueout[0] = groupvalueout[1];
		groupest[0] = groupest[1];
		groupvar[1] = tv;
		groupattno[1] = ta;
		groupeqop[1] = te;
		groupvalueout[1] = tvo;
		groupest[1] = tn;
		driveattno = groupattno[0];
	}

	if (ngroup > 0)
	{
		for (g = 0; g < ngroup; g++)
		{
			drive[g].attno = groupattno[g];
			drive[g].var = groupvar[g];
			drive[g].collation = groupvar[g]->varcollid;
			drive[g].eqop = groupeqop[g];
			drive[g].valueout = groupvalueout[g];
		}
		ndrive = ngroup;
	}
	else if (sumall)
	{
		/* §14's sum-over-all: one index, and it groups nothing. */
		drive[0].attno = driveattno;
		drive[0].var = (rangevar != NULL) ? rangevar : notnullvar;
		drive[0].collation = InvalidOid;
		drive[0].eqop = InvalidOid;
		drive[0].valueout = false;
		ndrive = 1;
	}

	/*
	 * The column count(DISTINCT) counts is one more driving column (DESIGN.md
	 * §26): the only one without a GROUP BY, the inner one beside it.  Every
	 * per-column rule of lion_collect_targets() applies to it as to a grouping
	 * column - a SCALAR index (a multi-key one is never found, and
	 * count(DISTINCT tags) counts arrays, not elements), the DISTINCT's
	 * collation, and the DISTINCT's own equality as strategy 1 of the index's
	 * opfamily, so that its entries are exactly the classes DISTINCT counts.
	 * No value of it is ever printed.
	 */
	if (distvar != NULL)
	{
		Assert(ndrive == ngroup && ndrive < LION_MAX_GROUPCOLS);
		drive[ndrive].attno = distvar->varattno;
		drive[ndrive].var = distvar;
		drive[ndrive].collation = distcoll;
		drive[ndrive].eqop = disteqop;
		drive[ndrive].valueout = false;
		ndrive++;

		/*
		 * Every entry of k is tested, whether or not the WHERE leaves it any
		 * row, so the walk is n_distinct(k) of the whole table long.
		 */
		distest = estimate_num_groups(root, list_make1(distvar),
									  Max(input_rel->tuples, 1.0), NULL, NULL);
	}

	/*
	 * The share of the driving column's entries the range walks (DESIGN.md
	 * §28): the selectivity of the range clauses alone, which for a column
	 * whose rows spread evenly over its values is also the share of its
	 * values, and overstates the walk for a skewed one.  A count(DISTINCT k)
	 * over a range on k tests exactly those entries.
	 */
	if (rangevar != NULL)
	{
		rangesel = clauselist_selectivity(root, rangeclauses, rti,
										  JOIN_INNER, NULL);
		if (distvar != NULL && ngroup == 0)
			distest = lion_range_entries(root, input_rel, distvar, rangesel);
	}

	if (!lion_collect_targets(root, input_rel, drive, ndrive, whereattnos,
							 clauseinfos, &targets))
		return;
	if (targets == NIL)
		return;					/* everything was pruned: leave it to the planner */
	first = (LionCountTarget *) linitial(targets);

	/* ---- build the path ---- */
	if (ngroup == 1)
		numgroups = groupest[0];
	else if (ngroup == 2)
	{
		numgroups = estimate_num_groups(root,
										list_make2(groupvar[0], groupvar[1]),
										input_rel->rows, NULL, NULL);
	}
	else if (sumall && rangevar != NULL)
	{
		/* Every entry in the range is visited (DESIGN.md §28). */
		numgroups = lion_range_entries(root, input_rel, rangevar, rangesel);
	}
	else if (sumall)
	{
		/* Every entry of the driving index is visited, one group or not. */
		Assert(notnullvar != NULL);
		numgroups = estimate_num_groups(root, list_make1(notnullvar),
										input_rel->rows, NULL, NULL);
	}
	else if (distvar != NULL)
	{
		/* ... and so is every entry of k's (DESIGN.md §26). */
		numgroups = distest;
	}
	else
		numgroups = 1.0;

	/*
	 * How many rows the node itself produces.  One per group, except for a
	 * partitioned GROUP BY, which emits one PARTIAL row per group per
	 * partition and lets the Finalize Agg above combine them (DESIGN.md §16):
	 * that is the sum of the partitions' own group estimates, each made
	 * against the partition's statistics and capped by its row count.
	 */
	if (ngroup == 0)
		outrows = 1.0;
	else if (!partitioned)
		outrows = numgroups;
	else
	{
		outrows = 0.0;
		foreach(lc, targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);
			double		relrows = Max(t->rel->rows, 1.0);
			double		relgroups;

			if (t->drivevar[0] != NULL && ngroup == 2 &&
				t->drivevar[1] != NULL)
				relgroups = estimate_num_groups(root,
												list_make2(t->drivevar[0],
														   t->drivevar[1]),
												relrows, NULL, NULL);
			else if (t->drivevar[0] != NULL)
				relgroups = estimate_num_groups(root,
												list_make1(t->drivevar[0]),
												relrows, NULL, NULL);
			else
				relgroups = numgroups;
			outrows += Min(relgroups, relrows);
		}
		outrows = Max(outrows, 1.0);

		/*
		 * ... and those rows are partial aggregates, so the node's target is
		 * the partially-grouped one and the grouped rel gets a Finalize Agg
		 * over it further down.
		 */
		partialtarget = lion_make_partial_target(root, output_rel->reltarget,
												 having);
	}

	/*
	 * A plain table's own Oids go in LION_PRIV_OIDS, which is where the
	 * executor and EXPLAIN have always read them.  A partitioned one leaves
	 * them invalid - there is no single index - and fills LION_PRIV_PARTS
	 * instead, one OidList per partition in the same clause order.
	 */
	oids = list_make3_oid(rte->relid,
						  (!partitioned && first->driveidx[0] != NULL) ?
						  first->driveidx[0]->indexoid : InvalidOid,
						  (!partitioned && first->driveidx[1] != NULL) ?
						  first->driveidx[1]->indexoid : InvalidOid);
	ints = list_make4_int((int) rti, (int) groupattno[0], (int) groupattno[1],
						  (singlegroup ? LION_FLAG_SINGLEGROUP : 0) |
						  (sumall ? LION_FLAG_SUMALL : 0) |
						  (driveattno != 0 ? LION_FLAG_GROUPIDX : 0) |
						  (rangevar != NULL ? LION_FLAG_RANGE : 0));
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;
		int			i = 0;

		forthree(l1, whereattnos, l2, whereconsts, l3, wherekinds)
		{
			oids = lappend_oid(oids, partitioned ? InvalidOid :
							   ((IndexOptInfo *) list_nth(first->whereidx,
														  i))->indexoid);
			ints = lappend_int(ints, lfirst_int(l1));
			consts = lappend(consts, copyObject((Node *) lfirst(l2)));
			ckinds = lappend_int(ckinds, lfirst_int(l3));
			i++;
		}
	}

	if (partitioned)
	{
		foreach(lc, targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);
			List	   *one;
			ListCell   *l1;

			one = list_make3_oid(t->heapoid,
								 t->driveidx[0] ? t->driveidx[0]->indexoid :
								 InvalidOid,
								 t->driveidx[1] ? t->driveidx[1]->indexoid :
								 InvalidOid);
			foreach(l1, t->whereidx)
				one = lappend_oid(one, ((IndexOptInfo *) lfirst(l1))->indexoid);
			parts = lappend(parts, one);
		}
	}

	/*
	 * The strategy of a multi-key clause travels with its operator: the
	 * executor re-extracts the query and EXPLAIN prints the operator's name,
	 * and both need the Oid.
	 */

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = partialtarget ? partialtarget :
		output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	/*
	 * PATHKEYS (DESIGN.md §21).  A single-column GROUP BY driven by the
	 * index's own entry scan emits its groups in DIRECTORY order, which for an
	 * index that orders by the key type's own comparison is exactly what an
	 * `ORDER BY <group col>` asks for - so the Sort above the node disappears.
	 *
	 * Four things disqualify it:
	 *
	 *	- two group columns: the nested loop of §20 emits (outer, inner) pairs,
	 *	  which are sorted by the outer key alone and not by the pair;
	 *	- an IN list driving the groups (§15): the node walks the located sets
	 *	  rather than the index, and claiming an order for that would tie the
	 *	  planner to a detail of how the list is located;
	 *	- a partitioned table: each partition is ordered, but the Finalize
	 *	  HashAggregate core puts on top destroys it (§16);
	 *	- a NULLABLE group column, because the reserved NULL entry sorts
	 *	  FIRST and `ORDER BY col` means NULLS LAST.  A column the planner
	 *	  knows is NOT NULL has no NULL group to emit, so the two agree;
	 *	- and GROUP BY coalesce(col, c) (DESIGN.md §10), whose group c the
	 *	  walk holds back and emits after the last entry, wherever c sorts.
	 */
	cpath->path.pathkeys = NIL;
	if (ngroup == 1 && !partitioned && !sumall && !singlegroup &&
		groupcoal == NULL &&
		first->driveidx[0] != NULL &&
		bms_is_member(groupattno[0], lion_notnullattnums(root, input_rel)) &&
		lion_index_orders_naturally(first->driveidx[0], first->drivecol[0]))
	{
		bool		sumshort;
		bool		groupdrive;
		Oid			sortop = InvalidOid;
		Oid			eqop = InvalidOid;
		bool		hashable;

		(void) lion_inlist_shape(first->driveidx[0], first->drivecol[0],
								 first->driveidx[1],
								 first->whereidx, first->wherecol,
								 whereclauses, wherekinds,
								 ors, false, &sumshort, &groupdrive);
		/*
		 * The node emits ASCENDING, NULLS FIRST, always.  The query's own
		 * GROUP BY clause is not a safe source for the direction:
		 * standard_qp_callback() rewrites its sort operators to match the
		 * query's ORDER BY when it can, so `ORDER BY k DESC` would hand us a
		 * descending SortGroupClause and we would claim an order the node does
		 * not produce.  So the ordering operator is the key type's own `<`,
		 * and the clause is copied only for its sortgroupref and its equality.
		 */
		get_sort_group_operators(exprType((Node *) groupvar[0]),
								 true, true, false,
								 &sortop, &eqop, NULL, &hashable);

		if (!groupdrive && OidIsValid(sortop))
		{
			SortGroupClause *sgc;

			sgc = copyObject((SortGroupClause *)
							 linitial(root->processed_groupClause));
			sgc->sortop = sortop;
			sgc->nulls_first = false;
			cpath->path.pathkeys =
				make_pathkeys_for_sortclauses(root, list_make1(sgc),
											  root->processed_tlist);
		}
	}
	cpath->flags = 0;
	cpath->custom_paths = NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	/*
	 * The shape marker comes first, so that lion_begin_custom_scan() can
	 * refuse a list it does not recognise instead of reading it positionally.
	 */
	cpath->custom_private = list_make1(list_make2_int(LION_PRIV_MAGIC,
													  LION_PRIV_NMEMBERS));
	cpath->custom_private = lappend(cpath->custom_private, oids);
	cpath->custom_private = lappend(cpath->custom_private, ints);
	cpath->custom_private = lappend(cpath->custom_private, consts);
	cpath->custom_private = lappend(cpath->custom_private, ckinds);
	cpath->custom_private = lappend(cpath->custom_private, parts);
	cpath->custom_private = lappend(cpath->custom_private, whereopnos);
	cpath->custom_private = lappend(cpath->custom_private, ors);
	cpath->custom_private = lappend(cpath->custom_private,
									(partialtarget != NULL) ? NIL : having);
	cpath->custom_private = lappend(cpath->custom_private,
									(distvar != NULL) ?
									list_make1_int((int) distvar->varattno) :
									NIL);
	cpath->custom_private = lappend(cpath->custom_private, NIL);	/* join */
	cpath->custom_private = lappend(cpath->custom_private,
									lion_replaced_functions(input_rel,
															output_rel->reltarget->exprs,
															having,
															root->processed_groupClause,
															NULL));

	/*
	 * GROUP BY coalesce(col, c) (DESIGN.md §10): c, and the grouping equality
	 * the walk tells c's own entry by, under the grouping collation - which
	 * lion_collect_targets() has made the driving index's strategy 1 and its
	 * collation, per relation.
	 */
	cpath->custom_private =
		lappend(cpath->custom_private,
				(groupcoal != NULL) ?
				list_make2(copyObject(groupcoal),
						   list_make2_oid(groupeqop[0],
										  groupvar[0]->varcollid)) :
				NIL);
	cpath->methods = &lion_count_path_methods;

	/*
	 * Beside a GROUP BY, count(DISTINCT k) is the (g, k) nested loop of
	 * DESIGN.md §20, g outer and k inner, and is priced as those pairs
	 * (DESIGN.md §26); without one, k's entries are the "groups" of the walk.
	 */
	lion_cost_count_path(root, cpath, targets, whereclauses, wherekinds, ors,
						numgroups, groupest[0],
						(ngroup == 2) ? groupest[1] :
						(distvar != NULL && ngroup == 1) ? distest : 0,
						outrows,
						(distvar == NULL) ? LION_DISTINCT_NONE :
						distcounts ? LION_DISTINCT_COUNT :
						LION_DISTINCT_EXISTS,
						(rangevar == NULL) ?
						((sumall && distvar == NULL) ? LION_RANGED_SUMALL :
						 LION_RANGED_NONE) :
						sumall ? LION_RANGED_SUM : LION_RANGED_WALK,
						rangesel);

	/*
	 * The HAVING the node applies itself costs an evaluation per group and
	 * lets a fraction of the groups through: the same accounting cost_agg()
	 * does for an Agg's quals, so that the two plans stay comparable.  A
	 * partitioned table's HAVING is the Finalize Agg's and is priced there.
	 */
	if (having != NIL && partialtarget == NULL)
	{
		QualCost	qual_cost;
		double		groups = cpath->path.rows;

		cost_qual_eval(&qual_cost, having, root);
		cpath->path.startup_cost += qual_cost.startup;
		cpath->path.total_cost += qual_cost.startup +
			groups * qual_cost.per_tuple;
		cpath->path.rows = clamp_row_est(groups *
										 clauselist_selectivity(root, having,
																0, JOIN_INNER,
																NULL));
	}

	/*
	 * A partitioned GROUP BY produces partial aggregates, so what goes into
	 * the grouped rel is core's Finalize HashAggregate over the node - which
	 * combines the per-partition counts and, unlike anything this node could
	 * hold, spills to disk when the groups do not fit in hash_mem
	 * (DESIGN.md §16).  Everything else is already the finished answer.
	 */
	if (partialtarget != NULL)
	{
		AggClauseCosts agg_final_costs;

		MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
		get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);

		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, &cpath->path,
								 output_rel->reltarget,
								 AGG_HASHED, AGGSPLIT_FINAL_DESERIAL,
								 root->processed_groupClause,
								 having,
								 &agg_final_costs,
								 numgroups));
		return;
	}

	add_path(output_rel, &cpath->path);
}

void
lion_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	if (lion_prev_create_upper_paths_hook != NULL)
		lion_prev_create_upper_paths_hook(root, stage, input_rel, output_rel,
										 extra);

	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (!lion_enable_count_pushdown)
		return;

	/*
	 * No lion index may be read while PostgreSQL 16's old_snapshot_threshold
	 * is set (DESIGN.md §9, lion_check_old_snapshot()), so there is no count
	 * to push down, into one table or into a join.
	 */
	if (lion_old_snapshot_threshold_active())
		return;

	/*
	 * A join of two tables may be the FK-side join of DESIGN.md §27, either
	 * way round; each orientation that qualifies is tried, and the cost model
	 * chooses among them and the ordinary plan.
	 */
	if (input_rel->reloptkind == RELOPT_JOINREL)
	{
		LionFkJoin	fj[2];
		int			nfj = lion_fkjoin_recognize(root, input_rel, fj);
		int			i;

		for (i = 0; i < nfj; i++)
			lion_try_count_path(root, input_rel, output_rel,
							   (GroupPathExtraData *) extra, &fj[i]);
		return;
	}

	/*
	 * A range past the histogram's ends is priced with the ends the
	 * directory holds (DESIGN.md §28, "The endpoint probe").
	 */
	if (!lion_probe_begin(root, input_rel, input_rel->baserestrictinfo))
	{
		lion_try_count_path(root, input_rel, output_rel,
						   (GroupPathExtraData *) extra, NULL);
		return;
	}
	PG_TRY();
	{
		lion_try_count_path(root, input_rel, output_rel,
						   (GroupPathExtraData *) extra, NULL);
	}
	PG_FINALLY();
	{
		lion_probe_end();
	}
	PG_END_TRY();
}

/*
 * The position of a dimension column in the child plan's target list, which
 * the executor reads it from (DESIGN.md §27).  The child was planned with
 * CP_EXACT_TLIST from the dimension rel's own target, which holds every
 * dimension column anything above the scan needs, so a column that is not
 * there is planner drift and not a query to decline.
 */
static AttrNumber
lion_child_resno(Plan *child, Var *var)
{
	ListCell   *lc;

	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Var		   *cv = (Var *) tle->expr;

		if (cv != NULL && IsA(cv, Var) && cv->varno == var->varno &&
			cv->varattno == var->varattno && cv->varlevelsup == 0)
			return tle->resno;
	}

	elog(ERROR, "LionCount: column %d of relation %d is not in the join's child plan",
		 (int) var->varattno, (int) var->varno);
	return 0;					/* keep the compiler quiet */
}

/*
 * Turn an FK-side join path (DESIGN.md §27) into a CustomScan.
 *
 * The node's own tuple - custom_scan_tlist - is every dimension column the
 * target list uses (and the join key's, which the join clause's value
 * expression references), each read from the child's current row, plus the
 * partial counts.  The target list itself is the partially-grouped target and
 * may compute expressions over those columns (`upper(d.name)`); setrefs.c
 * rewrites it, and the key's value expression in custom_exprs, into INDEX_VAR
 * references against custom_scan_tlist, and the node's projection evaluates
 * it per row.  HAVING is the Agg's, so there is no qual here.  The rows of a
 * count(DISTINCT) carry no counts, and may carry the fact's join column,
 * which is read from the key's column of the child.
 */
static Plan *
lion_plan_fkjoin_path(PlannerInfo *root, RelOptInfo *rel,
					  CustomPath *best_path, List *tlist, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *child;
	List	   *consts;
	List	   *want;
	List	   *ctlist = NIL;
	List	   *kinds = NIL;
	List	   *priv;
	List	   *join;
	List	   *ints;
	int			joinclause;
	Node	   *keyexpr;
	Var		   *keyvar;
	Index		factrelid;
	AttrNumber	fkattno;
	AttrNumber	keyresno;
	ListCell   *lc;

	if (list_length(custom_plans) != 1)
		elog(ERROR, "LionCount: a join needs exactly one child plan");
	child = (Plan *) linitial(custom_plans);

	join = (List *) list_nth(best_path->custom_private, LION_PRIV_JOIN);
	joinclause = linitial_int(join);
	consts = (List *) list_nth(best_path->custom_private, LION_PRIV_CONSTS);
	keyexpr = (Node *) list_nth(consts, joinclause);
	keyvar = (Var *) lion_strip(keyexpr);
	Assert(keyvar != NULL && IsA(keyvar, Var));
	keyresno = lion_child_resno(child, keyvar);

	/*
	 * The fact's join column, which the rows of a count(DISTINCT) carry as
	 * the key (lion_fkjoin_agg_is_distinct()): the fact rel and the column
	 * are the join clause's.
	 */
	ints = (List *) list_nth(best_path->custom_private, LION_PRIV_INTS);
	factrelid = (Index) linitial_int(ints);
	fkattno = (AttrNumber) list_nth_int(ints, 4 + joinclause);

	want = pull_var_clause((Node *) tlist,
						   PVC_INCLUDE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);
	want = lappend(want, keyvar);

	foreach(lc, want)
	{
		Node	   *expr = (Node *) lfirst(lc);
		int			kind;
		ListCell   *l2;
		bool		dup = false;

		if (IsA(expr, Var) && ((Var *) expr)->varno == (int) factrelid &&
			((Var *) expr)->varattno == fkattno &&
			(lsecond_int(join) == LION_JOIN_INNER) &&
			(lthird_int(join) & LION_JOINFLAG_ROWS) != 0)
			kind = LION_TL_CHILDCOL(keyresno);
		else if (IsA(expr, Var))
			kind = LION_TL_CHILDCOL(lion_child_resno(child, (Var *) expr));
		else if (IsA(expr, Aggref))
			kind = LION_TL_COUNT;	/* every count the planner accepted */
		else
		{
			elog(ERROR, "unexpected expression in LionCount join target list");
			kind = 0;			/* keep the compiler quiet */
		}

		foreach(l2, ctlist)
		{
			if (equal(((TargetEntry *) lfirst(l2))->expr, expr))
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		ctlist = lappend(ctlist,
						 makeTargetEntry((Expr *) copyObject(expr),
										 list_length(ctlist) + 1,
										 NULL, false));
		kinds = lappend_int(kinds, kind);
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;

	/* The clause values go to custom_exprs, as for every other shape. */
	cscan->custom_exprs = consts;
	priv = list_copy(best_path->custom_private);
	lfirst(list_nth_cell(priv, LION_PRIV_CONSTS)) = NIL;
	lfirst(list_nth_cell(priv, LION_PRIV_HAVING)) = NIL;
	lfirst(list_nth_cell(priv, LION_PRIV_JOIN)) =
		lappend_int(list_copy(join), (int) keyresno);

	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->custom_private = lappend(priv, kinds);
	cscan->methods = &lion_count_scan_methods;

	return &cscan->scan.plan;
}

/*
 * Make the name "LionCount" resolvable when a plan is read back - by a
 * parallel worker, above all, which reads the leader's plan before it has
 * planned anything itself (DESIGN.md §27, "Parallel").  From _PG_init.
 */
void
lion_count_scan_register(void)
{
	if (GetCustomScanMethods("LionCount", true) == NULL)
		RegisterCustomScanMethods(&lion_count_scan_methods);
}

/*
 * Turn the path into a CustomScan.
 *
 * scan.scanrelid is 0 because this is an upper node, so custom_scan_tlist has
 * to describe the tuple the node produces and setrefs.c rewrites the plan's
 * targetlist into INDEX_VAR references against it (set_customscan_references).
 */
static Plan *
lion_plan_custom_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *ctlist = NIL;
	List	   *kinds = NIL;
	List	   *priv;
	List	   *ints;
	List	   *ckinds;
	List	   *having;
	List	   *want;
	bool	   *inor;
	AttrNumber	groupattno;
	AttrNumber	groupattno2;
	AttrNumber	distattno;
	List	   *dist;
	ListCell   *lc;

	/*
	 * The name has to be resolvable before any CustomScan node of ours is
	 * written out or read back.  _PG_init registers it
	 * (lion_count_scan_register()); this is for a library loaded some other
	 * way, and registering twice is harmless.
	 */
	lion_count_scan_register();

	if ((List *) list_nth(best_path->custom_private, LION_PRIV_JOIN) != NIL)
		return lion_plan_fkjoin_path(root, rel, best_path, tlist, custom_plans);

	ints =(List *) list_nth(best_path->custom_private, LION_PRIV_INTS);
	ckinds = (List *) list_nth(best_path->custom_private, LION_PRIV_CLAUSEKINDS);
	groupattno = (AttrNumber) lsecond_int(ints);
	groupattno2 = (AttrNumber) lthird_int(ints);
	dist = (List *) list_nth(best_path->custom_private, LION_PRIV_DISTINCT);
	distattno = (dist != NIL) ? (AttrNumber) linitial_int(dist) : 0;

	/*
	 * A leaf of an OR constrains no column of the result (DESIGN.md §19), so
	 * it neither pins a value the target list may print nor makes a
	 * `count(col)` zero: the other arms select rows it says nothing about.
	 */
	inor = lion_or_leaf_map((List *) list_nth(best_path->custom_private,
											  LION_PRIV_ORS),
							list_length(ckinds));

	/*
	 * The tuple the node produces has to hold every column and count the
	 * target list prints AND every one the HAVING compares: setrefs.c
	 * rewrites both into references to custom_scan_tlist, and an aggregate
	 * it cannot find there would be left as an Aggref, which no executor
	 * node but Agg can evaluate.  A count the HAVING alone mentions becomes
	 * a column the projection above simply does not print.
	 */
	having = (List *) list_nth(best_path->custom_private, LION_PRIV_HAVING);
	want = list_copy(tlist);
	if (having != NIL)
	{
		List	   *refs = pull_var_clause((Node *) having,
										   PVC_INCLUDE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);

		foreach(lc, refs)
			want = lappend(want, makeTargetEntry((Expr *) lfirst(lc),
												 list_length(want) + 1,
												 NULL, true));
	}

	foreach(lc, want)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Node	   *expr = (Node *) tle->expr;
		int			kind;
		ListCell   *l2;
		bool		dup = false;

		if (IsA(expr, Aggref))
		{
			Aggref	   *agg = (Aggref *) expr;
			AttrNumber	attno = 0;

			if (agg->args != NIL)
			{
				Node	   *arg = lion_strip((Node *)
											((TargetEntry *) linitial(agg->args))->expr);

				Assert(arg != NULL && IsA(arg, Var));
				attno = ((Var *) arg)->varattno;
			}

			/*
			 * count(*) and every count(col) the planner accepted are the
			 * count of the group, except two cases DESIGN.md §14 spells out:
			 * count of the group column is 0 in the NULL group, and count of
			 * a column a clause pins to NULL is always 0.
			 *
			 * A count(DISTINCT) is the walked one of DESIGN.md §26 when it
			 * names that column, and otherwise one of a column the planner
			 * proved unique, which is that column's count(col) and takes the
			 * same cases (lion_agg_distinct_unique()): the planner never lets
			 * one column be both.
			 */
			kind = LION_TL_COUNT;
			if (agg->aggdistinct != NIL && distattno != 0 &&
				attno == distattno)
				kind = LION_TL_COUNT_DISTINCT;
			else if (attno != 0)
			{
				int			i;

				if (distattno != 0 && attno == distattno)
					kind = LION_TL_COUNT_DISTCOL;	/* §26: k's non-NULL rows */
				else if (groupattno != 0 && attno == groupattno)
					kind = LION_TL_COUNT_GROUPCOL;
				else if (groupattno2 != 0 && attno == groupattno2)
					kind = LION_TL_COUNT_GROUPCOL2;
				else
				{
					for (i = 0; i < list_length(ckinds); i++)
					{
						if (!inor[i] &&
							list_nth_int(ckinds, i) == LION_CLAUSE_NULL &&
							list_nth_int(ints, 4 + i) == (int) attno)
						{
							kind = LION_TL_COUNT_ZERO;
							break;
						}
					}
				}
			}
		}
		else if (IsA(expr, Var))
		{
			AttrNumber	attno = ((Var *) expr)->varattno;
			int			i;

			if (groupattno != 0 && attno == groupattno)
				kind = LION_TL_GROUPKEY;
			else if (groupattno2 != 0 && attno == groupattno2)
				kind = LION_TL_GROUPKEY2;
			else
			{
				/*
				 * The value printed for the column is the key the clause's
				 * entry stored, so only a clause that pins the column to ONE
				 * value will do: an equality, or an `IS NULL` (whose entry
				 * stores no key and prints NULL).  A column may carry an
				 * `IS NOT NULL` clause as well, and that one says nothing
				 * about the value.
				 */
				kind = -1;
				for (i = 4; i < list_length(ints); i++)
				{
					int			ckind = list_nth_int(ckinds, i - 4);

					if (inor[i - 4])
						continue;	/* §19: an OR leaf pins nothing */
					if (ckind != LION_CLAUSE_EQ && ckind != LION_CLAUSE_NULL)
						continue;
					if (list_nth_int(ints, i) == (int) attno)
					{
						kind = LION_TL_WHEREKEY + (i - 4);
						break;
					}
				}
				if (kind < 0)
					elog(ERROR, "LionCount: column %d is neither grouped nor constrained",
						 attno);
			}
		}
		else if (IsA(expr, CoalesceExpr) &&
				 (List *) list_nth(best_path->custom_private,
								   LION_PRIV_COALESCE) != NIL)
		{
			/*
			 * GROUP BY coalesce(col, c) (DESIGN.md §10): the only expression
			 * the planner lets through is the grouping one, and the node
			 * emits its value - the entry's key, or c for the group of the
			 * NULL entry and c's own.
			 */
			kind = LION_TL_GROUPKEY;
		}
		else
			elog(ERROR, "unexpected expression in LionCount target list");

		foreach(l2, ctlist)
		{
			if (equal(((TargetEntry *) lfirst(l2))->expr, expr))
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		ctlist = lappend(ctlist,
						 makeTargetEntry((Expr *) copyObject(expr),
										 list_length(ctlist) + 1,
										 tle->resname ? pstrdup(tle->resname) : NULL,
										 false));
		kinds = lappend_int(kinds, kind);
	}
	pfree(inor);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = having;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;

	/*
	 * The clause values move from custom_private into custom_exprs, which is
	 * the only field of a CustomScan the planner's later passes look inside:
	 * set_customscan_references() fixes its expressions up and
	 * SS_finalize_plan() collects the Param ids it finds there into the
	 * plan's extParam/allParam, which is what makes the executor rescan this
	 * node when an exec Param changes (DESIGN.md §10).  A value left only in
	 * custom_private would be invisible to both.
	 */
	cscan->custom_exprs = (List *) list_nth(best_path->custom_private,
											LION_PRIV_CONSTS);
	priv = list_copy(best_path->custom_private);
	lfirst(list_nth_cell(priv, LION_PRIV_CONSTS)) = NIL;
	lfirst(list_nth_cell(priv, LION_PRIV_HAVING)) = NIL;

	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->custom_private = lappend(priv, kinds);
	cscan->methods = &lion_count_scan_methods;

	return &cscan->scan.plan;
}


/* =====================================================================
 * Executor
 * ===================================================================== */

static Node *
lion_create_custom_scan_state(CustomScan *cscan)
{
	LionCountScanState *st = (LionCountScanState *)
		newNode(sizeof(LionCountScanState), T_CustomScanState);

	st->css.methods = &lion_count_exec_methods;
	return (Node *) st;
}

/*
 * Open one relation's heap and indexes: a plain table once, or one partition
 * for the length of its own processing (DESIGN.md §16).
 *
 * The executor holds a lock on every range table entry of the plan, and the
 * partitions are range table entries too (the planner locked them when it
 * expanded the parent, and AcquireExecutorLocks() relocks the whole flat
 * range table for a cached plan), so nothing here takes a relation lock.  The
 * indexes are not range table entries and get their own AccessShareLock.
 */
/*
 * Read the KEYS of the inner index of a two-column GROUP BY, once for this
 * relation (DESIGN.md §20).
 *
 * Only the keys: an entry's head block or INLINE payload would be worthless
 * without the buffer pin that goes with it (DESIGN.md §9), and holding one pin
 * per distinct inner value for the whole scan is exactly what the pin budget
 * forbids.  So each pair re-locates its inner posting set with
 * lion_posting_set_lookup(), which is one bucket page - and the bucket count
 * is sized to the index's entry count, so that lookup is a hit in shared
 * buffers for every index small enough for the cost model to have chosen this
 * plan at all.
 *
 * The alternative, walking the inner index's entry scan once per OUTER group,
 * needs no memory but re-reads every bucket page of the inner index
 * outer_entries times.  It is kept as the fallback for the case the keys do
 * not fit the work_mem budget - a grouping the cost model did not expect, the
 * §16 lesson that a plan-time bound is only as good as estimate_num_groups -
 * and then innerkey is left NULL.
 */
static void
lion_load_inner_keys(LionCountScanState *st)
{
	LionEntryScan es;
	LionState  *istate = lion_index_column_state(st->groupidx2,
												st->groupidxcol2);
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	Size		budget = (Size) work_mem * INT64CONST(1024);
	int			cap = 64;
	int			n = 0;
	Datum	   *keys;
	bool	   *isnull;
	bool		full = false;

	Assert(st->innerkey == NULL);

	MemoryContextReset(st->innercxt);
	oldcxt = MemoryContextSwitchTo(st->innercxt);
	keys = (Datum *) palloc(sizeof(Datum) * cap);
	isnull = (bool *) palloc(sizeof(bool) * cap);
	MemoryContextSwitchTo(oldcxt);

	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "LionCount inner entry scan",
								   ALLOCSET_SMALL_SIZES);

	lion_entry_scan_begin_col(&es, st->groupidx2, st->groupidxcol2);
	for (;;)
	{
		LionPostingSet ps;
		Datum		key;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(tmpcxt);
		oldcxt = MemoryContextSwitchTo(tmpcxt);
		if (!lion_entry_scan_next(&es, &key, &ps))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}
		MemoryContextSwitchTo(st->innercxt);
		if (n >= cap)
		{
			cap *= 2;
			keys = (Datum *) repalloc(keys, sizeof(Datum) * cap);
			isnull = (bool *) repalloc(isnull, sizeof(bool) * cap);
		}
		isnull[n] = ps.keyisnull;
		keys[n] = ps.keyisnull ? (Datum) 0 :
			datumCopy(key, istate->typbyval, istate->typlen);
		n++;
		MemoryContextSwitchTo(oldcxt);

		/* The set's pin goes now: only the key is kept. */
		lion_posting_set_release(&ps);

		if ((n & 0xff) == 0 &&
			MemoryContextMemAllocated(st->innercxt, true) > budget)
		{
			full = true;
			break;
		}
	}
	lion_entry_scan_end(&es);
	MemoryContextDelete(tmpcxt);

	if (full)
	{
		/* Too many to keep: walk the inner index per outer group instead. */
		MemoryContextReset(st->innercxt);
		st->innerkey = NULL;
		st->innerisnull = NULL;
		st->ninnerkey = 0;
		return;
	}

	st->innerkey = keys;
	st->innerisnull = isnull;
	st->ninnerkey = n;
}

/*
 * The KEY COLUMN (1-based) of `index` that holds heap column `heapattno`
 * (DESIGN.md §24).  A single-column index answers 1 for its own column and
 * nothing else; a multicolumn one is searched, because the columns may be in
 * any order - and, with partitions, in a DIFFERENT order in each of them.
 *
 * The search is the planner's own (lion_find_roaring_index()): the first key
 * column on that heap column whose opclass is multi-key exactly when
 * `multikey` says so, which is how the executor arrives at the very column
 * the plan was made for without the plan carrying it.  An index may list one
 * heap column twice, under a multi-key and a scalar opclass - `(tags
 * array_ops, tags <a whole-array class>)` - and the two hold different
 * entries, elements under one and whole arrays under the other.  The first
 * column on the heap column used to be taken whatever its opclass, so
 * `count(DISTINCT tags)` counted elements, and a multi-key query was handed
 * to a scalar column that has no extraction function at all (the 2026-09-27
 * review).  The kind a caller needs is always known: multi-key for a
 * multi-key clause (LION_CLAUSE_MULTI), scalar for every other clause and for
 * every index whose entries drive the scan.  Two columns of the SAME kind on
 * one heap column are told apart the way the planner tells them apart, by
 * position: it matches the first and never tries the second
 * (lion_match_index()).  An index with no such column is not the one the
 * plan was made for, and that is said rather than read.
 */
static AttrNumber
lion_index_col_for(Relation index, AttrNumber heapattno, bool multikey)
{
	int			c;

	for (c = 0; c < IndexRelationGetNumberOfKeyAttributes(index); c++)
	{
		if (index->rd_index->indkey.values[c] != heapattno)
			continue;
		if (lion_opfamily_is_multikey(index->rd_opfamily[c],
									 index->rd_opcintype[c]) != multikey)
			continue;
		return (AttrNumber) (c + 1);
	}

	elog(ERROR, "lion index \"%s\" has no %s key column on column %d of \"%s\"",
		 RelationGetRelationName(index), multikey ? "multi-key" : "scalar",
		 (int) heapattno, get_rel_name(index->rd_index->indrelid));
	return 0;					/* keep the compiler quiet */
}

/*
 * The attribute number heap column `parentattno` of `parentoid` has in `heap`.
 *
 * Everything the plan carries is in the PARENT's numbering (DESIGN.md §16),
 * and a partition may number its columns differently - so the column an
 * index's indkey names has to be translated before it can be looked for
 * there.  Partitions match their parent's columns BY NAME, which is the same
 * mapping the executor's own tuple conversion uses.
 */
static AttrNumber
lion_heap_attno_in(Relation heap, Oid parentoid, AttrNumber parentattno)
{
	char	   *name;
	AttrNumber	attno;

	if (RelationGetRelid(heap) == parentoid || parentattno <= 0)
		return parentattno;

	name = get_attname(parentoid, parentattno, false);
	attno = get_attnum(RelationGetRelid(heap), name);
	if (attno == InvalidAttrNumber)
		elog(ERROR, "relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(heap), name);
	pfree(name);

	return attno;
}

/*
 * The relations the statement this node runs in modifies or row-locks, by
 * Oid: its result relations and its row marks, which are exactly the range
 * table entries ScanRelIsReadOnly() tests a core scan's relation against.
 */
static List *
lion_statement_written_rels(EState *estate)
{
	PlannedStmt *pstmt = estate->es_plannedstmt;
	Bitmapset  *rtis;
	List	   *oids = NIL;
	int			rti = -1;

	if (pstmt == NULL)
		return NIL;

	rtis = lion_pstmt_written_rtis(pstmt);
	while ((rti = bms_next_member(rtis, rti)) >= 0)
	{
		RangeTblEntry *rte = exec_rt_fetch((Index) rti, estate);

		if (rte->rtekind == RTE_RELATION && OidIsValid(rte->relid))
			oids = list_append_unique_oid(oids, rte->relid);
	}
	bms_free(rtis);
	return oids;
}

/*
 * Is heap read-only for this statement, in the sense of ScanRelIsReadOnly()?
 * (DESIGN.md §11, "On-access pruning sets the visibility map too.")
 *
 * Core asks whether the SCAN's range table entry is a result relation or has
 * a row mark.  Ours never is: the node is only planted in a SELECT with no
 * row marks, and a relation the statement also modifies - `UPDATE t SET x =
 * (SELECT count(*) FROM t WHERE ...)`, a data-modifying CTE, INSERT ...
 * SELECT - is a different entry of the same table.  So the question is asked
 * by relation instead: the counted table, or for a partition that partition
 * or any of its ancestors (an INSERT routed through the parent names only the
 * parent), must not be among the relations the statement writes or locks.
 * Setting all-visible bits the same statement is about to clear would be
 * wasted work, and that is all this decides: the correctness argument does
 * not depend on it.
 */
static bool
lion_rel_read_only(LionCountScanState *st, Relation heap)
{
	Oid			relid = RelationGetRelid(heap);
	bool		result = true;

	if (st->writtenrels == NIL)
		return true;
	if (list_member_oid(st->writtenrels, relid) ||
		list_member_oid(st->writtenrels, st->heapoid))
		return false;
	if (heap->rd_rel->relispartition)
	{
		List	   *ancestors = get_partition_ancestors(relid);
		ListCell   *lc;

		foreach(lc, ancestors)
		{
			if (list_member_oid(st->writtenrels, lfirst_oid(lc)))
			{
				result = false;
				break;
			}
		}
		list_free(ancestors);
	}
	return result;
}

static void
lion_open_relation(LionCountScanState *st, Oid heapoid, Oid groupidxoid,
				  Oid groupidxoid2, const Oid *clauseidxoid)
{
	int			i;

	Assert(st->heap == NULL);

	/*
	 * A parallel worker (DESIGN.md §27, "Parallel") holds no lock of the
	 * leader's and takes its own, as ExecGetRangeTableRelation() does for the
	 * scans of a worker.
	 */
	st->heap = table_open(heapoid, IsParallelWorker() ? AccessShareLock :
						  NoLock);
	Assert(CheckRelationLockedByMe(st->heap, AccessShareLock, true));
	lion_check_table_am(st->heap);	/* the planner declined it; see there */
	st->rel_read_only = lion_rel_read_only(st, st->heap);

	for (i = 0; i < st->nclause; i++)
	{
		st->clause[i].idx = index_open(clauseidxoid != NULL ?
									   clauseidxoid[i] : st->clause[i].idxoid,
									   AccessShareLock);
		st->clause[i].idxcol =
			lion_index_col_for(st->clause[i].idx,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->clause[i].attno),
							   st->clause[i].kind == LION_CLAUSE_MULTI);
	}

	/*
	 * The driving index's key column comes from the index that was really
	 * opened rather than from the plan (DESIGN.md §24): the planner only has
	 * to be right about WHICH index, and a partition's own index may put the
	 * same heap column at a different position from the parent's.  A driving
	 * column is always a scalar one: its entries have to be the column's
	 * values, one per row.
	 */
	if (OidIsValid(groupidxoid))
	{
		st->groupidx = index_open(groupidxoid, AccessShareLock);
		st->groupidxcol =
			lion_index_col_for(st->groupidx,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->driveattno),
							   false);
	}
	if (OidIsValid(groupidxoid2))
	{
		st->groupidx2 = index_open(groupidxoid2, AccessShareLock);
		st->groupidxcol2 =
			lion_index_col_for(st->groupidx2,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->innerattno),
							   false);
	}

	/*
	 * index_beginscan() would take a relation-level predicate lock on each of
	 * these (the AM has no ampredlocks); we read them without a scan, so take
	 * it here, before any lookup, so that absent keys are covered as well.
	 * Without it two SERIALIZABLE transactions could each count an absent
	 * key, insert it, and both commit.
	 */
	{
		Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;

		for (i = 0; i < st->nclause; i++)
			PredicateLockRelation(st->clause[i].idx, snapshot);
		if (st->groupidx != NULL)
			PredicateLockRelation(st->groupidx, snapshot);
		if (st->groupidx2 != NULL)
			PredicateLockRelation(st->groupidx2, snapshot);
	}

	/*
	 * A two-column GROUP BY reads the inner index's keys once for this
	 * relation (DESIGN.md §20); the sets themselves are located per pair,
	 * because each one holds a buffer pin.
	 */
	if (st->groupidx2 != NULL)
		lion_load_inner_keys(st);
}

/*
 * The reverse.  Every posting set of this relation must already have been
 * released: DESIGN.md §9 wants no pin to outlive the relation it belongs to,
 * and a partition's pins must not outlive that partition's turn.
 */
static void
lion_close_relation(LionCountScanState *st)
{
	int			i;

	/* the counts' map page is this relation's */
	lion_vis_cache_release_vm(st->viscache);

	if (st->groupidx != NULL)
	{
		index_close(st->groupidx, AccessShareLock);
		st->groupidx = NULL;
		st->groupidxcol = 0;
	}
	if (st->groupidx2 != NULL)
	{
		index_close(st->groupidx2, AccessShareLock);
		st->groupidx2 = NULL;
		st->groupidxcol2 = 0;
	}
	st->innerkey = NULL;
	st->innerisnull = NULL;
	st->ninnerkey = 0;
	if (st->innercxt != NULL)
		MemoryContextReset(st->innercxt);
	for (i = 0; i < st->nclause; i++)
	{
		if (st->clause[i].idx != NULL)
		{
			index_close(st->clause[i].idx, AccessShareLock);
			st->clause[i].idx = NULL;
			st->clause[i].idxcol = 0;
		}
	}
	if (st->heap != NULL)
	{
		table_close(st->heap, NoLock);
		st->heap = NULL;
	}
}

/*
 * The EXECUTE checks of the plan the node replaces (LION_PRIV_EXECUTE,
 * lion_replaced_functions()), made for the current user every time the node
 * is initialised, as ExecInitNode() makes them for that plan - so a cached
 * plan answers a REVOKE and a SET ROLE the way the ordinary one does.  In
 * core's order: the scan's quals, then the Agg's grouping equality, then its
 * aggregates.
 *
 * Plain EXPLAIN initialises the plan too, and core checks the quals and the
 * aggregates then as well; the grouping equality it checks only when a
 * HashAggregate - the plan a grouped count competes with - builds its hash
 * table, which EXPLAIN does not, so neither does this.
 */
static void
lion_check_replaced_execute(List *exec, int eflags)
{
	ListCell   *lc;

	if (exec == NIL || !IsA(exec, List) || list_length(exec) != 3)
		elog(ERROR, "LionCount: malformed EXECUTE list");

	foreach(lc, (List *) lsecond(exec))
		lion_check_execute(lfirst_oid(lc));
	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
	{
		foreach(lc, (List *) lthird(exec))
			lion_check_execute(lfirst_oid(lc));
	}
	foreach(lc, (List *) linitial(exec))
		lion_check_aggregate_execute(lfirst_oid(lc));
}

static void
lion_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
	LionCountScanState *st = (LionCountScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *shape;
	List	   *oids;
	List	   *ints;
	List	   *exprs;
	List	   *ckinds;
	List	   *partlist;
	List	   *clauseops;
	List	   *orlist;
	List	   *kinds;
	List	   *dist;
	List	   *join;
	List	   *coal;
	int			flags;
	int			i;
	int			k;

	/*
	 * custom_private is read positionally, so check that it is the list this
	 * build writes before reading a single offset of it.  A mismatch means
	 * the planner half and the executor half of this file have drifted apart
	 * (or a plan from another build has been handed to us); saying so is far
	 * better than decoding Oids out of the wrong member.
	 */
	shape = (list_length(cscan->custom_private) == LION_PRIV_NMEMBERS) ?
		(List *) list_nth(cscan->custom_private, LION_PRIV_VERSION) : NIL;
	if (shape == NIL || !IsA(shape, IntList) || list_length(shape) != 2 ||
		linitial_int(shape) != LION_PRIV_MAGIC ||
		lsecond_int(shape) != LION_PRIV_NMEMBERS)
		elog(ERROR, "LionCount: unrecognized custom_private shape (%d members)",
			 list_length(cscan->custom_private));

	/* Before anything is opened or read (DESIGN.md §9, "Privileges"). */
	lion_check_replaced_execute((List *) list_nth(cscan->custom_private,
												  LION_PRIV_EXECUTE),
								eflags);

	oids = (List *) list_nth(cscan->custom_private, LION_PRIV_OIDS);
	ints = (List *) list_nth(cscan->custom_private, LION_PRIV_INTS);
	ckinds = (List *) list_nth(cscan->custom_private, LION_PRIV_CLAUSEKINDS);
	exprs = cscan->custom_exprs;
	partlist = (List *) list_nth(cscan->custom_private, LION_PRIV_PARTS);
	clauseops = (List *) list_nth(cscan->custom_private, LION_PRIV_CLAUSEOPS);
	orlist = (List *) list_nth(cscan->custom_private, LION_PRIV_ORS);
	dist = (List *) list_nth(cscan->custom_private, LION_PRIV_DISTINCT);
	join = (List *) list_nth(cscan->custom_private, LION_PRIV_JOIN);
	coal = (List *) list_nth(cscan->custom_private, LION_PRIV_COALESCE);
	kinds = (List *) list_nth(cscan->custom_private, LION_PRIV_TLKINDS);

	st->heapoid = linitial_oid(oids);
	st->groupidxoid = lsecond_oid(oids);
	st->groupidxoid2 = lthird_oid(oids);
	st->scanrelid = (Index) linitial_int(ints);
	st->groupattno = (AttrNumber) lsecond_int(ints);
	st->groupattno2 = (AttrNumber) lthird_int(ints);
	flags = lfourth_int(ints);
	st->singlegroup = (flags & LION_FLAG_SINGLEGROUP) != 0;
	st->sumall = (flags & LION_FLAG_SUMALL) != 0;
	st->hasgroupidx = (flags & LION_FLAG_GROUPIDX) != 0;
	st->hasrange = (flags & LION_FLAG_RANGE) != 0;
	st->nclause = list_length(ckinds);
	st->distattno = (dist != NIL) ? (AttrNumber) linitial_int(dist) : 0;

	/* GROUP BY coalesce(g, c) (DESIGN.md §10): c and its equality */
	st->hascoal = false;
	st->coalcount = 0;
	st->coalwalked = false;
	if (coal != NIL)
	{
		List	   *ops;

		if (list_length(coal) != 2 || !IsA(linitial(coal), Const) ||
			!IsA(lsecond(coal), OidList) ||
			list_length((List *) lsecond(coal)) != 2 ||
			st->groupattno == 0 || st->groupattno2 != 0 ||
			st->distattno != 0 || !st->hasgroupidx)
			elog(ERROR, "LionCount: malformed coalesce group");
		st->coalconst = (Const *) linitial(coal);
		ops = (List *) lsecond(coal);
		st->coaleqop = linitial_oid(ops);
		st->coalcoll = lsecond_oid(ops);
		if (st->coalconst->constisnull || !OidIsValid(st->coaleqop))
			elog(ERROR, "LionCount: malformed coalesce group");
		fmgr_info_cxt(get_opcode(st->coaleqop), &st->coaleqfn,
					  estate->es_query_cxt);
		st->hascoal = true;
	}

	/*
	 * One value expression per clause, in custom_exprs (see the shape marker
	 * above).  A mismatch is planner/executor drift, exactly like a wrong
	 * shape marker, and is said rather than decoded.
	 */
	if (list_length(exprs) != st->nclause)
		elog(ERROR, "LionCount: %d clauses but %d value expressions",
			 st->nclause, list_length(exprs));

	/*
	 * The FK-side join (DESIGN.md §27): which clause is the join key, and
	 * which column of the child's rows carries its value.  Every other shape
	 * has neither, and no child.
	 */
	st->joinclause = -1;
	st->joinkeyresno = 0;
	st->jointype = LION_JOIN_INNER;
	st->joincollect = false;
	st->joinrows = false;
	st->joinsum = false;
	st->joinunique = false;
	st->joinwalk = false;
	st->joinsortop = InvalidOid;
	st->joinsortcoll = InvalidOid;
	if (join != NIL)
	{
		if (list_length(join) != 6 ||
			list_length(cscan->custom_plans) != 1)
			elog(ERROR, "LionCount: malformed join");
		st->joinclause = linitial_int(join);
		st->jointype = lsecond_int(join);
		st->joincollect = (lthird_int(join) & LION_JOINFLAG_COLLECT) != 0;
		st->joinrows = (lthird_int(join) & LION_JOINFLAG_ROWS) != 0;
		st->joinsum = (lthird_int(join) & LION_JOINFLAG_SUM) != 0;
		st->joinunique = (lthird_int(join) & LION_JOINFLAG_UNIQUE) != 0;
		st->joinwalk = (lthird_int(join) & LION_JOINFLAG_WALK) != 0;
		st->joinsortop = (Oid) list_nth_int(join, 3);
		st->joinsortcoll = (Oid) list_nth_int(join, 4);
		st->joinkeyresno = (AttrNumber) list_nth_int(join, 5);
		if (st->joinclause < 0 || st->joinclause >= st->nclause ||
			st->joinkeyresno <= 0 ||
			(st->jointype != LION_JOIN_INNER &&
			 st->jointype != LION_JOIN_SEMI &&
			 st->jointype != LION_JOIN_ANTI) ||
			(st->joinunique &&
			 (st->jointype != LION_JOIN_INNER ||
			  !OidIsValid(st->joinsortop))) ||
			(st->joinsum && st->joinrows))
			elog(ERROR, "LionCount: malformed join");
	}

	st->ntlist = list_length(kinds);
	st->tlkind = (int *) palloc(sizeof(int) * Max(st->ntlist, 1));
	for (i = 0; i < st->ntlist; i++)
	{
		st->tlkind[i] = list_nth_int(kinds, i);
		if (LION_TL_IS_CHILDCOL(st->tlkind[i]) && st->joinclause < 0)
			elog(ERROR, "LionCount: a child column without a join");
	}

	/*
	 * A summed join's one row stands for no dimension row (LION_JOINFLAG_SUM),
	 * so what the plan's target list reads of it must be counts alone - the
	 * join key's column is in custom_scan_tlist for the clause's value, and is
	 * read by nothing else.  Said, rather than trusted, like a malformed join.
	 */
	if (st->joinsum)
	{
		List	   *vars = pull_var_clause((Node *) cscan->scan.plan.targetlist,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_RECURSE_PLACEHOLDERS);
		ListCell   *lc;

		foreach(lc, vars)
		{
			Var		   *var = (Var *) lfirst(lc);

			if (var->varno != INDEX_VAR || var->varattno < 1 ||
				var->varattno > st->ntlist ||
				st->tlkind[var->varattno - 1] != LION_TL_COUNT)
				elog(ERROR, "LionCount: a summed join reads more than its counts");
		}
		list_free(vars);
	}

	/*
	 * Which tests of a count(DISTINCT k) walk have to COUNT rather than stop
	 * at the first visible row (DESIGN.md §26), from what the target list -
	 * the HAVING's counts included - asks for.  Without a GROUP BY every
	 * other count is a sum over k's entries; beside one, count(k) is a sum
	 * over the (g, k) pairs, and count(*) and count(g) are the group's own.
	 */
	st->distfull = false;
	st->distgroupcount = false;
	for (i = 0; st->distattno != 0 && i < st->ntlist; i++)
	{
		switch (st->tlkind[i])
		{
			case LION_TL_COUNT_DISTCOL:
				st->distfull = true;
				break;
			case LION_TL_COUNT:
			case LION_TL_COUNT_GROUPCOL:
			case LION_TL_COUNT_GROUPCOL2:
				if (st->groupattno == 0)
					st->distfull = true;
				else
					st->distgroupcount = true;
				break;
			default:
				break;
		}
	}

	st->clause = (LionClauseState *)
		palloc0(sizeof(LionClauseState) * Max(st->nclause, 1));
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];

		cl->kind = list_nth_int(ckinds, i);
		cl->idxoid = list_nth_oid(oids, 3 + i);
		cl->attno = (AttrNumber) list_nth_int(ints, 4 + i);
		cl->opno = list_nth_oid(clauseops, i);
		cl->strategy = 0;

		/*
		 * A literal's value is ready now and never changes, so it is taken
		 * straight from the Const; anything else - a Param, the ARRAY[] of a
		 * generic IN list, a stable expression - gets an ExprState and is
		 * evaluated at the start of each scan (lion_eval_clause_values()).
		 */
		cl->valexpr = (Expr *) list_nth(exprs, i);
		cl->valtype = exprType((Node *) cl->valexpr);

		/*
		 * The join key's value is not evaluated at all: it is a column of the
		 * child's current row, read per row (lion_next_join_row()).  Its
		 * expression is kept for EXPLAIN, which deparses it as the dimension
		 * column it references.
		 */
		if (i == st->joinclause)
		{
			cl->con = NULL;
			cl->valstate = NULL;
			continue;
		}

		if (IsA(cl->valexpr, Const))
		{
			cl->con = (Const *) cl->valexpr;
			cl->val = cl->con->constvalue;
			cl->valisnull = cl->con->constisnull;
		}
		else
		{
			cl->con = NULL;
			cl->valstate = ExecInitExpr(cl->valexpr, &node->ss.ps);
		}
	}

	/*
	 * Which HEAP column the driving index's entries belong to (DESIGN.md §24
	 * needs it to name that index's KEY COLUMN).  With a GROUP BY it is the
	 * outer group column; a sum-over-all (§14) has none, and its driver is the
	 * index of the FIRST `IS NOT NULL` clause - which is exactly the clause
	 * the planner took its driving column from (`notnullvar`, set at the first
	 * such leaf of the same list this array was built from, and an OR leaf can
	 * never be one).
	 */
	st->driveattno = st->groupattno;
	if (st->sumall)
	{
		/*
		 * ... or, when the plan has RANGE clauses (DESIGN.md §28), the column
		 * they bound, which is the one the planner drove the sum from: they
		 * all name it.
		 */
		int			drivekind = st->hasrange ? LION_CLAUSE_RANGE :
			LION_CLAUSE_NOTNULL;

		st->driveattno = 0;
		for (i = 0; i < st->nclause; i++)
		{
			if (st->clause[i].kind == drivekind)
			{
				st->driveattno = st->clause[i].attno;
				break;
			}
		}
		if (st->driveattno == 0)
			elog(ERROR, "LionCount: sum-over-all without an IS NOT NULL or range clause");
	}

	/*
	 * count(DISTINCT k) (DESIGN.md §26): without a GROUP BY k's own entries
	 * drive the scan; beside one, k's index is the inner side of the nested
	 * loop, which is also what a second grouping column's is (§20).
	 */
	if (st->distattno != 0 && st->groupattno == 0)
		st->driveattno = st->distattno;
	if (st->groupattno2 != 0)
		st->innerattno = st->groupattno2;
	else if (st->distattno != 0 && st->groupattno != 0)
		st->innerattno = st->distattno;
	else
		st->innerattno = 0;
	if (st->distattno != 0 && !st->hasgroupidx)
		elog(ERROR, "LionCount: count(DISTINCT) without its index");

	/*
	 * RANGE clauses bound the driving walk (DESIGN.md §28), so there has to
	 * be one, and they have to be on its column: anything else is planner
	 * drift, said here rather than counted wrong.
	 */
	for (i = 0; i < st->nclause; i++)
	{
		if (st->clause[i].kind != LION_CLAUSE_RANGE)
			continue;
		if (!st->hasrange || !st->hasgroupidx ||
			st->clause[i].attno != st->driveattno)
			elog(ERROR, "LionCount: a range clause that does not bound the driving walk");
	}

	/*
	 * The OR restrictions (DESIGN.md §19) and the sources the clauses make
	 * up: one per plain clause, one per OR.  A clause that is an OR leaf has
	 * no source of its own - its posting sets go into the OR's, which is the
	 * union of the arms - and that is the only thing that tells the two apart
	 * anywhere below.
	 */
	st->nor = list_length(orlist);
	st->inor = lion_or_leaf_map(orlist, st->nclause);
	if (st->nor > 0)
	{
		st->ors = (LionOrState *) palloc0(sizeof(LionOrState) * st->nor);
		for (i = 0; i < st->nor; i++)
		{
			List	   *one = (List *) list_nth(orlist, i);
			LionOrState *o = &st->ors[i];

			o->first = linitial_int(one);
			o->narms = lsecond_int(one);
			o->armlen = (int *) palloc0(sizeof(int) * Max(o->narms, 1));
			o->nleaves = 0;
			for (k = 0; k < o->narms; k++)
			{
				o->armlen[k] = list_nth_int(one, 2 + k);
				o->nleaves += o->armlen[k];
			}
			if (o->narms < 1 || o->nleaves < 1 ||
				o->first < 0 || o->first + o->nleaves > st->nclause)
				elog(ERROR, "LionCount: malformed OR structure");
		}
	}

	st->item = (LionSourceItem *)
		palloc0(sizeof(LionSourceItem) * Max(st->nclause + 1, 1));
	st->nitem = 0;
	for (i = 0; i < st->nclause; i++)
	{
		int			orno = -1;

		/* The join key is looked up per child row, into slot 0, not here. */
		if (i == st->joinclause)
			continue;

		/* A range bounds the driving walk and is no source (DESIGN.md §28). */
		if (st->clause[i].kind == LION_CLAUSE_RANGE)
			continue;

		/*
		 * A range taken as a source (DESIGN.md §32) is ONE source however many
		 * bounds it has: the first bound on its column opens it, for all of
		 * them.  Under an OR the same goes per arm (lion_locate_or()).
		 */
		st->item[st->nitem].rangesrc = false;
		if (st->clause[i].kind == LION_CLAUSE_RANGESRC && !st->inor[i])
		{
			for (k = 0; k < i; k++)
			{
				if (st->clause[k].kind == LION_CLAUSE_RANGESRC &&
					!st->inor[k] && st->clause[k].attno == st->clause[i].attno)
					break;
			}
			if (k < i)
				continue;
			st->item[st->nitem].rangesrc = true;
		}

		if (st->inor[i])
		{
			/* Only the FIRST leaf of an OR opens a source, for the whole OR. */
			for (k = 0; k < st->nor; k++)
			{
				if (st->ors[k].first == i)
				{
					orno = k;
					break;
				}
			}
			if (orno < 0)
				continue;
		}

		st->item[st->nitem].clauseno = i;
		st->item[st->nitem].orno = orno;
		st->nitem++;
	}

	/* One target per live leaf partition, in the planner's order. */
	st->npart = list_length(partlist);
	if (st->npart > 0)
	{
		st->part = (LionPartState *) palloc0(sizeof(LionPartState) * st->npart);
		for (i = 0; i < st->npart; i++)
		{
			List	   *one = (List *) list_nth(partlist, i);
			int			j;

			st->part[i].heapoid = linitial_oid(one);
			st->part[i].groupidxoid = lsecond_oid(one);
			st->part[i].groupidxoid2 = lthird_oid(one);
			st->part[i].clauseidxoid = (Oid *)
				palloc0(sizeof(Oid) * Max(st->nclause, 1));
			for (j = 0; j < st->nclause; j++)
				st->part[i].clauseidxoid[j] = list_nth_oid(one, 3 + j);
		}
	}

	st->located = false;
	st->valsdone = false;
	st->wheremissing = false;
	st->scanning = false;
	st->done = false;
	st->curpart = 0;
	st->partopen = false;
	memset(&st->stats, 0, sizeof(st->stats));
	memset(st->rangeeval, 0, sizeof(st->rangeeval));
	st->summaries = 0;
	st->rangeprobed = 0;
	st->rangesrc_collected = 0;
	st->rangesrc_walked = 0;
	st->rangesrc_held = 0;
	st->wherecollected = 0;
	st->wherespilled = 0;
	memset(&st->wherecoll, 0, sizeof(st->wherecoll));
	st->wherecoll.pinbuf = InvalidBuffer;
	st->ingroupleft = 0;

	st->pergroup = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount per-group",
										 ALLOCSET_SMALL_SIZES);
	st->outercxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount outer group",
										 ALLOCSET_SMALL_SIZES);
	st->innercxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount inner keys",
										 ALLOCSET_SMALL_SIZES);
	st->wherecxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount where keys",
										 ALLOCSET_SMALL_SIZES);
	st->keycxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionCount clause keys",
									   ALLOCSET_SMALL_SIZES);
	st->valcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionCount clause values",
									   ALLOCSET_SMALL_SIZES);
	st->viscache = lion_vis_cache_create(estate->es_query_cxt);
	st->filter = NULL;
	st->writtenrels = lion_statement_written_rels(estate);
	st->rel_read_only = false;

	/*
	 * The FK-side join's dimension side (DESIGN.md §27) is an ordinary plan,
	 * initialised here - EXPLAIN without ANALYZE prints it too - and run
	 * under the same snapshot as the counts.
	 */
	st->child = NULL;
	st->childslot = NULL;
	st->joinlookups = 0;
	st->joinmissing = 0;
	st->joincollected = false;
	st->joinfiltered = false;
	st->joinfilterrows = -1;
	st->joinshared = NULL;
	st->joinreported = false;
	memset(&st->joinworkerstats, 0, sizeof(st->joinworkerstats));
	st->joinworkerlookups = 0;
	st->joinworkermissing = 0;
	st->joinworkerdirpages = 0;
	st->joinworkerfilterrows = -1;
	memset(&st->joinfilter, 0, sizeof(st->joinfilter));
	st->joinfilter.pinbuf = InvalidBuffer;
	st->joinsort = NULL;
	st->joinsortdone = false;
	st->joinsortslot = NULL;
	st->joinkeycxt = NULL;
	st->joinhaveprev = false;
	st->joinkeypos = 0;
	st->joinchunk = -1;
	st->joinsorted = 0;
	st->joinhavesortstats = false;
	st->joinworkersorted = 0;
	st->joinchildrows = 0;
	st->joinposting = 0;

	/*
	 * Timed only when core times the nodes: under EXPLAIN ANALYZE with its
	 * TIMING option (or auto_explain's), which is what es_instrument says in
	 * every participant, as it says so to InstrAlloc() for each node.
	 */
	st->jointiming = (estate->es_instrument & INSTRUMENT_TIMER) != 0;
	st->joinworkerchildrows = 0;
	st->joinworkerposting = 0;
	for (i = 0; i < LION_JT_N; i++)
	{
		INSTR_TIME_SET_ZERO(st->jointime[i]);
		INSTR_TIME_SET_ZERO(st->joinworkertime[i]);
	}
	if (st->joinclause >= 0)
	{
		st->child = ExecInitNode((Plan *) linitial(cscan->custom_plans),
								 estate, eflags);
		node->custom_ps = list_make1(st->child);
	}

	/*
	 * A forward semi join over a non-unique key (DESIGN.md §27) sorts the
	 * child's rows by their key and compares neighbours with the equality of
	 * the sort operator's btree family, which is the join operator's family
	 * (lion_fkjoin_recognize()) for the key's own type.
	 */
	if (st->joinunique)
	{
		TupleDesc	childdesc = ExecGetResultType(st->child);
		Form_pg_attribute keyatt;
		Oid			eqop;

		if (st->joinkeyresno > childdesc->natts)
			elog(ERROR, "LionCount: malformed join");

		/*
		 * Only the key comes out of the sort, so the key is the only column
		 * of the child the target list may read: the dimension of a forward
		 * semi join is not visible to the query above it, and what the join
		 * rows of a count(DISTINCT) carry is the key (lion_plan_fkjoin_path()).
		 */
		for (i = 0; i < st->ntlist; i++)
		{
			if (LION_TL_IS_CHILDCOL(st->tlkind[i]) &&
				LION_TL_CHILDRESNO(st->tlkind[i]) != st->joinkeyresno)
				elog(ERROR, "LionCount: a distinct-key join reads a column other than its key");
		}

		keyatt = TupleDescAttr(childdesc, st->joinkeyresno - 1);
		st->joinkeytype = keyatt->atttypid;
		st->joinkeybyval = keyatt->attbyval;
		st->joinkeylen = keyatt->attlen;
		eqop = get_equality_op_for_ordering_op(st->joinsortop, NULL);
		if (!OidIsValid(eqop))
			elog(ERROR, "could not find equality operator for ordering operator %u",
				 st->joinsortop);
		fmgr_info_cxt(get_opcode(eqop), &st->joineqfn, estate->es_query_cxt);
		st->joinsortslot = ExecInitExtraTupleSlot(estate, childdesc,
												  &TTSOpsVirtual);
		st->joinkeycxt = AllocSetContextCreate(estate->es_query_cxt,
											   "LionCount join key",
											   ALLOCSET_SMALL_SIZES);
	}

	/*
	 * Lookups in key order (DESIGN.md §27): the batches of rows - or of the
	 * distinct keys, which the target list reads through joinsortslot above -
	 * live in a context of their own, emptied at every batch, and a batched
	 * row goes back to the target list through a slot of the child's shape.
	 */
	st->joinwalkbegun = false;
	st->joinbatchcxt = NULL;
	st->joinbatch = NULL;
	st->joinbatchn = 0;
	st->joinbatchcap = 0;
	st->joinbatchpos = 0;
	st->joinchilddone = false;
	st->joinbatchslot = NULL;
	st->joinbatches = 0;
	st->joinlasthave = false;
	st->joinworkerbatches = 0;
	if (st->joinwalk)
	{
		TupleDesc	childdesc = ExecGetResultType(st->child);
		Form_pg_attribute keyatt;

		if (st->joinkeyresno > childdesc->natts)
			elog(ERROR, "LionCount: malformed join");
		keyatt = TupleDescAttr(childdesc, st->joinkeyresno - 1);
		st->joinkeytype = keyatt->atttypid;
		st->joinkeybyval = keyatt->attbyval;
		st->joinkeylen = keyatt->attlen;
		/*
		 * A batch is full when the context's blocks reach work_mem, and a
		 * block may overshoot it by as much as a block: an eighth of it at
		 * most, rather than the allocator's usual 8 MB.
		 */
		st->joinbatchcxt =
			AllocSetContextCreate(estate->es_query_cxt,
								  "LionCount join batch",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  Min((Size) ALLOCSET_DEFAULT_MAXSIZE,
									  Max((Size) ALLOCSET_DEFAULT_INITSIZE,
										  pg_prevpower2_size_t((Size) work_mem * 1024 / 8))));
		st->joinbatchslot = ExecInitExtraTupleSlot(estate, childdesc,
												   &TTSOpsMinimalTuple);
	}

	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0)
		return;

	/*
	 * A plain table is opened once and stays open.  The executor already
	 * holds locks on every range table entry, so the heap is opened without
	 * taking another one.  The indexes are not range table entries, so they
	 * get their own AccessShareLock.
	 *
	 * A partitioned one opens nothing here: lion_open_relation() opens one
	 * partition at a time (DESIGN.md §16).
	 */
	if (st->npart == 0)
	{
		lion_open_relation(st, st->heapoid, st->groupidxoid, st->groupidxoid2,
						  NULL);

		/*
		 * A materialized view created WITH NO DATA has an empty heap and
		 * empty indexes, and counting them would answer 0 where core's scan
		 * refuses to run at all.  So refuse exactly as ExecOpenScanRelation()
		 * does, and under the same exemption for CREATE TABLE AS ... WITH NO
		 * DATA (EXPLAIN without ANALYZE has returned above).  Nothing in the
		 * planner looks at relispopulated, and a REFRESH invalidates the
		 * plan.  A partition is never a materialized view.
		 */
		if ((eflags & EXEC_FLAG_WITH_NO_DATA) == 0 &&
			!RelationIsScannable(st->heap))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("materialized view \"%s\" has not been populated",
							RelationGetRelationName(st->heap)),
					 errhint("Use the REFRESH MATERIALIZED VIEW command.")));
	}

	/*
	 * Slot 0 is the (outer) group's posting set, 1 .. nitem the WHERE items,
	 * and - for a two-column GROUP BY (DESIGN.md §20), or the (g, k) pairs of
	 * a count(DISTINCT k) per group (§26) - slot nitem + 1 the inner one's.
	 */
	st->nsource = st->nitem + 1 + (st->innerattno != 0 ? 1 : 0);
	st->sources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * st->nsource);
	st->sources[0].nsets = 1;
	st->sources[0].sets = &st->groupset;
	st->sources[0].negated = false;
	if (st->innerattno != 0)
	{
		st->sources[st->nitem + 1].nsets = 1;
		st->sources[st->nitem + 1].sets = &st->groupset2;
		st->sources[st->nitem + 1].negated = false;
	}

	/*
	 * The alternative source list of a driver that makes one WHERE item
	 * redundant (DESIGN.md §14 and §15; see the comment on ingroupitem).  It is
	 * never longer than st->sources, and lion_locate_where() fills it in per
	 * relation.
	 */
	st->ingroupitem = -1;
	st->sumallitem = -1;
	st->dsources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * st->nsource);
	st->disttests = 0;
	st->batchitem = -1;

	/*
	 * A count against the collected WHERE (lion_group_count()) takes one slot
	 * for the collected set on top of what it keeps of st->sources.
	 */
	st->wsources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * (st->nsource + 1));
}

/*
 * Evaluate the clause values that are not literals (DESIGN.md §10).
 *
 * Called once per scan, before anything is looked up, and again after every
 * ReScan: a nested loop or a LATERAL reference sets a new exec Param between
 * the two, and the node has to see the new value.  A generic prepared plan's
 * PARAM_EXTERN is constant for the statement but is still only available
 * here, and so is a stable expression's value: a cached plan carries the
 * expression, never the value it had when it was planned.
 *
 * ExecEvalExprSwitchContext() leaves its result in the per-tuple memory of
 * the node's ExprContext, which nothing here owns, so the value is copied
 * into a context of the node's own that lives exactly as long as the scan.
 */
static void
lion_eval_clause_values(LionCountScanState *st)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	MemoryContext oldcxt;
	int			i;

	st->valsdone = true;

	MemoryContextReset(st->valcxt);
	oldcxt = MemoryContextSwitchTo(st->valcxt);

	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];
		int16		typlen;
		bool		typbyval;
		Datum		val;
		bool		isnull;

		if (cl->valstate == NULL)
			continue;			/* a literal: cl->val is already right */

		val = ExecEvalExprSwitchContext(cl->valstate, econtext, &isnull);
		cl->valisnull = isnull;
		if (isnull)
		{
			cl->val = (Datum) 0;
			continue;
		}

		get_typlenbyval(cl->valtype, &typlen, &typbyval);
		cl->val = datumCopy(val, typbyval, typlen);
	}

	MemoryContextSwitchTo(oldcxt);
}

/* ---------------------------------------------------------------------
 * Locating the posting sets of one relation's WHERE clauses
 *
 * Each clause produces some located posting sets and a LionKeyNode tree over
 * them, and the two go into a LionCountSource that the merge in lion_count.c
 * evaluates.  A plain clause is one source; an OR restriction is one source
 * over the sets of all its leaves (DESIGN.md §19).
 * --------------------------------------------------------------------- */

static LionKeyNode *
lion_key_node(int keyno)
{
	LionKeyNode *n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));

	n->kind = LION_KN_KEY;
	n->keyno = keyno;
	return n;
}

/*
 * An AND or OR over nargs subtrees, or the subtree itself when there is only
 * one of them.  args is consumed.
 */
static LionKeyNode *
lion_bool_node(LionKeyNodeKind kind, LionKeyNode **args, int nargs)
{
	LionKeyNode *n;

	Assert(nargs >= 1);
	if (nargs == 1)
		return args[0];

	n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	n->kind = kind;
	n->nargs = nargs;
	n->args = args;
	return n;
}

/*
 * Renumber a tree's leaves, which name posting sets by position: the leaves
 * of an OR's arms are concatenated into one array, so each clause's tree has
 * to be moved to where its own sets ended up.
 */
static void
lion_shift_keynos(LionKeyNode *node, int delta)
{
	int			i;

	if (node == NULL || delta == 0)
		return;
	if (node->kind == LION_KN_KEY)
	{
		node->keyno += delta;
		return;
	}
	for (i = 0; i < node->nargs; i++)
		lion_shift_keynos(node->args[i], delta);
}

/*
 * Locate the posting sets of one `col = ANY (array)` clause (DESIGN.md §15):
 * one set per distinct non-NULL element, which the count then unions.
 */
static int
lion_locate_array(LionClauseState *cl, LionPostingSet **sets)
{
	ArrayType  *arr = DatumGetArrayTypeP(cl->val);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			nsets;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	/*
	 * A parameter's array has no length cap (DESIGN.md §15), and past some
	 * nine million values the sets pass the 1GB a plain allocation may have.
	 * A plain count locates such a list a batch at a time instead
	 * (lion_count_batched()); every other shape holds it whole.
	 */
	*sets = (LionPostingSet *)
		palloc_extended(sizeof(LionPostingSet) * Max(nelems, 1),
						MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

	/*
	 * One call rather than a lookup per element: the values are hashed first
	 * and their entries located in (bucket, hash) order, so the bucket pages
	 * are read in block order and duplicates are dropped in one pass over
	 * that order instead of by comparing every value with every earlier one -
	 * which at LION_MAX_ARRAY_ELEMS values is half a million datumIsEqual()
	 * calls (DESIGN.md §15).  Looking a value up twice could not change the
	 * answer either way, because a union of a set with itself is that set; it
	 * would only cost the merge another sub-cursor.
	 */
	nsets = lion_posting_set_lookup_many_col(cl->idx, cl->idxcol, elemtype,
											nelems, elems, nulls, *sets, NULL);

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(cl->val))
		pfree(arr);

	return nsets;
}

/*
 * What one value of an IN list costs once located: its LionPostingSet, the
 * copies of its INLINE payload and stored key, its leaf of the source's tree
 * and the pointer to it - some 200 bytes, rounded up.
 */
#define LION_ARRAY_SET_BYTES	256

/*
 * The most values of one IN list a count locates at once (DESIGN.md §15, "A
 * list too long to locate at once"): what a work_mem of located sets holds,
 * and never fewer than the longest list a literal may be, which is located
 * whole as it always was.
 */
static int
lion_array_batch_size(void)
{
	Size		n = (Size) work_mem * 1024 / LION_ARRAY_SET_BYTES;

	n = Min(n, (Size) (INT_MAX / 2));
	return (int) Max(n, (Size) LION_MAX_ARRAY_ELEMS);
}

/*
 * Is WHERE item k, the IN list cl, to be located and counted a batch at a
 * time?  A parameter's array has no length cap (DESIGN.md §15), and the
 * located sets of a long one were all held at once, work_mem or not - about
 * 200 bytes a value, and past nine million values an array of them larger
 * than an allocation may be (2026-09-28 review).
 *
 * The count of a list is a SUM over pieces of it in one shape: a count of
 * the relation as one row (lion_count_relation()).  The entries of one
 * scalar index are disjoint (§15), so with the list cut into batches of
 * whole entries B_1 .. B_m, R the other positive sources and N the negated
 * ones,
 *
 *		|((B_1 ∪ ... ∪ B_m) ∩ R) \ N| = Σ_j |(B_j ∩ R) \ N|,
 *
 * the terms being disjoint - lion_run_batches()'s argument, one level up.  A
 * GROUP BY, a count(DISTINCT) and an FK-side join count the list many times
 * over and hold it whole, as a list under an OR does, whose union is not a
 * disjoint one.  One list per relation is batched.
 *
 * The values are sorted as a lookup sorts them (lion_probe_sort()) and a
 * batch ends only where the hash changes: two values of one equality class
 * hash alike and sort together, so no class is split and no entry is located
 * in two batches - the argument that lets a plain scan locate a long list
 * piece by piece (§29.4).  What is held whole is the values themselves: the
 * array, and a Datum and a hash per value.
 */
static bool
lion_array_batch_prepare(LionCountScanState *st, int k, LionClauseState *cl)
{
	ArrayType  *arr;
	Oid			elemtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;

	if (st->hasgroupidx || st->joinclause >= 0 || st->batchitem >= 0 ||
		cl->valisnull)
		return false;

	arr = DatumGetArrayTypeP(cl->val);
	if (ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) <= lion_array_batch_size())
	{
		if ((Pointer) arr != DatumGetPointer(cl->val))
			pfree(arr);
		return false;
	}

	elemtype = ARR_ELEMTYPE(arr);
	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	st->batchval = (Datum *)
		palloc_extended(sizeof(Datum) * Max(nelems, 1), MCXT_ALLOC_HUGE);
	st->batchhash = (uint32 *)
		palloc_extended(sizeof(uint32) * Max(nelems, 1), MCXT_ALLOC_HUGE);
	st->nbatchval = lion_probe_sort(cl->idx, cl->idxcol, elemtype, nelems,
									elems, nulls, st->batchval,
									st->batchhash);

	/*
	 * Byte-for-byte equal values are the same key, and the sort puts them
	 * side by side: keep one.  The lookup would skip the rest anyway, but a
	 * batch runs on past its size while the hash stays the same, so a list of
	 * one value repeated millions of times was one batch of that many sets.
	 */
	if (st->nbatchval > 1)
	{
		int			in;
		int			out = 1;

		for (in = 1; in < st->nbatchval; in++)
		{
			if (st->batchhash[in] == st->batchhash[out - 1] &&
				datumIsEqual(st->batchval[in], st->batchval[out - 1],
							 elmbyval, elmlen))
				continue;
			st->batchval[out] = st->batchval[in];
			st->batchhash[out] = st->batchhash[in];
			out++;
		}
		st->nbatchval = out;
	}
	st->batchtype = elemtype;
	st->batchitem = k;

	/* a by-reference value points into arr, which stays in wherecxt */
	pfree(elems);
	pfree(nulls);
	return true;
}

/*
 * Locate the posting sets of one multi-key clause (DESIGN.md §17).
 *
 * The query is extracted again here, with the index's OWN extractQuery
 * function - which lion_match_index() has already insisted is the one the
 * planner used - so the keys and the boolean tree are the same the plan was
 * costed with.  The tree becomes the source's combining expression; the
 * merge in lion_count.c evaluates it over the sets with the same cursors it
 * uses for an IN list, so the DESIGN.md §9 pin discipline is unchanged.
 *
 * A query the node only has at run time - a Param, a stable expression - is
 * extracted as a SUPERSET instead (DESIGN.md §17, "A query known only at run
 * time"), and cl->qmode says how it came out: KEYS is what a literal gives;
 * LOSSY locates the keys of a wider tree; NONE and ALL locate nothing, NONE
 * because no row can match and ALL because every row may.  The caller makes
 * the last two no source at all and a source that selects nothing, and
 * rechecks LOSSY and ALL in the heap (lion_build_filter()).
 */
static int
lion_locate_multikey(LionClauseState *cl, LionPostingSet **sets,
					LionKeyNode **tree)
{
	LionState   *istate = lion_index_column_state(cl->idx, cl->idxcol);
	StrategyNumber strategy;
	LionQuery	q;
	Buffer		lastpinned = InvalidBuffer;
	int			i;

	/*
	 * Only a multi-key column has an extractQuery to call; a scalar one's
	 * state leaves it unset (lion_fill_state()).  lion_open_relation() asked
	 * for a multi-key column, so this is drift - but it is an error, not a
	 * call through an empty FmgrInfo (the 2026-09-27 review).
	 */
	if (!istate->multikey)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("key column %d of lion index \"%s\" is not a multi-key column",
						(int) cl->idxcol, RelationGetRelationName(cl->idx))));

	strategy = (StrategyNumber)
		get_op_opfamily_strategy(cl->opno,
								 cl->idx->rd_opfamily[cl->idxcol - 1]);

	if (cl->con == NULL)
	{
		lion_extract_query_superset(istate, cl->val, strategy, &q);
		cl->qmode = q.mode;
		if (q.mode != LION_QMODE_KEYS && q.mode != LION_QMODE_LOSSY)
		{
			*sets = NULL;
			*tree = NULL;
			return 0;
		}
	}
	else
	{
		lion_extract_query(istate, cl->val, strategy, &q);

		/*
		 * The plan was only made because this extraction came out exact
		 * (lion_multikey_query_is_exact()), against this very function and
		 * this very constant.  A different answer now would mean the count
		 * could silently miss rows, so say so instead.
		 */
		if (q.mode != LION_QMODE_KEYS)
			elog(ERROR, "roaring count: query for index \"%s\" is no longer exact",
				 RelationGetRelationName(cl->idx));
		cl->qmode = LION_QMODE_KEYS;
	}

	*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(q.nkeys, 1));
	*tree = q.tree;

	/*
	 * Up to LION_MAX_QUERY_KEYS lookups, and every one of them may keep an
	 * INLINE leaf pinned for as long as the node runs: they draw on the list
	 * pin budget, as an IN list's do (DESIGN.md §15, "The pin budget").
	 */
	for (i = 0; i < q.nkeys; i++)
	{
		(void) lion_posting_set_lookup_budgeted_col(cl->idx, cl->idxcol,
													q.keys[i], InvalidOid,
													&(*sets)[i], &lastpinned);
		CHECK_FOR_INTERRUPTS();
	}

	return q.nkeys;
}

/*
 * Locate one clause's posting sets into *sets and return the tree that
 * combines them, its leaves numbered from 0.  NULL means the clause selects
 * no rows at all - a NULL parameter, an empty IN list - and then *nsets is 0
 * and nothing was located.
 *
 * This is the whole of a clause's run-time meaning, and it is the same
 * whether the clause is a source of its own or a leaf of an OR (DESIGN.md
 * §19).
 */
static LionKeyNode *
lion_locate_leaf(LionClauseState *cl, LionPostingSet **sets, int *nsets)
{
	LionKeyNode *tree = NULL;
	int			n = 0;
	int			i;

	*sets = NULL;
	*nsets = 0;
	cl->qmode = LION_QMODE_NONE;	/* until a multi-key query says otherwise */

	/*
	 * A parameter that came out NULL selects no rows at all, whatever the
	 * clause: `k = NULL`, `k = ANY (NULL)` and a NULL multi-key query are all
	 * never true (every one of those operators is strict).  The clause is
	 * then not looked up.
	 */
	if (cl->valisnull && LION_CLAUSE_IS_POSITIVE(cl->kind) &&
		cl->kind != LION_CLAUSE_NULL)
		return NULL;

	switch (cl->kind)
	{
		case LION_CLAUSE_EQ:
			*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet));
			n = 1;
			(void) lion_posting_set_lookup_col(cl->idx, cl->idxcol, cl->val,
											  cl->valtype, &(*sets)[0]);
			tree = lion_key_node(0);
			break;

		case LION_CLAUSE_ARRAY:
			n = lion_locate_array(cl, sets);
			if (n > 0)
			{
				LionKeyNode **args = (LionKeyNode **)
					palloc(sizeof(LionKeyNode *) * n);

				for (i = 0; i < n; i++)
					args[i] = lion_key_node(i);
				tree = lion_bool_node(LION_KN_OR, args, n);
			}
			break;

		case LION_CLAUSE_MULTI:
			n = lion_locate_multikey(cl, sets, &tree);
			if (n == 0)
				tree = NULL;
			break;

		case LION_CLAUSE_NULL:
		case LION_CLAUSE_NOTNULL:
			*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet));
			n = 1;
			if (lion_posting_set_lookup_null_col(cl->idx, cl->idxcol,
												&(*sets)[0]))
				tree = lion_key_node(0);
			else
			{
				/*
				 * No NULL entry at all: `IS NULL` selects nothing, and
				 * `IS NOT NULL` has nothing to subtract.
				 */
				n = 0;
				tree = NULL;
			}
			break;

		default:
			elog(ERROR, "LionCount: unknown clause kind %d", cl->kind);
	}

	*nsets = n;
	return tree;
}

/*
 * The range the RANGESRC clauses on heap column attno among clauses
 * [first, first + n) make - one OR arm's, or every clause outside an OR when
 * toplevel - ANDed into one, as a driving walk's bounds are (DESIGN.md §28),
 * and resolved against the index the planner matched them to.  That is one
 * index for all of them: a column's clauses are matched to the one index
 * that holds it (lion_match_index()), so two would be planner drift.
 */
static LionRangeSource *
lion_rangesrc_range(LionCountScanState *st, int first, int n, bool toplevel,
					AttrNumber attno)
{
	LionRangeSource *rs = (LionRangeSource *) palloc0(sizeof(LionRangeSource));
	int			i;

	for (i = first; i < first + n; i++)
	{
		LionClauseState *cl = &st->clause[i];
		StrategyNumber strategy;

		if (cl->kind != LION_CLAUSE_RANGESRC || cl->attno != attno ||
			(toplevel && st->inor[i]))
			continue;
		if (rs->index == NULL)
		{
			rs->index = cl->idx;
			rs->col = cl->idxcol;
			lion_range_init(&rs->range, cl->idx, cl->idxcol);
		}
		else if (RelationGetRelid(cl->idx) != RelationGetRelid(rs->index) ||
				 cl->idxcol != rs->col)
			elog(ERROR, "LionCount: the bounds of one range on two indexes");

		strategy = get_op_opfamily_strategy(cl->opno,
											cl->idx->rd_opfamily[cl->idxcol - 1]);
		lion_range_add(&rs->range, cl->idx, strategy, get_opcode(cl->opno),
					   cl->valtype, cl->val, cl->valisnull,
					   cl->idx->rd_indcollation[cl->idxcol - 1]);
	}
	if (rs->index == NULL)
		elog(ERROR, "LionCount: a range source without bounds");
	return rs;
}

/*
 * Locate a range taken as a source (DESIGN.md §32, "A range as a source"):
 * the rows whose key lies in it, which is the union of the disjoint sets a
 * walk of it hands out - entries, and the summaries of the buckets it covers
 * whole.  They are COLLECTED into one private set when that fits what is
 * left of a hash table's memory (get_hash_memory_limit(), shared by every
 * range of the relation), which the counts then read like any other set.
 * One that does not fit is left to be walked at every count instead
 * (src->rangewalk, lion_node_count()) - unless it is an OR's leaf, which
 * cannot be taken apart that way (`walkable` false; the planner declines one
 * it expects to be large).  That one used to be collected whatever it took,
 * in memory; it gets the same memory as the others now, and past it is
 * collected a window of container keys at a time into a temporary file
 * (lion_range_collect(), 2026-09-28 review).
 *
 * Returns the source's tree the way lion_locate_leaf() does: NULL when the
 * range selects nothing - an empty range, a NULL bound, or no row in it - and
 * also when it is to be walked, which `*walked` then says.
 */
static LionKeyNode *
lion_locate_range(LionCountScanState *st, LionRangeSource *rs, bool walkable,
				  LionPostingSet **sets, int *nsets, bool *walked)
{
	Size		limit = get_hash_memory_limit();
	Size		budget;
	LionPostingSet ps;
	Size		held;
	bool		spilled;
	int64		nread;
	int64		nsums;

	*sets = NULL;
	*nsets = 0;
	*walked = false;

	if (rs->range.empty)
		return NULL;

	budget = (st->rangesrc_held < limit) ? limit - st->rangesrc_held : 0;
	if (!lion_range_collect(rs->index, rs->col, &rs->range, budget, !walkable,
							&ps, &held, &spilled, &nread, &nsums))
	{
		st->rangesrc_walked++;
		*walked = true;
		return NULL;
	}

	st->rangesrc_collected++;
	if (spilled)
		st->rangesrc_spilled++;
	st->summaries += nsums;
	if (!ps.found)
		return NULL;
	st->rangesrc_held += held;

	*sets = (LionPostingSet *) palloc(sizeof(LionPostingSet));
	(*sets)[0] = ps;
	*nsets = 1;
	return lion_key_node(0);
}

/*
 * Locate one OR restriction as a single source: the union of its arms, each
 * arm the AND of its leaves (DESIGN.md §19).
 *
 * The leaves' sets are concatenated into one array, because a LionKeyNode
 * names a set by its position in the source's array; each leaf's own tree is
 * renumbered onto its slice of it.  An arm with a leaf that selects nothing
 * selects nothing itself and is dropped; an OR with no arm left selects
 * nothing at all.
 *
 * THE PIN RULE (DESIGN.md §9 and §19).  A source is only allowed to serve its
 * containers from pinless private copies while some OTHER positive source is
 * still read the pinned way, and `lion_source_pinned()` decides that per
 * source: for an OR it needs EVERY child to hold a pin, because which of them
 * contributed a given container key is not known in advance and a dead TID
 * may have come from a single one of them.  That is what makes this source
 * carry the interlock at all, and it is why the source is marked
 * nomaterialize: the leaves of a union are never copied out.
 */
static void
lion_locate_or(LionCountScanState *st, LionOrState *orst, LionCountSource *src)
{
	LionPostingSet **leafsets;
	LionKeyNode **leaftree;
	int		   *leafn;
	bool	   *absorbed;
	LionKeyNode **arms;
	int			narms = 0;
	int			total = 0;
	int			off = 0;
	int			leaf = 0;
	int			armfirst = 0;
	int			arm = 0;
	int			i;
	int			j;

	leafsets = (LionPostingSet **)
		palloc0(sizeof(LionPostingSet *) * orst->nleaves);
	leaftree = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * orst->nleaves);
	leafn = (int *) palloc0(sizeof(int) * orst->nleaves);
	absorbed = (bool *) palloc0(sizeof(bool) * orst->nleaves);

	for (i = 0; i < orst->nleaves; i++)
	{
		LionClauseState *cl = &st->clause[orst->first + i];

		while (i >= armfirst + orst->armlen[arm])
			armfirst += orst->armlen[arm++];

		/*
		 * A range in an arm (DESIGN.md §32) is one leaf however many bounds
		 * it has there: the first bound on its column stands for all of them
		 * and the others are absorbed into it.
		 */
		if (cl->kind == LION_CLAUSE_RANGESRC)
		{
			bool		walked;

			for (j = armfirst; j < i; j++)
			{
				LionClauseState *prev = &st->clause[orst->first + j];

				if (prev->kind == LION_CLAUSE_RANGESRC &&
					prev->attno == cl->attno)
					break;
			}
			if (j < i)
			{
				absorbed[i] = true;
				continue;
			}
			leaftree[i] = lion_locate_range(st,
											lion_rangesrc_range(st,
																orst->first + armfirst,
																orst->armlen[arm],
																false, cl->attno),
											false, &leafsets[i], &leafn[i],
											&walked);
			Assert(!walked);
		}
		else
			leaftree[i] = lion_locate_leaf(cl, &leafsets[i], &leafn[i]);
		total += leafn[i];
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * One array for the whole source, with every leaf's tree moved onto it,
	 * and each leaf's own array let go as it is copied: an IN list of a
	 * parameter can be millions of sets long (DESIGN.md §15).
	 */
	src->sets = (LionPostingSet *)
		palloc_extended(sizeof(LionPostingSet) * Max(total, 1),
						MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	src->nsets = total;
	src->nomaterialize = true;
	for (i = 0; i < orst->nleaves; i++)
	{
		if (leafn[i] > 0)
		{
			memcpy(&src->sets[off], leafsets[i],
				   sizeof(LionPostingSet) * leafn[i]);
			pfree(leafsets[i]);
			leafsets[i] = NULL;
		}
		lion_shift_keynos(leaftree[i], off);
		off += leafn[i];
	}
	Assert(off == total);

	arms = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * orst->narms);
	for (i = 0; i < orst->narms; i++)
	{
		LionKeyNode **conj = (LionKeyNode **)
			palloc0(sizeof(LionKeyNode *) * orst->armlen[i]);
		int			nconj = 0;
		bool		empty = false;

		for (j = 0; j < orst->armlen[i]; j++, leaf++)
		{
			if (absorbed[leaf])
				continue;		/* a bound of a range an earlier leaf is */
			if (leaftree[leaf] == NULL)
				empty = true;	/* an AND with a leaf that selects nothing */
			else
				conj[nconj++] = leaftree[leaf];
		}

		if (empty || nconj == 0)
			continue;			/* this arm contributes nothing to the union */

		arms[narms++] = lion_bool_node(LION_KN_AND, conj, nconj);
	}

	if (narms == 0)
	{
		src->tree = NULL;
		st->wheremissing = true;
		return;
	}

	src->tree = lion_bool_node(LION_KN_OR, arms, narms);

	/* Every arm wants a key no entry holds: the union selects nothing. */
	if (!lion_sets_satisfiable(src->nsets, src->sets, src->tree))
		st->wheremissing = true;
}

/*
 * Remember the key a clause's entry holds, so that a target list which prints
 * the pinned column can report it after the posting set is gone.  The first
 * relation that has the key wins; with partitions the others hold a key that
 * compares equal to it by the index's own equality, which is exactly the
 * guarantee the printed value rests on in the single-table case as well.
 */
static void
lion_save_clause_key(LionCountScanState *st, LionClauseState *cl,
					const LionPostingSet *ps)
{
	MemoryContext oldcxt;
	LionState   *istate;

	if (cl->hasstoredkey || !ps->found || !ps->hasstoredkey)
		return;

	if (ps->keyisnull)
	{
		cl->storedkey = (Datum) 0;
		cl->keyisnull = true;
		cl->hasstoredkey = true;
		return;
	}

	istate = lion_index_column_state(cl->idx, cl->idxcol);
	oldcxt = MemoryContextSwitchTo(st->keycxt);
	cl->storedkey = datumCopy(ps->storedkey, istate->typbyval, istate->typlen);
	MemoryContextSwitchTo(oldcxt);
	cl->keyisnull = false;
	cl->hasstoredkey = true;
}

/*
 * The type an argument of type `type` has when the parser hands it to a
 * function that declares `declared` (coerce_type(), parse_coerce.c), for the
 * row filter below, which rebuilds the call the executor would have made.  The
 * clause analysis peeled the relabels off both operands (lion_strip()), so the
 * column is a bare Var of its own type and the value an expression of its own
 * - a domain, say - where the parser had passed:
 *
 *	- the actual type, domains included, to an argument of that very type and
 *	  to one declared any, anyelement, anynonarray, anycompatible or
 *	  anycompatiblenonarray;
 *	- the BASE type to the other polymorphic types (anyarray, anyrange, ...),
 *	  which relabel a domain over an array, a range or an enum to what it is
 *	  over: a function declared on anyarray never sees the domain;
 *	- and the declared type itself to any other argument, which can only have
 *	  got here by a binary coercion - of a domain over that type, or of a
 *	  type binary-coercible to it.
 */
static Oid
lion_arg_type_passed(Oid type, Oid declared)
{
	if (!OidIsValid(declared) || type == declared)
		return type;

	switch (declared)
	{
		case ANYOID:
		case ANYELEMENTOID:
		case ANYNONARRAYOID:
		case ANYCOMPATIBLEOID:
		case ANYCOMPATIBLENONARRAYOID:
			return type;
		case ANYARRAYOID:
		case ANYENUMOID:
		case ANYRANGEOID:
		case ANYMULTIRANGEOID:
		case ANYCOMPATIBLEARRAYOID:
		case ANYCOMPATIBLERANGEOID:
		case ANYCOMPATIBLEMULTIRANGEOID:
			return getBaseType(type);
		default:
			return declared;
	}
}

/*
 * The heap recheck this scan's multi-key queries need (DESIGN.md §17, "A
 * query known only at run time"), as a row filter over the relation being
 * counted, set on the visibility cache every count of this execution is
 * handed (lion_vis_cache_set_filter()) - or none, which is what every scan
 * whose queries all came out exact gets.
 *
 * Each clause is `col op value`: the clause's own operator and its value as
 * this scan evaluated it, and the column as THIS relation numbers it, which
 * the index says (a partition may number it differently from the parent,
 * DESIGN.md §16).  The collation is the index column's, which
 * lion_match_index() found equal to the clause's input collation whenever
 * the clause has one; a clause over a type that has none ignores it.
 */
static void
lion_build_filter(LionCountScanState *st)
{
	MemoryContext oldcxt;
	LionRowFilter *filter;
	int			n = 0;
	int			i;

	st->filter = NULL;
	lion_vis_cache_set_filter(st->viscache, NULL);

	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];

		if (cl->kind == LION_CLAUSE_MULTI && !st->inor[i] &&
			(cl->qmode == LION_QMODE_LOSSY || cl->qmode == LION_QMODE_ALL))
			n++;
	}
	if (n == 0)
		return;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);
	filter = (LionRowFilter *) palloc0(sizeof(LionRowFilter));
	filter->heap = st->heap;
	filter->clauses = (LionRowFilterClause *)
		palloc0(sizeof(LionRowFilterClause) * n);
	filter->tmpcxt = AllocSetContextCreate(st->wherecxt,
										   "LionCount row filter",
										   ALLOCSET_SMALL_SIZES);
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];
		LionRowFilterClause *c;
		Form_pg_attribute att;
		Oid			lefttype;
		Oid			righttype;
		Oid			coltype;
		Oid			valtype;
		Expr	   *col;
		int16		typlen;
		bool		typbyval;

		if (!(cl->kind == LION_CLAUSE_MULTI && !st->inor[i] &&
			  (cl->qmode == LION_QMODE_LOSSY || cl->qmode == LION_QMODE_ALL)))
			continue;

		c = &filter->clauses[filter->nclauses++];
		c->attno = cl->idx->rd_index->indkey.values[cl->idxcol - 1];
		if (c->attno <= 0 || c->attno > RelationGetDescr(st->heap)->natts)
			elog(ERROR, "LionCount: a multi-key clause on an index expression");
		c->notnull = false;
		c->collation = cl->idx->rd_indcollation[cl->idxcol - 1];
		c->value = cl->val;
		fmgr_info_cxt(get_opcode(cl->opno), &c->flinfo, st->wherecxt);

		/*
		 * The call the executor would have made for the clause, expression
		 * included, which is what a polymorphic operator's function asks
		 * its argument types of (get_fn_expr_argtype()).  That includes the
		 * parser's relabels (lion_arg_type_passed()): a column of a domain
		 * over int[] reaches `@>(anyarray, anyarray)` as int[], and the Var
		 * alone would have shown the function the domain (the 2026-09-27
		 * review).  The value is a Const of the type the parser would have
		 * given it, as constant folding leaves a relabelled literal.
		 */
		att = TupleDescAttr(RelationGetDescr(st->heap), c->attno - 1);
		op_input_types(cl->opno, &lefttype, &righttype);
		col = (Expr *) makeVar(1, c->attno, att->atttypid, att->atttypmod,
							   att->attcollation, 0);
		coltype = lion_arg_type_passed(att->atttypid, lefttype);
		if (coltype != att->atttypid)
			col = (Expr *) makeRelabelType(col, coltype, -1,
										   type_is_collatable(coltype) ?
										   att->attcollation : InvalidOid,
										   COERCE_IMPLICIT_CAST);
		valtype = lion_arg_type_passed(cl->valtype, righttype);
		get_typlenbyval(valtype, &typlen, &typbyval);
		fmgr_info_set_expr((Node *)
						   make_opclause(cl->opno, BOOLOID, false, col,
										 (Expr *) makeConst(valtype, -1,
															InvalidOid,
															typlen, cl->val,
															false, typbyval),
										 InvalidOid, c->collation),
						   &c->flinfo);
	}
	MemoryContextSwitchTo(oldcxt);

	st->filter = filter;
	lion_vis_cache_set_filter(st->viscache, filter);
}

/*
 * Locate the posting sets of every WHERE clause of the relation the node is
 * counting.  They stay located until lion_release_where() - with partitions
 * for one partition's turn, with a plain table until the node is done - and
 * keep their pins (for INLINE entries) until the node first hands a row to
 * the executor, when they become NOPIN copies (lion_pause_run(); DESIGN.md
 * §15, "Paused and finished counts").  Until then that is the DESIGN.md
 * section 9 discipline applied for the length of the counts rather than for
 * one container; after it, each count takes its interlock from the set that
 * drives it.
 */
static void
lion_locate_where(LionCountScanState *st)
{
	MemoryContext oldcxt;
	int			k;

	/* what this locates may be pinned until the next pause unpins it */
	st->wherepinned = true;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);

	for (k = 0; k < st->nitem; k++)
	{
		LionClauseState *cl = &st->clause[st->item[k].clauseno];
		LionCountSource *src = &st->sources[k + 1];

		src->nsets = 0;
		src->sets = NULL;
		src->tree = NULL;
		src->negated = false;
		src->nomaterialize = false;
		src->disjoint = false;
		src->rangewalk = NULL;

		if (st->item[k].orno >= 0)
		{
			lion_locate_or(st, &st->ors[st->item[k].orno], src);
			continue;
		}

		/* A range as a source (DESIGN.md §32): collected, or walked. */
		if (st->item[k].rangesrc)
		{
			LionRangeSource *rs = lion_rangesrc_range(st, 0, st->nclause, true,
													  cl->attno);
			bool		walked;

			src->tree = lion_locate_range(st, rs, true, &src->sets,
										  &src->nsets, &walked);
			if (walked)
				src->rangewalk = rs;
			else if (src->tree == NULL)
				st->wheremissing = true;
			continue;
		}

		/*
		 * An IN list too long to locate at once, in a count that can take it
		 * a batch at a time (lion_array_batch_prepare()): the count locates
		 * it, and until then it is a positive source of no sets.
		 */
		if (cl->kind == LION_CLAUSE_ARRAY &&
			lion_array_batch_prepare(st, k, cl))
		{
			src->disjoint = true;
			continue;
		}

		src->negated = (cl->kind == LION_CLAUSE_NOTNULL);
		src->tree = lion_locate_leaf(cl, &src->sets, &src->nsets);

		/*
		 * A multi-key query no key narrows (DESIGN.md §17, "A query known
		 * only at run time"): as far as this clause goes every row is a
		 * candidate, so it is no source of the intersection - a negated
		 * source with nothing to subtract, which is exactly how `IS NOT NULL`
		 * over a column without NULLs reads - and the row filter below tests
		 * it on each candidate the other sources leave.
		 */
		if (cl->kind == LION_CLAUSE_MULTI && cl->qmode == LION_QMODE_ALL)
		{
			src->negated = true;
			continue;
		}

		/*
		 * An IN list is one scalar index's entries, one per distinct listed
		 * value: disjoint by construction, so a count of their union is the
		 * SUM of their counts and lion_count_sources() may skip the k-way
		 * merge entirely (DESIGN.md §15).  Only a clause that is a source of
		 * its own may say so; under an OR the source's sets are several
		 * clauses' and overlap freely.
		 */
		src->disjoint = (cl->kind == LION_CLAUSE_ARRAY);

		/*
		 * A positive clause that can select nothing makes the whole count 0,
		 * and saying so here saves the merge - and, in the GROUP BY path,
		 * every group of it.  A negated one simply has nothing to subtract.
		 */
		if (LION_CLAUSE_IS_POSITIVE(cl->kind) &&
			!lion_sets_satisfiable(src->nsets, src->sets, src->tree))
			st->wheremissing = true;

		/*
		 * Only a clause that pins the column to ONE value can have its key
		 * printed, and those are the ones with a single set.
		 */
		if (LION_CLAUSE_PINS_VALUE(cl->kind) && src->nsets == 1)
			lion_save_clause_key(st, cl, &src->sets[0]);
	}

	/*
	 * The RANGE clauses, as one bound on the driving walk (DESIGN.md §28),
	 * resolved against THIS relation's driving index: a partition's may be
	 * another index, put its column elsewhere and compare the bounds through
	 * a family of its own (§16).  The planner found every one of them on the
	 * driving index, and the executor opened both by the plan's Oids, so a
	 * clause on another index or column is drift and not a query.  A NULL
	 * bound - a Param that came out NULL - selects nothing.
	 */
	if (st->hasrange)
	{
		lion_range_init(&st->range, st->groupidx, st->groupidxcol);
		for (k = 0; k < st->nclause; k++)
		{
			LionClauseState *cl = &st->clause[k];
			StrategyNumber strategy;

			if (cl->kind != LION_CLAUSE_RANGE)
				continue;
			if (RelationGetRelid(cl->idx) != RelationGetRelid(st->groupidx) ||
				cl->idxcol != st->groupidxcol)
				elog(ERROR, "LionCount: range clause on another index than the driving one");
			strategy = get_op_opfamily_strategy(cl->opno,
												st->groupidx->rd_opfamily[st->groupidxcol - 1]);
			lion_range_add(&st->range, st->groupidx, strategy,
						   get_opcode(cl->opno), cl->valtype, cl->val,
						   cl->valisnull,
						   st->groupidx->rd_indcollation[st->groupidxcol - 1]);
		}
		if (st->range.empty)
			st->wheremissing = true;
	}

	MemoryContextSwitchTo(oldcxt);

	/*
	 * Can an IN list drive the groups instead of the index's entry scan
	 * (DESIGN.md §15)?  Only when the clause's index IS the index that would
	 * drive them - same relation, same column, so the entries are the same
	 * entries - and the grouping is the plain one-column form.  A two-column
	 * GROUP BY (§20) and a sum-over-all (§14) both walk the entries for
	 * reasons of their own and are left alone, and so does the (g, k) loop of
	 * a count(DISTINCT k) per group (§26), whose groups are emitted in g's
	 * directory order, which the planner may have claimed as pathkeys.
	 *
	 * The count(DISTINCT k) walk WITHOUT a GROUP BY is driven like a GROUP BY
	 * k whose groups are summed (§26), and takes a list on k the same way -
	 * and an equality on k as a list of one, which a GROUP BY never sees
	 * (the planner folds the grouping column it pins).
	 */
	st->ingroupitem = -1;
	st->ingroupset = 0;
	if (st->hasgroupidx && st->driveattno != 0 && st->innerattno == 0 &&
		!st->sumall)
	{
		for (k = 0; k < st->nitem; k++)
		{
			LionClauseState *cl;

			if (st->item[k].orno >= 0)
				continue;
			cl = &st->clause[st->item[k].clauseno];
			if (!(cl->kind == LION_CLAUSE_ARRAY ||
				  (cl->kind == LION_CLAUSE_EQ && st->distattno != 0)) ||
				cl->attno != st->driveattno ||
				cl->idxoid != st->groupidxoid ||
				cl->idxcol != st->groupidxcol)
				continue;
			st->ingroupitem = k;
			break;
		}
	}
	st->ingroupleft = 0;
	if (st->ingroupitem >= 0)
	{
		LionCountSource *src = &st->sources[st->ingroupitem + 1];

		for (k = 0; k < src->nsets; k++)
		{
			if (src->sets[k].found)
				st->ingroupleft++;
		}
	}

	/*
	 * And the sum-over-all's own `IS NOT NULL` (DESIGN.md §14, the sumallitem
	 * half of the comment on the field).  The clause has to be the one on the
	 * DRIVING index AND on its driving KEY COLUMN: another column's NULL entry
	 * says nothing about this column's entries, and since DESIGN.md §24 the
	 * two may live in one relation, so the Oid alone no longer tells them
	 * apart (`a IS NOT NULL AND b IS NOT NULL` over one index on (a, b) would
	 * otherwise drop b's NULL set and subtract a's from a's own entries,
	 * which removes nothing: every b NULL would be counted).
	 *
	 * The count(DISTINCT k) walk without a GROUP BY (§26) drops a
	 * `k IS NOT NULL` the same way, for the same reason: the NULL entry is
	 * the only one it removes anything from, and that entry is never a
	 * distinct value.
	 */
	st->sumallitem = -1;
	if ((st->sumall || (st->distattno != 0 && st->groupattno == 0)) &&
		st->hasgroupidx)
	{
		for (k = 0; k < st->nitem; k++)
		{
			LionClauseState *cl;

			if (st->item[k].orno >= 0)
				continue;
			cl = &st->clause[st->item[k].clauseno];
			if (cl->kind != LION_CLAUSE_NOTNULL ||
				cl->attno != st->driveattno ||
				cl->idxoid != st->groupidxoid ||
				cl->idxcol != st->groupidxcol)
				continue;
			st->sumallitem = k;
			break;
		}
	}

	if (st->ingroupitem >= 0 || st->sumallitem >= 0)
	{
		int			drop = (st->ingroupitem >= 0) ? st->ingroupitem
			: st->sumallitem;
		int			n = 1;		/* slot 0 is the driver's own set */

		st->dsources[0] = st->sources[0];
		for (k = 0; k < st->nitem; k++)
		{
			if (k == drop)
				continue;
			st->dsources[n++] = st->sources[k + 1];
		}
		st->ndsource = n;
	}

	lion_build_filter(st);
	st->located = true;
}

static void
lion_release_where(LionCountScanState *st)
{
	int			i;
	int			j;

	if (st->sources == NULL)
		return;

	for (i = 0; i < st->nitem; i++)
	{
		LionCountSource *src = &st->sources[i + 1];

		for (j = 0; j < src->nsets; j++)
			lion_posting_set_release(&src->sets[j]);
		src->nsets = 0;
		src->sets = NULL;
		src->tree = NULL;		/* it lived in wherecxt, reset below */
		src->nomaterialize = false;
		src->disjoint = false;
		src->rangewalk = NULL;
	}
	st->rangesrc_held = 0;

	/*
	 * The collected WHERE (lion_group_count()): its file, if it spilled, is
	 * closed here, and its memory goes with wherecxt below.
	 */
	lion_posting_set_release(&st->wherecoll);
	st->wtried = false;
	st->wcollected = false;
	st->wknown = false;
	st->wcompound = false;
	st->wsingle = NULL;
	st->wreach = 0;
	st->wspent = 0;
	st->wcounts = 0;
	st->wckeys = 0;

	/* ... and so do the values of a list counted in batches */
	st->batchitem = -1;
	st->batchval = NULL;
	st->batchhash = NULL;
	st->nbatchval = 0;

	/* the row filter lives in wherecxt too, and names this relation */
	st->filter = NULL;
	lion_vis_cache_set_filter(st->viscache, NULL);

	if (st->wherecxt != NULL)
		MemoryContextReset(st->wherecxt);
	st->located = false;
	st->wheremissing = false;
	st->ingroupitem = -1;
	st->ingroupset = 0;
	st->ingroupleft = 0;
	st->sumallitem = -1;
}

static TupleTableSlot *
lion_emit_tuple(LionCountScanState *st, Datum key, bool keyisnull,
			   Datum key2, bool key2isnull, int64 count)
{
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	int			i;

	/*
	 * What the previous group's projection and HAVING allocated is dead once
	 * the executor asks for the next tuple (ExecScan() resets at the same
	 * point).  Nothing of the node's own lives in this memory: the clause
	 * values were copied out of it (lion_eval_clause_values()).
	 */
	ResetExprContext(econtext);

	ExecClearTuple(slot);
	for (i = 0; i < st->ntlist; i++)
	{
		int			kind = st->tlkind[i];

		/*
		 * A dimension column of the FK-side join, from the child's row - of
		 * which a summed join's one row has none: nothing reads its dimension
		 * columns (LION_JOINFLAG_SUM).
		 */
		if (LION_TL_IS_CHILDCOL(kind))
		{
			if (st->childslot == NULL)
			{
				Assert(st->joinsum);
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
				continue;
			}
			slot->tts_values[i] = slot_getattr(st->childslot,
											   LION_TL_CHILDRESNO(kind),
											   &slot->tts_isnull[i]);
			continue;
		}

		slot->tts_isnull[i] = false;
		switch (kind)
		{
			case LION_TL_GROUPKEY:
				slot->tts_values[i] = key;
				slot->tts_isnull[i] = keyisnull;
				break;

			case LION_TL_GROUPKEY2:
				slot->tts_values[i] = key2;
				slot->tts_isnull[i] = key2isnull;
				break;

			case LION_TL_COUNT:
				slot->tts_values[i] = Int64GetDatum(count);
				break;

			case LION_TL_COUNT_GROUPCOL:
				/* count(group column): 0 in the NULL group (DESIGN.md §14) */
				slot->tts_values[i] = Int64GetDatum(keyisnull ? 0 : count);
				break;

			case LION_TL_COUNT_GROUPCOL2:
				slot->tts_values[i] = Int64GetDatum(key2isnull ? 0 : count);
				break;

			case LION_TL_COUNT_ZERO:
				/* count(col) where a clause pins col to NULL */
				slot->tts_values[i] = Int64GetDatum(0);
				break;

			case LION_TL_COUNT_DISTINCT:
				/* count(DISTINCT k) of the finished group (DESIGN.md §26) */
				slot->tts_values[i] = Int64GetDatum(st->distcount);
				break;

			case LION_TL_COUNT_DISTCOL:
				/* count(k): the rows of k's non-NULL entries (§26) */
				slot->tts_values[i] = Int64GetDatum(st->distcolcount);
				break;

			default:
				{
					/*
					 * A column a clause pins to one value: report the key the
					 * index stored for it, which is the value the heap holds
					 * (or NULL, for an `IS NULL` clause).
					 */
					LionClauseState *cl = &st->clause[kind - LION_TL_WHEREKEY];

					/*
					 * A clause with no entry anywhere counts zero rows, and a
					 * group of zero rows is never emitted, so the key is
					 * always there by the time we get here.
					 */
					Assert(cl->hasstoredkey);
					if (cl->hasstoredkey)
					{
						slot->tts_values[i] = cl->storedkey;
						slot->tts_isnull[i] = cl->keyisnull;
					}
					else
					{
						slot->tts_values[i] = (Datum) 0;
						slot->tts_isnull[i] = true;
					}
					break;
				}
		}
	}
	ExecStoreVirtualTuple(slot);

	econtext->ecxt_scantuple = slot;

	/*
	 * HAVING (DESIGN.md §10): the plan's qual, rewritten by setrefs.c to read
	 * the counts and keys of this very tuple.  A group that fails it is
	 * consumed like any other; the caller fetches the next one.
	 */
	if (st->css.ss.ps.qual != NULL && !ExecQual(st->css.ss.ps.qual, econtext))
	{
		InstrCountFiltered1(st, 1);
		st->filtered = true;
		return NULL;
	}

	if (st->css.ss.ps.ps_ProjInfo != NULL)
		return ExecProject(st->css.ss.ps.ps_ProjInfo);
	return slot;
}

/* ---------------------------------------------------------------------
 * Counting one relation
 *
 * These three are what a plain table and one partition of a partitioned one
 * have in common (DESIGN.md §16): the caller has opened the relation with
 * lion_open_relation() and located its WHERE clauses, and every posting set
 * they take is released before they return, so no pin of this relation
 * outlives its turn.
 * --------------------------------------------------------------------- */

/*
 * Is any WHERE item of the relation being counted a source that SELECTS
 * rows?  Always, except when a multi-key query no key narrows was the only
 * one (DESIGN.md §17, "A query known only at run time"): it is no source then
 * (lion_locate_where()), and `IS NOT NULL` only subtracts.
 */
static bool
lion_where_has_positive(LionCountScanState *st)
{
	int			k;

	for (k = 0; k < st->nitem; k++)
	{
		if (!st->sources[k + 1].negated)
			return true;
	}
	return false;
}

/*
 * A count with nothing to take its candidates from: every WHERE clause that
 * selects rows was a multi-key query no key narrows, so the candidates are
 * every row of the relation, and the heap is read once, sequentially, with
 * the row filter - those queries - and the `IS NOT NULL` clauses tested on
 * each row the snapshot sees.  It is the ordinary plan's work, which the
 * cost model charged for a value it could not estimate (lion_cost_recheck()).
 */
static int64
lion_count_scan_filtered(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	LionRowFilter scan;
	int			k;

	scan = *st->filter;
	scan.clauses = (LionRowFilterClause *)
		palloc0(sizeof(LionRowFilterClause) * (st->filter->nclauses + st->nitem));
	memcpy(scan.clauses, st->filter->clauses,
		   sizeof(LionRowFilterClause) * st->filter->nclauses);
	for (k = 0; k < st->nitem; k++)
	{
		LionClauseState *cl = &st->clause[st->item[k].clauseno];
		LionRowFilterClause *c;

		if (st->item[k].orno >= 0 || cl->kind != LION_CLAUSE_NOTNULL)
			continue;
		c = &scan.clauses[scan.nclauses++];
		c->attno = cl->idx->rd_index->indkey.values[cl->idxcol - 1];
		c->notnull = true;
	}

	return lion_count_heap_filtered(st->heap, estate->es_snapshot, &scan,
									&st->stats);
}

/*
 * The intersection of the WHERE clauses, with no index driving the count.
 */
/*
 * Is src a positive source that can drive a count of F minus a NULL entry
 * (the complement of DESIGN.md §28)?  A COLLECTED set - a range taken as a
 * source (§32) - cannot: it holds no pin, and a count it drove alone would
 * have nothing to carry the §9 interlock and would recheck every row in the
 * heap.  A range still to be walked can: its pieces are located sets.
 */
static bool
lion_source_drives(const LionCountSource *src)
{
	if (src->negated)
		return false;
	if (src->rangewalk != NULL)
		return true;
	if (src->nsets == 1 && src->sets[0].found && src->sets[0].mat != NULL &&
		!BlockNumberIsValid(src->sets[0].head))
		return false;
	return true;
}

static int64 lion_walk_count(LionCountScanState *st, LionCountSource *sources,
							 int nsource, int w, bool exists);

/*
 * Every count the node makes, and every existence test (`exists`: 1 or 0),
 * goes through here: the AND of sources, as lion_count_sources_cached()
 * counts it - once any range taken as a source that was too large to collect
 * (LionCountSource.rangewalk; DESIGN.md §32) has been expanded.  Such a range
 * is the union of the disjoint sets a walk of it hands out, so the count is
 * the SUM of the counts of those sets, each ANDed with the other sources in
 * its place (lion_walk_count()); the counts of the pieces come back through
 * here and expand the next such range, if there is one.
 */
static int64
lion_node_count(LionCountScanState *st, int nsource, LionCountSource *sources,
				bool exists)
{
	EState	   *estate = st->css.ss.ps.state;
	int			w;

	for (w = 0; w < nsource; w++)
	{
		if (sources[w].rangewalk != NULL)
			return lion_walk_count(st, sources, nsource, w, exists);
	}

	if (exists)
		return lion_exists_sources_cached(st->heap, estate->es_snapshot,
										  nsource, sources, &st->stats,
										  st->viscache, st->rel_read_only) ?
			1 : 0;
	return lion_count_sources_cached(st->heap, estate->es_snapshot, nsource,
									 sources, &st->stats, st->viscache,
									 st->rel_read_only);
}

/*
 * The count of the relation with its IN list located a batch at a time
 * (lion_array_batch_prepare()): the sum of the counts of the batches, each
 * ANDed with the other sources exactly as the whole list would have been.  A
 * batch is lion_array_batch_size() values, moved on to where the hash
 * changes, and its sets are released - pins and all - before the next one is
 * located, so a list of any length holds one batch of sets at a time.  A
 * batch none of whose values has an entry adds nothing, as a list with no
 * entry makes the whole count 0.
 */
static int64
lion_count_batched(LionCountScanState *st)
{
	LionClauseState *cl = &st->clause[st->item[st->batchitem].clauseno];
	LionCountSource *src = &st->sources[st->batchitem + 1];
	int			batch = lion_array_batch_size();
	MemoryContext batchcxt;
	MemoryContext oldcxt;
	int64		count = 0;
	int			start = 0;

	batchcxt = AllocSetContextCreate(st->wherecxt, "LionCount list batch",
									 ALLOCSET_DEFAULT_SIZES);
	while (start < st->nbatchval)
	{
		int			end = start + Min(batch, st->nbatchval - start);
		LionPostingSet *sets;
		int			nsets;
		int			nfound;
		int			i;

		while (end < st->nbatchval &&
			   st->batchhash[end] == st->batchhash[end - 1])
			end++;

		st->listbatches++;
		oldcxt = MemoryContextSwitchTo(batchcxt);
		sets = (LionPostingSet *)
			palloc_extended(sizeof(LionPostingSet) * (end - start),
							MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		nsets = lion_posting_set_lookup_many_col(cl->idx, cl->idxcol,
												 st->batchtype, end - start,
												 &st->batchval[start], NULL,
												 sets, &nfound);
		src->sets = sets;
		src->nsets = nsets;
		src->tree = NULL;
		if (nfound > 0)
		{
			LionKeyNode **args = (LionKeyNode **)
				palloc(sizeof(LionKeyNode *) * nsets);

			for (i = 0; i < nsets; i++)
				args[i] = lion_key_node(i);
			src->tree = lion_bool_node(LION_KN_OR, args, nsets);
		}
		MemoryContextSwitchTo(oldcxt);

		if (src->tree != NULL)
		{
			MemoryContextReset(st->pergroup);
			oldcxt = MemoryContextSwitchTo(st->pergroup);
			count += lion_node_count(st, st->nitem, &st->sources[1], false);
			MemoryContextSwitchTo(oldcxt);
		}

		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		src->sets = NULL;
		src->nsets = 0;
		src->tree = NULL;
		MemoryContextReset(batchcxt);
		start = end;
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextDelete(batchcxt);

	return count;
}

static int64
lion_count_relation(LionCountScanState *st)
{
	MemoryContext oldcxt;
	int64		count;

	Assert(st->nitem > 0);
	if (st->wheremissing)
		return 0;

	/* the positive list is there, so this is never the filtered scan below */
	if (st->batchitem >= 0)
		return lion_count_batched(st);

	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);
	if (st->filter != NULL && !lion_where_has_positive(st))
		count = lion_count_scan_filtered(st);
	else
		count = lion_node_count(st, st->nitem, &st->sources[1], false);
	MemoryContextSwitchTo(oldcxt);

	return count;
}

/*
 * A SUMMED WALK PROBED AT THE OTHER SOURCES' ROWS (DESIGN.md §32, "Summed
 * ranges: dense and probed").
 *
 * A walk over a summarized column counts each set it hands out - a key of a
 * partial bucket, a whole bucket's summary - ANDed with the other sources F.
 * On a column whose rows lie all over the heap a summary has a few rows at
 * nearly every container key, and each of those counts is a merge of
 * thousands of containers with F, however few rows F has.  Once the walk has
 * handed out LION_PROBE_SWITCH times the rows of F's smallest positive source
 * - it has shown itself to be the larger side - the rest of it is PROBED
 * instead (lion_range_probe_begin()): F is collected once, each further set
 * is read only at F's container keys, and the rows it holds among F's are
 * counted ONCE at the end of the walk.  The sets counted before the switch
 * and the ones probed after it are disjoint, as every set of the walk is, so
 * their counts add up.
 *
 * The switch is bounded regret: the probe costs a count of F to collect and
 * one to finish, and it is only taken after the walk has already read more
 * than F holds.  It is not taken when F has no positive source, holds a
 * range still to be walked, would not carry the §9 interlock, or does not fit
 * work_mem - the walk then counts every set as it always has - nor for a walk
 * that uses no summaries: a column without them keeps what §28 measured.
 */
typedef struct LionSumProbe
{
	double		frows;			/* F's smallest positive source's rows, or
								 * -1: never probe */
	double		walked;			/* rows of the sets handed out so far */
	bool		tried;
	LionRangeProbe *rp;
	MemoryContext cxt;			/* where the probe is begun: outlives the
								 * walk's batches */
} LionSumProbe;

static void
lion_sum_probe_init(LionSumProbe *sp, const LionEntryScan *walk,
					const LionCountSource *sources, int nsource, int slot)
{
	Size		maxbytes = (Size) work_mem * 1024;
	int			i;
	int			j;

	sp->frows = -1.0;
	sp->walked = 0.0;
	sp->tried = false;
	sp->rp = NULL;
	sp->cxt = CurrentMemoryContext;

	if (!walk->usesum)
		return;
	for (i = 0; i < nsource; i++)
	{
		double		rows = 0.0;

		if (i == slot)
			continue;
		if (sources[i].rangewalk != NULL)
		{
			sp->frows = -1.0;
			return;
		}
		if (sources[i].negated)
			continue;
		for (j = 0; j < sources[i].nsets; j++)
			if (sources[i].sets[j].found)
				rows += (double) sources[i].sets[j].ntids;
		if (sp->frows < 0.0 || rows < sp->frows)
			sp->frows = rows;
	}

	/* a copy of F takes some two bytes a row: not worth trying past memory */
	if (sp->frows * 2.0 * sizeof(uint16) > (double) maxbytes)
		sp->frows = -1.0;
}

/*
 * The walk has handed out `rows` more: the probe, if the rest of it is to be
 * probed - begun now, when this is where the walk has shown itself to be the
 * larger side - or NULL.
 */
static LionRangeProbe *
lion_sum_probe_due(LionCountScanState *st, LionSumProbe *sp, double rows,
				   LionCountSource *sources, int nsource, int slot)
{
	EState	   *estate = st->css.ss.ps.state;
	LionCountSource *others;
	MemoryContext oldcxt;
	int			n = 0;
	int			i;

	sp->walked += rows;
	if (sp->rp != NULL || sp->tried || sp->frows < 0.0 ||
		sp->walked < LION_PROBE_SWITCH * sp->frows)
		return sp->rp;
	sp->tried = true;

	oldcxt = MemoryContextSwitchTo(sp->cxt);
	others = (LionCountSource *) palloc(sizeof(LionCountSource) * nsource);
	for (i = 0; i < nsource; i++)
		if (i != slot)
			others[n++] = sources[i];
	sp->rp = lion_range_probe_begin(st->heap, estate->es_snapshot, n, others,
									(Size) work_mem * 1024, &st->stats);
	pfree(others);
	MemoryContextSwitchTo(oldcxt);

	if (sp->rp != NULL)
		st->rangeprobed++;
	return sp->rp;
}

/* The count of what the probe gathered: the rest of the walk's sum. */
static int64
lion_sum_probe_finish(LionCountScanState *st, LionSumProbe *sp)
{
	EState	   *estate = st->css.ss.ps.state;
	int64		count;

	if (sp->rp == NULL)
		return 0;
	count = lion_range_probe_count(sp->rp, st->heap, estate->es_snapshot,
								   &st->stats, st->viscache,
								   st->rel_read_only);
	lion_range_probe_end(sp->rp);
	sp->rp = NULL;
	return count;
}

/*
 * Sum the counts of the entries one walk of the driving column returns - all
 * of them (LION_WALK_ALL), or one part of a range's (LION_WALK_*) - each ANDed
 * with sources[1 .. nsource - 1]; sources[0] is the driver's slot, which is
 * borrowed for the walk's entries and handed back as it was.
 *
 * The entries of one directory leaf are taken together (DESIGN.md §28,
 * "Counting a walk").  When they are small they are counted as ONE source
 * whose sets are DISJOINT - distinct entries of one scalar column - so the
 * count of the batch is the count of their union ANDed with the rest, and the
 * sum over the leaves is the sum over the entries: §15's argument, the one the
 * sum-over-all has always rested on, taken a leaf at a time instead of an
 * entry at a time.  lion_count_sources_cached() then counts the batch as §15
 * counts an IN list - summed set by set when it is the only positive source
 * and summing is cheaper, merged as a union otherwise.  Large entries are
 * counted one by one, as they always were (LION_SUM_UNION_MAX_ITEMS).
 *
 * Pins (DESIGN.md §9): every INLINE set of the batch holds the pin of the one
 * leaf it was copied from until it has been counted, so a walk of any width
 * holds one directory leaf pinned.  The WHERE sources are the relation's,
 * located once, and are materialized on their second use as for any GROUP BY;
 * the batch is then the positive source that carries the interlock.
 */
static int64
lion_sum_walk(LionCountScanState *st, LionCountSource *sources, int nsource,
			  int part)
{
	LionCountSource saved = sources[0];
	LionPostingSet *sets;
	LionSumProbe probe;
	int64		total = 0;
	int			i;

	/*
	 * A sum only adds up what the walk hands out, so it may be handed the
	 * column's SUMMARY entries in place of the keys they cover (DESIGN.md
	 * §32): still disjoint sets, still exactly the rows of the part.
	 */
	lion_entry_scan_begin_sum(&st->escan, st->groupidx, st->groupidxcol,
							  st->hasrange ? &st->range : NULL, part);
	st->scanning = true;

	/* One leaf's entries, which is at most what lion_count.c copies of one. */
	sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) *
									 st->escan.maxbatch);
	lion_sum_probe_init(&probe, &st->escan, sources, nsource, 0);

	for (;;)
	{
		MemoryContext oldcxt;
		LionRangeProbe *rp;
		int			nsets = 0;
		int			per;
		double		items = 0;
		double		rows = 0;
		Datum		key;
		bool		more = true;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		/*
		 * The first entry may read a new leaf; the rest are what that leaf
		 * holds of the walk.
		 */
		do
		{
			if (!lion_entry_scan_next(&st->escan, &key, &sets[nsets]))
			{
				more = false;
				break;
			}

			/* The NULL entry's rows are the ones `IS NOT NULL` excludes. */
			if (st->sumallitem >= 0 && sets[nsets].keyisnull)
			{
				lion_posting_set_release(&sets[nsets]);
				continue;
			}
			items += sets[nsets].ncontainers;
			rows += (double) sets[nsets].ntids;
			nsets++;
		} while (lion_entry_scan_batch_left(&st->escan) > 0);

		/*
		 * Past the rows of the other sources, the rest of the walk is probed
		 * at theirs and counted once at the end (lion_sum_probe_due()).
		 */
		rp = lion_sum_probe_due(st, &probe, rows, sources, nsource, 0);
		if (rp != NULL)
		{
			for (i = 0; i < nsets; i++)
				lion_range_probe_add(rp, &sets[i]);
			st->stats.sets_summed += nsets;
		}

		/* Small entries are one count, large ones one count each. */
		per = (items <= (double) nsets * LION_SUM_UNION_MAX_ITEMS) ?
			Max(nsets, 1) : 1;

		for (i = 0; rp == NULL && i < nsets; i += per)
		{
			int64		summed = st->stats.sets_summed;
			int			n = Min(per, nsets - i);

			sources[0].nsets = n;
			sources[0].sets = &sets[i];
			sources[0].tree = NULL;
			sources[0].negated = false;
			sources[0].nomaterialize = false;
			sources[0].disjoint = (n > 1);

			total += lion_node_count(st, nsource, sources, false);

			/*
			 * "Posting Sets Summed" is the entries whose counts the walk added
			 * up, however they were counted.
			 */
			st->stats.sets_summed = summed + n;
		}

		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		MemoryContextSwitchTo(oldcxt);

		if (!more)
			break;
	}

	sources[0] = saved;
	pfree(sets);
	st->summaries += st->escan.nsummaries;
	lion_entry_scan_end(&st->escan);
	st->scanning = false;

	/* the probed rest of the walk, counted once (§9 as for any count) */
	total += lion_sum_probe_finish(st, &probe);
	return total;
}

/*
 * count(*) WHERE k IS NOT NULL AND <sources[1 .. nsource - 1]>, k being the
 * driving column: the WHERE sources minus k's reserved NULL entry, which is
 * what `k IS NOT NULL` is as a source (DESIGN.md §14) - one merge, however
 * many entries k has.  The caller has checked that a positive source is among
 * them (lion_range_choose()).
 */
static int64
lion_count_nonnull(LionCountScanState *st, LionCountSource *sources,
				   int nsource)
{
	LionCountSource *srcs;
	LionPostingSet nullset;
	MemoryContext oldcxt;
	int64		count;
	int			n = 0;
	int			i;

	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);

	srcs = (LionCountSource *) palloc0(sizeof(LionCountSource) * nsource);
	for (i = 1; i < nsource; i++)
		srcs[n++] = sources[i];

	if (lion_posting_set_lookup_null_col(st->groupidx, st->groupidxcol,
										 &nullset))
	{
		srcs[n].nsets = 1;
		srcs[n].sets = &nullset;
		srcs[n].tree = NULL;
		srcs[n].negated = true;
		n++;
	}

	count = lion_node_count(st, n, srcs, false);
	lion_posting_set_release(&nullset);
	MemoryContextSwitchTo(oldcxt);

	return count;
}

/*
 * How to evaluate the range that bounds a sum (DESIGN.md §28, "The
 * complement").  The VALUE entries of the range's column are three runs in
 * directory order - BELOW the range, INSIDE it, ABOVE it - and the rows the
 * sum wants are those of INSIDE ANDed with the WHERE sources F.  The entries
 * being disjoint and every row with a value being under exactly one of them,
 *
 *		|INSIDE ∩ F| = |F − NULL(k)| − |BELOW ∩ F| − |ABOVE ∩ F|
 *
 * and |F − NULL(k)| is one merge.  So the sum can walk whichever side has
 * fewer entries, and when BELOW and ABOVE are both empty - the range covers
 * every key the column has - it walks nothing at all.
 *
 * Which side is smaller is decided here exactly, not from statistics: the
 * INSIDE walk and the BELOW-then-ABOVE walk are stepped a leaf at a time in
 * turn, counting what each would return without locating anything
 * (lion_entry_scan_skip_leaf()), until one of them runs out.  That costs at
 * most twice the leaves of the side that is then walked, which is small next
 * to counting it, and it is what makes "every key is inside" an exact answer:
 * the walks of BELOW and ABOVE ran to their ends and found no entry.  (An
 * entry with a visible row exists from before the snapshot until the count is
 * over - VACUUM deletes only empty ones - so a walk that saw none saw that
 * there is none.  The same holds for the walks that count BELOW and ABOVE
 * afterwards, which are ordinary walks.)
 *
 * The complement is not taken when
 *
 *	- the range is unordered, or empty: there is no run to take apart, or no
 *	  row to count;
 *	- no WHERE source is positive: |F − NULL(k)| has nothing to drive it, and
 *	  a count of every row of the table is not something an index can give;
 *	- the driving index is PARTIAL: its entries hold only the rows its
 *	  predicate admits, and F − NULL(k) counts the others too.
 */
static int
lion_range_choose_on(Relation index, AttrNumber col, LionRange *range,
					 LionCountSource *sources, int nsource, int skip)
{
	LionEntryScan in;
	LionEntryScan below;
	LionEntryScan above;
	int64		nin = 0;
	int64		nout = 0;
	bool		positive = false;
	int			eval;
	int			i;

	if (!range->ordered || range->empty)
		return LION_RANGE_EVAL_INSIDE;

	/*
	 * |F − NULL(k)| needs a source of F that can drive it, which a range
	 * collected as a source (DESIGN.md §32) cannot (lion_source_drives()).
	 */
	for (i = 0; i < nsource; i++)
		if (i != skip && lion_source_drives(&sources[i]))
			positive = true;
	if (!positive)
		return LION_RANGE_EVAL_INSIDE;
	if (RelationGetIndexPredicate(index) != NIL)
		return LION_RANGE_EVAL_INSIDE;	/* the planner never picks one */

	/*
	 * The walks that will be counted, summaries and all (DESIGN.md §32): a
	 * side is as long as the sets it would count, whatever they stand for.
	 */
	lion_entry_scan_begin_sum(&in, index, col, range, LION_WALK_INSIDE);
	lion_entry_scan_begin_sum(&below, index, col, range, LION_WALK_BELOW);
	lion_entry_scan_begin_sum(&above, index, col, range, LION_WALK_ABOVE);

	for (;;)
	{
		if (!in.done)
			nin += lion_entry_scan_skip_leaf(&in);
		if (!below.done)
			nout += lion_entry_scan_skip_leaf(&below);
		else if (!above.done)
			nout += lion_entry_scan_skip_leaf(&above);

		if (in.done || (below.done && above.done))
			break;
		CHECK_FOR_INTERRUPTS();
	}

	if (!below.done || !above.done)
		eval = LION_RANGE_EVAL_INSIDE;
	else if (nout == 0)
		eval = LION_RANGE_EVAL_FULL;
	else if (!in.done || nout < nin)
		eval = LION_RANGE_EVAL_COMPLEMENT;
	else
		eval = LION_RANGE_EVAL_INSIDE;

	lion_entry_scan_end(&in);
	lion_entry_scan_end(&below);
	lion_entry_scan_end(&above);

	return eval;
}

/* ... for the range that bounds the node's own sum, driven from slot 0. */
static int
lion_range_choose(LionCountScanState *st, LionCountSource *sources,
				  int nsource)
{
	return lion_range_choose_on(st->groupidx, st->groupidxcol, &st->range,
								sources, nsource, 0);
}

/*
 * The SUM over the sets one part (LION_WALK_*) of a walk of rs's range hands
 * out, each ANDed with the other sources in slot's place: the counting half of
 * lion_sum_walk(), for a range taken as a source that was too large to
 * collect (DESIGN.md §32).  It has an entry scan and a memory context of its
 * own, because it runs inside the counts of another walk - the driver's - and
 * must leave that walk's batch alone.  The entries of one leaf are one count
 * when they are small and one each when they are not, as lion_sum_walk()
 * takes them, and an existence test stops at the first piece with a row.
 */
static int64
lion_walk_range_part(LionCountScanState *st, LionRangeSource *rs, int part,
					 LionCountSource *sources, int nsource, int slot,
					 bool exists)
{
	LionEntryScan es;
	LionPostingSet *sets;
	LionCountSource saved = sources[slot];
	LionSumProbe probe;
	MemoryContext cxt;
	int64		total = 0;
	bool		found = false;

	lion_entry_scan_begin_sum(&es, rs->index, rs->col, &rs->range, part);
	sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) * es.maxbatch);

	/* A count may be probed (lion_sum_walk()); an existence test stops early. */
	lion_sum_probe_init(&probe, &es, sources, nsource, slot);
	if (exists)
		probe.frows = -1.0;
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"LionCount range source walk",
								ALLOCSET_DEFAULT_SIZES);

	for (;;)
	{
		MemoryContext oldcxt;
		LionRangeProbe *rp;
		int			nsets = 0;
		int			per;
		int			i;
		double		items = 0;
		double		rows = 0;
		Datum		key;
		bool		more = true;

		CHECK_FOR_INTERRUPTS();
		oldcxt = MemoryContextSwitchTo(cxt);

		do
		{
			if (!lion_entry_scan_next(&es, &key, &sets[nsets]))
			{
				more = false;
				break;
			}
			items += sets[nsets].ncontainers;
			rows += (double) sets[nsets].ntids;
			nsets++;
		} while (lion_entry_scan_batch_left(&es) > 0);

		rp = lion_sum_probe_due(st, &probe, rows, sources, nsource, slot);
		if (rp != NULL)
		{
			for (i = 0; i < nsets; i++)
				lion_range_probe_add(rp, &sets[i]);
		}

		per = (items <= (double) nsets * LION_SUM_UNION_MAX_ITEMS) ?
			Max(nsets, 1) : 1;
		for (i = 0; rp == NULL && i < nsets && !found; i += per)
		{
			int			n = Min(per, nsets - i);
			int64		c;

			sources[slot].nsets = n;
			sources[slot].sets = &sets[i];
			sources[slot].tree = NULL;
			sources[slot].negated = false;
			sources[slot].nomaterialize = false;
			sources[slot].disjoint = (n > 1);
			sources[slot].rangewalk = NULL;

			c = lion_node_count(st, nsource, sources, exists);
			total += c;
			if (exists && c > 0)
				found = true;
		}

		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(cxt);

		if (!more || found)
			break;
	}

	sources[slot] = saved;
	st->summaries += es.nsummaries;
	lion_entry_scan_end(&es);
	MemoryContextDelete(cxt);
	pfree(sets);
	total += lion_sum_probe_finish(st, &probe);
	return exists ? (found ? 1 : 0) : total;
}

/*
 * The AND of sources with a range taken as a source in slot w that was too
 * large to collect (DESIGN.md §32, "A range as a source"): the range's column
 * drives the count as it drives a summed range (§28) - the sum over the sets
 * of its inside, or the count of the other sources minus the column's NULL
 * entry less the sums over the entries below and above it, whichever side
 * the leaf-by-leaf race finds shorter (lion_range_choose_on()).  An existence
 * test walks the inside, and stops at the first set with a row.
 */
static int64
lion_walk_count(LionCountScanState *st, LionCountSource *sources, int nsource,
				int w, bool exists)
{
	LionRangeSource *rs = sources[w].rangewalk;
	LionCountSource *srcs;
	int64		total = 0;
	int			eval = LION_RANGE_EVAL_INSIDE;

	srcs = (LionCountSource *) palloc(sizeof(LionCountSource) * nsource);
	memcpy(srcs, sources, sizeof(LionCountSource) * nsource);
	srcs[w].rangewalk = NULL;

	if (!exists)
		eval = lion_range_choose_on(rs->index, rs->col, &rs->range, srcs,
									nsource, w);

	if (eval == LION_RANGE_EVAL_INSIDE)
		total = lion_walk_range_part(st, rs, LION_WALK_INSIDE, srcs, nsource,
									 w, exists);
	else
	{
		LionCountSource *rest;
		LionPostingSet nullset;
		bool		hasnull;
		int			n = 0;
		int			i;

		/* the other sources minus the column's NULL entry, in w's place */
		rest = (LionCountSource *) palloc0(sizeof(LionCountSource) * nsource);
		for (i = 0; i < nsource; i++)
			if (i != w)
				rest[n++] = srcs[i];
		hasnull = lion_posting_set_lookup_null_col(rs->index, rs->col,
												   &nullset);
		if (hasnull)
		{
			rest[n].nsets = 1;
			rest[n].sets = &nullset;
			rest[n].negated = true;
			n++;
		}
		total = lion_node_count(st, n, rest, false);
		if (hasnull)
			lion_posting_set_release(&nullset);
		pfree(rest);

		if (eval == LION_RANGE_EVAL_COMPLEMENT)
		{
			total -= lion_walk_range_part(st, rs, LION_WALK_BELOW, srcs,
										  nsource, w, false);
			total -= lion_walk_range_part(st, rs, LION_WALK_ABOVE, srcs,
										  nsource, w, false);
		}
		Assert(total >= 0);
	}

	pfree(srcs);
	return total;
}

/*
 * The sum over every entry of the driving index (DESIGN.md §14,
 * `col IS NOT NULL` with nothing else to drive the merge).
 *
 * The entries of one SCALAR index are DISJOINT - a row has one value in the
 * column, so its TID is under exactly one of them - which is what makes a sum
 * over them the count of their union at all (DESIGN.md §15 states the argument
 * in full; §14 has always rested on it).  The same disjointness is what lets
 * the driver's own `IS NOT NULL` be dropped instead of subtracted, which
 * st->sumallitem says and lion_locate_where() decides: that clause is the
 * column's NULL entry as a negated source, and
 *
 *	- subtracting it from ANOTHER entry of the same index removes nothing, the
 *	  two being disjoint, and
 *	- subtracting it from ITSELF leaves nothing, so that entry contributes 0
 *	  and skipping it outright is the same answer.
 *
 * What that saves is not bookkeeping: the NULL entry of a column with many
 * NULLs has a container at every container key, so the merge was running a
 * lion_container_andnot() against a dense bitset at each of the entry's
 * container keys, for every entry of the index.  A `IS NOT NULL` on another
 * column is a different index's set and keeps its source.
 *
 * The entries are counted a leaf at a time (lion_sum_walk()), and a range on
 * the driving column may be answered from the entries it does NOT select
 * (lion_range_choose()); DESIGN.md §28 has both.
 */
static int64
lion_sumall_relation(LionCountScanState *st)
{
	LionCountSource *sources;
	int64		total;
	int			nsource;
	int			eval = LION_RANGE_EVAL_INSIDE;

	if (st->wheremissing)
		return 0;

	if (st->sumallitem >= 0)
	{
		sources = st->dsources;
		nsource = st->ndsource;
	}
	else
	{
		sources = st->sources;
		nsource = st->nsource;
	}

	if (st->hasrange)
		eval = lion_range_choose(st, sources, nsource);

	switch (eval)
	{
		case LION_RANGE_EVAL_FULL:
			total = lion_count_nonnull(st, sources, nsource);
			break;
		case LION_RANGE_EVAL_COMPLEMENT:
			total = lion_count_nonnull(st, sources, nsource);
			total -= lion_sum_walk(st, sources, nsource, LION_WALK_BELOW);
			total -= lion_sum_walk(st, sources, nsource, LION_WALK_ABOVE);
			Assert(total >= 0);
			break;
		default:
			total = lion_sum_walk(st, sources, nsource,
								  st->hasrange ? LION_WALK_INSIDE :
								  LION_WALK_ALL);
			break;
	}

	if (st->hasrange)
		st->rangeeval[eval]++;
	return total;
}

/* ---------------------------------------------------------------------
 * The WHERE sets of a GROUP BY, collected once (DESIGN.md §10)
 * --------------------------------------------------------------------- */

/*
 * What the WHERE items sources[1 .. nwhere] of the relation being counted
 * are, as lion_group_count() weighs them: worked out once per relation, from
 * the hints of the sets lion_locate_where() located once per relation.
 *
 * A range still to be walked - one too large to collect (DESIGN.md §32) - is
 * no item here: every count walks it beside the collected set, as the planner
 * priced it.  A NEGATED item is one to combine when it has a set to subtract,
 * and nothing when it has none (`IS NOT NULL` over a column without NULLs, a
 * multi-key query no key narrows); the collection needs a POSITIVE item to
 * start from, and a WHERE without one is never collected.
 *
 * The REACH of a set is the container keys its rows can lie at: its rows, up
 * to the relation's container keys.  A merge reads about that many containers
 * of its driver and probes each other source at about as many keys, so the
 * collection - driven by the positive item with the fewest rows - and the
 * counts are weighed in it.
 */
static void
lion_where_describe(LionCountScanState *st, LionCountSource *sources,
					int nwhere)
{
	LionCountSource *lone = NULL;
	int			nitems = 0;
	int			npositive = 0;
	int			k;
	int			j;

	st->wknown = true;
	st->wcompound = false;
	st->wsingle = NULL;
	st->wreach = -1;
	if (st->wckeys <= 0)
		st->wckeys = (double) (RelationGetNumberOfBlocks(st->heap) /
							   LION_BLOCKS_PER_CONTAINER + 1);

	for (k = 1; k <= nwhere; k++)
	{
		LionCountSource *src = &sources[k];
		double		rows = 0;
		bool		any = false;

		if (src->rangewalk != NULL)
			continue;
		for (j = 0; j < src->nsets; j++)
		{
			if (src->sets[j].found)
			{
				any = true;
				rows += (double) src->sets[j].ntids;
			}
		}
		if (src->negated)
		{
			if (any)
				nitems++;
			continue;
		}
		nitems++;
		npositive++;
		lone = src;
		rows = Min(rows, st->wckeys);
		if (st->wreach < 0 || rows < st->wreach)
			st->wreach = rows;
	}

	if (npositive == 0)
	{
		st->wtried = true;		/* nothing to intersect */
		return;
	}

	/*
	 * Two items or more, or one that is a union - an IN list, an OR (§19), a
	 * multi-key query of several keys (§17) - are merged again by every
	 * count, whatever copies the count keeps of their sets.  One item of one
	 * set is worth collecting only where no count keeps a copy of it.
	 */
	if (nitems > 1 || lone->nsets > 1 || lone->nomaterialize ||
		(lone->tree != NULL && lone->tree->kind != LION_KN_KEY))
		st->wcompound = true;
	else if (lone->nsets == 1)
		st->wsingle = &lone->sets[0];
}

/*
 * Collect the WHERE items sources[1 .. nwhere] into st->wherecoll: the
 * intersection of the positive ones less what the negated ones subtract, as
 * one private, pinless posting set (lion_sources_collect()).  Made at most
 * once per relation, whatever comes of it.
 *
 * It is budgeted like every set the node collects: what the ranges taken as
 * sources have left of a hash table's memory (get_hash_memory_limit(),
 * lion_locate_range()), and past that it SPILLS to a temporary file, as the
 * FK-side join's copy of its fact filters does (DESIGN.md §27).  A spilled
 * set keeps sixteen bytes a container key in memory, and a count reads it a
 * container at a time.
 *
 * Not on a standby.  Whether a count there may trust the visibility map
 * depends on the WAL mode of every index it reads (lion_sources_all_rmgr()),
 * and a collected set carries the TIDs of every item under the name of one
 * index; the counts read the items there, as they always did.
 */
static void
lion_where_collect(LionCountScanState *st, LionCountSource *sources,
				   int nwhere)
{
	EState	   *estate = st->css.ss.ps.state;
	LionCountSource *items;
	MemoryContext oldcxt;
	Size		limit = get_hash_memory_limit();
	bool		spilled;
	int			n = 0;
	int			k;

	st->wtried = true;
	if (RecoveryInProgress())
		return;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);
	items = (LionCountSource *) palloc(sizeof(LionCountSource) * nwhere);
	for (k = 1; k <= nwhere; k++)
	{
		if (sources[k].rangewalk == NULL)
			items[n++] = sources[k];
	}
	if (lion_sources_collect(st->heap, estate->es_snapshot, n, items,
							 (st->rangesrc_held < limit) ?
							 limit - st->rangesrc_held : 0,
							 true, &st->wherecoll, &spilled, &st->stats))
	{
		st->wcollected = true;
		st->wherecollected++;
		if (spilled)
			st->wherespilled++;
	}
	pfree(items);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * One count of a GROUP BY: the group's set in sources[0] - and, beside a
 * second GROUP BY column (§20), the inner group's in the slots after the WHERE
 * items - ANDed with the WHERE items sources[1 .. nwhere].  It is
 * lion_node_count(), except that once the WHERE items are collected into
 * st->wherecoll the count reads that one set in their place, and that before
 * each count it decides whether to collect them now (DESIGN.md §10, "The
 * WHERE sets, collected once").
 *
 * WHEN.  Collecting reads the items once, a merge over all their containers.
 * A count reads them again at the container keys of its own set: probes into
 * each of them, and for a set the count keeps no copy of, its pages - which is
 * how a GROUP BY of many groups used to read the WHERE about once per group.
 * Neither is known in advance: the walk finds out how many groups there
 * are, and how many rows each has, as it goes.  So the counts go on reading
 * the items until what they have read of them, with what THIS count would
 * read, reaches what the collection reads, and the items are collected then.
 * Collecting at that point costs at most what the counts so far and this one
 * would have read: never more than twice the better of never collecting and
 * collecting before the first group.  `ownrows` is the rows of the count's own
 * smallest set - the count reads the items at no more keys than that set
 * spans - and `more` says that another count of this relation is sure to
 * follow.  Without
 * one, the first count is never preceded by a collection - which is what keeps
 * a GROUP BY that has one group the count it always was.
 *
 * WHY THE COUNT IS STILL EXACT (DESIGN.md §9).  The collected set is a copy,
 * pinless and possibly stale, and it is safe on the terms of
 * lion_posting_set_materialize(): it is only ever counted ANDed with the
 * group's own set, which each count locates afresh under a pin of its own -
 * the walk's entry, the listed value located again (§15), the inner set of a
 * pair (§20).  A dead TID the copy still lists is then either gone from the
 * group's container, and out of the intersection, or in it - which means the
 * group's page was read before VACUUM's ambulkdelete got past it, so VACUUM
 * has not finished ambulkdelete on that index, so it has not set the TID's
 * heap page all-visible, and the TID goes to the heap recheck where the
 * snapshot decides.  The copy cannot lack a row the snapshot sees: it was
 * made after the snapshot was taken, and a visible row was in every index
 * before its transaction committed.  With no pinned positive source in a
 * count the merge trusts no map (cx.novm), as for any count; none of the
 * shapes that come here lacks one.
 */
static int64
lion_group_count(LionCountScanState *st, int nsource, LionCountSource *sources,
				 int nwhere, uint64 ownrows, bool more)
{
	int			n = 0;
	int			k;

	if (!st->wtried && !st->wknown)
		lion_where_describe(st, sources, nwhere);
	if (!st->wtried)
	{
		double		own = Min(Min((double) ownrows, st->wckeys), st->wreach);

		if ((st->wcounts > 0 || more) &&
			st->wspent + own >= st->wreach &&
			(st->wcompound ||
			 (st->wsingle != NULL && lion_posting_set_rewalked(st->wsingle))))
			lion_where_collect(st, sources, nwhere);
		st->wspent += own;
	}
	st->wcounts++;

	if (!st->wcollected)
		return lion_node_count(st, nsource, sources, false);

	/* the group, the collected set, what it does not hold, a second group */
	st->wsources[n++] = sources[0];
	memset(&st->wsources[n], 0, sizeof(LionCountSource));
	st->wsources[n].nsets = 1;
	st->wsources[n].sets = &st->wherecoll;
	n++;
	for (k = 1; k <= nwhere; k++)
	{
		if (sources[k].rangewalk != NULL)
			st->wsources[n++] = sources[k];
	}
	for (k = nwhere + 1; k < nsource; k++)
		st->wsources[n++] = sources[k];
	Assert(n <= st->nsource + 1);

	return lion_node_count(st, n, st->wsources, false);
}

/*
 * The next group of the relation the node has open, as one row.
 *
 * Returns NULL and sets *exhausted once the relation's entry scan has run
 * out; every other return is a row.  This is the streaming group loop of
 * DESIGN.md §10, shared by the single-table path and by each partition of a
 * partitioned one (§16), which is why nothing here knows about partitions:
 * the caller has opened one relation and located its WHERE clauses.
 *
 * GROUP BY coalesce(g, c) (st->hascoal, DESIGN.md §10) takes two entries out
 * of the stream: the NULL entry, whose rows are c's group, and the entry whose
 * key the grouping equality finds equal to c, if the walk meets one.  Their
 * counts are added up in coalcount and the one group they make is emitted,
 * with the value c, after the last entry - wherever the two came in the walk,
 * and whichever of them exists.  An entry of c that VACUUM removes before the
 * walk reaches it held no row this snapshot sees, so the group is the same
 * without it; a key equal to c inserted meanwhile holds none either.  The
 * classes of one index are disjoint, so no other entry can equal c.
 */
static TupleTableSlot *
lion_next_group(LionCountScanState *st, bool *exhausted)
{
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		Datum		key;
		bool		keyisnull;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		/*
		 * The key of the group we returned last time lives in pergroup, and
		 * the caller is done with it by now: a scan node's tuple is only
		 * guaranteed until its next call.
		 */
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (st->coalwalked ||
			!lion_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);

			/*
			 * The group of c, once: a HAVING that rejects it sends the caller
			 * back here, and the walk is not asked for another entry after
			 * it has run out.
			 */
			if (st->hascoal && !st->coalwalked)
			{
				st->coalwalked = true;
				if (st->coalcount > 0)
					return lion_emit_tuple(st, st->coalconst->constvalue,
										   false, (Datum) 0, true,
										   st->coalcount);
			}
			*exhausted = true;
			return NULL;
		}

		/* another entry already in hand is another count to come */
		count = lion_group_count(st, st->nsource, st->sources, st->nitem,
								 st->groupset.ntids,
								 lion_entry_scan_batch_left(&st->escan) > 0);
		keyisnull = st->groupset.keyisnull;
		lion_posting_set_release(&st->groupset);

		if (st->hascoal &&
			(keyisnull ||
			 DatumGetBool(FunctionCall2Coll(&st->coaleqfn, st->coalcoll, key,
											st->coalconst->constvalue))))
		{
			st->coalcount += count;
			MemoryContextSwitchTo(oldcxt);
			continue;
		}
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		return lion_emit_tuple(st, key, keyisnull, (Datum) 0, true, count);
	}
}

/*
 * The same when an IN list on the grouping column drives the groups
 * (DESIGN.md §15): the groups are the listed values, so the node steps through
 * that clause's located posting sets instead of the index's entries.
 *
 * Each group's rows are its own entry's, intersected with whatever else the
 * WHERE says: the IN clause itself is not intersected, because the entries of
 * a scalar index are disjoint and `entry ∩ (entry ∪ the rest of the list)` is
 * the entry.  With no other clause that leaves ONE source, which is the plain
 * single-key count, and the answer for the whole query is then the same sum
 * the ungrouped form short-circuits to - split into its terms.
 *
 * Pins and memory (DESIGN.md §9).  The sets belong to the clause and were
 * located once for this relation, with their pins, by lion_locate_where(), and
 * the key the loop emits is the copy the set already holds, which outlives the
 * row.  A set that holds no pin - every INLINE one once a row has gone up
 * (lion_pause_run()), and any located past the §15 pin budget - is located
 * again for its group, into groupset, and released after the count: it is the
 * source that carries the interlock for the group, the other WHERE sets being
 * NOPIN copies by then too - or, once they are worth it, one collected set
 * (lion_group_count()), which is a pinless copy as well.
 */
static TupleTableSlot *
lion_next_group_inlist(LionCountScanState *st, bool *exhausted)
{
	LionCountSource *src = &st->sources[st->ingroupitem + 1];
	LionClauseState *cl = &st->clause[st->item[st->ingroupitem].clauseno];
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		LionPostingSet *ps;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);

		if (st->ingroupset >= src->nsets)
		{
			*exhausted = true;
			return NULL;
		}
		ps = &src->sets[st->ingroupset++];
		if (!ps->found)
			continue;			/* a listed value with no entry: no group */
		st->ingroupleft--;

		Assert(ps->hasstoredkey && !ps->keyisnull);

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		st->dsources[0].nsets = 1;
		st->dsources[0].sets = ps;
		st->dsources[0].tree = NULL;
		st->dsources[0].negated = false;
		st->dsources[0].nomaterialize = false;
		st->dsources[0].disjoint = false;

		/*
		 * The entry as it is now, under a pin of its own.  One that has gone
		 * meanwhile held no row anyone can see (VACUUM deletes only an empty
		 * entry, §18): no group.
		 */
		if (ps->nopin)
		{
			if (!lion_posting_set_lookup_col(cl->idx, cl->idxcol,
											 ps->storedkey, InvalidOid,
											 &st->groupset))
			{
				lion_posting_set_release(&st->groupset);
				MemoryContextSwitchTo(oldcxt);
				continue;
			}
			st->dsources[0].sets = &st->groupset;
		}

		/* the WHERE is every other item: collected once, when that pays */
		count = lion_group_count(st, st->ndsource, st->dsources,
								 st->ndsource - 1, st->dsources[0].sets->ntids,
								 st->ingroupleft > 0);
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		return lion_emit_tuple(st, ps->storedkey, false, (Datum) 0, true,
							  count);
	}
}

/*
 * The same for a GROUP BY over TWO indexed columns (DESIGN.md §20).
 *
 * A nested loop over the two indexes' entries: the outer index - the one with
 * fewer entries, chosen at plan time - drives the scan exactly as a single
 * group column's does, and for each of its entries every key of the inner
 * index is tried.  A pair's count is the intersection of the two groups'
 * posting sets with the WHERE sources, and only a pair with at least one
 * visible row is a group at all, so only those are emitted.
 *
 * Pins and memory (DESIGN.md §9).  The outer group's set is located once and
 * held for the whole of its inner loop, in outercxt - with its pin until the
 * first of its pairs goes up as a row, and as a NOPIN copy after that
 * (lion_pause_run()), the pairs being carried by their inner sets; each
 * pair's inner set is located, counted and released inside pergroup, so at
 * most two group pins exist at a time however many distinct values either
 * column has.  The inner keys were read once per relation into innercxt
 * (lion_load_inner_keys()); when they did not fit its budget, innerkey is
 * NULL and the inner index's entry scan is walked once per outer group
 * instead, which holds one pin at a time as well.  The WHERE items are
 * collected once per relation when that pays (lion_group_count()), and the
 * pairs after that are counted against the copy - the inner set carrying the
 * interlock, as it does for the WHERE sets' copies once a row has gone up.
 *
 * That walk outlives many pairs, and so do the memory contexts
 * lion_entry_scan_begin_col() creates for its position and its batch, under
 * whatever context is current: it is begun in the query's context, never
 * inside pergroup, whose reset before every pair would delete them under the
 * walk (the 2026-09-27 review found exactly that: a leaf copied into freed
 * memory at the next pair, and the contexts deleted twice at the end).
 */
static TupleTableSlot *
lion_next_group2(LionCountScanState *st, bool *exhausted)
{
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		Datum		ikey = (Datum) 0;
		bool		ikeyisnull;
		bool		more;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		/* ---- the outer group ---- */
		if (!st->outeropen)
		{
			MemoryContextReset(st->outercxt);
			oldcxt = MemoryContextSwitchTo(st->outercxt);
			if (!lion_entry_scan_next(&st->escan, &st->outerkey, &st->groupset))
			{
				MemoryContextSwitchTo(oldcxt);
				*exhausted = true;
				return NULL;
			}
			MemoryContextSwitchTo(oldcxt);
			st->outerisnull = st->groupset.keyisnull;
			st->outeropen = true;
			st->inneridx = 0;
		}

		/* ---- the fallback: the inner index's walk, per outer group ---- */
		if (st->innerkey == NULL && !st->scanning2)
		{
			oldcxt = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);
			lion_entry_scan_begin_col(&st->escan2, st->groupidx2,
									 st->groupidxcol2);
			MemoryContextSwitchTo(oldcxt);
			st->scanning2 = true;
		}

		/*
		 * The row we returned last time may point into pergroup (the fallback
		 * path's inner key does), and the caller is done with it by now: a
		 * scan node's tuple is only guaranteed until its next call.
		 */
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		/* ---- the next inner group of it ---- */
		if (st->innerkey != NULL)
		{
			int			i = st->inneridx;

			if (i >= st->ninnerkey)
			{
				MemoryContextSwitchTo(oldcxt);
				lion_posting_set_release(&st->groupset);
				st->outeropen = false;
				continue;
			}
			st->inneridx++;

			ikey = st->innerkey[i];
			ikeyisnull = st->innerisnull[i];
			if (ikeyisnull)
				(void) lion_posting_set_lookup_null_col(st->groupidx2,
													   st->groupidxcol2,
													   &st->groupset2);
			else
				(void) lion_posting_set_lookup_col(st->groupidx2,
												  st->groupidxcol2, ikey,
												  InvalidOid, &st->groupset2);
		}
		else
		{
			Assert(st->scanning2);
			if (!lion_entry_scan_next(&st->escan2, &ikey, &st->groupset2))
			{
				MemoryContextSwitchTo(oldcxt);
				lion_entry_scan_end(&st->escan2);
				st->scanning2 = false;
				lion_posting_set_release(&st->groupset);
				st->outeropen = false;
				continue;
			}
			ikeyisnull = st->groupset2.keyisnull;
		}

		/*
		 * The WHERE items are collected once, when that pays, and the pair is
		 * counted against the copy; another inner key, or another outer entry
		 * in hand, is another count to come.
		 */
		more = (st->innerkey != NULL) ? (st->inneridx < st->ninnerkey) :
			(lion_entry_scan_batch_left(&st->escan2) > 0);
		more = more || lion_entry_scan_batch_left(&st->escan) > 0;
		count = lion_group_count(st, st->nsource, st->sources, st->nitem,
								 Min(st->groupset.ntids, st->groupset2.ntids),
								 more);
		lion_posting_set_release(&st->groupset2);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		return lion_emit_tuple(st, st->outerkey, st->outerisnull,
							  ikey, ikeyisnull, count);
	}
}

/*
 * One test of a count(DISTINCT k) walk (DESIGN.md §26): the rows of
 * (sources) when the target list needs them counted, and otherwise 1 or 0 for
 * whether there is at least one visible row at all - which is where the walk
 * saves its work, because an existence test stops at the first container that
 * shows one (lion_exists_sources_cached()).  Runs in pergroup, like a group's
 * count.
 */
static int64
lion_distinct_test(LionCountScanState *st, int nsource,
				   LionCountSource *sources, bool count)
{
	st->disttests++;
	return lion_node_count(st, nsource, sources, !count);
}

/*
 * count(DISTINCT k) without a GROUP BY (DESIGN.md §26): the one row.
 *
 * A scalar index puts every row under exactly one entry of k - its value's,
 * or the reserved NULL entry (§14, §15) - so the distinct non-NULL values of k
 * among the rows the WHERE selects are exactly the non-NULL entries whose set,
 * intersected with the WHERE sources, has a visible row.  So this is the
 * GROUP BY k walk of lion_next_group() with each group's count replaced by
 * an existence test and the groups summed into one row:
 *
 *	- the entries are k's index's, or - when the WHERE pins k itself to a
 *	  value or a list - that clause's own located sets, which are then not
 *	  intersected (st->ingroupitem, the §15 driver);
 *	- the NULL entry is never a distinct value.  It is skipped, except that
 *	  a total over every row (count(*)) counts it, and that a GROUP BY the
 *	  planner folded to one constant group asks it, at the end and only if
 *	  nothing else matched, whether the group exists at all;
 *	- when the target list wants rows as well (count(*), count(k), ...) each
 *	  test is a full count, summed: the entries are disjoint, so their counts
 *	  add up to the rows of their union.
 *
 * Pins (DESIGN.md §9): one entry's set at a time, released before the next
 * is located, exactly as in the GROUP BY walk; a list's sets belong to the
 * clause and were located once by lion_locate_where().
 */
static TupleTableSlot *
lion_distinct_relation(LionCountScanState *st)
{
	LionCountSource *sources;
	MemoryContext oldcxt;
	int			nsource;
	int64		ndistinct = 0;
	int64		nonnull = 0;
	int64		total = 0;
	bool		found = false;
	bool		listdrive = (st->ingroupitem >= 0);
	Datum		key;

	if (st->ingroupitem >= 0 || st->sumallitem >= 0)
	{
		sources = st->dsources;
		nsource = st->ndsource;
	}
	else
	{
		sources = st->sources;
		nsource = st->nsource;
	}

	if (!listdrive)
	{
		lion_entry_scan_begin_range(&st->escan, st->groupidx, st->groupidxcol,
									st->hasrange ? &st->range : NULL);
		st->scanning = true;
	}

	for (;;)
	{
		LionPostingSet *ps;
		int64		n;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (listdrive)
		{
			LionCountSource *src = &st->sources[st->ingroupitem + 1];

			if (st->ingroupset >= src->nsets)
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}
			ps = &src->sets[st->ingroupset++];
			if (!ps->found)
			{
				MemoryContextSwitchTo(oldcxt);
				continue;		/* a listed value with no entry */
			}
			sources[0].nsets = 1;
			sources[0].sets = ps;
			sources[0].tree = NULL;
			sources[0].negated = false;
			sources[0].nomaterialize = false;
			sources[0].disjoint = false;
		}
		else
		{
			if (!lion_entry_scan_next(&st->escan, &key, &st->groupset))
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}
			ps = &st->groupset;
		}

		if (ps->keyisnull)
		{
			/*
			 * Not a value.  Its rows are part of the total, unless the WHERE
			 * says `k IS NOT NULL` (st->sumallitem), which excludes exactly
			 * them.
			 */
			if (st->distfull && st->sumallitem < 0)
				total += lion_distinct_test(st, nsource, sources, true);
		}
		else
		{
			n = lion_distinct_test(st, nsource, sources, st->distfull);
			if (n > 0)
			{
				ndistinct++;
				nonnull += n;
				total += n;
			}
		}

		if (!listdrive)
			lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
	}

	if (!listdrive)
	{
		lion_entry_scan_end(&st->escan);
		st->scanning = false;
	}

	found = (ndistinct > 0 || total > 0);

	/*
	 * A folded GROUP BY has a row only if some row matches, and a row whose k
	 * is NULL matches too.  Only asked when nothing else answered it, and
	 * never under a list or a range on k (DESIGN.md §28), which no NULL
	 * satisfies.
	 */
	if (st->singlegroup && !found && !listdrive && st->sumallitem < 0 &&
		!st->hasrange)
	{
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);
		if (lion_posting_set_lookup_null_col(st->groupidx, st->groupidxcol,
											 &st->groupset))
			found = (lion_distinct_test(st, nsource, sources, false) > 0);
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
	}

	st->done = true;
	if (st->singlegroup && !found)
		return NULL;

	st->distcount = ndistinct;
	st->distcolcount = nonnull;
	return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, total);
}

/*
 * count(DISTINCT k) per GROUP BY g (DESIGN.md §26): the (g, k) nested loop of
 * lion_next_group2(), g outer and k inner, with one row per GROUP rather than
 * per pair.
 *
 * For each entry of g, first the group itself: is there a visible row in
 * g ∩ WHERE at all?  (Counted, when the target list asks for count(*) or
 * count(g).)  A group without one is not emitted - a group exists only if one
 * of its rows is visible - and a group whose rows all have k NULL is emitted
 * with a distinct count of 0, which is what SQL says it is.  Then one test per
 * non-NULL key of k, of g ∩ WHERE ∩ k, and the row.
 *
 * Pins and memory are §20's: the outer set is located once and held for the
 * group's whole inner loop (in outercxt), each pair's inner set is located,
 * tested and released inside pergroup, so at most two group pins exist at a
 * time; the inner keys were read once per relation (lion_load_inner_keys()),
 * or, when they did not fit, k's entry scan is walked once per group.
 */
static TupleTableSlot *
lion_next_group_distinct(LionCountScanState *st, bool *exhausted)
{
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		int64		count;
		int64		ndistinct = 0;
		int64		nonnull = 0;

		CHECK_FOR_INTERRUPTS();

		/* the previous group's key lived in outercxt; its row is consumed */
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		MemoryContextReset(st->outercxt);
		oldcxt = MemoryContextSwitchTo(st->outercxt);
		if (!lion_entry_scan_next(&st->escan, &st->outerkey, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			*exhausted = true;
			return NULL;
		}
		MemoryContextSwitchTo(oldcxt);
		st->outerisnull = st->groupset.keyisnull;
		st->outeropen = true;

		/* ---- the group: slot 0 and the WHERE items, not the inner slot ---- */
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);
		count = lion_distinct_test(st, st->nsource - 1, st->sources,
								   st->distgroupcount);
		MemoryContextSwitchTo(oldcxt);
		if (count == 0)
		{
			lion_posting_set_release(&st->groupset);
			st->outeropen = false;
			continue;
		}

		/* ---- every non-NULL key of k ---- */
		if (st->innerkey != NULL)
		{
			int			i;

			for (i = 0; i < st->ninnerkey; i++)
			{
				int64		n;

				CHECK_FOR_INTERRUPTS();
				if (st->innerisnull[i])
					continue;

				MemoryContextReset(st->pergroup);
				oldcxt = MemoryContextSwitchTo(st->pergroup);
				(void) lion_posting_set_lookup_col(st->groupidx2,
												   st->groupidxcol2,
												   st->innerkey[i], InvalidOid,
												   &st->groupset2);
				n = lion_distinct_test(st, st->nsource, st->sources,
									   st->distfull);
				lion_posting_set_release(&st->groupset2);
				MemoryContextSwitchTo(oldcxt);

				if (n > 0)
				{
					ndistinct++;
					nonnull += n;
				}
			}
		}
		else
		{
			lion_entry_scan_begin_col(&st->escan2, st->groupidx2,
									 st->groupidxcol2);
			st->scanning2 = true;
			for (;;)
			{
				Datum		ikey;
				int64		n;

				CHECK_FOR_INTERRUPTS();
				MemoryContextReset(st->pergroup);
				oldcxt = MemoryContextSwitchTo(st->pergroup);
				if (!lion_entry_scan_next(&st->escan2, &ikey, &st->groupset2))
				{
					MemoryContextSwitchTo(oldcxt);
					break;
				}
				n = 0;
				if (!st->groupset2.keyisnull)
					n = lion_distinct_test(st, st->nsource, st->sources,
										   st->distfull);
				lion_posting_set_release(&st->groupset2);
				MemoryContextSwitchTo(oldcxt);

				if (n > 0)
				{
					ndistinct++;
					nonnull += n;
				}
			}
			lion_entry_scan_end(&st->escan2);
			st->scanning2 = false;
		}

		lion_posting_set_release(&st->groupset);
		st->outeropen = false;

		st->distcount = ndistinct;
		st->distcolcount = nonnull;
		return lion_emit_tuple(st, st->outerkey, st->outerisnull,
							  (Datum) 0, true, count);
	}
}

/*
 * Whichever of them the plan asks for.
 */
static TupleTableSlot *
lion_next_group_any(LionCountScanState *st, bool *exhausted)
{
	if (st->distattno != 0)
		return lion_next_group_distinct(st, exhausted);
	if (st->groupattno2 != 0)
		return lion_next_group2(st, exhausted);
	if (st->ingroupitem >= 0)
		return lion_next_group_inlist(st, exhausted);
	return lion_next_group(st, exhausted);
}

static void lion_unpin_where(LionCountScanState *st);

/*
 * EXPLAIN ANALYZE's clock of the FK-side join (DESIGN.md §27, "Where a key's
 * time goes"): charge the time since *since to `phase` (to nothing when phase
 * is negative) and start the next phase now.  It does nothing unless the node
 * is timed - under EXPLAIN ANALYZE with its TIMING option, as core times a
 * node - so an untimed run pays a test and no clock read.
 */
static inline void
lion_join_clock(LionCountScanState *st, int phase, instr_time *since)
{
	instr_time	now;

	if (!st->jointiming)
		return;
	INSTR_TIME_SET_CURRENT(now);
	if (phase >= 0)
		INSTR_TIME_ACCUM_DIFF(st->jointime[phase], now, *since);
	*since = now;
}

/* The child's next row, counted, and timed when the node is. */
static TupleTableSlot *
lion_join_child_next(LionCountScanState *st)
{
	TupleTableSlot *slot;
	instr_time	t;

	INSTR_TIME_SET_ZERO(t);
	lion_join_clock(st, -1, &t);
	slot = ExecProcNode(st->child);
	lion_join_clock(st, LION_JT_CHILD, &t);
	if (!TupIsNull(slot))
		st->joinchildrows++;
	return slot;
}

/*
 * One key's count - or existence test, for a semi or anti join and for the
 * rows of a count(DISTINCT) - of the fk set located into groupset, ANDed with
 * the fact filters or their collected copy; the posting pages it reads are
 * EXPLAIN ANALYZE's (DESIGN.md §27, "Where a key's time goes").
 */
static int64
lion_join_count_key(LionCountScanState *st)
{
	int64		pages = lion_posting_pages_read;
	int64		count;
	bool		exists = (st->jointype != LION_JOIN_INNER || st->joinrows);

	count = st->joinfiltered ?
		lion_node_count(st, 2, st->joinsources, exists) :
		lion_node_count(st, st->nsource, st->sources, exists);
	st->joinposting += lion_posting_pages_read - pages;
	return count;
}

/*
 * The fact filters of the FK-side join, collected once per run into a private
 * posting set when the plan asks for it (DESIGN.md §27): what each dimension
 * row's count reads instead of the filters themselves.
 *
 * Without it every count merges the fk set with the filter SOURCES, and a
 * source is sought at each of the fk set's container keys - a descent of its
 * posting tree, a leaf copied, and for a multi-key clause or a union every one
 * of its sets merged at that key - which for an fk of a few rows per key over
 * a large heap is a descent per ROW of the fk set, per dimension row.  The
 * copy is the filters' intersection, made in one pass over their containers,
 * and a count seeks it with a binary search in memory.
 *
 * Its safety is lion_sources_collect()'s: the copy is only ever counted beside
 * the dimension row's fk set, which is located under its own pin and carries
 * the §9 interlock, exactly as a materialized WHERE set is only counted beside
 * a source that does.  The WHERE sets themselves stay located, as they always
 * are for the length of a run.  Not on a standby, where the interlock depends
 * on the WAL mode of every index read (lion_count_sources_cached()) and the
 * ordinary counts are left to decide it - the planner prices a standby's
 * counts as the probing they are (lion_cost_fkjoin_rel()).
 *
 * Past a hash join's memory the copy SPILLS to a temporary file, as the hash
 * join would (2026-09-28 review).  It used to give up there, and every
 * dimension row's count then read the filters themselves: for an IN list of
 * a parameter the planner had estimated at ten values, a union of every one
 * of its sets built again for each row - hour-scale for a long list over many
 * dimension rows.  A spilled copy is read a container at a time, and a count
 * seeks it with a binary search over what memory keeps of it.
 */
static void
lion_join_collect(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;
	LionCountStats cstats;
	instr_time	t;
	bool		ok;
	bool		spilled;
	int			k;

	st->joincollected = true;
	if (!st->joincollect || st->wheremissing || st->nitem == 0)
		return;
	if (RecoveryInProgress())
		return;

	/*
	 * The copy is an intersection, so it needs a positive source to start
	 * from: filters that are all `IS NOT NULL` are subtracted from each fk
	 * set as they always were.  The planner does not ask for a copy then.
	 */
	for (k = 0; k < st->nitem; k++)
	{
		if (!st->sources[k + 1].negated)
			break;
	}
	if (k == st->nitem)
		return;

	/*
	 * A range taken as a source that was too large to collect on its own
	 * (DESIGN.md §32) is not a set to collect either; the counts walk it.
	 */
	for (k = 0; k < st->nitem; k++)
	{
		if (st->sources[k + 1].rangewalk != NULL)
			return;
	}

	/*
	 * The budget is a hash join's (get_hash_memory_limit(): work_mem times
	 * hash_mem_multiplier): the copy stands where the ordinary plan's hash
	 * table would, and a participant of a parallel plan makes one of its own,
	 * as each participant of a hash join below a Gather builds its own table.
	 */
	INSTR_TIME_SET_ZERO(t);
	lion_join_clock(st, -1, &t);
	memset(&cstats, 0, sizeof(cstats));
	oldcxt = MemoryContextSwitchTo(st->outercxt);
	ok = lion_sources_collect(st->heap, estate->es_snapshot, st->nitem,
							  &st->sources[1], get_hash_memory_limit(), true,
							  &st->joinfilter, &spilled, &cstats);
	MemoryContextSwitchTo(oldcxt);
	lion_join_clock(st, LION_JT_COLLECT, &t);

	/*
	 * What EXPLAIN ANALYZE reports of the copy is what the counts read of it
	 * ("Where a key's time goes"); a filter that is itself a copy (a range
	 * collected into memory, §32) was read once to make this one, and that
	 * is the collection's, whose containers are counted as any are.
	 */
	cstats.copy_containers = 0;
	cstats.copy_seeks = 0;
	cstats.copy_file_reads = 0;
	lion_count_stats_add(&st->stats, &cstats);
	if (!ok)
		return;

	st->joinfiltered = true;
	st->joinfilterrows = (int64) st->joinfilter.ntids;
	if (spilled)
		st->joinspilled++;

	/*
	 * No count reads the WHERE sets again this run - each reads its fk set
	 * and the copy - so their pins go now, before the child runs, rather than
	 * at the first row that goes up: pins of index pages held while the
	 * child runs user code are what keep a VACUUM of the fact table waiting
	 * ("Visibility and the §9 interlock").
	 */
	lion_unpin_where(st);

	/* The filters select no row at all: as a clause with no entry does. */
	if (!st->joinfilter.found)
	{
		st->wheremissing = true;
		return;
	}

	memset(st->joinsources, 0, sizeof(st->joinsources));
	st->joinsources[0].nsets = 1;
	st->joinsources[0].sets = &st->groupset;
	st->joinsources[1].nsets = 1;
	st->joinsources[1].sets = &st->joinfilter;
}

/*
 * The forward semi join over a non-unique key (DESIGN.md §27, "Forward semi
 * joins over a non-unique key"): run the whole child and sort the keys of its
 * rows, so that they come out in order and equal ones next to each other.  A
 * NULL key joins nothing and is left out.  The sort is a datum sort of the key
 * alone - what a Sort node over one column does - within work_mem, spilling
 * past it as a Sort node's does, in the per-query memory, where it lives until
 * the run is reset (lion_reset_run()).
 */
static void
lion_join_sort_keys(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	st->joinsort = tuplesort_begin_datum(st->joinkeytype, st->joinsortop,
										 st->joinsortcoll, false, work_mem,
										 NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(oldcxt);

	for (;;)
	{
		TupleTableSlot *slot;
		Datum		key;
		bool		isnull;

		CHECK_FOR_INTERRUPTS();
		slot = lion_join_child_next(st);
		if (TupIsNull(slot))
			break;
		key = slot_getattr(slot, st->joinkeyresno, &isnull);
		if (isnull)
			continue;
		tuplesort_putdatum(st->joinsort, key, false);
		st->joinsorted++;
	}
	tuplesort_performsort(st->joinsort);
	tuplesort_get_stats(st->joinsort, &st->joinsortstats);
	st->joinhavesortstats = true;
	st->joinsortdone = true;
}

/*
 * The next distinct key of the sorted child rows, as the row that carries it,
 * or NULL when there are no more.  A row whose key the sort operator's
 * equality - the join operator's, for the key's type - finds equal to the
 * previous distinct key's is skipped: its fact rows are that key's, and have
 * been counted.
 *
 * In a parallel plan every participant has sorted every row, so the distinct
 * keys come out in the same sequence in each, and they are divided by their
 * position in it: a participant counts the keys of the runs of
 * LION_FKJOIN_UNIQUE_CHUNK it claims from the shared counter, and passes over
 * the others, which another participant claimed.  A participant claims the
 * run after its last one only once it has finished that one, so the counter
 * never gives out a run behind the participant's position, and every run is
 * counted by exactly one of them - however many start, and whether the leader
 * takes part or not.  A plan run without its workers, or a node that is not
 * parallel-aware, has no shared counter and counts every key.
 */
static TupleTableSlot *
lion_join_next_key(LionCountScanState *st)
{
	TupleTableSlot *slot = st->joinsortslot;

	for (;;)
	{
		Datum		key;
		bool		isnull;
		int64		chunk;
		MemoryContext oldcxt;

		CHECK_FOR_INTERRUPTS();
		if (!tuplesort_getdatum(st->joinsort, true, false, &key, &isnull,
								NULL))
			return NULL;
		Assert(!isnull);
		if (st->joinhaveprev &&
			DatumGetBool(FunctionCall2Coll(&st->joineqfn, st->joinsortcoll,
										   st->joinprevkey, key)))
			continue;

		/* the sort's copy lasts until the next key: keep one of our own */
		ExecClearTuple(slot);
		MemoryContextReset(st->joinkeycxt);
		oldcxt = MemoryContextSwitchTo(st->joinkeycxt);
		st->joinprevkey = datumCopy(key, st->joinkeybyval, st->joinkeylen);
		MemoryContextSwitchTo(oldcxt);
		st->joinhaveprev = true;

		chunk = st->joinkeypos++ / LION_FKJOIN_UNIQUE_CHUNK;
		if (st->joinshared != NULL)
		{
			if (st->joinchunk < chunk)
				st->joinchunk = (int64)
					pg_atomic_fetch_add_u32(&st->joinshared->nextchunk, 1);
			if (st->joinchunk != chunk)
				continue;
		}

		memset(slot->tts_isnull, true,
			   sizeof(bool) * slot->tts_tupleDescriptor->natts);
		slot->tts_values[st->joinkeyresno - 1] = st->joinprevkey;
		slot->tts_isnull[st->joinkeyresno - 1] = false;
		return ExecStoreVirtualTuple(slot);
	}
}

/*
 * LOOKUPS IN KEY ORDER (DESIGN.md §27).  Each child row's lookup used to be a
 * descent of the fk index's directory - the root, the internal levels and a
 * leaf - in the order the dimension's rows came, which is not the fk index's.
 * Over a high-cardinality fk the directory is large, the leaves those lookups
 * read are spread all over it, and consecutive rows share none of them.  So a
 * plan that asks for it (LION_JOINFLAG_WALK, which the cost model sets where
 * the walk saves a page a key: lion_cost_fkjoin_walk()) reads the child's
 * rows a BATCH at a time, sorts the batch into the directory's order and
 * locates the keys with one walk of the leaves (LionLookupWalk, lion_count.c):
 * a key on the leaf the last one was found on costs that leaf, one on the next
 * leaf a step right, and only a key further away a descent.
 *
 * Why the answers are the same.  Each row's count is the one the row-at-a-time
 * loop computes, from the same set: the walk locates exactly the entry a
 * descent would, and the count is made the same way, under the same
 * snapshot, the set located, counted and released before the next row's is
 * located.  Only the ORDER of the rows changes, and nothing above the node
 * depends on it: the node claims no order (its path has no pathkeys), a
 * partial count is grouped and added up by core's Finalize Agg whatever order
 * the rows come in, and the rows of a count(DISTINCT) go through a Sort.  A
 * key that appears again in a batch - two dimension rows with one key, which
 * a semi or anti join counts once each - comes out next to the first by the
 * sort, and takes its answer instead of a second lookup: the count of one key
 * under one snapshot is one number.
 *
 * The batch holds rows as the child made them, copied, and is bounded by
 * work_mem - another input of the node, as each input of a hash join has an
 * allowance of its own - with at least one row in it.  Nothing is pinned
 * while it is filled, since the child may run for as long as its quals take:
 * the walk lets go of its leaf, and the WHERE sets let go of theirs as they
 * do before a row goes up (lion_pause_run()), which a row-at-a-time run first
 * does after the child's first rows and a batched one would otherwise do only
 * after its whole first batch.  Every count after that is carried by its fk
 * set's own pin, as every count after the first row always was (§27,
 * "Visibility and the §9 interlock").
 */

/* The most rows a batch takes whatever work_mem allows: its array's limit. */
#define LION_JOIN_BATCH_MAX		((int) (MaxAllocSize / sizeof(LionJoinEnt) - 1))

static void lion_pause_run(LionCountScanState *st);

/* The directory's order of two batched rows; NULL keys (an anti join's) first. */
static int
lion_join_ent_cmp(const void *a, const void *b, void *arg)
{
	const LionJoinEnt *x = (const LionJoinEnt *) a;
	const LionJoinEnt *y = (const LionJoinEnt *) b;

	if (x->isnull != y->isnull)
		return x->isnull ? -1 : 1;
	if (!x->isnull)
	{
		int			c = lion_lookup_walk_cmp((LionLookupWalk *) arg,
											 x->key, x->hash, y->key, y->hash);

		if (c != 0)
			return c;
	}
	return (x->seq > y->seq) - (x->seq < y->seq);
}

/*
 * Empty the batch: what it held, the slots that point into it, and the walk's
 * place, which the next batch's first key may be left of.
 */
static void
lion_join_batch_reset(LionCountScanState *st)
{
	if (st->joinwalkbegun)
		lion_lookup_walk_restart(&st->joinwalker);
	if (st->joinbatchslot != NULL)
		ExecClearTuple(st->joinbatchslot);
	if (st->joinsortslot != NULL)
		ExecClearTuple(st->joinsortslot);
	if (st->joinbatchcxt != NULL)
		MemoryContextReset(st->joinbatchcxt);
	st->joinbatch = NULL;
	st->joinbatchn = 0;
	st->joinbatchcap = 0;
	st->joinbatchpos = 0;
	st->joinlasthave = false;
}

/*
 * Read the next batch: child rows (or distinct keys) until work_mem is full or
 * the child has no more, the rows whose NULL key joins nothing left out but
 * for an anti join, whose rows they are.  Then sort it into the directory's
 * order.  False when the child had no row left at all.
 */
static bool
lion_join_fill_batch(LionCountScanState *st)
{
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	Size		limit = (Size) work_mem * 1024;
	MemoryContext oldcxt;
	int			n = 0;

	ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
	st->childslot = NULL;
	lion_join_batch_reset(st);
	lion_pause_run(st);

	while (!st->joinchilddone)
	{
		TupleTableSlot *slot;
		LionJoinEnt *ent;
		Datum		key;
		bool		isnull;

		if (n > 0 &&
			(MemoryContextMemAllocated(st->joinbatchcxt, false) >= limit ||
			 n >= LION_JOIN_BATCH_MAX))
			break;

		CHECK_FOR_INTERRUPTS();
		slot = st->joinunique ? lion_join_next_key(st) :
			lion_join_child_next(st);
		if (TupIsNull(slot))
		{
			st->joinchilddone = true;
			break;
		}
		key = slot_getattr(slot, st->joinkeyresno, &isnull);
		if (isnull && !anti)
			continue;

		oldcxt = MemoryContextSwitchTo(st->joinbatchcxt);
		if (n >= st->joinbatchcap)
		{
			int			cap = (st->joinbatchcap == 0) ? 1024 :
				(int) Min((Size) st->joinbatchcap * 2,
						  (Size) LION_JOIN_BATCH_MAX);

			if (st->joinbatch == NULL)
				st->joinbatch = (LionJoinEnt *)
					palloc(sizeof(LionJoinEnt) * cap);
			else
				st->joinbatch = (LionJoinEnt *)
					repalloc(st->joinbatch, sizeof(LionJoinEnt) * cap);
			st->joinbatchcap = cap;
		}
		ent = &st->joinbatch[n];
		ent->seq = n;
		ent->isnull = isnull;
		ent->key = (Datum) 0;
		ent->hash = 0;
		if (!isnull)
		{
			ent->key = datumCopy(key, st->joinkeybyval, st->joinkeylen);
			ent->hash = lion_lookup_walk_hash(&st->joinwalker, ent->key);
		}
		ent->tuple = st->joinunique ? NULL : ExecCopySlotMinimalTuple(slot);
		MemoryContextSwitchTo(oldcxt);
		n++;
	}

	st->joinbatchn = n;
	if (n == 0)
		return false;
	st->joinbatches++;
	if (n > 1)
		qsort_arg(st->joinbatch, n, sizeof(LionJoinEnt), lion_join_ent_cmp,
				  &st->joinwalker);
	return true;
}

/* A batched row as the child's current row, for the target list. */
static void
lion_join_batch_row(LionCountScanState *st, const LionJoinEnt *ent)
{
	if (st->joinunique)
	{
		TupleTableSlot *slot = st->joinsortslot;

		ExecClearTuple(slot);
		memset(slot->tts_isnull, true,
			   sizeof(bool) * slot->tts_tupleDescriptor->natts);
		slot->tts_values[st->joinkeyresno - 1] = ent->key;
		slot->tts_isnull[st->joinkeyresno - 1] = false;
		st->childslot = ExecStoreVirtualTuple(slot);
	}
	else
		st->childslot = ExecStoreMinimalTuple(ent->tuple, st->joinbatchslot,
											  false);
}

/*
 * lion_join_next_row() for a plan that looks the keys up in key order: the
 * same rows, counted the same way, from the batches above.
 */
static bool
lion_join_next_walked(LionCountScanState *st, int64 *countp)
{
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	MemoryContext oldcxt;

	for (;;)
	{
		LionJoinEnt *ent;
		bool		found;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		if (st->joinbatchpos >= st->joinbatchn &&
			!lion_join_fill_batch(st))
		{
			lion_join_batch_reset(st);
			return false;
		}
		ent = &st->joinbatch[st->joinbatchpos++];

		if (ent->isnull)
		{
			/* joins nothing: an anti join's row, which only it batched */
			Assert(anti);
			if (!st->joinsum)
				lion_join_batch_row(st, ent);
			*countp = 1;
			return true;
		}
		st->joinlookups++;

		if (st->joinlasthave &&
			datumIsEqual(ent->key, st->joinlastkey, st->joinkeybyval,
						 st->joinkeylen))
		{
			found = st->joinlastfound;
			count = st->joinlastcount;
		}
		else
		{
			instr_time	t;

			INSTR_TIME_SET_ZERO(t);
			lion_join_clock(st, -1, &t);
			MemoryContextReset(st->pergroup);
			oldcxt = MemoryContextSwitchTo(st->pergroup);
			found = lion_lookup_walk_find(&st->joinwalker, ent->key, ent->hash,
										  &st->groupset);
			lion_join_clock(st, LION_JT_LOOKUP, &t);
			count = found ? lion_join_count_key(st) : 0;
			lion_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
			lion_join_clock(st, LION_JT_COUNT, &t);

			st->joinlasthave = true;
			st->joinlastkey = ent->key;
			st->joinlastfound = found;
			st->joinlastcount = count;
		}

		if (!found)
			st->joinmissing++;
		if (anti)
			count = 1 - count;	/* no entry, or a test that found nothing */
		if (count == 0)
			continue;

		if (!st->joinsum)
			lion_join_batch_row(st, ent);
		*countp = count;
		return true;
	}
}

/*
 * The FK-side join (DESIGN.md §27): the next dimension row with fact rows -
 * its partial count in *countp, and the row itself in childslot, for the
 * target list - or false when there is none left.
 *
 * Each row of the child plan - the dimension side, under the query's snapshot,
 * with its quals, RLS and privileges applied by core - carries a key.  A NULL
 * key joins nothing, since the join operator is strict.  Any other is looked
 * up in the fk index, and its posting set, ANDed with the fact's WHERE
 * sources, is counted exactly as §15's GROUP BY driver counts one group: the
 * set is source slot 0, located, counted and released inside pergroup before
 * the next child row is fetched, so one fk set and the WHERE sets are all the
 * pins there are (DESIGN.md §9), and a lookup over the pin budget comes out
 * NOPIN and is taken care of by the count (§15).  A key with no entry, or
 * whose rows the fact filters and the snapshot leave none of, produces no row:
 * an inner join has no pair for it.  When the WHERE sources were collected
 * into one private set (lion_join_collect()) the count is the fk set ANDed
 * with that copy instead, which is the same intersection.
 *
 * A SEMI join's row is the dimension row itself, once, when it joins at least
 * one fact row, so its count is an EXISTENCE test (§26's, the same merge
 * stopped at the first visible row) and the partial count is 1.  An ANTI
 * join's row is the dimension row that joins none: a NULL key, a key with no
 * entry, a key whose test finds nothing, and every row when a fact filter
 * selects nothing at all.  The rows of a count(DISTINCT) (joinrows) are the
 * same existence tests over an inner join: the dimension rows that join,
 * once each, with no count at all.
 *
 * A forward semi join over a non-unique key (joinunique) reads its rows from
 * the sorted child instead, one per DISTINCT key (lion_join_next_key()), and
 * counts each as an inner join counts a dimension row.  The posting sets of
 * distinct keys are disjoint - a fact row has one fk value, and two distinct
 * keys cannot both equal it - so the counts add up to the fact rows with at
 * least one matching dimension row, each once: the semi join's count.
 *
 * A summed join (joinsum, lion_next_join_row()) wants no row, only the
 * count: the child's next row is fetched with no pin of the node's held
 * (lion_pause_run(), which has something to let go of only the first time -
 * the WHERE sets' pins - as a row-at-a-time join that hands its rows up lets
 * go of them at its first row).
 */
static bool
lion_join_next_row(LionCountScanState *st, int64 *countp)
{
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	MemoryContext oldcxt;

	for (;;)
	{
		TupleTableSlot *childslot;
		Datum		key;
		bool		isnull;
		bool		found;
		int64		count;
		instr_time	t;

		CHECK_FOR_INTERRUPTS();

		if (st->joinsum)
			lion_pause_run(st);
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		childslot = st->joinunique ? lion_join_next_key(st) :
			lion_join_child_next(st);
		if (TupIsNull(childslot))
		{
			st->childslot = NULL;
			return false;
		}

		key = slot_getattr(childslot, st->joinkeyresno, &isnull);
		if (isnull || st->wheremissing)
		{
			/* joins nothing: a row only of an anti join */
			if (!anti)
				continue;
			st->childslot = childslot;
			*countp = 1;
			return true;
		}
		st->joinlookups++;

		INSTR_TIME_SET_ZERO(t);
		lion_join_clock(st, -1, &t);
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		/* a descent, with the key's probe resolved once for the node */
		found = lion_lookup_walk_descend(&st->joinwalker, key, &st->groupset);
		lion_join_clock(st, LION_JT_LOOKUP, &t);
		if (!found)
		{
			lion_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
			st->joinmissing++;
			if (!anti)
				continue;
			st->childslot = childslot;
			*countp = 1;
			return true;
		}

		count = lion_join_count_key(st);
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
		lion_join_clock(st, LION_JT_COUNT, &t);

		if (anti)
			count = 1 - count;
		if (count == 0)
			continue;

		st->childslot = childslot;
		*countp = count;
		return true;
	}
}

/*
 * The FK-side join's next row (DESIGN.md §27): one PARTIAL row per dimension
 * row with fact rows - its dimension columns and its count - which core's
 * Finalize Agg above groups by the dimension columns and adds up, the join's
 * count for each group being a sum over the group's dimension rows; or, for
 * joinrows, a row core's plain Agg aggregates as it would the join's.
 *
 * SUMMED (joinsum, LION_JOINFLAG_SUM): when the target list is counts alone -
 * `SELECT count(*) FROM fact JOIN dim ...` and its semi, anti and distinct-key
 * forms, with no dimension column to group by or to print - the rows'
 * partial counts differ in nothing the Finalize Agg looks at but the count,
 * and it adds them up.  So the node adds them up itself and hands up ONE
 * partial row, their sum, per run of each participant: exactly what a
 * Partial Aggregate below a Gather hands up.  Every dimension row that used
 * to go up alone paid for it - its projection, the return through the
 * executor, a pause that let the walk's leaf go (read again for the next
 * key), the Finalize Agg's transition, and in a parallel plan a trip through
 * the Gather's tuple queue into the leader, which reads every worker's rows
 * alone - and none of that was the key's own work.  An empty join hands up
 * no row, as a join whose every row has count 0 did: a plain Finalize Agg
 * answers 0 for it, and a GROUP BY folded to constants has no group, as core
 * forms none over an empty input.
 */
static TupleTableSlot *
lion_next_join_row(LionCountScanState *st)
{
	LionClauseState *jcl = &st->clause[st->joinclause];
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	bool		walked = false;
	int64		count;
	MemoryContext oldcxt;

	if (!st->joincollected)
		lion_join_collect(st);

	/*
	 * A fact clause that selects nothing leaves no dimension row a count - and
	 * makes every one of them a row of an anti join, which the loop below
	 * emits without a lookup.
	 */
	if (st->wheremissing && !anti)
	{
		st->done = true;
		return NULL;
	}

	if (st->joinunique && !st->joinsortdone)
		lion_join_sort_keys(st);

	/*
	 * The walk of the fk index's directory, begun once for the node in its
	 * query memory, which resolves the key's probe once for all the lookups
	 * (lion_lookup_walk_begin()) - in key order when the plan asks for it and
	 * the keys can be sorted into it, and otherwise a descent per key.  Keys
	 * the directory cannot be sorted by - a cross-type key whose family has
	 * no ordering of its own for that type (lion_probe_init()) - are looked
	 * up a row at a time, as they would be without the walk.
	 */
	if (!st->wheremissing)
	{
		if (!st->joinwalkbegun)
		{
			oldcxt = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);
			lion_lookup_walk_begin(&st->joinwalker, jcl->idx, jcl->idxcol,
								   jcl->valtype);
			MemoryContextSwitchTo(oldcxt);
			st->joinwalkbegun = true;
		}
		walked = (st->joinwalk && lion_lookup_walk_ordered(&st->joinwalker));
	}

	if (st->joinsum)
	{
		int64		total = 0;

		while (walked ? lion_join_next_walked(st, &count) :
			   lion_join_next_row(st, &count))
			total += count;
		st->done = true;
		st->childslot = NULL;
		if (total == 0)
			return NULL;
		return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, total);
	}

	if (!(walked ? lion_join_next_walked(st, &count) :
		  lion_join_next_row(st, &count)))
	{
		st->done = true;
		return NULL;
	}
	return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, count);
}

/*
 * Walk one partition without a group key: open it, locate its clauses, count
 * it, then let go of everything it owns (DESIGN.md §16).
 */
static int64
lion_run_partition(LionCountScanState *st, int p)
{
	int64		count;

	lion_open_relation(st, st->part[p].heapoid, st->part[p].groupidxoid,
					  st->part[p].groupidxoid2, st->part[p].clauseidxoid);
	lion_locate_where(st);

	if (st->hasgroupidx)
	{
		/* No group key of its own: the only driver left is a sum-over-all. */
		Assert(st->sumall);
		count = lion_sumall_relation(st);
	}
	else
		count = lion_count_relation(st);

	lion_release_where(st);
	lion_close_relation(st);

	return count;
}

/*
 * GROUP BY over a partitioned table: one PARTIAL aggregate per group per
 * partition (DESIGN.md §16).
 *
 * The partitions are walked in the planner's order and each one is opened,
 * iterated and closed in turn, its groups emitted as they are counted.  The
 * node therefore holds no cross-partition state at all - no hash table, no
 * per-node group memory beyond one partition's iteration state - and the
 * Finalize HashAggregate core puts above it combines the partial counts,
 * spilling to disk under hash_mem like any HashAggregate.  A group with rows
 * in several partitions is emitted once per partition, and the §9 pin
 * discipline is unchanged: a partition's posting sets are all released
 * before its indexes are closed.
 */
static TupleTableSlot *
lion_next_partial_group(LionCountScanState *st)
{
	for (;;)
	{
		if (!st->partopen)
		{
			if (st->curpart >= st->npart)
			{
				st->done = true;
				return NULL;
			}

			lion_open_relation(st, st->part[st->curpart].heapoid,
							  st->part[st->curpart].groupidxoid,
							  st->part[st->curpart].groupidxoid2,
							  st->part[st->curpart].clauseidxoid);
			lion_locate_where(st);
			st->partopen = true;

			/*
			 * A positive clause with no entry in THIS partition selects
			 * nothing here, whatever the others hold, so its entry scan is
			 * skipped and no group of it is emitted.
			 */
			if (!st->wheremissing)
			{
				lion_entry_scan_begin_range(&st->escan, st->groupidx,
											st->groupidxcol,
											st->hasrange ? &st->range : NULL);
				st->scanning = true;
				/* each partition's group of c is its own partial row */
				st->coalcount = 0;
				st->coalwalked = false;
			}
		}

		if (st->scanning)
		{
			bool		exhausted;
			TupleTableSlot *slot = lion_next_group_any(st, &exhausted);

			if (!exhausted)
				return slot;

			lion_entry_scan_end(&st->escan);
			st->scanning = false;
		}

		/* This partition is done: release its sets, then close it. */
		lion_release_where(st);
		lion_close_relation(st);
		st->partopen = false;
		st->curpart++;
	}
}

/*
 * The partitioned form of the node without a group key (DESIGN.md §16): the
 * one row is the sum over every partition, so nothing comes out until the
 * last of them has been counted.
 */
static TupleTableSlot *
lion_exec_partitioned(LionCountScanState *st)
{
	int64		total = 0;
	int			p;

	if (st->hasgroupidx && st->groupattno != 0)
		return lion_next_partial_group(st);

	for (p = 0; p < st->npart; p++)
		total += lion_run_partition(st, p);

	st->done = true;

	/*
	 * A plain aggregate always produces its one row; a GROUP BY whose columns
	 * the planner folded to constants produces one only if the group exists.
	 */
	if (total == 0 && st->singlegroup)
		return NULL;

	return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, total);
}

/*
 * A row is about to go up to the executor, which may not ask for the next one
 * for as long as a cursor stays open - so nothing the node keeps across the
 * row may hold a buffer pin, or an idle cursor would hold VACUUM up on that
 * page (§11 takes a cleanup lock on every page it rewrites).  DESIGN.md §9's
 * pin is the interlock for a count's visibility-map questions and nothing
 * else, and between two rows no count is running:
 *
 *	- the walks let go of the directory leaf they stand on
 *	  (lion_entry_scan_pause());
 *	- every located WHERE set that holds a pin - an INLINE one - gives it up
 *	  and becomes NOPIN (lion_posting_set_unpin()): a private copy, which is
 *	  what a set located past the §15 pin budget has always been, and which
 *	  every count already knows how to take.  Each count after this row gets
 *	  the interlock from the set that drives it, which every shape that
 *	  returns more than one row locates afresh under a pin of its own - a
 *	  group's entry, the inner set of a (g, k) pair (§20, §26), the fk set of
 *	  a dimension row (§27), and the listed value an IN list drives the
 *	  groups by, which lion_next_group_inlist() locates again for that
 *	  reason.  A count that is left with no source that carries it trusts no
 *	  map and rechecks, and a lone NOPIN set is located again before it is
 *	  counted, as always (lion_count_sources_cached());
 *	- and the outer group's set of a two-column GROUP BY (§20) does the same:
 *	  its remaining pairs are carried by their inner sets.
 */
static void
lion_pause_run(LionCountScanState *st)
{
	if (st->scanning)
		lion_entry_scan_pause(&st->escan);
	if (st->scanning2)
		lion_entry_scan_pause(&st->escan2);

	lion_unpin_where(st);
	if (st->outeropen)
		lion_posting_set_unpin(&st->groupset);

	/*
	 * ... and a walk of the fk index in key order (§27) lets go of the leaf it
	 * stands on; the next key reads it again by its block number.
	 */
	if (st->joinwalkbegun)
		lion_lookup_walk_pause(&st->joinwalker);
}

/*
 * The WHERE sets let go of the pins they were located with: NOPIN copies from
 * here on, every count of them carried by another source's pin (DESIGN.md
 * §15, "Paused and finished counts").  They gain pins only where they are
 * located (lion_locate_where()), so after the first time there is nothing
 * left to unpin in them - and walking them all again after every row made a
 * list-driven GROUP BY of N values N-squared.
 */
static void
lion_unpin_where(LionCountScanState *st)
{
	int			i;
	int			j;

	if (st->sources == NULL || !st->wherepinned)
		return;
	for (i = 1; i <= st->nitem; i++)
	{
		LionCountSource *src = &st->sources[i];

		for (j = 0; j < src->nsets; j++)
			lion_posting_set_unpin(&src->sets[j]);
	}
	st->wherepinned = false;
}

/*
 * The node has produced its last row.  A plain table stays open until the
 * node ends, and everything located in it used to stay located with it - the
 * WHERE sets and their pins, which a cursor left open after its last FETCH, or
 * after the one row of a plain count, held until the transaction ended.
 * Nothing is counted again before a rescan, which locates everything afresh,
 * so the walks, the group sets and the WHERE sets all go now.  (A partitioned
 * scan has let go of each partition's at the end of its turn already.)
 */
static void
lion_finish_run(LionCountScanState *st)
{
	if (st->scanning)
	{
		lion_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	if (st->scanning2)
	{
		lion_entry_scan_end(&st->escan2);
		st->scanning2 = false;
	}
	lion_posting_set_release(&st->groupset);
	lion_posting_set_release(&st->groupset2);
	st->outeropen = false;
	lion_join_batch_reset(st);
	lion_release_where(st);

	/*
	 * ... and the visibility-map page the counts kept pinned from one to the
	 * next, which a paused node keeps as an index-only scan keeps its own:
	 * no VACUUM waits for a map page's pin (lion_vis_cache_release_vm()).
	 */
	lion_vis_cache_release_vm(st->viscache);
}

static TupleTableSlot *
lion_exec_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;
	int64		dirbefore = lion_dir_pages_read;
	TupleTableSlot *slot;

	/*
	 * One group per call, except that a group the HAVING rejects is not a
	 * result: keep going until one passes or the groups run out.
	 */
	for (;;)
	{
		if (st->done)
		{
			slot = NULL;
			break;
		}
		st->filtered = false;
		slot = lion_exec_custom_scan_internal(node);
		if (slot != NULL || !st->filtered)
			break;
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * No index pin outlives the call (DESIGN.md §15, "Paused and finished
	 * counts"): a node that is done lets go of everything, and one that
	 * returns a row with more to come keeps its sets but none of their pins -
	 * only the visibility-map page its counts share, as an index-only scan
	 * keeps its own.  The row itself points at nothing either releases: a key
	 * the WHERE pinned is in keycxt, a group's key in pergroup or outercxt,
	 * and a listed value's in the set, which a pause keeps.
	 */
	if (st->done)
		lion_finish_run(st);
	else if (slot != NULL)
		lion_pause_run(st);
	st->dirpages += lion_dir_pages_read - dirbefore;

	return slot;
}

static TupleTableSlot *
lion_exec_custom_scan_internal(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;
	int64		count;

	/*
	 * The parameters first: a generic prepared plan's `k = $1` and a nested
	 * loop's exec Param are only values here, and ReScan has thrown the
	 * previous ones away (DESIGN.md §10).
	 */
	if (!st->valsdone)
		lion_eval_clause_values(st);

	/* A partitioned table counts one partition at a time. */
	if (st->npart > 0)
		return lion_exec_partitioned(st);

	if (!st->located)
		lion_locate_where(st);

	/* ---- the FK-side join: one partial row per dimension row ---- */
	if (st->joinclause >= 0)
		return lion_next_join_row(st);

	/* ---- no index to iterate: exactly one row ---- */
	if (!st->hasgroupidx)
	{
		/* Without a group index a clause has to drive the count. */
		st->done = true;
		count = lion_count_relation(st);

		/*
		 * A plain aggregate always produces its one row; a GROUP BY whose
		 * columns the planner folded to constants produces one only if the
		 * group exists.
		 */
		if (count == 0 && st->singlegroup)
			return NULL;

		return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, count);
	}

	/* ---- the group index drives the count ---- */
	if (st->wheremissing)
	{
		st->done = true;
		/*
		 * A sum over all entries still has to report its one row, and so does
		 * a count(DISTINCT k) without a GROUP BY (DESIGN.md §26) - unless the
		 * planner folded a GROUP BY to one group, which does not exist then.
		 */
		st->distcount = 0;
		st->distcolcount = 0;
		if ((st->sumall || (st->distattno != 0 && st->groupattno == 0)) &&
			!st->singlegroup)
			return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, 0);
		return NULL;
	}

	/* ---- count(DISTINCT k) over the WHERE: one row (DESIGN.md §26) ---- */
	if (st->distattno != 0 && st->groupattno == 0)
		return lion_distinct_relation(st);

	/* ---- every entry of the index, summed into one row ---- */
	if (st->sumall)
	{
		int64		total = lion_sumall_relation(st);

		st->done = true;

		/*
		 * A GROUP BY the planner folded to one group has no row when the group
		 * is empty - which a range-bounded sum (DESIGN.md §28) can be:
		 * `... WHERE g = 3 AND k < 20 GROUP BY g`.
		 */
		if (total == 0 && st->singlegroup)
			return NULL;
		return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, total);
	}

	/*
	 * An IN list on the grouping column drives the groups itself (DESIGN.md
	 * §15) and never looks at the index's entries, so there is no entry scan to
	 * begin.  (The partitioned path opens one per partition either way; a scan
	 * that is only begun holds nothing, so it costs the flag it sets.)
	 */
	if (!st->scanning && st->ingroupitem < 0)
	{
		lion_entry_scan_begin_range(&st->escan, st->groupidx, st->groupidxcol,
									st->hasrange ? &st->range : NULL);
		st->scanning = true;
		st->coalcount = 0;
		st->coalwalked = false;
	}

	/* ---- GROUP BY: one row per non-empty group ---- */
	{
		bool		exhausted;
		TupleTableSlot *slot = lion_next_group_any(st, &exhausted);

		if (exhausted)
			st->done = true;
		return slot;
	}
}

/*
 * Everything the node built while running, undone.  A partitioned scan may be
 * standing in the middle of a partition (a LIMIT above it, an error being
 * unwound), so the relation it has open is closed here too; every posting set
 * has been released before any of them, which is what DESIGN.md §9 requires.
 */
static void
lion_reset_run(LionCountScanState *st)
{
	int			i;

	if (st->scanning)
	{
		lion_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	if (st->scanning2)
	{
		lion_entry_scan_end(&st->escan2);
		st->scanning2 = false;
	}
	lion_posting_set_release(&st->groupset);
	lion_posting_set_release(&st->groupset2);
	st->outeropen = false;
	st->inneridx = 0;
	st->coalcount = 0;
	st->coalwalked = false;
	lion_release_where(st);

	/* The FK-side join's collected filters (in outercxt, reset below). */
	lion_posting_set_release(&st->joinfilter);
	st->joincollected = false;
	st->joinfiltered = false;

	/* ... and a forward semi join's sorted keys, sorted again next run. */
	if (st->joinsort != NULL)
	{
		tuplesort_end(st->joinsort);
		st->joinsort = NULL;
	}
	if (st->joinsortslot != NULL)
		ExecClearTuple(st->joinsortslot);
	if (st->joinkeycxt != NULL)
		MemoryContextReset(st->joinkeycxt);
	st->joinsortdone = false;
	st->joinhaveprev = false;
	st->joinkeypos = 0;
	st->joinchunk = -1;

	/* ... and the batch of keys being looked up in key order, and the walk */
	lion_join_batch_reset(st);
	st->joinchilddone = false;

	if (st->npart > 0)
		lion_close_relation(st);

	if (st->pergroup != NULL)
		MemoryContextReset(st->pergroup);
	if (st->outercxt != NULL)
		MemoryContextReset(st->outercxt);

	for (i = 0; i < st->nclause; i++)
	{
		st->clause[i].hasstoredkey = false;
		st->clause[i].keyisnull = false;
		st->clause[i].storedkey = (Datum) 0;
	}
	if (st->keycxt != NULL)
		MemoryContextReset(st->keycxt);

	if (st->viscache != NULL)
		lion_vis_cache_reset(st->viscache);

	/*
	 * The clause values go too: a rescan of a parameterised inner side has to
	 * read the new exec Param rather than the value the last scan copied
	 * (DESIGN.md §10).  A literal's value lives in the Const and stays.
	 */
	st->valsdone = false;
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];

		if (cl->valstate != NULL)
		{
			cl->val = (Datum) 0;
			cl->valisnull = false;
		}
	}
	if (st->valcxt != NULL)
		MemoryContextReset(st->valcxt);

	st->curpart = 0;
	st->partopen = false;
}

static void
lion_rescan_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;

	lion_reset_run(st);
	st->done = false;

	/*
	 * The join's child (DESIGN.md §27) starts over too.  Core propagates a
	 * changed parameter to outer and inner plans but not to custom_ps, so it
	 * is handed on here; a child that has one rescans itself on its next
	 * ExecProcNode().
	 */
	if (st->child != NULL)
	{
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(st->child, node->ss.ps.chgParam);
		if (st->child->chgParam == NULL)
			ExecReScan(st->child);
	}
	st->childslot = NULL;
}

/*
 * A parallel FK-side join (DESIGN.md §27, "Parallel") keeps one small struct
 * in the Gather's dynamic shared memory: what the participants add up for
 * EXPLAIN ANALYZE.  Nothing else is shared - each participant reads its share
 * of the dimension rows from the parallel-aware child, locates the fact
 * filters and makes its own copy of them - so a node that is not a join, or
 * a join run without a Gather, never gets here.
 */
static Size
lion_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	return MAXALIGN(sizeof(LionJoinShared));
}

static void
lion_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
					void *coordinate)
{
	LionCountScanState *st = (LionCountScanState *) node;
	LionJoinShared *shared = (LionJoinShared *) coordinate;

	memset(shared, 0, sizeof(LionJoinShared));
	SpinLockInit(&shared->mutex);
	shared->filterrows = -1;
	pg_atomic_init_u32(&shared->nextchunk, 0);
	st->joinshared = shared;

	/*
	 * Every participant locates the fact filters for itself, so each takes
	 * its share of the list pin budget (DESIGN.md §15, "The pin budget"):
	 * the workers planned and the leader, whether it takes part or not.
	 */
	shared->participants = pcxt->nworkers + 1;
	lion_list_pin_participants(shared->participants);
}

/*
 * Before the workers are launched again for a rescan.  The sums are kept:
 * they are the whole execution's, as the leader's own counters are.  The runs
 * of distinct keys a forward semi join's participants claim start again from
 * the first, since the rescan's keys are sorted again (and may be others).
 * No participant is running: the Gather has shut the workers down, and the
 * leader's own node claims nothing before the Gather launches them again.
 */
static void
lion_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
					  void *coordinate)
{
	LionJoinShared *shared = (LionJoinShared *) coordinate;

	pg_atomic_write_u32(&shared->nextchunk, 0);
	SpinLockAcquire(&shared->mutex);
	shared->sortedruns += shared->sorted;
	shared->sorted = 0;
	SpinLockRelease(&shared->mutex);
}

static void
lion_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	LionCountScanState *st = (LionCountScanState *) node;

	st->joinshared = (LionJoinShared *) coordinate;
	lion_list_pin_participants(st->joinshared->participants);
}

/*
 * The end of a participant's execution.  A worker adds its counters to the
 * shared sums, once.  The leader takes a copy of them for EXPLAIN: its
 * ExecShutdownNode() reaches this node before the Gather above it, so before
 * the dynamic shared memory is detached, and after the Gather has read every
 * worker's last row - which it does only once they have all finished
 * (gather_readnext() shuts them down), and every shape above this node reads
 * all of it.  A Gather stopped early would leave some worker's counters out
 * of EXPLAIN, and nothing else.
 */
static void
lion_shutdown_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;
	LionJoinShared *shared = st->joinshared;
	int			i;

	if (shared == NULL)
		return;

	if (IsParallelWorker())
	{
		if (st->joinreported)
			return;
		SpinLockAcquire(&shared->mutex);
		lion_count_stats_add(&shared->stats, &st->stats);
		shared->lookups += st->joinlookups;
		shared->missing += st->joinmissing;
		shared->dirpages += st->dirpages;
		shared->filterrows = Max(shared->filterrows, st->joinfilterrows);
		shared->spilled += st->joinspilled;
		shared->sorted = Max(shared->sorted, st->joinsorted);
		shared->batches += st->joinbatches;
		shared->childrows += st->joinchildrows;
		shared->posting += st->joinposting;
		for (i = 0; i < LION_JT_N; i++)
			INSTR_TIME_ADD(shared->time[i], st->jointime[i]);
		SpinLockRelease(&shared->mutex);
		st->joinreported = true;
		return;
	}

	SpinLockAcquire(&shared->mutex);
	st->joinworkerstats = shared->stats;
	st->joinworkerlookups = shared->lookups;
	st->joinworkermissing = shared->missing;
	st->joinworkerdirpages = shared->dirpages;
	st->joinworkerfilterrows = shared->filterrows;
	st->joinworkerspilled = shared->spilled;
	st->joinworkersorted = shared->sortedruns + shared->sorted;
	st->joinworkerbatches = shared->batches;
	st->joinworkerchildrows = shared->childrows;
	st->joinworkerposting = shared->posting;
	for (i = 0; i < LION_JT_N; i++)
		st->joinworkertime[i] = shared->time[i];
	SpinLockRelease(&shared->mutex);
}

static void
lion_end_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;

	lion_reset_run(st);

	/* the leader's share of the list pin budget is its whole again */
	if (st->joinshared != NULL && !IsParallelWorker())
		lion_list_pin_participants(1);

	if (st->child != NULL)
	{
		ExecEndNode(st->child);
		st->child = NULL;
	}
	st->childslot = NULL;

	/* A plain table's relations were opened once and are closed once. */
	if (st->npart == 0)
		lion_close_relation(st);

	if (st->pergroup != NULL)
	{
		MemoryContextDelete(st->pergroup);
		st->pergroup = NULL;
	}
	if (st->outercxt != NULL)
	{
		MemoryContextDelete(st->outercxt);
		st->outercxt = NULL;
	}
	if (st->innercxt != NULL)
	{
		MemoryContextDelete(st->innercxt);
		st->innercxt = NULL;
	}
	st->innerkey = NULL;
	st->innerisnull = NULL;
	st->ninnerkey = 0;
	if (st->wherecxt != NULL)
	{
		MemoryContextDelete(st->wherecxt);
		st->wherecxt = NULL;
	}
	if (st->keycxt != NULL)
	{
		MemoryContextDelete(st->keycxt);
		st->keycxt = NULL;
	}
	if (st->valcxt != NULL)
	{
		MemoryContextDelete(st->valcxt);
		st->valcxt = NULL;
	}
	if (st->joinkeycxt != NULL)
	{
		MemoryContextDelete(st->joinkeycxt);
		st->joinkeycxt = NULL;
	}
	if (st->joinbatchcxt != NULL)
	{
		MemoryContextDelete(st->joinbatchcxt);
		st->joinbatchcxt = NULL;
	}
	if (st->viscache != NULL)
	{
		lion_vis_cache_destroy(st->viscache);
		st->viscache = NULL;
	}
}

/*
 * "col = 3", "col = ANY ('{1,2,3}'::integer[])", "col IS NULL",
 * "col IS NOT NULL", "tags @> '{a,b}'::text[]", "col >= 10" (a range,
 * DESIGN.md §28, with the column on the left whichever side the query had it
 * on) - each with the clause's OWN operator and its value as core's EXPLAIN
 * would print it in a qual.
 *
 * Both used to be approximated, and the approximation hid a wrong answer (the
 * 2026-09-25 review): every equality was printed with `=`, so a clause on a
 * case-insensitive opclass's `===` read exactly like the `=` beside it that
 * the planner had dropped as its duplicate, and a literal was printed through
 * its type's output function alone, so `v = 'A'` came out as `(v = A)` and a
 * list as `ANY ({90,5,50,1})`.  Now the operator is named - `===` prints as
 * `===`, as the multi-key and range clauses always did - and the value is
 * deparsed like any other expression the plan carries: a literal with its
 * quotes and its type, and a parameter as `$1`, the same text core gives a
 * qual on one (DESIGN.md §10).
 */
static void
lion_explain_clause(LionCountScanState *st, LionClauseState *cl, List *ancestors,
				   ExplainState *es, StringInfo buf)
{
	const char *attname = get_attname(st->heapoid, cl->attno, false);

	switch (cl->kind)
	{
		case LION_CLAUSE_NULL:
			appendStringInfo(buf, "%s IS NULL", attname);
			break;
		case LION_CLAUSE_NOTNULL:
			appendStringInfo(buf, "%s IS NOT NULL", attname);
			break;
		default:
			{
				List	   *context;
				char	   *opname = get_opname(cl->opno);
				char	   *val;

				if (opname == NULL)
					elog(ERROR, "LionCount: cache lookup failed for operator %u",
						 cl->opno);

				context = set_deparse_context_plan(es->deparse_cxt,
												   st->css.ss.ps.plan,
												   ancestors);

				/*
				 * The FK-side join's key is a column of the OTHER table
				 * (DESIGN.md §27), so it is printed qualified: `fk = d.pk`,
				 * as core prints a join clause.
				 */
				val = deparse_expression((Node *) cl->valexpr, context,
										 st->joinclause >= 0 &&
										 cl == &st->clause[st->joinclause],
										 false);
				if (cl->kind == LION_CLAUSE_ARRAY)
					appendStringInfo(buf, "%s %s ANY (%s)", attname, opname,
									 val);
				else
					appendStringInfo(buf, "%s %s %s", attname, opname, val);
				pfree(opname);
				pfree(val);
				break;
			}
	}
}

/*
 * The KEY COLUMN an index answers one clause with, as ".col" (DESIGN.md §24).
 *
 * A SINGLE-column index prints nothing at all, so every plan the regression
 * suite had before multicolumn indexes existed is unchanged; a multicolumn one
 * has to say which of its columns it is being read for, because two clauses of
 * one query may now name the same index.
 *
 * The index is opened here rather than read from the executor state: EXPLAIN
 * without ANALYZE never opens anything (EXEC_FLAG_EXPLAIN_ONLY), and it is the
 * one case where the name is wanted and the relation is not in hand.  The heap
 * attribute number is the one the plan carries; a partitioned scan prints no
 * index name at all, so it never gets here with a parent's numbering.
 */
static const char *
lion_explain_col(Oid idxoid, AttrNumber heapattno, bool multikey)
{
	static char buf[NAMEDATALEN + 2];
	Relation	idx;
	AttrNumber	col;

	if (!OidIsValid(idxoid) || heapattno <= 0)
		return "";

	idx = index_open(idxoid, AccessShareLock);
	if (IndexRelationGetNumberOfKeyAttributes(idx) <= 1)
	{
		index_close(idx, AccessShareLock);
		return "";
	}

	col = lion_index_col_for(idx, heapattno, multikey);
	snprintf(buf, sizeof(buf), ".%s",
			 NameStr(TupleDescAttr(RelationGetDescr(idx), col - 1)->attname));
	index_close(idx, AccessShareLock);

	return buf;
}

/* ... for one clause, whose kind says which kind of column answers it. */
static const char *
lion_explain_clause_col(const LionClauseState *cl)
{
	return lion_explain_col(cl->idxoid, cl->attno,
							cl->kind == LION_CLAUSE_MULTI);
}

static void
lion_explain_custom_scan(CustomScanState *node, List *ancestors,
						ExplainState *es)
{
	LionCountScanState *st = (LionCountScanState *) node;
	StringInfoData buf;
	int			i;

	/*
	 * A partitioned scan uses one index per partition per clause, so there is
	 * no single name to print: the entries are the clauses alone, and the
	 * partitions get a line of their own (DESIGN.md §16).
	 */
	if (st->npart > 0)
	{
		initStringInfo(&buf);
		for (i = 0; i < st->npart; i++)
		{
			if (i > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, get_rel_name(st->part[i].heapoid));
		}
		ExplainPropertyText("Partitions", buf.data, es);
		pfree(buf.data);
	}

	initStringInfo(&buf);

	if (st->hasgroupidx)
	{
		if (st->npart == 0)
			appendStringInfo(&buf, "%s%s ", get_rel_name(st->groupidxoid),
							 lion_explain_col(st->groupidxoid,
											  st->driveattno, false));
		if (st->hasrange)
		{
			bool		firstrange = true;

			/* The walk's bounds (DESIGN.md §28): `(k >= 10 AND k < 20)`. */
			appendStringInfoChar(&buf, '(');
			for (i = 0; i < st->nclause; i++)
			{
				if (st->clause[i].kind != LION_CLAUSE_RANGE)
					continue;
				if (!firstrange)
					appendStringInfoString(&buf, " AND ");
				lion_explain_clause(st, &st->clause[i], ancestors, es, &buf);
				firstrange = false;
			}
			appendStringInfoChar(&buf, ')');
		}
		else if (st->groupattno != 0 || st->distattno != 0)
			appendStringInfo(&buf, "(%s)",
							 get_attname(st->heapoid, st->driveattno, false));
		else
			appendStringInfoString(&buf, "(all keys)");

		/*
		 * The inner index of a two-column GROUP BY (DESIGN.md §20), or of the
		 * (g, k) pairs of a count(DISTINCT k) per group (§26).
		 */
		if (st->innerattno != 0)
		{
			appendStringInfoString(&buf, ", ");
			if (st->npart == 0)
				appendStringInfo(&buf, "%s%s ", get_rel_name(st->groupidxoid2),
								 lion_explain_col(st->groupidxoid2,
												  st->innerattno, false));
			appendStringInfo(&buf, "(%s)",
							 get_attname(st->heapoid, st->innerattno, false));
		}
	}

	/*
	 * The FK-side join's key first (DESIGN.md §27): the fk index and the
	 * clause, whose value prints as the dimension column it is read from.
	 */
	if (st->joinclause >= 0)
	{
		LionClauseState *cl = &st->clause[st->joinclause];

		appendStringInfo(&buf, "%s%s (", get_rel_name(cl->idxoid),
						 lion_explain_clause_col(cl));
		lion_explain_clause(st, cl, ancestors, es, &buf);
		appendStringInfoChar(&buf, ')');
	}

	for (i = 0; i < st->nitem; i++)
	{
		int			orno = st->item[i].orno;

		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");

		if (orno < 0 && st->item[i].rangesrc)
		{
			/*
			 * A range taken as a source (DESIGN.md §32): its index and all
			 * of its bounds, `ix (k >= 10 AND k < 20)`.
			 */
			LionClauseState *first = &st->clause[st->item[i].clauseno];
			bool		firstbound = true;
			int			j;

			if (st->npart == 0)
				appendStringInfo(&buf, "%s%s ", get_rel_name(first->idxoid),
								 lion_explain_clause_col(first));
			appendStringInfoChar(&buf, '(');
			for (j = 0; j < st->nclause; j++)
			{
				if (st->clause[j].kind != LION_CLAUSE_RANGESRC || st->inor[j] ||
					st->clause[j].attno != first->attno)
					continue;
				if (!firstbound)
					appendStringInfoString(&buf, " AND ");
				lion_explain_clause(st, &st->clause[j], ancestors, es, &buf);
				firstbound = false;
			}
			appendStringInfoChar(&buf, ')');
		}
		else if (orno < 0)
		{
			if (st->npart == 0)
			{
				LionClauseState *cl = &st->clause[st->item[i].clauseno];

				appendStringInfo(&buf, "%s%s ", get_rel_name(cl->idxoid),
								 lion_explain_clause_col(cl));
			}
			appendStringInfoChar(&buf, '(');
			lion_explain_clause(st, &st->clause[st->item[i].clauseno],
							   ancestors, es, &buf);
			appendStringInfoChar(&buf, ')');
		}
		else
		{
			/*
			 * An OR is one source over several indexes (DESIGN.md §19), so it
			 * names all of them and then prints the boolean expression:
			 * `ix_a, ix_b ((a = 1) OR (b = 2))`.
			 */
			LionOrState *o = &st->ors[orno];
			int			leaf;
			int			arm;
			int			j;

			if (st->npart == 0)
			{
				for (leaf = 0; leaf < o->nleaves; leaf++)
				{
					LionClauseState *cl = &st->clause[o->first + leaf];

					appendStringInfo(&buf, "%s%s%s",
									 leaf > 0 ? ", " : "",
									 get_rel_name(cl->idxoid),
									 lion_explain_clause_col(cl));
				}
				appendStringInfoChar(&buf, ' ');
			}

			appendStringInfoChar(&buf, '(');
			leaf = 0;
			for (arm = 0; arm < o->narms; arm++)
			{
				if (arm > 0)
					appendStringInfoString(&buf, " OR ");
				if (o->armlen[arm] > 1)
					appendStringInfoChar(&buf, '(');
				for (j = 0; j < o->armlen[arm]; j++, leaf++)
				{
					if (j > 0)
						appendStringInfoString(&buf, " AND ");
					appendStringInfoChar(&buf, '(');
					lion_explain_clause(st, &st->clause[o->first + leaf],
									   ancestors, es, &buf);
					appendStringInfoChar(&buf, ')');
				}
				if (o->armlen[arm] > 1)
					appendStringInfoChar(&buf, ')');
			}
			appendStringInfoChar(&buf, ')');
		}
	}

	ExplainPropertyText("Lion Indexes", buf.data, es);
	pfree(buf.data);

	/*
	 * The FK-side join (DESIGN.md §27): what a dimension row counts - its
	 * join pairs, or whether it has any, for a semi or an anti join - and
	 * whether the fact filters are collected once for all of them.  An inner
	 * join that reads the filters per count prints neither, as it always has.
	 */
	if (st->joinclause >= 0)
	{
		/*
		 * A forward semi join over a non-unique key counts the fact rows of
		 * each DISTINCT key of the dimension, which it sorts to find them.
		 */
		if (st->jointype == LION_JOIN_SEMI || st->joinunique)
			ExplainPropertyText("Join Type", "Semi", es);
		else if (st->jointype == LION_JOIN_ANTI)
			ExplainPropertyText("Join Type", "Anti", es);
		if (st->joinunique)
			ExplainPropertyText("Join Keys", "distinct, sorted", es);

		/*
		 * The keys are looked up a batch at a time, sorted into the fk
		 * index's order ("Lookups in key order").
		 */
		if (st->joinwalk)
			ExplainPropertyText("Join Key Lookups", "in index order", es);
		if (st->joinrows)
			ExplainPropertyText("Join Rows",
								st->jointype == LION_JOIN_ANTI ?
								"the dimension rows without a match" :
								st->joinunique ?
								"the distinct keys with a match" :
								"the dimension rows with a match", es);
		if (st->joincollect)
			ExplainPropertyText("Fact Filters", "collected once", es);
	}

	if (st->hasgroupidx && st->groupattno != 0 && st->hascoal)
	{
		/*
		 * GROUP BY coalesce(g, c) (DESIGN.md §10), printed as core prints the
		 * expression, with c deparsed like a clause value.
		 */
		List	   *context = set_deparse_context_plan(es->deparse_cxt,
													   st->css.ss.ps.plan,
													   ancestors);
		char	   *val = deparse_expression((Node *) st->coalconst, context,
											 false, false);

		initStringInfo(&buf);
		appendStringInfo(&buf, "COALESCE(%s, %s)",
						 get_attname(st->heapoid, st->groupattno, false), val);
		ExplainPropertyText("Group Key", buf.data, es);
		pfree(buf.data);
		pfree(val);
	}
	else if (st->hasgroupidx && st->groupattno != 0)
	{
		initStringInfo(&buf);
		appendStringInfoString(&buf,
							   get_attname(st->heapoid, st->groupattno, false));
		if (st->groupattno2 != 0)
			appendStringInfo(&buf, ", %s",
							 get_attname(st->heapoid, st->groupattno2, false));
		ExplainPropertyText("Group Key", buf.data, es);
		pfree(buf.data);
	}

	/* The column count(DISTINCT) counts (DESIGN.md §26). */
	if (st->distattno != 0)
		ExplainPropertyText("Distinct Key",
							get_attname(st->heapoid, st->distattno, false), es);

	if (es->analyze)
	{
		/*
		 * The leader's own counters, plus - for a parallel FK-side join
		 * (DESIGN.md §27, "Parallel") - what its workers added up
		 * (lion_shutdown_custom_scan()).
		 */
		LionCountStats tot = st->stats;

		lion_count_stats_add(&tot, &st->joinworkerstats);

		ExplainPropertyInteger("Heap Blocks Skipped via VM", NULL,
							   tot.blocks_skipped_via_vm, es);
		ExplainPropertyInteger("Heap TIDs Rechecked", NULL,
							   tot.tids_rechecked, es);
		ExplainPropertyInteger("Heap Blocks Rechecked", NULL,
							   tot.blocks_rechecked, es);

		/*
		 * Rows the heap recheck of a multi-key query turned away (DESIGN.md
		 * §17, "A query known only at run time"): candidates of a superset
		 * that the query, tested on the row, does not select.  Printed only
		 * when there were some, as core prints "Rows Removed by Index
		 * Recheck".
		 */
		if (tot.rows_removed > 0)
			ExplainPropertyInteger("Rows Removed by Recheck", NULL,
								   tot.rows_removed, es);
		ExplainPropertyInteger("Containers Visited", NULL,
							   tot.containers_visited, es);
		/*
		 * Probes the AND merge did not make because the container key was
		 * already ruled out (DESIGN.md §25).  Each one is a seek into another
		 * source - a descent, or a step or two right - that the merge used to
		 * make before it knew whether anything survived at that key.
		 */
		ExplainPropertyInteger("Probes Avoided", NULL,
							   tot.probes_avoided, es);
		ExplainPropertyInteger("Heap Blocks From Cache", NULL,
							   tot.cache_hits, es);
		ExplainPropertyInteger("Heap Blocks Past Cache Budget", NULL,
							   tot.cache_full, es);
		/*
		 * Posting sets counted on their own and added up instead of merged:
		 * the disjoint-sum short-circuit of DESIGN.md §15.  Zero means every
		 * container key went through the k-way union, which is what an IN list
		 * ANDed with another clause, and every multi-key clause, still do.
		 */
		ExplainPropertyInteger("Posting Sets Summed", NULL,
							   tot.sets_summed, es);
		/*
		 * Directory pages - leaves and internal pages both - this node read
		 * (DESIGN.md §21).  A sorted IN list should cost about the leaves its
		 * values live on plus one descent, not a descent per value.
		 */
		ExplainPropertyInteger("Directory Pages Read", NULL,
							   st->dirpages + st->joinworkerdirpages, es);

		/*
		 * How the range bounding a sum was evaluated (DESIGN.md §28, "The
		 * complement"): from the entries it selects ("inside"), from the ones
		 * it does not ("complement"), or from none because it selects them all
		 * ("full domain").  A partitioned table, or a rescan, may take more
		 * than one way; each is then printed with how often it was taken.
		 */
		if (st->hasrange && st->sumall)
		{
			static const char *const evalname[] = {"inside", "complement",
			"full domain"};
			int			kinds = 0;
			int			k;

			initStringInfo(&buf);
			for (k = 0; k < (int) lengthof(evalname); k++)
				if (st->rangeeval[k] > 0)
					kinds++;
			for (k = 0; k < (int) lengthof(evalname); k++)
			{
				if (st->rangeeval[k] == 0)
					continue;
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, evalname[k]);
				if (kinds > 1)
					appendStringInfo(&buf, " %lld",
									 (long long) st->rangeeval[k]);
			}
			if (buf.len > 0)
				ExplainPropertyText("Range Evaluation", buf.data, es);
			pfree(buf.data);
		}

		/*
		 * The summaries (DESIGN.md §32) a sum added up in place of the keys
		 * they cover, among the Posting Sets Summed.  Only when there were
		 * any, so that an index without summaries prints what it always did.
		 */
		if (st->summaries > 0)
			ExplainPropertyInteger("Summaries Summed", NULL, st->summaries, es);

		/*
		 * Of those sums, the walks that were probed at the other sources'
		 * rows rather than counted a set at a time (DESIGN.md §32, "Summed
		 * ranges: dense and probed").  Only when there were any.
		 */
		if (st->rangeprobed > 0)
			ExplainPropertyInteger("Range Walks Probed", NULL, st->rangeprobed,
								   es);

		/*
		 * The ranges taken as sources (DESIGN.md §32): collected into memory,
		 * or - too large for it - summed over their walk at every count they
		 * are part of.  Again only when there were any.
		 */
		if (st->rangesrc_collected > 0)
			ExplainPropertyInteger("Range Sources Collected", NULL,
								   st->rangesrc_collected, es);
		if (st->rangesrc_walked > 0)
			ExplainPropertyInteger("Range Sources Walked", NULL,
								   st->rangesrc_walked, es);
		/* ... and of the collected, those an OR's leaf spilled to a file */
		if (st->rangesrc_spilled > 0)
			ExplainPropertyInteger("Range Sources Spilled", NULL,
								   st->rangesrc_spilled, es);

		/*
		 * The WHERE items of a GROUP BY collected into one set that the groups
		 * were counted against (DESIGN.md §10, "The WHERE sets, collected
		 * once"): once per relation - per partition, and again on a rescan -
		 * that did; and of those, the ones past a hash table's memory that
		 * went to a temporary file.  Only when there were any.
		 */
		if (st->wherecollected > 0)
			ExplainPropertyInteger("WHERE Sets Collected", NULL,
								   st->wherecollected, es);
		if (st->wherespilled > 0)
			ExplainPropertyInteger("WHERE Sets Spilled", NULL,
								   st->wherespilled, es);

		/*
		 * The batches an IN list too long to locate at once was counted in
		 * (DESIGN.md §15, "A list too long to locate at once").  Only when
		 * there were any.
		 */
		if (st->listbatches > 0)
			ExplainPropertyInteger("List Batches", NULL, st->listbatches, es);

		/*
		 * The existence (or count) tests a count(DISTINCT k) made: one per
		 * entry of k without a GROUP BY, one per group and one per (g, k) pair
		 * with one (DESIGN.md §26).
		 */
		if (st->distattno != 0)
			ExplainPropertyInteger("Distinct Keys Tested", NULL, st->disttests,
								   es);

		/*
		 * The FK-side join (DESIGN.md §27): the dimension rows whose key was
		 * looked up (a NULL key joins nothing and is not), and how many of
		 * those keys have no entry in the fk index at all.
		 */
		if (st->joinclause >= 0)
		{
			/*
			 * A forward semi join's keys: the child rows with one, sorted - by
			 * every participant of a parallel plan, so what one of them sorted
			 * in each run, summed over the runs - and how the sort went here,
			 * if it ran here.  "Looked Up" below is then the DISTINCT keys,
			 * each by one participant.
			 */
			if (st->joinunique)
			{
				ExplainPropertyInteger("Join Keys Sorted", NULL,
									   Max(st->joinsorted,
										   st->joinworkersorted), es);
				if (st->joinhavesortstats)
				{
					ExplainPropertyText("Join Key Sort Method",
										tuplesort_method_name(st->joinsortstats.sortMethod),
										es);
					ExplainPropertyInteger("Join Key Sort Space Used", "kB",
										   st->joinsortstats.spaceUsed, es);
					ExplainPropertyText("Join Key Sort Space Type",
										tuplesort_space_type_name(st->joinsortstats.spaceType),
										es);
				}
			}

			/*
			 * Where a key's time goes (DESIGN.md §27): the rows the child
			 * returned - the keys looked up, and the NULL keys, which join
			 * nothing; every participant's, so for a forward semi join over a
			 * non-unique key, whose participants each run the whole child,
			 * that many times the dimension's rows - ...
			 */
			ExplainPropertyInteger("Join Child Rows", NULL,
								   st->joinchildrows + st->joinworkerchildrows,
								   es);
			ExplainPropertyInteger("Join Keys Looked Up", NULL,
								   st->joinlookups + st->joinworkerlookups, es);
			ExplainPropertyInteger("Join Keys Without Entry", NULL,
								   st->joinmissing + st->joinworkermissing, es);

			/*
			 * ... what their counts read of the keys' own fk sets - their
			 * containers, and the pages of posting trees, which a set of a
			 * few rows stored INLINE in its directory leaf has none of - and
			 * of the visibility map: the containers it was asked about and the
			 * map pages pinned for them.
			 */
			ExplainPropertyInteger("Join Key Containers Read", NULL,
								   tot.key_containers, es);
			ExplainPropertyInteger("Join Posting Pages Read", NULL,
								   st->joinposting + st->joinworkerposting, es);
			ExplainPropertyInteger("Visibility Map Checks", NULL,
								   tot.vm_checks, es);
			ExplainPropertyInteger("Visibility Map Pages Pinned", NULL,
								   tot.vm_pins, es);

			/*
			 * The batches the keys were looked up in, in the fk index's order:
			 * one per work_mem of the child's rows, per participant and run.
			 */
			if (st->joinwalk)
				ExplainPropertyInteger("Join Key Batches", NULL,
									   st->joinbatches + st->joinworkerbatches,
									   es);

			/*
			 * The rows of the collected fact filters, or -1 when the plan
			 * collected them and the run could not (a copy over the memory
			 * limit, a standby) and every count read the filters instead - in
			 * a parallel plan, the largest copy any participant made.
			 */
			if (st->joincollect)
				ExplainPropertyInteger("Fact Filter Rows Collected", NULL,
									   Max(st->joinfilterrows,
										   st->joinworkerfilterrows), es);

			/*
			 * Copies past a hash join's memory, which went to a temporary
			 * file instead: one per participant and run.  Only when there
			 * were any.
			 */
			if (st->joinspilled + st->joinworkerspilled > 0)
				ExplainPropertyInteger("Fact Filter Copies Spilled", NULL,
									   st->joinspilled + st->joinworkerspilled,
									   es);

			/*
			 * What the counts read of the copy: its containers, the binary
			 * searches that found them, and of those the ones read back from
			 * a spilled copy's temporary file - one read each, never the
			 * copy again.
			 */
			if (st->joincollect)
			{
				ExplainPropertyInteger("Fact Filter Copy Containers Read", NULL,
									   tot.copy_containers, es);
				ExplainPropertyInteger("Fact Filter Copy Seeks", NULL,
									   tot.copy_seeks, es);
				ExplainPropertyInteger("Fact Filter Copy File Reads", NULL,
									   tot.copy_file_reads, es);
			}

			/*
			 * With TIMING, the time spent in each phase, summed over the
			 * participants: the child producing its rows, the lookups, the
			 * counts, and the collection of the fact filters.  What the node
			 * took besides - its batches, its rows handed up - is its own
			 * total less these.
			 */
			if (es->timing)
			{
				static const char *const phasename[LION_JT_N] = {
					"Join Child Time", "Join Lookup Time", "Join Count Time",
					"Fact Filter Collect Time"
				};

				for (i = 0; i < LION_JT_N; i++)
				{
					instr_time	t = st->jointime[i];

					if (i == LION_JT_COLLECT && !st->joincollect)
						continue;
					INSTR_TIME_ADD(t, st->joinworkertime[i]);
					ExplainPropertyFloat(phasename[i], "ms",
										 INSTR_TIME_GET_MILLISEC(t), 3, es);
				}
			}
		}
	}
}
