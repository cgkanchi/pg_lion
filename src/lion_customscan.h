/*-------------------------------------------------------------------------
 *
 * lion_customscan.h
 *		A CustomScan that answers
 *
 *			SELECT count(*) [, k] FROM t WHERE <clause> [AND <clause> ...]
 *			[GROUP BY k]
 *
 *		out of roaring posting sets, visiting the heap only for pages the
 *		visibility map does not vouch for.  DESIGN.md section 10 is the
 *		specification; the counting itself lives in the count engine
 *		(lion_count.h) and follows
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
 * that every count reads instead of the clauses' own posting sets.  Where an
 * aggregate needs the dimension rows themselves - count(DISTINCT), min, max -
 * the node emits those rows for a core Agg to aggregate instead, carrying
 * each row's count when a count stands beside them, and a LionJoinAgg above
 * that Agg gives the query its own aggregates back.
 *
 * The same semi and anti joins are also offered as JOIN paths, by
 * set_join_pathlist_hook (DESIGN.md §27, "The semi and anti join as a join
 * path"): a CustomPath of a join rel whose join is a semi or anti join with
 * the fact alone on its inner side, over a path of its outer side - any rel
 * - which emits the outer rows with (or without) a match, for whatever needs
 * them above.  EXPLAIN calls it LionSemiJoin or LionAntiJoin; it is the same
 * node, in the rows mode of a count(DISTINCT).
 *
 * This header is private to the LionCount custom scan.  Its planner half
 * is the lion_plan_*.c files and its executor half the lion_exec_*.c files;
 * what they share - the custom_private layout, the cost constants, the
 * node's state, and the functions one of them calls in another - is
 * declared here, and nothing else includes it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_CUSTOMSCAN_H
#define LION_CUSTOMSCAN_H


#include <math.h>

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/partition.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/extension.h"
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
#include "optimizer/appendinfo.h"
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
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
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
#include "lion_costs.h"
#include "lion_count.h"
#include "lion_fkjoin.h"

/* GUCs and the previous hooks, all owned here and installed by _PG_init. */

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
/*
 * The third and later GROUP BY columns of the decoded walk (DESIGN.md §34):
 * LION_TL_GROUPKEYN(g) is the key of plan column g, 2 <= g <
 * LION_MAX_GROUPCOLS, and LION_TL_COUNT_GROUPCOLN(g) is count() of it - 0 in
 * its NULL group - as LION_TL_GROUPKEY2 and LION_TL_COUNT_GROUPCOL2 are of
 * column 1.
 */
#define LION_TL_GROUPKEYN(g)		(8 + (g) - 2)
#define LION_TL_COUNT_GROUPCOLN(g)	(8 + (LION_MAX_GROUPCOLS - 2) + (g) - 2)
#define LION_TL_IS_GROUPKEYN(kind)	((kind) >= 8 && \
									 (kind) < 8 + (LION_MAX_GROUPCOLS - 2))
#define LION_TL_IS_COUNT_GROUPCOLN(kind) \
	((kind) >= 8 + (LION_MAX_GROUPCOLS - 2) && \
	 (kind) < 8 + 2 * (LION_MAX_GROUPCOLS - 2))
#define LION_TL_GROUPN_COL(kind)	((kind) < 8 + (LION_MAX_GROUPCOLS - 2) ? \
									 (kind) - 8 + 2 : \
									 (kind) - 8 - (LION_MAX_GROUPCOLS - 2) + 2)
/* LION_TL_WHEREKEY + i: the key stored in the i'th clause's entry */
#define LION_TL_WHEREKEY		(8 + 2 * (LION_MAX_GROUPCOLS - 2))

/*
 * The aggregates over the entries of lion columns (DESIGN.md §37):
 * LION_TL_WAGG(j) is the j'th of them (LionWAgg), LION_TL_WKEY(c) the key
 * of column c that their arguments are evaluated on, which no row prints.
 * Far above any clause's LION_TL_WHEREKEY.
 */
#define LION_TL_WAGG(j)				(0x10000000 + (j))
#define LION_TL_IS_WAGG(kind)		((kind) >= 0x10000000 && (kind) < 0x20000000)
#define LION_TL_WAGG_NO(kind)		((kind) - 0x10000000)
#define LION_TL_WKEY(c)				(0x20000000 + (c))
#define LION_TL_IS_WKEY(kind)		((kind) >= 0x20000000 && (kind) < 0x30000000)
#define LION_TL_WKEY_COL(kind)		((kind) - 0x20000000)

/*
 * The window store's gather (DESIGN.md §40, "The custom shapes"):
 * LION_TL_SKEY(g) is the value of the g'th GROUP BY column the node gathers,
 * LION_TL_SAGG(a) the a'th aggregate it computes from the gathered values,
 * one of the LION_SAGG_* below.  Far above the kinds of §37.
 */
#define LION_TL_SKEY(g)				(0x30000000 + (g))
#define LION_TL_IS_SKEY(kind)		((kind) >= 0x30000000 && (kind) < 0x38000000)
#define LION_TL_SKEY_NO(kind)		((kind) - 0x30000000)
#define LION_TL_SAGG(a)				(0x38000000 + (a))
#define LION_TL_IS_SAGG(kind)		((kind) >= 0x38000000 && (kind) < 0x40000000)
#define LION_TL_SAGG_NO(kind)		((kind) - 0x38000000)

/*
 * An aggregate over a gathered column x: count(x), count(DISTINCT x), and the
 * aggregates of §37 over x itself - LION_SAGG_WAGG(LION_WAGG_*).  count(*),
 * and a count(col) the group's rows answer, are LION_TL_COUNT as everywhere.
 */
#define LION_SAGG_NONE		0
#define LION_SAGG_COUNTCOL	1	/* count(x): the rows with x non-NULL */
#define LION_SAGG_DISTINCT	2	/* count(DISTINCT x) */
#define LION_SAGG_WAGG(k)	(2 + (k))	/* sum, avg, min, max of x */
#define LION_SAGG_IS_WAGG(s)	((s) > 2 && (s) <= 2 + LION_WAGG_EXTREME)
#define LION_SAGG_WAGG_KIND(s)	((s) - 2)

/* ... and what each one computes */
#define LION_WAGG_NONE		0
#define LION_WAGG_SUM		1	/* sum(int2), sum(int4): an int8 */
#define LION_WAGG_SUM8		2	/* sum(int8): a numeric */
#define LION_WAGG_AVG		3	/* avg(int2), avg(int4): a numeric */
#define LION_WAGG_AVG8		4	/* avg(int8): a numeric */
#define LION_WAGG_EXTREME	5	/* min, max, bool_and, bool_or: the first
								 * value in the aggregate's sort order */

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
#define LION_JOINFLAG_COUNTS	0x20	/* with ROWS, an inner join's: each
										 * row carries its count of join
										 * pairs, which the Agg above adds up
										 * in place of count() ("Every
										 * aggregate over the node's rows") */
#define LION_JOINFLAG_OUTER		0x40	/* with ROWS, a semi or anti join's:
										 * the node is a JOIN path, and its
										 * rows are the join rel's - the outer
										 * side's rows with a match, or with
										 * none ("The semi and anti join as a
										 * join path") */
#define LION_JOINFLAG_ORDERED	0x80	/* with OUTER: the rows go up in the
										 * order the child gave them, a batch
										 * counted whole in key order and its
										 * rows then handed up in their own,
										 * and the path claims the child's
										 * pathkeys */

/*
 * Where an FK-side join's time goes (DESIGN.md §27, "Where a key's time
 * goes"): the phases EXPLAIN ANALYZE times when its TIMING option is on, each
 * summed over the participants of a parallel plan.
 */
#define LION_JT_CHILD		0	/* the child plan, producing its rows */
#define LION_JT_LOOKUP		1	/* locating each key's fk set */
#define LION_JT_COUNT		2	/* counting it, or testing it, and letting it go */
#define LION_JT_LOCATE		3	/* locating the fact filters - a range among
								 * them collected (§32) - once a run, or once
								 * a leaf's turn */
#define LION_JT_COLLECT		4	/* collecting the fact filters, once a run */
#define LION_JT_N			5

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
 * THE PRICES.  Every LION_*_COST below is a multiple of cpu_operator_cost or
 * cpu_tuple_cost (LION_FKJOIN_SORT_PAGE_COST, of seq_page_cost and
 * random_page_cost), and the multiplier is a planner setting (lion_costs.c;
 * DESIGN.md §31, "The settings"): LION_CONTAINER_COST is
 * (lion_container_cost * cpu_operator_cost), and lion_container_cost is
 * pg_lion.container_cost.  Each comment is the measurement or the derivation
 * behind its setting's default.
 */

/*
 * The fixed cost of one count of the FK-side join (DESIGN.md §27) - one per
 * dimension row - over and above the containers it reads and the lookup that
 * locates its set: a merge set up and torn down.  Derived from what the count
 * does (DESIGN.md §27, "The cost of a key, after the per-key path"): once it
 * works in the node's scratch memory and keeps the node's map page pinned,
 * what is left is some twenty allocations from memory already there, two
 * cursors built and the plan's decisions, about half a microsecond - this,
 * 25, until the refit of 2026-09-29 (DESIGN.md §27, "The per-key terms,
 * refitted") fitted the probing counts of 61 serial joins against their
 * counters at 271 ns a count besides the containers it reads and the pages
 * it descends: 14.  §10 and §26 measured 1.0 us a count or test that also
 * made and deleted a memory context, pinned a map page and, when it merged
 * two sources, malloc'd and freed two blocks every time: what the per-key
 * fixes took off.
 */
#define LION_FKJOIN_COUNT_COST	(lion_fkjoin_count_cost * cpu_tuple_cost)

/*
 * ... and each row the node hands up: a partial count per dimension row that
 * joins, or the row itself for count(DISTINCT) - the per-tuple context reset,
 * a virtual tuple, the projection, the return through the executor, and the
 * pause that lets go of its index pins (the leaf a walk in key order then
 * finds again for the next key is in that key's lookup, which
 * lion_cost_fkjoin_walk() charges).  About 0.2 us from the operations (the
 * same derivation), where one cpu_tuple_cost used to be charged: 10.  The
 * refit of 2026-09-29 found 340 to 630 ns a row handed up, serial, and 200 to
 * 570 in two workers, fitted beside the keys' lookups and batches: 20.  A
 * summed join (LION_JOINFLAG_SUM) hands up one row per participant and run.  What
 * core does with the rows above the node - its Finalize Agg's transitions, a
 * Gather's tuple queue - core charges.
 */
#define LION_FKJOIN_ROW_COST	(lion_fkjoin_row_cost * cpu_tuple_cost)

/*
 * ... and one PROBE of such a count into a fact filter's set: a seek of the
 * filter's (materialized, §9) set to one of the fk set's container keys and
 * the AND of the two containers there.  Measured on the assert build at two
 * million fact rows and a thousand dimension rows (DESIGN.md §27): the
 * thousand counts ANDed with a 10% filter read 708,160 containers in 157 ms
 * against 354,076 in 54 ms without it, about 0.3 us per probe - 80, which
 * had the posting pages the seeks read in it.  Refitted on the release build
 * with the pages apart (LION_FKJOIN_PROBE_PAGE_COST, below): 51 ns a filter
 * container sought, besides the fk container's own LION_CONTAINER_COST - 10.
 */
#define LION_FKJOIN_PROBE_COST	(lion_fkjoin_probe_cost * cpu_operator_cost)

/*
 * ... and each posting page such a probe reads.  A filter set too large to
 * be copied into memory for the counts (LION_MATERIALIZE_MAX_*) is read from
 * its posting tree at every count, and from the second row on it is a NOPIN
 * set whose cursor starts at the tree's root: the first probe of a count
 * descends it, and a probe after it steps right while the keys it is sought
 * to are close (lion_fkjoin_probe_one()).
 *
 * Refitted on the release build (DESIGN.md §27, "The per-key terms,
 * refitted", 2026-09-29) from the probing counts of 61 serial joins - fact
 * filters of one and three sets, 100 to 500,000 keys of 1 to 1,000 rows -
 * against their counters: 270 ns a posting page (54 cpu_operator_cost).  A
 * set small enough to be copied is probed in memory at LION_MEMORY_PROBE_COST
 * instead.
 */
#define LION_FKJOIN_PROBE_PAGE_COST	(lion_fkjoin_probe_page_cost * cpu_operator_cost)

/*
 * ... and, per count, each SET of a fact filter that is a union (an IN list,
 * an OR across columns, a multi-key query of several keys): its cursor set
 * up and positioned whether or not the fk set's keys find anything in it.
 * It was LION_UNION_SET_COST - measured on the assert build at 4 to 6 us a
 * set a count, when every count built the union again at every key - until
 * the refit of 2026-09-29, which found 217 ns (11 cpu_tuple_cost): since
 * unions are probed (§29.11) a count builds none for a key's few rows, and
 * what the union costs besides is its sets' probes, which are priced as
 * probes.  A GROUP BY's counts keep LION_UNION_SET_COST.
 */
#define LION_FKJOIN_SET_COST	(lion_fkjoin_set_cost * cpu_tuple_cost)

/*
 * ... and, when the fact filters are COLLECTED once into a private copy
 * (lion_sources_collect(), DESIGN.md §27 "The fact filters, collected once"):
 *
 *	COPY_COUNT		one count of an fk set against the copy - the merge of two
 *					sources set up and torn down - in place of COUNT_COST;
 *	COPY_PROBE		per fk container, the copy looked up at its key in memory
 *					and the two containers ANDed, in place of a probe into
 *					each filter's posting tree;
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
 *
 * COPY_PROBE was 15 while the lookup was a binary search of the copy that
 * followed a pointer to a container at every step: 0.115 us an fk container
 * of one member on the release build.  Since the copy is looked up directly
 * by its keys and a container of a member or two is ANDed by looking its
 * members up (DESIGN.md §27, "The copy, looked up by key") the same takes
 * 25 to 30 ns, five cpu_operator_cost.  COPY_MEMBER is unchanged: the AND of
 * a larger fk container still merges it with the copy's, and at six members a
 * container the node is still slower than the hash join it is priced above.
 *
 * Refitted on the release build (DESIGN.md §27, "The per-key terms,
 * refitted", 2026-09-29) from the counts against the copy of 61 serial
 * joins, 100 to 500,000 keys of 1 to 1,000 rows: 659 ns a count besides its
 * containers - COPY_COUNT 33 where it was 25 - 52 ns an fk container, of which
 * LION_CONTAINER_COST, charged to every count's fk containers, is 40 -
 * COPY_PROBE 2 - and 14 ns a member past a container's first (COPY_MEMBER's
 * 15, unchanged).
 */
#define LION_FKJOIN_COPY_COUNT_COST	(lion_fkjoin_copy_count_cost * cpu_tuple_cost)
#define LION_FKJOIN_COPY_PROBE_COST	(lion_fkjoin_copy_probe_cost * cpu_operator_cost)
#define LION_FKJOIN_COPY_MEMBER_COST	(lion_fkjoin_copy_member_cost * cpu_operator_cost)
#define LION_FKJOIN_COPY_CONTAINER_COST	(lion_fkjoin_copy_container_cost * cpu_operator_cost)

/*
 * ... and, making the copy, each container of the DRIVER of the filters'
 * merge - the sparsest filter, read end to end while every other is probed at
 * its container keys (LION_FKJOIN_PROBE_COST a probe) - read and ANDed.  No
 * count is made and the visibility map is not asked, which is most of what
 * LION_CONTAINER_COST is a price of; this is two cpu_operator_cost, §10's
 * price of a container before the release-build fit, which "Cost, revisited"
 * (DESIGN.md §27) left as it was: the collected shapes came out at 374 to 595
 * units a millisecond with it.
 */
#define LION_FKJOIN_COLLECT_CONTAINER_COST	(lion_fkjoin_collect_container_cost * cpu_operator_cost)

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
 * once each, as core's cost_tuplesort() charges one merge pass:
 *
 *	SORT_PAGE		each of those page accesses, three quarters of them in
 *					sequence and a quarter at random - cost_tuplesort()'s own
 *					mix, which is not measured here.
 */
#define LION_FKJOIN_SORT_COMPARE_COST	(lion_fkjoin_sort_compare_cost * cpu_operator_cost)
#define LION_FKJOIN_SORT_KEY_COST	(lion_fkjoin_sort_key_cost * cpu_operator_cost)
#define LION_FKJOIN_SORT_PAGE_COST \
	(lion_fkjoin_sort_seq_page_cost * seq_page_cost + \
	 lion_fkjoin_sort_random_page_cost * random_page_cost)

/*
 * Looking the child's rows up in the fk index's key order (DESIGN.md §27,
 * "Lookups in key order"), which the node does a batch at a time:
 *
 *	BATCH_ROW		per row, its place in the batch - the row copied in, its
 *					share of the sort (log2 of the batch comparisons through the
 *					opclass's comparison function) and the slot it is handed
 *					back to the target list in.  It was priced as a directory
 *					page visit, 120 - on purpose high, beside a page visit a
 *					row the walk was charged as well, so that a walk was never
 *					taken over a directory of height 1.  Refitted with the
 *					key's lookup on its leaf (LION_FKJOIN_LOOKUP_COST): a walked
 *					key takes 740 ns besides the leaves it steps over, which
 *					75 and the lookup's 75 are (DESIGN.md §27, "The per-key
 *					terms, refitted"), and a descent of a directory of height
 *					1 - its root and its leaf - takes 860 ns and the lookup's.
 *	BATCH_ENT_BYTES	per row, what a batch holds besides the row's own columns:
 *					the entry that sorts it, the MinimalTuple's header and the
 *					allocator's chunk headers of the row and of a key copied
 *					by reference.
 */
#define LION_FKJOIN_BATCH_ROW_COST	(lion_fkjoin_batch_row_cost * cpu_operator_cost)
#define LION_FKJOIN_BATCH_ENT_BYTES	(sizeof(LionJoinEnt) + 48)

/* The most rows a batch takes whatever work_mem allows: its array's limit. */
#define LION_JOIN_BATCH_MAX		((int) (MaxAllocSize / sizeof(LionJoinEnt) - 1))

/*
 * ... and each key a walk looks up on the leaf it stands on: the leaf's
 * binary search, the entry decoded and its set located - its INLINE payload
 * copied - which a descent's leaf level has in its price already.
 *
 * Refitted on the release build (DESIGN.md §27, "The per-key terms,
 * refitted", 2026-09-29) from the lookups of 122 serial joins, walked and
 * descended, of plain and partitioned facts: 377 ns a key looked up (75
 * cpu_operator_cost) and 430 ns a directory page visited, which is
 * LION_DESCENT_COST's to price.  Fitted with the time the batches take, a
 * walked key came to 740 ns besides the leaves it steps over - its lookup
 * and its place in its batch, LION_FKJOIN_BATCH_ROW_COST, 75 where it was
 * 120.  A walk used to be charged a page visit a row as well as the leaves
 * it steps over, where a key on the leaf the walk stands on reads no page
 * (the walks' counters show 0.02 to 0.3 directory pages a key where their
 * keys are dense): a partitioned fact's lookups, a batch sorted once and
 * walked in every leaf, were priced at two to three times their time.
 */
#define LION_FKJOIN_LOOKUP_COST	(lion_fkjoin_lookup_cost * cpu_operator_cost)

/*
 * How many GROUP BY columns the node understands.  One is driven by that
 * index's entry scan; two are the nested loop of lion_next_group2() (DESIGN.md
 * §20), whose cost is the product of the two entry counts; three and more are
 * the decoded walk of DESIGN.md §34, which reads every column's sets once, key
 * by key, whatever the number of their combinations.
 */
#define LION_MAX_GROUPCOLS	8
StaticAssertDecl(LION_MAX_GROUPCOLS <= LION_MAX_DECODE_COLS,
				 "the decoded walk takes every GROUP BY column the node does");

/*
 * Kinds of WHERE clause the pushdown understands.  EQ, ARRAY and NULL select
 * rows (they are positive sources of the count); NOTNULL removes them
 * (DESIGN.md §14: the NULL rows are exactly the members of the index's
 * reserved NULL entry, so `IS NOT NULL` is their complement), and so does NE
 * (§35): `col <> c` is every row but those of c's entry and the NULL one, the
 * union of the two subtracted.
 */
#define LION_CLAUSE_EQ		0	/* col = const */
#define LION_CLAUSE_ARRAY	1	/* col = ANY (const array), DESIGN.md §15 */
#define LION_CLAUSE_NULL		2	/* col IS NULL */
#define LION_CLAUSE_NOTNULL	3	/* col IS NOT NULL */
#define LION_CLAUSE_MULTI	4	/* col @> / && / @@ const, DESIGN.md §17 */
#define LION_CLAUSE_RANGE	5	/* col < / <= / >= / > const, DESIGN.md §28 */
#define LION_CLAUSE_RANGESRC	6	/* the same on a column that does not drive
									 * the count: a source, DESIGN.md §32 */
#define LION_CLAUSE_NE		7	/* col <> const, DESIGN.md §35 */

/*
 * A RANGE clause is neither: it is not a source of the count at all, but a
 * bound on the entry walk that drives it (DESIGN.md §28), so it is never
 * located, never merged and never priced as a lookup.  A RANGESRC clause is a
 * bound of a range on ANOTHER column, and the range is a positive source: the
 * rows whose key lies in it (DESIGN.md §32, "A range as a source").
 */
#define LION_CLAUSE_IS_POSITIVE(k) \
	((k) != LION_CLAUSE_NOTNULL && (k) != LION_CLAUSE_RANGE && \
	 (k) != LION_CLAUSE_NE)

/* ... and which are subtracted: a negated source (DESIGN.md §14, §35) */
#define LION_CLAUSE_IS_NEGATED(k) \
	((k) == LION_CLAUSE_NOTNULL || (k) == LION_CLAUSE_NE)

/*
 * Which clause kinds pin their column to ONE value, so that a target list
 * asking for that column can be answered with the key the entry stored.  A
 * multi-key clause pins nothing: `tags @> '{a}'` says what the array
 * contains, not what it is.
 */
#define LION_CLAUSE_PINS_VALUE(k) \
	((k) == LION_CLAUSE_EQ || (k) == LION_CLAUSE_NULL)

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
#define LION_DISTINCT_TEST_COST	(lion_distinct_test_cost * cpu_tuple_cost)

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
#define LION_ENTRY_COUNT_COST	(lion_entry_count_cost * cpu_tuple_cost)
#define LION_LIST_GROUP_COST	(lion_list_group_cost * cpu_tuple_cost)

/*
 * ... and each set of a WHERE source that is a union (an IN list, an OR
 * across columns), which every count of a GROUP BY builds again: its cursor
 * set up and positioned over the set's copy, and its merge (DESIGN.md §15,
 * §19).  Measured on the release build: 2 us a set for 4 groups ANDed with a
 * list of 1,000 values of 100 rows each, and 2.7 and 6 us a set, a few us of
 * which are the union's own set-up, for 200 groups of 250 rows ANDed with
 * `a = 17 OR b = 3` and with `b IN (3, 4)`.
 */
#define LION_UNION_SET_COST		(lion_union_set_cost * cpu_tuple_cost)

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
#define LION_RANGE_ENTRY_COST	(lion_range_entry_cost * cpu_tuple_cost)

/*
 * ... and of one SMALL entry of a summed range, counted with the rest of its
 * leaf as one union (DESIGN.md §28, "Counting a walk"): the leaf is read once
 * for all of them, and what is left per entry is its copy, its cursor and its
 * share of the union.  Measured on the assert build over the 2M-row repro
 * table: 0.3 to 0.5 us an entry of one row, against 1.5 before.
 */
#define LION_RANGE_UNION_ENTRY_COST	(lion_range_union_entry_cost * cpu_tuple_cost)

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
#define LION_PROBE_STEP_COST	(lion_probe_step_cost * cpu_operator_cost)

/*
 * ... and when a walk turns to probing: once it has handed out this many
 * times the rows of the other sources' smallest positive one
 * (lion_sum_probe_due()), which the cost model predicts the same way.
 */
#define LION_PROBE_SWITCH	2.0

/*
 * One FOLD of a range collected as a source (DESIGN.md §32, "What collecting
 * a range costs"): a container of the union that is not a bitset yet taking
 * what came for it - the members waiting for it through a bitset image
 * (lion_container_add_many()), or a larger container merged in at once
 * (lion_container_or()) - and put back at its new size, optimized.  An image
 * filled, counted and emptied each time, a few microseconds whatever came.
 * Fitted on a packaged (release) build to the time 62 collections took -
 * ranges of a few days to all of a four-year timestamp, in heap order and at
 * random, over 1M, 5M and 20M rows, with buckets of 1,024, 4,096 and 16,384
 * rows and without summaries, 0.9 ms to 4.1 s - with the folds each made
 * counted, beside the walk's own price, each container the walk ORs into the
 * union at LION_CONTAINER_COST and each member an ARRAY merge moves at
 * LION_AND_MEMBER_COST: 2.1 us at 500 units a millisecond, 57 of the 62
 * within a factor of 1.5.  The others are columns in heap order folded key
 * by key into a RUN of up to a run a heap block, which takes up to 7 us -
 * which a union no longer is: it is widened into a bitset instead (below).
 */
#define LION_RANGE_FOLD_COST	(lion_range_fold_cost * cpu_operator_cost)

/*
 * The decoded walk of DESIGN.md §34, per unit of its work, fitted to a
 * release build (PostgreSQL 16 as packaged, 2026-09-30) over 5M rows of
 * 120 a page, all-visible, three to five columns of 2 to 100 values, with
 * and without a WHERE of 30% - 62 to 329 ms, at 500 units a millisecond:
 *
 *	- LION_DECODE_CONTAINER_COST: a container of a column's value at a key,
 *	  its cursor stepped to it and its members listed (0.5 us);
 *	- LION_DECODE_MEMBER_COST: a member stored into its key's array (1 ns);
 *	- LION_DECODE_ROW_COST: a row the WHERE keeps counted under its
 *	  combination - the arrays of the columns after the first read, a cell of
 *	  the tally added to (3 ns, and LION_DECODE_ROW_COL_COST, 2 ns, a column
 *	  after the first) - and LION_DECODE_ROW_MISS_COST more once the tally's
 *	  array is past what a core's cache holds (LION_DECODE_CACHE_BYTES);
 *	- LION_DECODE_HASH_ROW_COST: the same row into a hash table of the
 *	  combinations, when an array of them all would not fit (50 ns: 5M rows
 *	  into 100,000 combinations, 250 ms).
 */
#define LION_DECODE_CONTAINER_COST	(100.0 * cpu_operator_cost)
#define LION_DECODE_MEMBER_COST		(0.2 * cpu_operator_cost)
#define LION_DECODE_ROW_COST		(0.6 * cpu_operator_cost)
#define LION_DECODE_ROW_COL_COST	(0.4 * cpu_operator_cost)
#define LION_DECODE_ROW_MISS_COST	(1.6 * cpu_operator_cost)
#define LION_DECODE_HASH_ROW_COST	(10.0 * cpu_operator_cost)
#define LION_DECODE_CACHE_BYTES		(512.0 * 1024.0)

/*
 * The folds a key's union that is WIDENED into a bitset costs (DESIGN.md §32,
 * "The collected union, dense"): the fold that made it the RUN it is - the
 * members of a key's first containers set aside and folded in, when they
 * came one or two rows at a time - and its optimization back when the walk
 * is over, which takes what a fold does, most of a fold being its
 * optimization; the widening between them is an image filled and copied, a
 * tenth of a fold or less.  Every container that comes to it in between is
 * ORed in place, a few nanoseconds.  Counted over 2M rows in heap order, a
 * key of one row and of ten, without summaries and with them, ranges of
 * 1,000 to 1.9 million keys: one fold or two a union key.  Timed apart
 * (-O2), a widening took 0.1 to 0.4 us and a fold 1.4 to 8.
 */
#define LION_RANGE_WIDEN_FOLDS	2.0

/*
 * What a collected range's union takes in memory beyond its containers' own
 * bytes (lion_cost_range_source()): each container's entry in the hash table
 * and its chunk's header - lion_range_union_cb() counts
 * GetMemoryChunkSpace() and LION_RANGE_UNION_OVERHEAD - and what a container
 * of rows stored in the heap's order takes: a RUN, a run for each heap block.
 */
#define LION_RANGE_UNION_ENTRY_BYTES	64.0
#define LION_RANGE_RUN_BYTES	((double) LION_CONTAINER_HDRSZ + \
								 LION_BLOCKS_PER_CONTAINER * 2 * sizeof(uint16))

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
#define LION_CONTAINER_COST		(lion_container_cost * cpu_operator_cost)
#define LION_MEMBER_COST		(lion_member_cost * cpu_operator_cost)
#define LION_MEMBER_CAP			1024.0
#define LION_PROBE_COST			(lion_probe_cost * cpu_operator_cost)
#define LION_MEMORY_PROBE_COST	(lion_memory_probe_cost * cpu_operator_cost)
#define LION_AND_MEMBER_COST	(lion_and_member_cost * cpu_operator_cost)

/*
 * THE UNION OF A LIST'S SETS AT ONE KEY (DESIGN.md §29.11, "Unions probed"),
 * where the leapfrog builds it (lion_or_hot_raw(), lion_merge_cpu_cost_sets()):
 * a bitset image cleared, its members set in it and counted, UNION_KEY a
 * key and UNION_MEMBER a member.  Measured on the release build of
 * PostgreSQL 18: the image's clear and count 85 ns with its AVX-512 popcount
 * (670 on 16's), a member 1.0 to 1.5 ns; fitted over lists of 2 to 32
 * values at 150 ns and 1.25 ns.
 */
#define LION_UNION_KEY_COST		(lion_union_key_cost * cpu_operator_cost)
#define LION_UNION_MEMBER_COST	(lion_union_member_cost * cpu_operator_cost)

/*
 * One level of an entry directory descended - a page pinned and locked, a
 * binary search, the entry decoded - for a count's lookup of a key, an IN
 * list's of each of its values, and the FK-side join's of each dimension row's
 * key (DESIGN.md §21, §27).  Fitted on the release build to the FK-side join's
 * 23 shapes (DESIGN.md §27, "Cost, revisited"): 0.6 us a directory page read,
 * median residual 9%.  The page itself is charged separately, as I/O.
 */
#define LION_DESCENT_COST		(lion_descent_cost * cpu_operator_cost)

/*
 * A page of a lion index that a custom path reads while the index is
 * resident (lion_index_page_cost(), DESIGN.md §22, §39): a buffer hit,
 * pinned, locked and stepped through.  Measured on the release build (§10,
 * "The reference"): a warm page read in order 0.6 us, against the 2 us that
 * seq_page_cost stands for at 500 units a millisecond; the posting leaves a
 * probe's seeks cross fit at 0.7 us (LION_PROBE_COST's fit); a directory
 * page descended 0.6 us with its search (LION_DESCENT_COST).  120
 * cpu_operator_cost is the 0.6 us.
 */
#define LION_RESIDENT_PAGE_COST	(lion_resident_page_cost * cpu_operator_cost)

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
#define LION_RECHECK_TID_COST	(lion_recheck_tid_cost * cpu_tuple_cost)
#define LION_RECHECK_GROUP_TID_COST	(lion_recheck_group_tid_cost * cpu_tuple_cost)

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
#define LION_FLAG_DECODE			0x10	/* the GROUP BY columns are counted by
										 * the decoded walk (DESIGN.md §34),
										 * named in LION_PRIV_GROUPN */

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
 *		(InvalidOid if none), then one index Oid per WHERE clause - or
 *		InvalidOid for a clause the partition's bounds imply, which it leaves
 *		out (§16, "Clauses the partition bounds imply")
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
 *	13	List of String: the WHERE clauses no posting set answers that every
 *		partition's bounds imply, left out of the plan altogether (DESIGN.md
 *		§16, "Clauses the partition bounds imply"), as EXPLAIN prints them -
 *		deparsed when the plan is made, since it carries no expression of
 *		them to deparse later
 *	14	List: the FK-side join's GROUP BY of a fact column (DESIGN.md §27,
 *		"Grouped by a fact column"), or empty: an IntList of the column's
 *		attnum; an OidList, one per relation counted - the table, or each
 *		live leaf partition in member 5's order - of the lion index whose
 *		entries are its groups there, or InvalidOid where the partition's
 *		bounds give the column one value; and a List of Const, one per
 *		relation, that value (a NULL Const for the NULL group) where the
 *		index Oid is InvalidOid, and read by nothing where it is not
 *	15	List, or empty: the GROUP BY columns of the decoded walk (DESIGN.md
 *		§34), LION_FLAG_DECODE: an IntList of their attnums, in the order the
 *		walk takes them (the first is the one member 2 names, and the second
 *		slot there is 0: nothing is the inner side of a nested loop), and
 *		an OidList of the lion index each is read from
 *	16	IntList: the column a sum over EVERY row drives by, or empty
 *		(DESIGN.md §35, "Every row"): with LION_FLAG_SUMALL and nothing in
 *		the WHERE that selects rows - `count(*)` alone, or beside `IS NOT
 *		NULL` and `<>` clauses only - the planner picks the lion-indexed
 *		column with the fewest entries, whose entries, the NULL one included,
 *		are every row of the table; attnum the PARENT's.  Empty for a sum
 *		driven by a range, or by the first `IS NOT NULL`, which name their
 *		column themselves
 *	17	IntList: the top k of a GROUP BY ordered by its count (DESIGN.md
 *		§36), or empty: k - the query's LIMIT and OFFSET added up - then how
 *		many entries the walk of the entries' own counts keeps as candidates,
 *		then 1 when the groups tied with the k-th must all come out (a second
 *		ORDER BY key, or WITH TIES) and 0 when any of them will do.  The node
 *		then emits only groups that can be among the first k, which the Sort
 *		and Limit above it put in order and cut
 *	18	List, or empty: the aggregates over the entries of lion columns,
 *		weighted by their counts (DESIGN.md §37): an IntList of the columns'
 *		attnums, an OidList of the lion index each is read from and an IntList
 *		of its key column there; lion_plan_custom_path() adds a List of one
 *		IntList per aggregate - its LION_WAGG_* kind, the column's position in
 *		those lists and the width of its integer argument - whose argument
 *		expressions it appends to custom_exprs after the clause values
 *	19	List, or empty: the columns the node GATHERS from the window store
 *		of one lion index (DESIGN.md §40, "The custom shapes") - their values
 *		for every row it counts, from which it computes its groups and
 *		aggregates itself: an OidList of the index; an IntList of the
 *		columns' attnums and one of their column numbers in the index; and a
 *		List of one IntList per GROUP BY column, {its position in those
 *		lists, its equality operator, its collation}.  lion_plan_custom_path()
 *		adds a List of one IntList per aggregate over a gathered column: its
 *		LION_SAGG_* kind, its column's position, the width of an integer
 *		argument, the aggregate, its input collation, and - for a
 *		count(DISTINCT) - the equality the DISTINCT compares with and its
 *		collation.  The count itself is the count(*) every other member
 *		describes: of the WHERE, or the sum over every row.  The FK-side
 *		join's path carries members 0 - 18 (lion_plan_fkjoin.c) and its plan
 *		an empty one here (lion_plan_fkjoin_path())
 *	20	IntList: LION_TL_* for each custom_scan_tlist column (added at plan
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
#define LION_PRIV_IMPLIED	13
#define LION_PRIV_FACTGROUP	14
#define LION_PRIV_GROUPN		15
#define LION_PRIV_ALLROWS	16
#define LION_PRIV_TOPK		17
#define LION_PRIV_WAGG		18
#define LION_PRIV_STORE		19
#define LION_PRIV_TLKINDS	20

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
 * Shape 15 let a partition's index Oid in member 5 be InvalidOid: a clause
 * the partition's bounds imply, which it leaves out (DESIGN.md §16), and
 * added the IMPLIED member (13), the clauses every partition leaves out, in
 * front of the target-list kinds.
 *
 * Shape 16 moved no member but gave the JOIN member (10) two flags a build
 * before it would ignore, LION_JOINFLAG_OUTER and LION_JOINFLAG_ORDERED: the
 * semi or anti join as a join path, whose rows are the outer side's (DESIGN.md
 * §27, "The semi and anti join as a join path").  Read by an older build, such
 * a plan would be taken for the rows of a count(DISTINCT).
 *
 * Shape 17 added the FACTGROUP member (14) in front of the target-list
 * kinds: an FK-side join grouped by a fact column (DESIGN.md §27, "Grouped by
 * a fact column"), whose groups an older build would have summed into one.
 *
 * Shape 18 added the GROUPN member (15) in front of the target-list kinds, and
 * the target-list kinds of the third and later GROUP BY columns, which moved
 * LION_TL_WHEREKEY: the decoded walk of DESIGN.md §34, of up to
 * LION_MAX_GROUPCOLS columns.
 *
 * Shape 19 added the ALLROWS member (16) in front of the target-list kinds,
 * and the NE clause kind of DESIGN.md §35: a sum over every row driven by a
 * column no clause names, which an older build would have looked for among
 * the clauses and not found, and a `<>` it would have counted as an
 * equality.
 *
 * Shape 20 added the TOPK member (17) in front of the target-list kinds: a
 * GROUP BY that emits only the groups that can be among the first k by
 * count (DESIGN.md §36), which an older build would have emitted whole -
 * right, but not what the plan was priced as.
 *
 * Shape 21 added the WAGG member (18) in front of the target-list kinds, and
 * the target-list kinds of its aggregates and keys: sums, averages, minima
 * and maxima over the entries of lion columns (DESIGN.md §37), which an older
 * build would have taken for counts.
 *
 * Shape 22 added the STORE member (19) in front of the target-list kinds,
 * and the target-list kinds of the gathered GROUP BY columns and aggregates
 * (DESIGN.md §40, "The custom shapes"): a count whose groups an older build
 * would not have formed at all.
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
#define LION_PRIV_MAGIC		0x52424916
#define LION_PRIV_NMEMBERS	21

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
 *
 * A partition's relations are opened when the node is initialised and stay
 * open until it ends (lion_open_parts(), lion_close_parts()), as the scans
 * under core's Append keep theirs: the relcache references are what tells a
 * TRUNCATE, DROP, REINDEX or ALTER of one of them in the same session that
 * a query is still using it (CheckTableNotInUse()).  NULL for an index the
 * partition does not have, and clauseidx[i] for a clause its bounds imply.
 */
typedef struct LionPartState
{
	Oid			heapoid;
	Oid			groupidxoid;	/* InvalidOid when no index drives the scan */
	Oid			groupidxoid2;	/* the inner one of a two-column GROUP BY */
	Oid		   *clauseidxoid;	/* one per WHERE clause */
	Oid			fgidxoid;		/* the FK-side join's fact group: the index
								 * of its groups here, or InvalidOid ... */
	Const	   *fgconst;		/* ... and then the one value the bounds give
								 * it (DESIGN.md §27, "Grouped by a fact
								 * column") */
	Relation	heap;
	Relation	groupidx;
	Relation	groupidx2;
	Relation   *clauseidx;		/* one per WHERE clause */
	Relation	fgidx;
} LionPartState;

/*
 * A dimension row's count in one group of the fact column an FK-side join
 * groups by (DESIGN.md §27, "Grouped by a fact column"), waiting to go up:
 * the batch entry it is of, the group's value, and the count.
 */
typedef struct LionJoinGroupRow
{
	int			ent;
	Datum		key;
	bool		isnull;
	int64		count;
} LionJoinGroupRow;

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
	int64		acc;			/* a partitioned fact table: the counts of the
								 * partitions visited so far, added up - or,
								 * for an existence test, whether any matched
								 * (lion_join_count_parts()) */
} LionJoinEnt;

/*
 * One leaf partition of a partitioned fact table, as the FK-side join keeps
 * it from one batch of keys to the next (DESIGN.md §27, "A partitioned fact
 * table").  The partition has a turn with each batch, at whose end everything
 * located in it is let go of (its relations stay open for the node's life,
 * LionPartState), but its fact filters are located and collected once a run:
 * `filter` is its copy of them, in `cxt`, which the later batches read - named
 * after one of the partition's indexes (filterindex), whose Relation the copy
 * is pointed at again at each turn - or, in a parallel plan whose workers
 * started, a view of the copy the participants made together (`shared`,
 * viewshared).  qmode is what the run's values made of its multi-key clauses
 * (lion_locate_multikey()), which the row filter of a later batch is built
 * from without locating them again.  missing says the filters select no row
 * of it at all.
 */
typedef struct LionJoinPart
{
	bool		collected;		/* its filters were located, and collected
								 * where the plan asks for it */
	bool		filtered;		/* ... into filter */
	bool		missing;
	bool		viewshared;
	Oid			filterindex;
	LionPostingSet filter;
	LionQueryMode *qmode;		/* one per clause */
	MemoryContext cxt;
	struct LionSharedCopy *shared;

	/*
	 * Probed, then collected (DESIGN.md §27): what probing its filters has
	 * cost so far this run, and what collecting them would - estimated at
	 * its first turn, -1 before, and 0 where no copy of them can be made or
	 * would fit - and the keys counted by probing them.
	 */
	double		rent;
	double		buy;
	int64		probed;
} LionJoinPart;

/*
 * The decoded walk of DESIGN.md §34, one run of it over the relation the node
 * counts.  Each GROUP BY column c is taken a CHUNK of its values at a time -
 * cap[c] of them, the keys read off its entry walk (escan[c]) - and one PASS
 * of the walk counts every combination of the chunks it stands at: the
 * chunks advance like the digits of an odometer, the last column fastest, so
 * that every combination of values is in exactly one pass.  The usual query,
 * whose columns have a few dozen values each, has one chunk per column and one
 * pass.  The tally of a pass goes up as partial counts, one row a combination
 * (the Finalize Agg above adds up what a spilled tally splits), and between
 * two rows nothing is pinned: the sets are located again for each pass.
 */
typedef struct LionDecodeRun
{
	int			ncol;
	AttrNumber	attno[LION_MAX_GROUPCOLS];	/* heap columns, in walk order */
	Oid			idxoid[LION_MAX_GROUPCOLS];
	Relation	idx[LION_MAX_GROUPCOLS];
	AttrNumber	idxcol[LION_MAX_GROUPCOLS]; /* key column of idx[c] (§24) */
	int			cap[LION_MAX_GROUPCOLS];	/* values in a chunk, at most */

	/* each column's entry walk, and the chunk it has read off it */
	LionEntryScan escan[LION_MAX_GROUPCOLS];
	bool		scanning[LION_MAX_GROUPCOLS];
	bool		scandone[LION_MAX_GROUPCOLS];	/* the walk ran out in this
												 * chunk: it is the last */
	int			nvals[LION_MAX_GROUPCOLS];
	Datum	   *keys[LION_MAX_GROUPCOLS];
	bool	   *isnull[LION_MAX_GROUPCOLS];
	LionPostingSet *sets[LION_MAX_GROUPCOLS];	/* as the walk found them,
												 * unpinned */
	MemoryContext chunkcxt[LION_MAX_GROUPCOLS];
	PGAlignedBlock *images[LION_MAX_GROUPCOLS]; /* cap[c] page images */

	MemoryContext passcxt;		/* the pass's located sets */
	struct LionDecodeTally *tally;
	bool		started;		/* the chunks of the first pass are read */
	bool		finished;		/* every pass has gone up */
	bool		emitting;		/* the tally of the pass is going up */
	Datum		rowkeys[LION_MAX_GROUPCOLS];	/* the row that went up last */
	bool		rownull[LION_MAX_GROUPCOLS];

	/* EXPLAIN ANALYZE */
	int64		passes;
	int64		rowsup;			/* partial rows handed up */
	LionDecodeStats stats;
} LionDecodeRun;

/*
 * A lion column whose entries the aggregates of DESIGN.md §37 are taken
 * over, and one of those aggregates.
 */
typedef struct LionWCol
{
	AttrNumber	attno;			/* the heap column */
	Oid			idxoid;
	AttrNumber	idxcol;			/* its key column in idxoid */
	Relation	idx;			/* open while the scan runs, or NULL */
	int			slotcol;		/* its key's column of the scan tuple, or -1 */
	uint32		bulkdeletes;	/* idx's, at the first look (DESIGN.md §37) */
} LionWCol;

typedef struct LionWAgg
{
	int			kind;			/* LION_WAGG_* */
	int			col;			/* its column, in the scan's wcol */
	int			argwidth;		/* 2, 4 or 8: a sum's or average's argument */
	ExprState  *arg;			/* the argument, over the key's column */
	bool		argiskey;		/* ... which is the key itself */
	FmgrInfo	cmp;			/* EXTREME: its sort operator */
	Oid			collation;
	int16		typlen;			/* EXTREME: the argument's type */
	bool		typbyval;
#ifdef HAVE_INT128
	int128		sum;			/* SUM, AVG: the weighted sum */
#endif
	int64		n;				/* ... and the rows with a value */
	Datum		ext;			/* EXTREME: the value so far, in wcxt */
	bool		hasext;
	Datum		result;
	bool		resnull;
} LionWAgg;

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
	 * The decoded walk of DESIGN.md §34 (LION_FLAG_DECODE): the GROUP BY
	 * columns it counts, three or more, and the run - its passes, its tally
	 * and the row it has got to (lion_next_group_decode()).  NULL for every
	 * other shape.
	 */
	struct LionDecodeRun *decode;

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
	AttrNumber	allattno;		/* ... of this column, the plan's choice, when
								 * nothing names it (DESIGN.md §35, "Every
								 * row"); 0 otherwise */
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
	 * The items of the plan, which item and nitem are for a plain table.  A
	 * partition leaves out the clauses its bounds imply (DESIGN.md §16,
	 * "Clauses the partition bounds imply"): its item and nitem are the
	 * plan's without them, made when it is opened (lion_relation_items()),
	 * and those clauses have no index open while it is (idx NULL).
	 */
	int			nplanitem;
	LionSourceItem *planitem;
	List	   *implied;		/* LION_PRIV_IMPLIED, for EXPLAIN */

	/*
	 * The relations to count.  npart is 0 for a plain table, whose heap and
	 * indexes are opened once for the life of the node; a partitioned one
	 * (DESIGN.md §16) has one LionPartState per live leaf partition, opens
	 * all of them for the life of the node too, and counts them one partition
	 * at a time - its turn, during which it is the relation below - so that
	 * no partition's buffer pin ever outlives that partition's processing.
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
	 * The groups of the entry walk, counted together once the WHERE is
	 * collected (DESIGN.md §10, "The groups of a walk, counted together";
	 * lion_count_groups_copy()): a batch of up to gbmax entries is taken
	 * from the walk, counted in one walk of container keys, and its rows are
	 * emitted in the walk's order, one a call.  gbatchcxt holds the batch -
	 * its located sets while they are counted, its keys and counts until the
	 * last row is emitted.  groupbatches and groupsbatched are what EXPLAIN
	 * ANALYZE reports.  gbimages is a page image for each group of a batch,
	 * which the groups' cursors of every batch copy their leaves to.
	 */
	MemoryContext gbatchcxt;
	PGAlignedBlock *gbimages;
	LionPostingSet *gbsets;
	Datum	   *gbkey;
	bool	   *gbnull;
	int64	   *gbcount;
	int			gbmax;
	int			gbn;
	int			gbpos;
	int64		groupbatches;
	int64		groupsbatched;

	/*
	 * The top k of a GROUP BY ordered by its count (DESIGN.md §36): topkn is
	 * k (0: every group), topkcand how many entries the walk of the entries'
	 * counts keeps, topkstrict that every group tied with the k-th must come
	 * out.  lion_topk_run() fills topkkey, topknull and topkcount with the
	 * groups it counted, in topkcxt, and the rows go up one a call; topkran
	 * says it has run, and topkwhole that it could not prove its candidates
	 * enough, so the walk counts every group as without a k.  topkwalked and
	 * topkcounted are what EXPLAIN ANALYZE reports.
	 */
	int64		topkn;
	int			topkcand;
	bool		topkstrict;
	bool		topkran;
	bool		topkwhole;
	MemoryContext topkcxt;
	Datum	   *topkkey;
	bool	   *topknull;
	int64	   *topkcount;
	int			topkout;
	int			topkpos;
	int64		topkwalked;
	int64		topkcounted;
	int64		topkwholes;

	/*
	 * The aggregates over the entries of lion columns (DESIGN.md §37): nwcol
	 * columns, each walked once, and nwagg aggregates over them, each an
	 * argument evaluated on an entry's key and weighted by the entry's rows.
	 * wneedcount says the target list has counts too, which the sum over
	 * every row answers as before.  wfast and wslow are what EXPLAIN ANALYZE
	 * reports: the walks that read the entries' own counts, and those that
	 * counted each entry.
	 */
	int			nwcol;
	struct LionWCol *wcol;
	int			nwagg;
	struct LionWAgg *wagg;
	bool		wneedcount;
	MemoryContext wcxt;
	int64		wentries;
	int64		wfast;
	int64		wslow;

	/*
	 * The gather of the window store (DESIGN.md §40, "The custom shapes";
	 * LION_PRIV_STORE), or NULL: the groups and aggregates the node computes
	 * from the stored values of the rows it counts.  lion_exec_store.c.
	 */
	struct LionStoreRun *store;

	/*
	 * A parallel GROUP BY (DESIGN.md §10, "A GROUP BY in parallel"; granged,
	 * the plan node being parallel-aware and no join): each participant claims
	 * a range of container keys at a time (lion_key_ranges(), the counter in
	 * joinshared), collects the WHERE of that range alone into wherecoll - in
	 * grangecxt, emptied at the next range - and counts every group of the
	 * entry walk against it, a partial row per group; the Finalize Agg above
	 * adds a group's rows up.  grange is the range being counted, -1 between
	 * them, and granges how many this participant counted, which EXPLAIN
	 * ANALYZE reports with the workers' (workerranges, and the workers'
	 * collections and batches beside it).
	 */
	bool		granged;
	int			grange;
	MemoryContext grangecxt;
	int64		granges;
	int64		workerranges;
	int64		workerwherecollected;
	int64		workerwherespilled;
	int64		workergroupbatches;
	int64		workergroupsbatched;

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
	bool		joincounts;		/* LION_JOINFLAG_COUNTS: rows, each with
								 * its count */
	bool		joinouter;		/* LION_JOINFLAG_OUTER: a semi or anti join
								 * path, whose rows are the outer side's */
	bool		joinordered;	/* LION_JOINFLAG_ORDERED: ... handed up in
								 * the child's order */
	bool		joincollected;
	bool		joinfiltered;
	LionPostingSet joinfilter;
	LionCountSource joinsources[2];
	int64		joinfilterrows;
	int64		joinspilled;	/* copies that went to a temporary file */

	/*
	 * PROBED, THEN COLLECTED (DESIGN.md §27): a plan that probes the fact
	 * filters, because the planner expected few dimension rows, keeps an
	 * account of what probing them has cost this run - joinrent, in the
	 * planner's units, from what the counts read (lion_join_count_key()) - and
	 * collects them once it reaches what collecting would cost, joinbuy
	 * (lion_join_copy_price(), from the located sets; -1 until they are
	 * priced, 0 where no copy of them can be made or would fit).  joinswitch
	 * says this run may still switch; joinprobed counts its keys counted by
	 * probing.  A partitioned fact table keeps the same account per leaf
	 * (LionJoinPart), joinrentp pointing at the one of the leaf whose turn it
	 * is.  joinswitches and joinswitchkeys are what EXPLAIN ANALYZE reports:
	 * the copies made by switching, and the keys probed before each, summed
	 * over the runs, the leaves and - joinworker* - the workers.
	 */
	bool		joinswitch;
	double		joinrent;
	double		joinbuy;
	int64		joinprobed;
	double	   *joinrentp;
	int64	   *joinprobedp;
	int64		joinswitches;
	int64		joinswitchkeys;
	int64		joinworkerswitches;
	int64		joinworkerswitchkeys;

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
	 * How a run over a plain fact table reads its child: in those batches,
	 * walked in key order (joinwalked), or a row at a time.  It is decided
	 * once, at the run's first row (joinbegun), and kept to the run's end:
	 * a copy of the fact filters made part way through that finds them to
	 * select nothing sets wheremissing, which each way answers for itself
	 * (DESIGN.md §27, "Probed, then collected"), and must not change the way
	 * - the batch in hand would be dropped and the child read on, or read
	 * again, a row at a time.
	 */
	bool		joinbegun;
	bool		joinwalked;

	/*
	 * A partitioned fact table (DESIGN.md §27, "A partitioned fact table"):
	 * every batch of keys is taken to each leaf partition in turn, and each
	 * key's counts added up in its entry (LionJoinEnt.acc).  joinpart is the
	 * partitions' state from batch to batch, their copies in joinpartcxt;
	 * joinvisitcxt is one partition's turn - its walk of the fk index, begun
	 * again each turn - and joinorder the order the batch is sorted in, which
	 * a partition whose fk index orders its keys another way sorts it into
	 * again.  joinrunfilterrows is the run's copies' rows, summed.
	 */
	LionJoinPart *joinpart;
	MemoryContext joinpartcxt;
	MemoryContext joinvisitcxt;
	LionWalkOrder joinorder;
	int64		joinrunfilterrows;

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

	/*
	 * One copy per query (DESIGN.md §27, "One copy per query"): in a parallel
	 * plan whose workers started, the fact filters are collected once, by all
	 * the participants together, into joinsharedcopy in the Gather's dynamic
	 * shared memory, and joinfilter is this participant's view of it
	 * (joinviewshared).  joinpcxt is the leader's parallel context, which
	 * says whether any worker started.  joincopies counts the shared copies
	 * this participant indexed - one a run - and joincopychunks the chunks of
	 * them it collected, for EXPLAIN ANALYZE.
	 */
	LionSharedCopy *joinsharedcopy;
	ParallelContext *joinpcxt;
	bool		joinviewshared;
	int64		joincopies;
	int64		joincopychunks;
	int64		joinworkercopies;
	int64		joinworkercopychunks;

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

	/*
	 * The FK-side join grouped by a fact column (DESIGN.md §27, "Grouped by a
	 * fact column"): fgattno is the column, in the parent's numbering, or 0.
	 * Each batch of keys is taken to each relation in turn - the plain table
	 * once, or every leaf partition (fgturn is the next) - and each key is
	 * counted there once per group of the column: in a partition whose
	 * bounds give it one value, the key's count with that value; otherwise
	 * once per entry of the column's lion index (fgidxoid, or the
	 * partition's), a chunk of up to LION_FKJOIN_GROUP_CHUNK located sets at a
	 * time (fgsets and fgkey, in fgcxt), each ANDed into the count as one more
	 * source (fgsrc).  The counts that are not 0 wait in fgrows - their keys
	 * in fgrowcxt - until the relation's turn is over, and go up one a call.
	 * fggroupcounts is how many counts there were, for EXPLAIN ANALYZE.
	 */
	AttrNumber	fgattno;
	Oid			fgidxoid;
	Relation	fgidx;			/* a plain table's, open for the node's life */
	int			fgturn;
	MemoryContext fgcxt;
	MemoryContext fgrowcxt;
	LionPostingSet *fgsets;
	Datum	   *fgkey;
	bool	   *fgnull;
	int			fgn;
	LionCountSource *fgsrc;
	int			fgnsrc;
	LionJoinGroupRow *fgrows;
	int			fgnrows;
	int			fgrowcap;
	int			fgrowpos;
	int64		fggroupcounts;
	int64		fgworkergroupcounts;
} LionCountScanState;

/*
 * What the participants of a parallel FK-side join add up for EXPLAIN
 * ANALYZE, in the Gather's dynamic shared memory (lion_shutdown_custom_scan()).
 * filterrows is the largest copy any participant made of the fact filters,
 * -1 when none did.  sorted is the keys a participant of this run sorted -
 * every participant sorts all of them - and sortedruns the same summed over
 * the runs before it (lion_reinitialize_dsm()).
 *
 * nextchunk is one of the two things the participants share while they run:
 * the next run of LION_FKJOIN_UNIQUE_CHUNK distinct keys of a forward semi
 * join over a non-unique key that nobody has claimed yet.  The other is the
 * fact filters' copy (copyready: a LionSharedCopy after this struct, in the
 * same DSM chunk), and copies and copychunks are what its participants
 * indexed and collected of it, summed; switches and switchkeys the copies
 * participants of a plan that probes the filters made of them part way
 * through, each its own, and the keys they probed before (DESIGN.md §27,
 * "Probed, then collected"), summed.  participants is how many the
 * leader started the plan for, itself included, which is how many ways each
 * of them divides the list pin budget (lion_list_pin_participants()).
 * batches is the batches of keys looked up in key order, summed, and
 * childrows, posting and time the rows, the posting pages and the phases of
 * "Where a key's time goes" (DESIGN.md §27), summed.
 *
 * A parallel GROUP BY (DESIGN.md §10, "A GROUP BY in parallel") keeps its
 * state here too: nextchunk is then the next range of container keys nobody
 * has claimed, of nranges ranges cut from ckeys keys (lion_key_ranges()), and
 * ranges, wherecollected, wherespilled, groupbatches and groupsbatched its
 * workers' counters, summed.
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
	int64		copies;
	int64		copychunks;
	int64		switches;
	int64		switchkeys;
	int64		ranges;
	int64		wherecollected;
	int64		wherespilled;
	int64		groupbatches;
	int64		groupsbatched;
	int64		factgroupcounts;
	pg_atomic_uint32 nextchunk;
	int			participants;
	int			nranges;
	uint32		ckeys;
	bool		copyready;		/* a LionSharedCopy follows this struct */
} LionJoinShared;

/*
 * The shared copy of the fact filters, right after the struct above - or,
 * over a partitioned fact table, one copy per leaf partition, one after the
 * other (DESIGN.md §27, "A partitioned fact table").
 */
#define LION_JOIN_SHARED_COPY(shared) \
	((LionSharedCopy *) ((char *) (shared) + MAXALIGN(sizeof(LionJoinShared))))
#define LION_JOIN_SHARED_PART_COPY(shared, p) \
	((LionSharedCopy *) ((char *) LION_JOIN_SHARED_COPY(shared) + \
						 (Size) (p) * lion_shared_copy_size()))





/*
 * The FK-side semi and anti join as a JOIN path (DESIGN.md §27, "The semi and
 * anti join as a join path") is the same node, run by the same executor
 * methods, under the names EXPLAIN shows for what it does: a join rel's rows,
 * the outer side's with a match (LionSemiJoin) or without one (LionAntiJoin).
 */




/*
 * The node above the core Agg of an FK-side join whose aggregates were
 * rewritten over the join's counted rows (DESIGN.md §27, "Every aggregate
 * over the node's rows"): it hands the Agg's rows up unchanged, under the
 * query's own aggregates.
 */





/* =====================================================================
 * Planner
 * ===================================================================== */


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
	bool	   *dropped;		/* one per WHERE clause: this partition's
								 * bounds imply it, and it is no source here
								 * (DESIGN.md §16, "Clauses the partition
								 * bounds imply"); NULL when none is */
	int			ndropped;
	Const	   *driveconst[LION_MAX_GROUPCOLS];	/* the one value the leaf's
												 * bounds give a driving
												 * column that asks for it
												 * (LionDriveInfo.bound), with
												 * no index in driveidx; NULL
												 * otherwise */
} LionCountTarget;

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
	bool		inor;			/* a leaf of an OR restriction (§19) */
	int			rinfono;		/* the restriction it came from, its position
								 * in the relation's baserestrictinfo; -1 for
								 * the FK-side join's key, which is none */
} LionClauseInfo;

/*
 * What a leaf partition's bounds may make of the WHERE clauses (DESIGN.md
 * §16, "Clauses the partition bounds imply"): the relation the clauses were
 * written against - the partitioned table the query names, whose numbering
 * they are in - and each restriction's clause by its position; and whether
 * something other than the WHERE clauses drives the count, so that a
 * partition left with no clause that selects rows still has its rows counted.
 * The OR restrictions (§19) are there as the plan has them - `ors` over the
 * clause lists, and each leaf's own clause in `leafclauses` - so that a
 * partition can leave out the arms its bounds refute ("OR arms the partition
 * bounds refute").
 */
typedef struct LionImply
{
	RelOptInfo *toprel;
	List	   *clauses;		/* Expr, one per baserestrictinfo entry */
	bool		driven;
	List	   *skipped;		/* String: the clauses no posting set answers
								 * that every partition implies, which are no
								 * clauses of the plan (LION_PRIV_IMPLIED) */
	List	   *leafclauses;	/* Expr, one per clause of the plan */
	List	   *ors;			/* the plan's LION_PRIV_ORS */
} LionImply;

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

	/*
	 * A leaf partition whose bounds give the column one value may take it
	 * from there instead of from an index (lion_leaf_bound_value()): every row
	 * of it is one group.  Only the FK-side join's fact group asks it
	 * (DESIGN.md §27, "Grouped by a fact column").
	 */
	bool		bound;
} LionDriveInfo;

/*
 * What a parallel GROUP BY (DESIGN.md §10, "A GROUP BY in parallel") needs of
 * the serial node's price: whether it is the price of the groups of a walk
 * counted together against the collected WHERE (batched), and what each range
 * of container keys a participant counts pays again of it (perrange): the
 * entry walk from its first entry, each group's set decoded, set up and sought
 * to the range's first key, and each WHERE source sought there.
 */
typedef struct LionRangeCost
{
	bool		batched;
	Cost		perrange;
} LionRangeCost;

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


/* Defined in one of the LionCount custom-scan files and used in another. */

/* lion_plan_match.c */
extern Node *lion_strip(Node *node);
extern bool lion_opfamily_is_multikey(Oid opfamily, Oid opcintype);
extern IndexOptInfo *lion_find_roaring_index(RelOptInfo *rel, AttrNumber attno,
											 bool multikey, AttrNumber *colp);
extern Oid lion_index_equality_op(IndexOptInfo *idx, AttrNumber col);
extern bool lion_type_equalimage(Oid typid, Oid collation);
extern bool lion_index_can_emit_value(IndexOptInfo *idx, AttrNumber col);
extern bool lion_contains_param(Node *expr);
extern int	lion_store_agg_classify(Aggref *agg, Index rti, AttrNumber *attno,
									int *width, Oid *eqop, Oid *collation);
extern IndexOptInfo *lion_find_store_index(RelOptInfo *rel, List *attnos,
										   List **idxcols);
extern int	lion_wagg_classify(Aggref *agg, Index rti, AttrNumber *attno,
							   int *argwidth);
extern IndexOptInfo *lion_match_index(RelOptInfo *rel, AttrNumber attno,
									  int kind, Oid opno, Oid cmptype,
									  StrategyNumber strategy,
									  Oid extractquery, Oid exprcoll,
									  AttrNumber *colp);
extern BoolExpr *lion_boolean_not_test(Node *clause);
extern List *lion_or_arms(Node *clause);
extern bool lion_analyze_leaf(PlannerInfo *root, Node *clause, Index rti,
							  bool allow_negated, bool allow_range,
							  bool allow_recheck, LionLeafInfo *out);
extern void lion_append_clause(const LionLeafInfo *leaf, Node *clause,
							   bool inor, List **whereattnos,
							   List **clauseinfos, List **whereclauses,
							   List **whereconsts, List **wherekinds,
							   List **whereopnos, List **whereinor);
extern bool *lion_or_leaf_map(List *ors, int nclause);
extern int *lion_or_group_map(List *ors, int nclause);
extern int *lion_rangesrc_leaders(List *whereidx, List *wherecol,
								  List *wherekinds, List *ors, int nclause);

/* lion_plan_partition.c */
extern Var *lion_child_var(PlannerInfo *root, Index childrelid,
						   AttrNumber parentattno);
extern List *lion_leaf_partition_qual(Relation relation, Index relid);
extern char *lion_deparse_rel_clause(PlannerInfo *root, Index rti,
									 Node *clause);
extern bool lion_implied_everywhere(PlannerInfo *root, RelOptInfo *toprel,
									RelOptInfo *rel, Node *clause);
extern bool lion_rinfos_implied_everywhere(PlannerInfo *root,
										   RelOptInfo *toprel, List *rinfos);
extern bool lion_pinned_not_implied(PlannerInfo *root, RelOptInfo *toprel,
									List *clauseinfos, List *clauses);
extern bool lion_collect_targets(PlannerInfo *root, RelOptInfo *rel,
								 const LionDriveInfo *drive, int ndrive,
								 List *whereattnos, List *clauseinfos,
								 const LionImply *imply, List **targets);
extern Bitmapset *lion_notnullattnums(PlannerInfo *root, RelOptInfo *rel);

/* lion_plan_target.c */
extern bool lion_agg_is_count(PlannerInfo *root, Aggref *agg, Index rti,
							  RelOptInfo *rel, const AttrNumber *groupattno,
							  int ngroup, const List *nonnullattnos,
							  const List *nullattnos);
extern bool lion_agg_distinct_var(Aggref *agg, Index rti, Var **var, Oid *eqop,
								  Oid *collation);
extern bool lion_group_coalesce(CoalesceExpr *ce, Index rti, Var **var,
								Const **con);
extern PathTarget *lion_make_partial_target(PlannerInfo *root,
											PathTarget *grouping_target,
											List *having);
extern List *lion_replaced_functions(RelOptInfo *rel, List *tlexprs,
									 List *having, List *groupclause,
									 const LionFkJoin *fj);

/*
 * The kinds of core plan a lion path is priced against (DESIGN.md §39,
 * lion_plan_units.c), each with its rate: the cost units a millisecond its
 * plans run at, as a multiple of the 500 lion's CPU constants are fitted at.
 */
typedef enum LionCompetitor
{
	LION_COMPETITOR_NONE,		/* no path of core's in the relation yet */
	LION_COMPETITOR_DISABLED,	/* ... or none that enable_* leaves on */
	LION_COMPETITOR_HASHAGG,	/* an aggregate that hashes */
	LION_COMPETITOR_AGG,		/* a plain or sorted one over a scan */
	LION_COMPETITOR_HASHJOIN,
	LION_COMPETITOR_MERGEJOIN,
	LION_COMPETITOR_NESTLOOP,	/* into a parameterized index scan */
	LION_COMPETITOR_SEQSCAN,
	LION_COMPETITOR_INDEXONLY,
	LION_COMPETITOR_INDEX,
	LION_COMPETITOR_BITMAP,
	LION_COMPETITOR_OTHER
} LionCompetitor;

/*
 * The units a lion path's own price is converted into (lion_units_for()), and
 * the margin it is offered at
 */
typedef struct LionUnits
{
	LionCompetitor kind;		/* the cheapest core path's kind */
	double		rate;			/* ... and its rate */
	double		margin;			/* pg_lion.pushdown_margin, or 1 */
} LionUnits;

/* lion_plan_units.c */
extern bool lion_path_has_lion(Path *path);
extern LionCompetitor lion_competitor_kind(Path *path);
extern double lion_competitor_rate(LionCompetitor kind);
extern Path *lion_competitor_path(RelOptInfo *rel);
extern void lion_units_for(RelOptInfo *rel, LionUnits *u);
extern void lion_units_pin(RelOptInfo *rel);
extern void lion_units_unpin(void);
extern Cost lion_units_price(const LionUnits *u, Cost own);
extern double lion_units_margin(void);
extern double lion_units_margin_for(RelOptInfo *rel);

/* lion_plan_cost.c */
extern double lion_index_dir_pages(IndexOptInfo *idx, double *height);
extern double lion_index_store_pages(IndexOptInfo *idx, int *nstored);
extern void lion_cost_store_path(PlannerInfo *root, CustomPath *cpath,
								 RelOptInfo *rel, IndexOptInfo *storeidx,
								 int ncols, int ngroup, double groups,
								 double outrows, int naggs, int ndistinct,
								 double distinctpairs);
extern double lion_index_column_share(PlannerInfo *root, RelOptInfo *rel,
									  IndexOptInfo *idx, AttrNumber col);
extern bool lion_index_orders_naturally(IndexOptInfo *idx, AttrNumber col);
extern double lion_range_entries(PlannerInfo *root, RelOptInfo *rel, Var *var,
								 Selectivity sel);
extern double lion_exists_fraction(double containers, double survivors);
extern double lion_posting_height(double leaves);
extern Cost lion_heap_page_cost(PlannerInfo *root, RelOptInfo *rel,
								double pages, double heap_pages);
extern Cost lion_index_page_cost(PlannerInfo *root, double idxpages,
								 Cost device);
extern bool lion_where_query_unknown(List *whereclauses, List *wherekinds,
									 List *whereinor);
extern int lion_inlist_shape(IndexOptInfo *groupidx, AttrNumber groupcol,
							 IndexOptInfo *groupidx2, List *whereidx,
							 List *wherecol, List *whereclauses,
							 List *wherekinds, List *ors, bool eqdrives,
							 bool *sumshort, bool *groupdrive);
extern bool lion_leaf_refuted_now(PlannerInfo *root, RelOptInfo *rel,
								  List *clauses);
extern Selectivity lion_probed_selectivity(PlannerInfo *root, RelOptInfo *rel,
										   List *clauses);
extern Selectivity lion_rel_clauses_selectivity(PlannerInfo *root,
												RelOptInfo *rel,
												List *clauses);
extern Cost lion_cost_range_source(PlannerInfo *root, RelOptInfo *rel,
								   IndexOptInfo *idx, AttrNumber col,
								   List *bounds, Selectivity sel,
								   double counts, bool inor);
extern void lion_target_lists(const LionCountTarget *t, List *whereclauses,
							  List *wherekinds, List *ors, List **idx,
							  List **col, List **clauses, List **kinds,
							  List **tors, int *joinclause);
extern void lion_cost_wagg_path(PlannerInfo *root, CustomPath *cpath,
								RelOptInfo *rel, List *idxs, List *cols,
								List *naggs, bool counts);
extern void lion_cost_topk_path(PlannerInfo *root, CustomPath *cpath,
								List *targets, List *whereclauses,
								List *wherekinds, List *ors, double entries,
								double drivefrac, double cand,
								double candrows, double outrows);
extern void lion_cost_count_path(PlannerInfo *root, CustomPath *cpath,
								 List *targets, List *whereclauses,
								 List *wherekinds, List *ors, double numgroups,
								 double outer_entries, double inner_entries,
								 double outrows, int distinct, int ranged,
								 double drivefrac, LionRangeCost *rc);
extern void lion_cost_decode_path(PlannerInfo *root, CustomPath *cpath,
								  List *targets, List *whereclauses,
								  List *wherekinds, List *ors, int ncol,
								  const double *groupest, double numgroups,
								  double outrows);
extern double lion_multikey_nkeys(IndexOptInfo *idx, AttrNumber col,
								  Node *clause);
extern Cost lion_cost_recheck(PlannerInfo *root, RelOptInfo *rel,
							  List *whereidx, List *wherecol,
							  List *whereclauses, List *wherekinds, List *ors,
							  double matched, double counts, double ceiling);

/* lion_plan_fkjoin_cost.c */
extern double lion_fkjoin_fk_ndistinct(PlannerInfo *root, RelOptInfo *rel,
									   Var *fkvar);
extern double lion_parallel_divisor(int workers);
extern Cost lion_cost_fkjoin_path(PlannerInfo *root, RelOptInfo *rel,
								  List *targets, Var *fkvar, int joinclause,
								  List *whereclauses, List *wherekinds,
								  List *ors, double dimrows, double found,
								  bool exists, double rowbytes, int workers,
								  bool *collect, bool *walk, bool force);

/* lion_plan_fkjoin.c */
extern double lion_leaf_turn_share(PlannerInfo *root, RelOptInfo *rel);
extern double lion_fact_groups(PlannerInfo *root, const LionCountTarget *t);
extern AttrNumber lion_fact_group_attno(List *priv);
extern void lion_try_fkjoin_path(PlannerInfo *root, RelOptInfo *rel,
								 RelOptInfo *output_rel,
								 GroupPathExtraData *extra,
								 const LionFkJoin *fj, List *having,
								 List *whereattnos, List *clauseinfos,
								 List *whereclauses, List *whereconsts,
								 List *wherekinds, List *whereopnos,
								 List *whereinor, List *ors,
								 const LionImply *imply);
extern void lion_try_semijoin_path(PlannerInfo *root, RelOptInfo *rel,
								   const LionFkJoin *fj, List *whereattnos,
								   List *clauseinfos, List *whereclauses,
								   List *whereconsts, List *wherekinds,
								   List *whereopnos, List *whereinor,
								   List *ors, const LionImply *imply);

/* lion_plan_count.c */
extern void lion_try_count_path(PlannerInfo *root, RelOptInfo *input_rel,
								RelOptInfo *output_rel,
								GroupPathExtraData *extra,
								const LionFkJoin *fj);

/* lion_plan_hooks.c */
extern Plan *lion_plan_join_agg_path(PlannerInfo *root, RelOptInfo *rel,
									 CustomPath *best_path, List *tlist,
									 List *clauses, List *custom_plans);
extern Plan *lion_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
								   CustomPath *best_path, List *tlist,
								   List *clauses, List *custom_plans);

/* lion_exec_begin.c */
extern Node *lion_create_custom_scan_state(CustomScan *cscan);
extern AttrNumber lion_index_col_for(Relation index, AttrNumber heapattno,
									 bool multikey);
extern AttrNumber lion_heap_attno_in(Relation heap, Oid parentoid,
									 AttrNumber parentattno);
extern void lion_open_relation(LionCountScanState *st, int p);
extern void lion_close_relation(LionCountScanState *st);
extern void lion_close_parts(LionCountScanState *st);
extern void lion_begin_custom_scan(CustomScanState *node, EState *estate,
								   int eflags);

/* lion_exec_fkjoin.c */
extern Node *lion_create_join_agg_state(CustomScan *cscan);
extern void lion_begin_join_agg(CustomScanState *node, EState *estate,
								int eflags);
extern TupleTableSlot *lion_exec_join_agg(CustomScanState *node);
extern void lion_end_join_agg(CustomScanState *node);
extern void lion_rescan_join_agg(CustomScanState *node);
extern void lion_join_locate_where(LionCountScanState *st);
extern void lion_join_switch_reset(LionCountScanState *st);
extern void lion_join_batch_reset(LionCountScanState *st);
extern TupleTableSlot *lion_next_join_row(LionCountScanState *st);

/* lion_exec_locate.c */
extern void lion_eval_clause_values(LionCountScanState *st);
extern LionKeyNode *lion_key_node(int keyno);
extern LionKeyNode *lion_bool_node(LionKeyNodeKind kind, LionKeyNode **args,
								   int nargs);
extern void lion_build_filter(LionCountScanState *st);
extern void lion_locate_where(LionCountScanState *st);
extern void lion_release_where(LionCountScanState *st);
extern TupleTableSlot *lion_emit_tuple(LionCountScanState *st, Datum key,
									   bool keyisnull, Datum key2,
									   bool key2isnull, int64 count);
extern TupleTableSlot *lion_emit_keys(LionCountScanState *st, int nkeys,
									  const Datum *keys, const bool *keyisnull,
									  int64 count);

/* lion_exec_count.c */
extern int64 lion_node_count(LionCountScanState *st, int nsource,
							 LionCountSource *sources, bool exists);
extern int64 lion_count_relation(LionCountScanState *st);
extern int64 lion_sumall_relation(LionCountScanState *st);
extern TupleTableSlot *lion_next_group_ranged(LionCountScanState *st,
											  bool *exhausted);
extern TupleTableSlot *lion_distinct_relation(LionCountScanState *st);
extern void lion_wagg_run(LionCountScanState *st);
extern void lion_wagg_finish(LionWAgg *a);
extern TupleTableSlot *lion_next_group_any(LionCountScanState *st,
										   bool *exhausted);
extern void lion_decode_reset(LionCountScanState *st);

/* lion_exec_store.c */
extern void lion_store_begin(LionCountScanState *st, CustomScan *cscan,
							 EState *estate);
extern TupleTableSlot *lion_store_next(LionCountScanState *st);
extern Datum lion_store_emit_value(LionCountScanState *st, int kind,
								   bool *isnull);
extern void lion_store_reset(LionCountScanState *st);
extern void lion_store_explain(LionCountScanState *st, ExplainState *es);

/* lion_exec_run.c */
extern void lion_pause_run(LionCountScanState *st);
extern void lion_unpin_where(LionCountScanState *st);
extern TupleTableSlot *lion_exec_custom_scan(CustomScanState *node);
extern void lion_rescan_custom_scan(CustomScanState *node);
extern Size lion_estimate_dsm(CustomScanState *node, ParallelContext *pcxt);
extern void lion_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
								void *coordinate);
extern void lion_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
								  void *coordinate);
extern void lion_initialize_worker(CustomScanState *node, shm_toc *toc,
								   void *coordinate);
extern void lion_shutdown_custom_scan(CustomScanState *node);
extern void lion_end_custom_scan(CustomScanState *node);

/* lion_exec_explain.c */
extern void lion_explain_custom_scan(CustomScanState *node, List *ancestors,
									 ExplainState *es);

extern const CustomPathMethods lion_antijoin_path_methods;

extern const CustomPathMethods lion_count_path_methods;

extern const CustomPathMethods lion_join_agg_path_methods;

extern const CustomPathMethods lion_semijoin_path_methods;

/* lion_plan_hooks.c */
extern bool		lion_enable_count_pushdown;
extern bool		lion_enable_filter_switch;
extern create_upper_paths_hook_type lion_prev_create_upper_paths_hook;
extern bool		lion_enable_semijoin;
extern set_join_pathlist_hook_type lion_prev_set_join_pathlist_hook;

#endif							/* LION_CUSTOMSCAN_H */
