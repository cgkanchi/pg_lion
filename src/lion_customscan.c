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
 * node sums the counts of every entry of that index instead.
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
 * clause whose value comes from the child's current row.
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
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
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

#include "storage/predicate.h"

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
 * The fixed cost of one count of the FK-side join (DESIGN.md §27) - one per
 * dimension row - over and above the containers it reads and the lookup that
 * locates its set: a merge set up and torn down.  It is the same work §26's
 * per-test cost measures, and the same number.
 */
#define LION_FKJOIN_COUNT_COST	(50.0 * cpu_tuple_cost)

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
#define LION_FKJOIN_PROBE_COST	(30.0 * cpu_operator_cost)

/*
 * ... and, per count, each SET of a fact filter that is a union (an IN list,
 * an OR across columns): the count builds its k-way union again (§15), and
 * each sub-cursor is set up and positioned whether or not the fk set's keys
 * find anything in it.  Measured as above, 1.5M fact rows and 300 dimension
 * rows: `t IN (n values)` took 57 ms at n = 30, 199 at 100, 599 at 300 and
 * 1195 at 1000 - 4 to 6 us per set per count, about 0.8 of the units the
 * ordinary plan's estimate spends per millisecond of the same run.
 */
#define LION_FKJOIN_SET_COST	(80.0 * cpu_tuple_cost)

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

/*
 * A RANGE clause is neither: it is not a source of the count at all, but a
 * bound on the entry walk that drives it (DESIGN.md §28), so it is never
 * located, never merged and never priced as a lookup.
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
 *		execution time from the opened index and the attnum in member 2.
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
 *		and Params - for a prepared one (DESIGN.md §10).  The PATH carries
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
 *		value expression is the dimension's column - and, added at plan time
 *		once the child plan exists, the position of that column in the
 *		child's target list.  The join key clause is not a source: the node
 *		looks it up once per child row, in source slot 0
 *	11	List of three OidLists: the functions whose evaluation the node
 *		replaces, for the EXECUTE checks the executor would have made on the
 *		plan it stands for (DESIGN.md §9, "Privileges"; checked at executor
 *		startup by lion_check_replaced_execute(), never at plan time): the
 *		aggregates of the target list and the HAVING; the functions of the
 *		WHERE clauses and of the FK-side join clause; and the equality
 *		functions of the GROUP BY
 *	12	IntList: LION_TL_* for each custom_scan_tlist column (added at plan
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
#define LION_PRIV_TLKINDS	12

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
 * Shape 6 changed no member's POSITION, which is exactly what the marker is
 * for: since DESIGN.md §24 an index Oid here may name a MULTICOLUMN index, and
 * the key column it is read for is not in the list at all - the executor
 * derives it from the index it really opened and the clause's heap attnum
 * (lion_index_col_for()), because a partition's index may put the same column
 * at a different position from the parent's.  A plan built before that would
 * have been made by a planner that never chose a multicolumn index, so it
 * would still decode correctly; saying so is cheaper than having to know that.
 */
#define LION_PRIV_MAGIC		0x5242490b
#define LION_PRIV_NMEMBERS	13

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
	 * value ready at plan time; a prepared one evaluates its Param through
	 * the node's ExprContext at the start of every scan and after every
	 * ReScan, because a nested loop changes an exec Param between them
	 * (DESIGN.md §10).
	 */
	Expr	   *valexpr;
	Const	   *con;			/* valexpr, when it is a Const; else NULL */
	ExprState  *valstate;		/* set up when valexpr is not a Const */
	Oid			valtype;		/* type valexpr produces */
	Datum		val;
	bool		valisnull;

	StrategyNumber strategy;	/* LION_CLAUSE_MULTI: 2, 3 or 5 */
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
} LionSourceItem;

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
	 * are located once per node execution and keep their pins (DESIGN.md
	 * section 9) until the node is reset or closed; the groups' sets are
	 * located, counted and released one group (one pair) at a time.
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
	int			sumallitem;
	LionCountSource *dsources;
	int			ndsource;

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
} LionCountScanState;

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
	 * A range comparison (DESIGN.md §28) has to be one of the index's range
	 * strategies, with the ordering its walk needs - proc 4 for the pair - in
	 * the same family; lionvalidate() insists on both together, and this is
	 * where a catalogue that disagrees is declined rather than walked
	 * linearly.  The equality and hash checks below apply to it as well: the
	 * executor resolves the bound's comparison through lion_probe_init(),
	 * which needs them.
	 */
	if (kind == LION_CLAUSE_RANGE)
	{
		/*
		 * A class declared on a polymorphic type (enum_ops is FOR TYPE
		 * anyenum) names its members on that type, and the bound is of the
		 * column's own enum: that is the class's own type, not another one.
		 */
		if (IsPolymorphicType(idx->opcintype[i]) &&
			IsBinaryCoercible(cmptype, idx->opcintype[i]))
			cmptype = idx->opcintype[i];
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
 * Extract a multi-key query at plan time and say whether the posting sets can
 * answer it exactly (DESIGN.md §17).  Only then is the clause pushed down:
 * an ALL-mode query would need every row rechecked against the heap, which is
 * what the ordinary bitmap plan already does and does better.
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
	FmgrInfo	flinfo;
	LionQuery	q;
	LionState	state;
	MemoryContext cxt;
	MemoryContext oldcxt;
	bool		exact;

	*extractquery = get_opfamily_proc(opfamily, lefttype, lefttype,
									  LION_EXTRACTQUERY_PROC);
	if (!OidIsValid(*extractquery))
		return false;

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
	fmgr_info(*extractquery, &flinfo);
	state.extractquery = flinfo;

	lion_extract_query(&state, con->constvalue, strategy, &q);
	exact = (q.mode == LION_QMODE_KEYS);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return exact;
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
	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->aggvariadic)
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
	RangeTblEntry *rte;
	double		total = 0;
	double		mine = 0;
	int			i;

	if (idx->nkeycolumns <= 1)
		return 1.0;
	if (rel->relid == 0 || rel->relid >= (Index) root->simple_rel_array_size)
		return 1.0;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return 1.0;

	for (i = 0; i < idx->nkeycolumns; i++)
	{
		AttrNumber	attno = idx->indexkeys[i];
		VariableStatData vardata;
		Var		   *var;
		double		nd;
		bool		isdefault;

		/* An expression column has no heap attribute to ask about. */
		if (attno <= 0)
			return 1.0 / (double) idx->nkeycolumns;

		var = makeVar(rel->relid, attno, get_atttype(rte->relid, attno), -1,
					  get_typcollation(get_atttype(rte->relid, attno)), 0);
		examine_variable(root, (Node *) var, 0, &vardata);
		nd = get_variable_numdistinct(&vardata, &isdefault);
		ReleaseVariableStats(vardata);
		pfree(var);

		nd = Max(nd, 1.0);
		total += nd;
		if (i == col - 1)
			mine = nd;
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
 * How many entries of `var`'s index a range walk visits (DESIGN.md §28): the
 * column's n_distinct over the WHOLE table - a walk visits an entry whatever
 * the other clauses leave of it - times the range's own selectivity, at least
 * one.  n_distinct is taken as examine_variable() gives it rather than
 * through estimate_num_groups(), which would scale it down by every clause of
 * the relation, the range included, and count the range twice.
 */
static double
lion_range_entries(PlannerInfo *root, RelOptInfo *rel, Var *var,
				  Selectivity sel)
{
	VariableStatData vardata;
	double		ndistinct;
	bool		isdefault;

	examine_variable(root, (Node *) var, rel->relid, &vardata);
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);

	return Max(1.0, ndistinct * sel);
}

/*
 * The correlation between `var`'s values and the heap's physical order, from
 * its statistics, as btcostestimate() reads it; 0 when there is none.
 */
static double
lion_var_correlation(PlannerInfo *root, RelOptInfo *rel, Var *var)
{
	VariableStatData vardata;
	double		corr = 0.0;

	examine_variable(root, (Node *) var, rel->relid, &vardata);
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		AttStatsSlot sslot;

		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_CORRELATION, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0)
				corr = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}
	ReleaseVariableStats(vardata);

	return corr;
}

/*
 * How many containers a posting set of `members` members can span: one per
 * LION_BLOCKS_PER_CONTAINER heap pages, and never more than one per member.
 */
static double
lion_containers_for(double heap_pages, double members)
{
	return Max(1.0, Min(heap_pages / LION_BLOCKS_PER_CONTAINER, members));
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

static Cost
lion_cost_count_rel(PlannerInfo *root, RelOptInfo *rel,
				   IndexOptInfo *groupidx, AttrNumber groupcol,
				   IndexOptInfo *groupidx2, AttrNumber groupcol2,
				   List *whereidx, List *wherecol, List *whereclauses,
				   List *wherekinds,
				   List *ors, double numgroups,
				   double outer_entries, double inner_entries, int distinct,
				   double drivefrac, Var *rangevar)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		dirty_pages;
	double		matching = Max(rel->rows, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		random_pages = 0;	/* directory leaves, one per lookup */
	Cost		descent_cost = 0;	/* comparisons on the way down (§21) */
	double		seq_pages = 0;	/* container chains, read in order */
	Cost		lookup_cost = 0;	/* an IN list's bucket pages, in order */
	Cost		probe_cost = 0; /* what the SOUGHT sources read (§22) */
	double		ncontainers = 0;
	double	   *andc;			/* containers of each top-level AND source */
	int			nand = 0;
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
	int			nsrc;
	int			driver = -1;	/* the source that drives the leapfrog */
	double		probes = 0;		/* how often the others are sought */
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
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

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
	andc = (double *) palloc0(sizeof(double) * Max(nclause, 1));
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

		container_pages = ((double) idx->pages - 1.0 - dirpages) * share;
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
			descent_cost += (height + 1.0) * 50.0 * cpu_operator_cost;
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
			double		clc = nkeys * lion_containers_for(heap_pages,
														  tuples * sel / nkeys);

			/*
			 * A source that is ANDed with the others is PROBED at their
			 * container keys since DESIGN.md §22, so the intersection costs
			 * the most selective source's containers once per source rather
			 * than the sum of all of them.  An OR leaf is not an AND source -
			 * it is part of one union, which is driven by whichever of its
			 * arms has a container at a key - and neither is a list that
			 * drives the groups, so both keep their own term.
			 */
			if (orgrp[ci - 1] >= 0 || (groupdrive && ci - 1 == inlistci))
				ncontainers += clc;
			else
				andc[nand++] = clc;

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
			ingroups = Min(nkeys, ingroups);
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

	/*
	 * The AND of the clauses (DESIGN.md §22).  With the posting tree the merge
	 * is a leapfrog join: whichever source keeps producing the largest
	 * container key advances sequentially and the others SEEK to it, so the
	 * work is the most selective source's containers times the number of
	 * sources - one probe each - and not the sum over all of them, which is
	 * what a walk of every chain cost.  Measured at one million rows,
	 * `c20k = 77 AND c200 = 17 AND c2 = 1` visits 424 containers as a walk and
	 * 140 as probes.  One source is its own minimum, so a single clause is
	 * priced exactly as before.
	 */
	if (nand > 0)
	{
		double		andmin = andc[0];
		int			k;

		for (k = 1; k < nand; k++)
			andmin = Min(andmin, andc[k]);

		ncontainers += andmin * nand;
	}
	pfree(andc);

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
	if (nsrc > 0)
	{
		int			s;

		/*
		 * Which one drives is decided from the MEMBERS, as lion_run_merge()
		 * decides it from the entries' `ntids` - not from the containers, which
		 * for a union of k sets are counted k times over and would hand the
		 * merge to whichever source happens to lie in the fewest of them.  What
		 * the driver then costs the others is its CONTAINER KEYS, of which
		 * there are no more than the heap has.
		 */
		for (s = 0; s < nsrc; s++)
			if (driver < 0 || srcmembers[s] < srcmembers[driver])
				driver = s;
		probes = Min(srccontainers[driver],
					 Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0));

		if (groupidx != NULL)
			probes = Max(ingroups, 1.0) *
				Min(lion_containers_for(heap_pages,
										matching / Max(numgroups, 1.0)),
					probes);
	}

	for (i = 0; i < nclause; i++)
	{
		double		pages = clausepages[i];

		if (pages <= 0.0)
			continue;			/* a negated clause, or nothing to read */

		if (clausesrc[i] < 0 || clausesrc[i] == driver)
			seq_pages += pages; /* walked: the driver, and a group's own list */
		else
		{
			pages = Min(pages,
						clausekeys[i] * lion_probed_pages(clauseleaves[i],
														  probes,
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

	/*
	 * The entry scan of the driving index - unless an IN list on that very
	 * column drives the groups instead (DESIGN.md §15), in which case its
	 * elements' lookups and containers, charged above, ARE the per-group work
	 * and the index's entries are never walked.
	 */
	if (groupidx != NULL && !groupdrive)
	{
		/*
		 * The entry scan walks ONE key column's entries and stops at the first
		 * entry of the next (DESIGN.md §24), so what it reads of a multicolumn
		 * index is that column's share of it and not the whole relation - and
		 * a range bounds the walk to drivefrac of those (§28), the share of
		 * the column's entries it selects.
		 */
		double		pergroup = lion_containers_for(heap_pages,
												   matching / Max(numgroups, 1.0));
		double		share = 1.0;

		seq_pages += Max(1.0, (double) groupidx->pages *
						 lion_index_column_share(root, rel, groupidx,
												 groupcol) * drivefrac);

		/*
		 * The count(DISTINCT k) walk over k's entries (DESIGN.md §26) tests
		 * each entry for ONE visible row and stops there, so it reads the
		 * share of each entry's intersection lion_exists_fraction() expects
		 * - one container or two when the entries are dense, all of it when
		 * the WHERE leaves most of them empty - and rechecks that share of
		 * the candidates.  Beside a GROUP BY this is the group's own test,
		 * which the pairs below come on top of.
		 */
		if (distinct != LION_DISTINCT_NONE &&
			(distinct == LION_DISTINCT_EXISTS || groupidx2 != NULL))
		{
			share = lion_exists_fraction(pergroup,
										 matching / Max(numgroups, 1.0));
			recheckshare = share;
		}
		ncontainers += numgroups * pergroup * share;
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
		pair_cost = Max(oe * ie, numgroups) * cpu_tuple_cost;
		merge_ops += Min(oe, ie) * tuples;
		ncontainers += oe * ie * (co + cinner);

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
	ncontainers = Max(ncontainers, 1.0);

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
	run += ncontainers * cpu_operator_cost * 2.0;	/* block mask + VM mask */
	run += merge_ops * cpu_operator_cost;
	run += recheck_pages * lion_heap_page_cost(root, rel, recheck_pages,
											  heap_pages);
	run += recheck_tids * cpu_tuple_cost;
	run += ingroups * cpu_tuple_cost;
	run += pair_cost;

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
					bool ranged, double drivefrac)
{
	Cost		run = 0;
	ListCell   *lc;

	foreach(lc, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);

		run += lion_cost_count_rel(root, t->rel,
								  t->driveidx[0], t->drivecol[0],
								  t->driveidx[1], t->drivecol[1],
								  t->whereidx, t->wherecol,
								  whereclauses, wherekinds, ors, numgroups,
								  outer_entries, inner_entries, distinct,
								  drivefrac, ranged ? t->drivevar[0] : NULL);

		/*
		 * A range-bounded walk (DESIGN.md §28) pays a fixed cost per entry it
		 * visits, in each relation it walks - a partition's entries are its
		 * own.  A count(DISTINCT) walk already pays its per-test cost for
		 * each of them (§26), which is the same work.
		 */
		if (ranged && distinct == LION_DISTINCT_NONE &&
			t->drivevar[0] != NULL)
			run += lion_range_entries(root, t->rel, t->drivevar[0],
									  drivefrac) * LION_RANGE_ENTRY_COST;
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
 * Cost the FK-side join (DESIGN.md §27) over the fact relation `rel`, for
 * `dimrows` dimension rows; the child plan's own cost is the caller's to add.
 *
 * What the node does per dimension row: one lookup of the key in the fk
 * index - a directory descent, whose leaf is charged at lion_heap_page_cost()'s
 * interpolated cost over as many distinct leaves as the lookups can touch,
 * because the dimension's rows arrive in its own order and not in key order -
 * then one count of that key's set ANDed with the fact filters: a merge set up
 * and torn down (LION_FKJOIN_COUNT_COST), the set's containers at §10's two
 * cpu_operator_cost each, a PROBE into each fact filter source at the
 * container keys the two have in common (LION_FKJOIN_PROBE_COST each), and
 * the set's chain pages, which are none at all when the fk entries are
 * INLINE.  Then a row out.
 *
 * A fact filter source that is a UNION - an IN list, or an OR across columns,
 * whose sets are the leaves' and their lists' elements together - is not
 * located once and then probed like one set: every count builds its k-way
 * union again (§15), so every count pays for each of its sets
 * (LION_FKJOIN_SET_COST) and for the union of their containers at the keys it
 * probes - lion_merge_ops() of the whole source, prorated to those keys.
 * That was the review's finding: charged as one set, a thousand-value IN list
 * over 300 dimension rows was chosen at 1.2 s against the hash join's 2 ms.
 *
 * The fact filters are located once for the whole scan and materialized on
 * their second use (§9), so each is one lookup and one walk of its chain, as
 * a single count prices it.
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
					 List *wherekinds, List *ors, double dimrows)
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
	double	   *srcmembers = (double *) palloc0(sizeof(double) * (nclause + nsrc + 1));
	double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		nd;
	double		perkey;
	double		share;
	double		height = 0;
	double		dirpages;
	double		container_pages;
	double		lookups;
	double		cfk;
	double		probes = 0;
	double		matched;
	double		recheck_tids;
	double		recheck_pages;
	Cost		run = 0;
	int			ci = 0;
	int			sno;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	dimrows = Max(dimrows, 1.0);

	/* How many rows one key of the fk column has, and in how many containers. */
	nd = estimate_num_groups(root, list_make1(fkvar), tuples, NULL, NULL);
	nd = Max(nd, 1.0);
	perkey = tuples / nd;
	cfk = lion_containers_for(heap_pages, perkey);

	/* ---- one lookup and one count per dimension row ---- */
	share = lion_index_column_share(root, rel, fkidx, fkcol);
	dirpages = lion_index_dir_pages(fkidx, &height);
	container_pages = Max(((double) fkidx->pages - 1.0 - dirpages) * share, 0.0);
	dirpages = Max(dirpages * share, 1.0);

	lookups = Min(dimrows, dirpages);
	run += lookups * lion_heap_page_cost(root, rel, lookups,
										 Max((double) fkidx->pages, 1.0));
	run += dimrows * (height + 1.0) * 50.0 * cpu_operator_cost;
	run += Min(dimrows * container_pages / nd, container_pages) * seq_page_cost;
	run += dimrows * LION_FKJOIN_COUNT_COST;

	/* ---- the fact filters: located once, each a source of every count ---- */
	forfour(lc1, t->whereidx, lc2, whereclauses, lc3, wherekinds,
			lc4, t->wherecol)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Node	   *clause = (Node *) lfirst(lc2);
		Selectivity sel;
		double		nkeys = 1.0;
		double		cshare;
		double		cdir;
		double		cheight = 0;
		double		cpages;

		if (ci == joinclause || !LION_CLAUSE_IS_POSITIVE(lfirst_int(lc3)))
		{
			ci++;
			continue;
		}

		sel = clause_selectivity(root, clause, 0, JOIN_INNER, NULL);
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

		/*
		 * Which source of the AND the clause's sets belong to: its OR's, or
		 * one of its own (numbered after the ORs).
		 */
		sno = (orgrp[ci] >= 0) ? orgrp[ci] : nsrc + ci;
		srcsets[sno] += nkeys;
		srcmembers[sno] += tuples * sel;
		ci++;

		/* Located once: a lookup per set, and the walk of their chains. */
		cshare = lion_index_column_share(root, rel, idx,
										 (AttrNumber) lfirst_int(lc4));
		cdir = lion_index_dir_pages(idx, &cheight);
		cpages = Max(((double) idx->pages - 1.0 - cdir) * cshare, 0.0);

		run += Min(nkeys, Max(cdir * cshare, 1.0)) * random_page_cost +
			nkeys * (cheight + 1.0) * 50.0 * cpu_operator_cost;
		run += Max(Min(nkeys, cpages), cpages * sel) * seq_page_cost;
	}

	/*
	 * Every count seeks each source at the container keys the fk set has, and
	 * a seek finds a container only where the source has one.  A union source
	 * builds its k-way union again at every count: each of its sets is set up
	 * (LION_FKJOIN_SET_COST), and the containers standing at the probed keys
	 * are merged - lion_merge_ops() of the whole source, prorated to the
	 * share of the heap's container keys the probes touch.
	 */
	for (sno = 0; sno < nclause + nsrc; sno++)
	{
		double		cs;

		if (srcsets[sno] <= 0.0)
			continue;
		cs = Min(cfk, lion_containers_for(heap_pages, srcmembers[sno]));
		probes += cs;
		if (srcsets[sno] > 1.0)
			run += dimrows *
				(srcsets[sno] * LION_FKJOIN_SET_COST +
				 lion_merge_ops(heap_pages, srcmembers[sno], srcsets[sno]) *
				 Min(cs / ckeys, 1.0) * cpu_operator_cost);
	}
	pfree(orgrp);
	pfree(srcsets);
	pfree(srcmembers);

	/*
	 * The containers: the fk set's own at every count, as §10 charges a
	 * container (a block mask and a visibility-map mask), and the probes into
	 * the fact filters' sets at the keys the fk set has.
	 */
	run += dimrows * cfk * cpu_operator_cost * 2.0;
	run += dimrows * probes * LION_FKJOIN_PROBE_COST;

	/* ---- the heap the visibility map cannot vouch for ---- */
	matched = Min(dimrows * perkey, tuples) * wheresel;
	recheck_tids = matched * dirtyfrac;
	recheck_pages = Min(recheck_tids, heap_pages * dirtyfrac);
	run += recheck_pages * lion_heap_page_cost(root, rel, recheck_pages,
											   heap_pages);
	run += recheck_tids * cpu_tuple_cost;

	/* ---- a partial row per dimension row ---- */
	run += dimrows * cpu_tuple_cost;

	return run;
}

/*
 * Is this expression a value the node can compare a column with - a literal,
 * or a parameter it evaluates at the start of the scan (DESIGN.md §10)?
 *
 * A Param is accepted wherever a Const is, which is what lets a prepared
 * statement's GENERIC plan reach the pushdown: the planner leaves `k = $1` as
 * a Param, the cost model uses its default selectivity, and the executor
 * evaluates it through the node's own ExprContext.  Both parameter kinds
 * qualify: PARAM_EXTERN for a prepared statement's own parameters and
 * PARAM_EXEC for the ones a nested loop or a LATERAL reference supplies,
 * which change between rescans.
 *
 * An ArrayExpr is accepted for the array of an IN list, because that is the
 * shape `k IN ($1, $2)` keeps in a generic plan, but only over literals and
 * parameters: the node evaluates the array ONCE per scan, and an element that
 * could be volatile does not mean the same thing evaluated once as it does
 * evaluated per row.
 */
static bool
lion_is_value_expr(Node *node, bool allow_array_expr)
{
	if (node == NULL)
		return false;
	if (IsA(node, Const))
		return true;
	if (IsA(node, Param))
	{
		Param	   *p = (Param *) node;

		return (p->paramkind == PARAM_EXTERN || p->paramkind == PARAM_EXEC);
	}
	if (allow_array_expr && IsA(node, ArrayExpr))
	{
		ArrayExpr  *a = (ArrayExpr *) node;
		ListCell   *lc;

		if (a->multidims || a->elements == NIL)
			return false;
		foreach(lc, a->elements)
		{
			if (!lion_is_value_expr(lion_strip((Node *) lfirst(lc)), false))
				return false;
		}
		return true;
	}
	return false;
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
 * One WHERE clause as the analysis below understands it: which column it
 * constrains, with what, and everything lion_match_index() will need in order
 * to find an index for it on each relation.
 */
typedef struct LionLeafInfo
{
	Var		   *var;
	Node	   *val;			/* the value expression, or a NULL placeholder */
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
 * bounds the entry walk that drives the count and is not a source at all: an
 * OR's arms are sources of one union, and a range would have to be the union
 * of its entries there.
 */
static bool
lion_analyze_leaf(Node *clause, Index rti, bool allow_negated,
				 bool allow_range, LionLeafInfo *out)
{
	memset(out, 0, sizeof(LionLeafInfo));
	out->opno = InvalidOid;
	out->cmptype = InvalidOid;
	out->extractquery = InvalidOid;
	out->collation = InvalidOid;

	if (clause == NULL)
		return false;

	if (IsA(clause, OpExpr))
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
			 *
			 * The QUERY, not just its value, decides whether the posting sets
			 * can answer this clause at all, so it has to be available now: a
			 * Param is refused here even though one is accepted for equality
			 * (DESIGN.md §17).  `tags @> $1` with `$1 = '{}'` extracts to ALL
			 * mode, which this node cannot answer - it has no way to recheck
			 * the operator against the heap - and by then there would be no
			 * plan left to fall back to.
			 */
			if (!IsA(left, Var) || !IsA(right, Const))
				return false;
			out->var = (Var *) left;
			out->val = right;
			if (((Const *) out->val)->constisnull)
				return false;

			/*
			 * Only an EXACT query is pushed down.  `tags @> '{}'`, `<@`, a
			 * tsquery with NOT/phrase/prefix/weights and anything with a NULL
			 * element all want every row rechecked in the heap, which is what
			 * the ordinary plan does anyway.
			 */
			if (!lion_multikey_query_is_exact(opfamily, lefttype, out->strategy,
											 (Const *) out->val,
											 &out->extractquery))
				return false;

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
	else if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;
		Node	   *arg;

		if (nt->argisrow)
			return false;
		if (nt->nulltesttype != IS_NULL && !allow_negated)
			return false;
		arg = lion_strip((Node *) nt->arg);
		if (arg == NULL || !IsA(arg, Var))
			return false;
		out->var = (Var *) arg;

		out->kind = (nt->nulltesttype == IS_NULL) ?
			LION_CLAUSE_NULL : LION_CLAUSE_NOTNULL;
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
 * treats them alike; inor says which are which.
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
	*whereclauses = lappend(*whereclauses, clause);
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
 * (A clause value is a literal or a parameter, possibly in an ARRAY[], and
 * calls nothing anyway.)
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
 * a strict equality never matches when NULL.
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

		if (v->varlevelsup != 0)
			return false;
		return (v->varno == fj->fkvar->varno &&
				v->varattno == fj->fkvar->varattno) ||
			(v->varno == fj->pkvar->varno &&
			 v->varattno == fj->pkvar->varattno);
	}
	return false;
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
 *	- the target list: the dimension's columns (in any non-volatile
 *	  expression) and count aggregates, nothing of the fact rel's.  The groups
 *	  are the dimension's and are formed by core's Finalize Agg, so neither
 *	  the grouping-equality rule nor the value-representation rule of §10 has
 *	  anything to check: no index drives the groups and no index key is
 *	  printed;
 *	- the path: a CustomPath whose child is the dimension's cheapest path and
 *	  whose target is the partially-grouped one, under a Finalize Agg - hashed
 *	  for a GROUP BY, sorted over no columns for a GROUP BY the planner folded
 *	  to constants (an empty join has no group then), plain without one.
 */
static void
lion_try_fkjoin_path(PlannerInfo *root, RelOptInfo *rel,
					 RelOptInfo *output_rel, GroupPathExtraData *extra,
					 const LionFkJoin *fj, List *having,
					 List *whereattnos, List *clauseinfos, List *whereclauses,
					 List *whereconsts, List *wherekinds, List *whereopnos,
					 List *whereinor, List *ors)
{
	Query	   *parse = root->parse;
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	List	   *exprs;
	List	   *items;
	LionLeafInfo leaf;
	LionDriveInfo nodrive[LION_MAX_GROUPCOLS];
	int			joinclause;
	List	   *targets = NIL;
	LionCountTarget *first;
	PathTarget *partialtarget;
	CustomPath *cpath;
	List	   *oids;
	List	   *ints;
	List	   *consts = NIL;
	List	   *ckinds = NIL;
	double		dimrows;
	double		numgroups;
	AggStrategy aggstrategy;
	AggClauseCosts agg_final_costs;
	bool		haveagg = false;
	ListCell   *lc;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	int			i;

	/*
	 * The grouping is done above the node, from partial counts, so the
	 * planner has to consider the aggregates splittable and a GROUP BY has to
	 * be hashable (a sorted Finalize Agg would need a Sort over the node,
	 * which v1 does not build).
	 */
	if (extra == NULL || (extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0)
		return;
	if (root->processed_groupClause != NIL &&
		!grouping_is_hashable(root->processed_groupClause))
		return;

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
			if (!lion_fkjoin_agg_is_count((Aggref *) node, fj))
				return;
			haveagg = true;
		}
		else
			return;
	}
	if (!haveagg)
		return;

	/*
	 * An IN list whose array is a parameter has no length until the executor
	 * has it (§15), and every count of this node rebuilds the list's union -
	 * so its work per dimension row is proportional to a length the cost model
	 * cannot see.  A single table answers such a list once; here a 43,000-value
	 * `= ANY ($1)` over 20,000 dimension rows ran for minutes (the 2026-09-23
	 * review), so the shape is refused.  Literal lists and `IN ($1, $2)` have
	 * their length at plan time and are priced per set.
	 */
	forboth(l1, wherekinds, l2, whereconsts)
	{
		Node	   *val = (Node *) lfirst(l2);

		if (lfirst_int(l1) == LION_CLAUSE_ARRAY &&
			!IsA(val, Const) && !IsA(val, ArrayExpr))
			return;
	}

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

	/* ---- the path ---- */
	dimrows = clamp_row_est(fj->dimpath->rows);
	partialtarget = lion_make_partial_target(root, output_rel->reltarget,
											 having);

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

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = partialtarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = list_make1(fj->dimpath);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = list_make1(list_make2_int(LION_PRIV_MAGIC,
													  LION_PRIV_NMEMBERS));
	cpath->custom_private = lappend(cpath->custom_private, oids);
	cpath->custom_private = lappend(cpath->custom_private, ints);
	cpath->custom_private = lappend(cpath->custom_private, consts);
	cpath->custom_private = lappend(cpath->custom_private, ckinds);
	cpath->custom_private = lappend(cpath->custom_private, NIL);	/* parts */
	cpath->custom_private = lappend(cpath->custom_private, whereopnos);
	cpath->custom_private = lappend(cpath->custom_private, ors);
	cpath->custom_private = lappend(cpath->custom_private, NIL);	/* having */
	cpath->custom_private = lappend(cpath->custom_private, NIL);	/* distinct */
	cpath->custom_private = lappend(cpath->custom_private,
									list_make1_int(joinclause));
	/* the dimension's GROUP BY is the Finalize Agg's, which checks it */
	cpath->custom_private = lappend(cpath->custom_private,
									lion_replaced_functions(rel,
															output_rel->reltarget->exprs,
															having, NIL, fj));
	cpath->methods = &lion_count_path_methods;

	/*
	 * The node streams one partial row per dimension row that has fact rows,
	 * so it starts when its child does, and it costs the child plus what it
	 * does per dimension row.  At most one row per dimension row comes out;
	 * the dimension key is unique, so that is also at most one per fk value.
	 */
	cpath->path.rows = dimrows;
	cpath->path.startup_cost = fj->dimpath->startup_cost;
	cpath->path.total_cost = fj->dimpath->total_cost +
		lion_cost_fkjoin_rel(root, rel, first, fj->fkvar, joinclause,
							 whereclauses, wherekinds, ors, dimrows);
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = fj->dimpath->disabled_nodes;
#endif

	/* ---- the Finalize Agg that groups the partial counts ---- */
	if (root->processed_groupClause != NIL)
	{
		aggstrategy = AGG_HASHED;
		numgroups = estimate_num_groups(root,
										get_sortgrouplist_exprs(root->processed_groupClause,
																root->processed_tlist),
										dimrows, NULL, NULL);
	}
	else
	{
		aggstrategy = (parse->groupClause != NIL) ? AGG_SORTED : AGG_PLAIN;
		numgroups = 1.0;
	}

	MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);

	add_path(output_rel, (Path *)
			 create_agg_path(root, output_rel, &cpath->path,
							 output_rel->reltarget,
							 aggstrategy, AGGSPLIT_FINAL_DESERIAL,
							 root->processed_groupClause,
							 having,
							 &agg_final_costs,
							 numgroups));
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
		 * ---- an OR across columns (DESIGN.md §19) ----
		 *
		 * Every arm has to be a positive clause the posting sets can answer,
		 * or an AND of such clauses, each on a column of this relation.  The
		 * whole restriction then becomes ONE source - the union of the arms -
		 * which is ANDed with the other sources and with the GROUP BY driver
		 * like any other.  A negated arm (`IS NOT NULL`, NOT) is declined:
		 * the complement of a posting set is not a posting set, and under a
		 * union there is nothing to subtract it from.
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
			BoolExpr   *orexpr = (BoolExpr *) clause;
			List	   *armlens = NIL;
			int			first = list_length(whereattnos);
			ListCell   *la;

			if (list_length(orexpr->args) < 2)
				return;

			foreach(la, orexpr->args)
			{
				Node	   *arm = (Node *) lfirst(la);
				int			nleaf = 0;

				if (IsA(arm, BoolExpr) &&
					((BoolExpr *) arm)->boolop == AND_EXPR)
				{
					ListCell   *lb;

					foreach(lb, ((BoolExpr *) arm)->args)
					{
						if (!lion_analyze_leaf((Node *) lfirst(lb), rti, false,
											  false, &leaf))
							return;
						lion_append_clause(&leaf, (Node *) lfirst(lb), true,
										  &whereattnos, &clauseinfos,
										  &whereclauses, &whereconsts,
										  &wherekinds, &whereopnos,
										  &whereinor);
						nleaf++;
					}
					if (nleaf == 0)
						return;
				}
				else
				{
					if (!lion_analyze_leaf(arm, rti, false, false, &leaf))
						return;
					lion_append_clause(&leaf, arm, true,
									  &whereattnos, &clauseinfos,
									  &whereclauses, &whereconsts,
									  &wherekinds, &whereopnos, &whereinor);
					nleaf = 1;
				}

				armlens = lappend_int(armlens, nleaf);
			}

			ors = lappend(ors,
						  list_concat(list_make2_int(first,
													 list_length(armlens)),
									  armlens));
			havepositive = true;
			continue;
		}

		if (!lion_analyze_leaf(clause, rti, true, true, &leaf))
			return;

		/*
		 * A range comparison (DESIGN.md §28) is not a source: it bounds the
		 * entry walk that DRIVES the count, which is decided once everything
		 * else is known.  Any number of them may name the one column - they
		 * are ANDed into one walk - and none may name another; the column
		 * must have no positive clause of its own besides (checked below,
		 * once all of them are known).  A strict comparison is never true of
		 * NULL, so the column is non-NULL in every row counted.
		 */
		if (leaf.kind == LION_CLAUSE_RANGE)
		{
			if (rangevar != NULL && rangevar->varattno != leaf.var->varattno)
				return;
			rangevar = leaf.var;
			rangeclauses = lappend(rangeclauses, rinfo);
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
			havepositive = true;

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
	 * A range and an equality, a list or a null test on one column are two
	 * positive clauses on it, which §10 leaves to the ordinary plan.  And a
	 * range is never a source (DESIGN.md §28): the FK-side join's fact
	 * filters all are, ANDed with every fk set, so it declines one.
	 */
	if (rangevar != NULL &&
		(list_member_int(posattnos, (int) rangevar->varattno) || fj != NULL))
		return;

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
		if (partitioned || ngroup > 1)
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
		else if (IsA(node, Aggref))
		{
			Aggref	   *agg = (Aggref *) node;
			AttrNumber	countcols[LION_MAX_GROUPCOLS + 1];
			int			ncountcols = ngroup;

			if (agg->aggdistinct != NIL)
			{
				/* checked above; a distinct count needs no other test */
				haveagg = true;
				continue;
			}

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
				Node	   *arg = (agg->args != NIL) ?
					lion_strip((Node *) ((TargetEntry *) linitial(agg->args))->expr) :
					NULL;
				bool		ofk = (arg != NULL && IsA(arg, Var) &&
								   ((Var *) arg)->varattno == distvar->varattno);

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
	 * The range has to bound the walk that DRIVES the count (DESIGN.md §28):
	 * k's entries under a GROUP BY k, a count(DISTINCT k) or the sum above,
	 * or g's under `g, count(DISTINCT k) ... GROUP BY g`.  Anywhere else it
	 * would have to be intersected with the driver as a source - the union of
	 * however many entries the range holds - which is declined, and so is a
	 * range beside a two-column GROUP BY.
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
	 *	- and a NULLABLE group column, because the reserved NULL entry sorts
	 *	  FIRST and `ORDER BY col` means NULLS LAST.  A column the planner
	 *	  knows is NOT NULL has no NULL group to emit, so the two agree.
	 */
	cpath->path.pathkeys = NIL;
	if (ngroup == 1 && !partitioned && !sumall && !singlegroup &&
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
						rangevar != NULL, rangesel);

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

	lion_try_count_path(root, input_rel, output_rel,
					   (GroupPathExtraData *) extra, NULL);
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
 * it per row.  HAVING is the Finalize Agg's, so there is no qual here.
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
	int			joinclause;
	Node	   *keyexpr;
	Var		   *keyvar;
	ListCell   *lc;

	if (list_length(custom_plans) != 1)
		elog(ERROR, "LionCount: a join needs exactly one child plan");
	child = (Plan *) linitial(custom_plans);

	joinclause = linitial_int((List *) list_nth(best_path->custom_private,
												LION_PRIV_JOIN));
	consts = (List *) list_nth(best_path->custom_private, LION_PRIV_CONSTS);
	keyexpr = (Node *) list_nth(consts, joinclause);
	keyvar = (Var *) lion_strip(keyexpr);
	Assert(keyvar != NULL && IsA(keyvar, Var));

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

		if (IsA(expr, Var))
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
		list_make2_int(joinclause, (int) lion_child_resno(child, keyvar));

	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->custom_private = lappend(priv, kinds);
	cscan->methods = &lion_count_scan_methods;

	return &cscan->scan.plan;
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
	 * written out or read back; plan time is the first moment that can
	 * happen, and registering twice is harmless.
	 */
	if (GetCustomScanMethods("LionCount", true) == NULL)
		RegisterCustomScanMethods(&lion_count_scan_methods);

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

			/*
			 * count(*) and every count(col) the planner accepted are the
			 * count of the group, except two cases DESIGN.md §14 spells out:
			 * count of the group column is 0 in the NULL group, and count of
			 * a column a clause pins to NULL is always 0.
			 */
			kind = LION_TL_COUNT;
			if (agg->aggdistinct != NIL)
			{
				/* the one column the planner accepted (DESIGN.md §26) */
				Assert(distattno != 0);
				kind = LION_TL_COUNT_DISTINCT;
			}
			else if (agg->args != NIL)
			{
				Node	   *arg = lion_strip((Node *)
											((TargetEntry *) linitial(agg->args))->expr);
				AttrNumber	attno;
				int			i;

				Assert(arg != NULL && IsA(arg, Var));
				attno = ((Var *) arg)->varattno;

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
 */
static AttrNumber
lion_index_col_for(Relation index, AttrNumber heapattno)
{
	int			c;

	for (c = 0; c < IndexRelationGetNumberOfKeyAttributes(index); c++)
	{
		if (index->rd_index->indkey.values[c] == heapattno)
			return (AttrNumber) (c + 1);
	}

	elog(ERROR, "lion index \"%s\" does not index column %d of \"%s\"",
		 RelationGetRelationName(index), (int) heapattno,
		 get_rel_name(index->rd_index->indrelid));
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

	st->heap = table_open(heapoid, NoLock);
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
												  st->clause[i].attno));
	}

	/*
	 * The driving index's key column comes from the index that was really
	 * opened rather than from the plan (DESIGN.md §24): the planner only has
	 * to be right about WHICH index, and a partition's own index may put the
	 * same heap column at a different position from the parent's.
	 */
	if (OidIsValid(groupidxoid))
	{
		st->groupidx = index_open(groupidxoid, AccessShareLock);
		st->groupidxcol =
			lion_index_col_for(st->groupidx,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->driveattno));
	}
	if (OidIsValid(groupidxoid2))
	{
		st->groupidx2 = index_open(groupidxoid2, AccessShareLock);
		st->groupidxcol2 =
			lion_index_col_for(st->groupidx2,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->innerattno));
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
	if (join != NIL)
	{
		if (list_length(join) != 2 ||
			list_length(cscan->custom_plans) != 1)
			elog(ERROR, "LionCount: malformed join");
		st->joinclause = linitial_int(join);
		st->joinkeyresno = (AttrNumber) lsecond_int(join);
		if (st->joinclause < 0 || st->joinclause >= st->nclause ||
			st->joinkeyresno <= 0)
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
		 * straight from the Const; anything else - a Param, or the ARRAY[]
		 * of a generic IN list - gets an ExprState and is evaluated at the
		 * start of each scan (lion_eval_clause_values()).
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
	if (st->joinclause >= 0)
	{
		st->child = ExecInitNode((Plan *) linitial(cscan->custom_plans),
								 estate, eflags);
		node->custom_ps = list_make1(st->child);
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
}

/*
 * Evaluate the clause values that are not literals (DESIGN.md §10).
 *
 * Called once per scan, before anything is looked up, and again after every
 * ReScan: a nested loop or a LATERAL reference sets a new exec Param between
 * the two, and the node has to see the new value.  A generic prepared plan's
 * PARAM_EXTERN is constant for the statement but is still only available
 * here.
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

	*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(nelems, 1));

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
 * Locate the posting sets of one multi-key clause (DESIGN.md §17).
 *
 * The query is extracted again here, with the index's OWN extractQuery
 * function - which lion_match_index() has already insisted is the one the
 * planner used - so the keys and the boolean tree are the same the plan was
 * costed with.  The tree becomes the source's combining expression; the
 * merge in lion_count.c evaluates it over the sets with the same cursors it
 * uses for an IN list, so the DESIGN.md §9 pin discipline is unchanged.
 */
static int
lion_locate_multikey(LionClauseState *cl, LionPostingSet **sets,
					LionKeyNode **tree)
{
	LionState   *istate = lion_index_column_state(cl->idx, cl->idxcol);
	LionQuery	q;
	int			i;

	lion_extract_query(istate, cl->val,
					  (StrategyNumber) get_op_opfamily_strategy(cl->opno,
																cl->idx->rd_opfamily[cl->idxcol - 1]),
					  &q);

	/*
	 * The plan was only made because this extraction came out exact
	 * (lion_multikey_query_is_exact()), against this very function and this
	 * very constant.  A different answer now would mean the count could
	 * silently miss rows, so say so instead.
	 */
	if (q.mode != LION_QMODE_KEYS)
		elog(ERROR, "roaring count: query for index \"%s\" is no longer exact",
			 RelationGetRelationName(cl->idx));

	*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(q.nkeys, 1));
	*tree = q.tree;

	for (i = 0; i < q.nkeys; i++)
	{
		(void) lion_posting_set_lookup_col(cl->idx, cl->idxcol, q.keys[i],
										  InvalidOid, &(*sets)[i]);
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
	LionKeyNode **arms;
	int			narms = 0;
	int			total = 0;
	int			off = 0;
	int			leaf = 0;
	int			i;
	int			j;

	leafsets = (LionPostingSet **)
		palloc0(sizeof(LionPostingSet *) * orst->nleaves);
	leaftree = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * orst->nleaves);
	leafn = (int *) palloc0(sizeof(int) * orst->nleaves);

	for (i = 0; i < orst->nleaves; i++)
	{
		leaftree[i] = lion_locate_leaf(&st->clause[orst->first + i],
									  &leafsets[i], &leafn[i]);
		total += leafn[i];
		CHECK_FOR_INTERRUPTS();
	}

	/* One array for the whole source, with every leaf's tree moved onto it. */
	src->sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) *
										   Max(total, 1));
	src->nsets = total;
	src->nomaterialize = true;
	for (i = 0; i < orst->nleaves; i++)
	{
		if (leafn[i] > 0)
			memcpy(&src->sets[off], leafsets[i],
				   sizeof(LionPostingSet) * leafn[i]);
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
 * Locate the posting sets of every WHERE clause of the relation the node is
 * counting.  They keep their pins (for INLINE entries) until
 * lion_release_where(), which is exactly the DESIGN.md section 9 discipline
 * applied for the length of that relation's processing rather than for one
 * container.  With partitions that is one partition's turn; with a plain
 * table it is the whole node execution.
 */
static void
lion_locate_where(LionCountScanState *st)
{
	MemoryContext oldcxt;
	int			k;

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

		if (st->item[k].orno >= 0)
		{
			lion_locate_or(st, &st->ors[st->item[k].orno], src);
			continue;
		}

		src->negated = (cl->kind == LION_CLAUSE_NOTNULL);
		src->tree = lion_locate_leaf(cl, &src->sets, &src->nsets);

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
	}
	if (st->wherecxt != NULL)
		MemoryContextReset(st->wherecxt);
	st->located = false;
	st->wheremissing = false;
	st->ingroupitem = -1;
	st->ingroupset = 0;
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

		/* A dimension column of the FK-side join, from the child's row. */
		if (LION_TL_IS_CHILDCOL(kind))
		{
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
 * The intersection of the WHERE clauses, with no index driving the count.
 */
static int64
lion_count_relation(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;
	int64		count;

	Assert(st->nitem > 0);
	if (st->wheremissing)
		return 0;

	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);
	count = lion_count_sources_cached(st->heap, estate->es_snapshot,
									 st->nitem, &st->sources[1],
									 &st->stats, st->viscache,
									 st->rel_read_only);
	MemoryContextSwitchTo(oldcxt);

	return count;
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
 * The caller owns the entry scan (this walks it to the end) and every posting
 * set it takes is released before the next one is located, so the §9 pin
 * budget is one entry's.
 */
static int64
lion_sumall_relation(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	LionCountSource *sources;
	MemoryContext oldcxt;
	int64		total = 0;
	int			nsource;
	Datum		key;

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

	lion_entry_scan_begin_range(&st->escan, st->groupidx, st->groupidxcol,
								st->hasrange ? &st->range : NULL);
	st->scanning = true;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (!lion_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}

		/* The NULL entry's rows are the ones `IS NOT NULL` excludes. */
		if (st->sumallitem >= 0 && st->groupset.keyisnull)
		{
			lion_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
			continue;
		}

		total += lion_count_sources_cached(st->heap, estate->es_snapshot,
										  nsource, sources,
										  &st->stats, st->viscache,
										  st->rel_read_only);
		st->stats.sets_summed++;
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
	}

	lion_entry_scan_end(&st->escan);
	st->scanning = false;
	return total;
}

/*
 * The next group of the relation the node has open, as one row.
 *
 * Returns NULL and sets *exhausted once the relation's entry scan has run
 * out; every other return is a row.  This is the streaming group loop of
 * DESIGN.md §10, shared by the single-table path and by each partition of a
 * partitioned one (§16), which is why nothing here knows about partitions:
 * the caller has opened one relation and located its WHERE clauses.
 */
static TupleTableSlot *
lion_next_group(LionCountScanState *st, bool *exhausted)
{
	EState	   *estate = st->css.ss.ps.state;
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

		if (!lion_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			*exhausted = true;
			return NULL;
		}

		count = lion_count_sources_cached(st->heap, estate->es_snapshot,
										 st->nsource, st->sources,
										 &st->stats, st->viscache,
										 st->rel_read_only);
		keyisnull = st->groupset.keyisnull;
		lion_posting_set_release(&st->groupset);
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
 * located once for this relation, with their pins, by lion_locate_where(); the
 * group loop neither takes nor releases any, and the key it emits is the copy
 * the set already holds, which outlives the row.
 */
static TupleTableSlot *
lion_next_group_inlist(LionCountScanState *st, bool *exhausted)
{
	EState	   *estate = st->css.ss.ps.state;
	LionCountSource *src = &st->sources[st->ingroupitem + 1];
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

		Assert(ps->hasstoredkey && !ps->keyisnull);

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		st->dsources[0].nsets = 1;
		st->dsources[0].sets = ps;
		st->dsources[0].tree = NULL;
		st->dsources[0].negated = false;
		st->dsources[0].nomaterialize = false;
		st->dsources[0].disjoint = false;

		count = lion_count_sources_cached(st->heap, estate->es_snapshot,
										 st->ndsource, st->dsources,
										 &st->stats, st->viscache,
										 st->rel_read_only);
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
 * held - with its pin - for the whole of its inner loop, in outercxt; each
 * pair's inner set is located, counted and released inside pergroup, so at
 * most two group pins exist at a time however many distinct values either
 * column has.  The inner keys were read once per relation into innercxt
 * (lion_load_inner_keys()); when they did not fit its budget, innerkey is
 * NULL and the inner index's entry scan is walked once per outer group
 * instead, which holds one pin at a time as well.
 */
static TupleTableSlot *
lion_next_group2(LionCountScanState *st, bool *exhausted)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		Datum		ikey = (Datum) 0;
		bool		ikeyisnull;
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
			if (!st->scanning2)
			{
				lion_entry_scan_begin_col(&st->escan2, st->groupidx2,
										 st->groupidxcol2);
				st->scanning2 = true;
			}
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

		count = lion_count_sources_cached(st->heap, estate->es_snapshot,
										 st->nsource, st->sources,
										 &st->stats, st->viscache,
										 st->rel_read_only);
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
	EState	   *estate = st->css.ss.ps.state;

	st->disttests++;
	if (count)
		return lion_count_sources_cached(st->heap, estate->es_snapshot,
										 nsource, sources, &st->stats,
										 st->viscache, st->rel_read_only);
	return lion_exists_sources_cached(st->heap, estate->es_snapshot,
									  nsource, sources, &st->stats,
									  st->viscache, st->rel_read_only) ? 1 : 0;
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

/*
 * The FK-side join (DESIGN.md §27): the next dimension row with fact rows, as
 * one partial row - its dimension columns and its count.
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
 * an inner join has no pair for it.
 *
 * The row is PARTIAL: core's Finalize Agg above groups them by the dimension
 * columns and adds the counts, which is the join's count for each group
 * because that count is a sum over the group's dimension rows.
 */
static TupleTableSlot *
lion_next_join_row(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	LionClauseState *jcl = &st->clause[st->joinclause];
	MemoryContext oldcxt;

	/* A fact clause that selects nothing leaves no dimension row a count. */
	if (st->wheremissing)
	{
		st->done = true;
		return NULL;
	}

	for (;;)
	{
		TupleTableSlot *childslot;
		Datum		key;
		bool		isnull;
		bool		found;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		childslot = ExecProcNode(st->child);
		if (TupIsNull(childslot))
		{
			st->childslot = NULL;
			st->done = true;
			return NULL;
		}

		key = slot_getattr(childslot, st->joinkeyresno, &isnull);
		if (isnull)
			continue;
		st->joinlookups++;

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		found = lion_posting_set_lookup_col(jcl->idx, jcl->idxcol, key,
										   jcl->valtype, &st->groupset);
		if (!found)
		{
			lion_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
			st->joinmissing++;
			continue;
		}

		count = lion_count_sources_cached(st->heap, estate->es_snapshot,
										 st->nsource, st->sources,
										 &st->stats, st->viscache,
										 st->rel_read_only);
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);

		if (count == 0)
			continue;

		st->childslot = childslot;
		return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, count);
	}
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
	lion_release_where(st);

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

static void
lion_end_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;

	lion_reset_run(st);

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
	if (st->viscache != NULL)
	{
		lion_vis_cache_destroy(st->viscache);
		st->viscache = NULL;
	}
}

/*
 * "col = 3", "col = ANY ('{1,2,3}')", "col IS NULL", "col IS NOT NULL",
 * "tags @> {a,b}", "tsv @@ 'a' & 'b'", "col >= 10" (a range, DESIGN.md §28,
 * with the column on the left whichever side the query had it on).
 *
 * A clause whose value is not a literal is printed as the expression the plan
 * carries, which for a prepared statement's parameter is `$1` - the same text
 * core's EXPLAIN gives a qual on one (DESIGN.md §10).
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
				Oid			outfunc;
				bool		isvarlena;
				char	   *val;

				if (cl->con == NULL)
				{
					List	   *context =
						set_deparse_context_plan(es->deparse_cxt,
												 st->css.ss.ps.plan,
												 ancestors);

					/*
					 * The FK-side join's key is a column of the OTHER table
					 * (DESIGN.md §27), so it is printed qualified: `fk =
					 * d.pk`, as core prints a join clause.
					 */
					val = deparse_expression((Node *) cl->valexpr, context,
											 st->joinclause >= 0 &&
											 cl == &st->clause[st->joinclause],
											 false);
				}
				else
				{
					getTypeOutputInfo(cl->con->consttype, &outfunc, &isvarlena);
					val = OidOutputFunctionCall(outfunc, cl->con->constvalue);
				}
				if (cl->kind == LION_CLAUSE_ARRAY)
					appendStringInfo(buf, "%s = ANY (%s)", attname, val);
				else if (cl->kind == LION_CLAUSE_MULTI ||
						 cl->kind == LION_CLAUSE_RANGE)
				{
					char	   *opname = get_opname(cl->opno);

					appendStringInfo(buf, "%s %s %s", attname, opname, val);
					pfree(opname);
				}
				else
					appendStringInfo(buf, "%s = %s", attname, val);
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
lion_explain_col(Oid idxoid, AttrNumber heapattno)
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

	col = lion_index_col_for(idx, heapattno);
	snprintf(buf, sizeof(buf), ".%s",
			 NameStr(TupleDescAttr(RelationGetDescr(idx), col - 1)->attname));
	index_close(idx, AccessShareLock);

	return buf;
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
											  st->driveattno));
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
												  st->innerattno));
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
						 lion_explain_col(cl->idxoid, cl->attno));
		lion_explain_clause(st, cl, ancestors, es, &buf);
		appendStringInfoChar(&buf, ')');
	}

	for (i = 0; i < st->nitem; i++)
	{
		int			orno = st->item[i].orno;

		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");

		if (orno < 0)
		{
			if (st->npart == 0)
			{
				LionClauseState *cl = &st->clause[st->item[i].clauseno];

				appendStringInfo(&buf, "%s%s ", get_rel_name(cl->idxoid),
								 lion_explain_col(cl->idxoid, cl->attno));
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
									 lion_explain_col(cl->idxoid, cl->attno));
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

	if (st->hasgroupidx && st->groupattno != 0)
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
		ExplainPropertyInteger("Heap Blocks Skipped via VM", NULL,
							   st->stats.blocks_skipped_via_vm, es);
		ExplainPropertyInteger("Heap TIDs Rechecked", NULL,
							   st->stats.tids_rechecked, es);
		ExplainPropertyInteger("Heap Blocks Rechecked", NULL,
							   st->stats.blocks_rechecked, es);
		ExplainPropertyInteger("Containers Visited", NULL,
							   st->stats.containers_visited, es);
		/*
		 * Probes the AND merge did not make because the container key was
		 * already ruled out (DESIGN.md §25).  Each one is a seek into another
		 * source - a descent, or a step or two right - that the merge used to
		 * make before it knew whether anything survived at that key.
		 */
		ExplainPropertyInteger("Probes Avoided", NULL,
							   st->stats.probes_avoided, es);
		ExplainPropertyInteger("Heap Blocks From Cache", NULL,
							   st->stats.cache_hits, es);
		ExplainPropertyInteger("Heap Blocks Past Cache Budget", NULL,
							   st->stats.cache_full, es);
		/*
		 * Posting sets counted on their own and added up instead of merged:
		 * the disjoint-sum short-circuit of DESIGN.md §15.  Zero means every
		 * container key went through the k-way union, which is what an IN list
		 * ANDed with another clause, and every multi-key clause, still do.
		 */
		ExplainPropertyInteger("Posting Sets Summed", NULL,
							   st->stats.sets_summed, es);
		/*
		 * Directory pages - leaves and internal pages both - this node read
		 * (DESIGN.md §21).  A sorted IN list should cost about the leaves its
		 * values live on plus one descent, not a descent per value.
		 */
		ExplainPropertyInteger("Directory Pages Read", NULL, st->dirpages, es);

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
			ExplainPropertyInteger("Join Keys Looked Up", NULL,
								   st->joinlookups, es);
			ExplainPropertyInteger("Join Keys Without Entry", NULL,
								   st->joinmissing, es);
		}
	}
}
