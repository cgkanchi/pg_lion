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
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "parser/parse_oper.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
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
/* LION_TL_WHEREKEY + i: the key stored in the i'th clause's entry */
#define LION_TL_WHEREKEY		6

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

#define LION_CLAUSE_IS_POSITIVE(k)	((k) != LION_CLAUSE_NOTNULL)

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

/* Flag bits of the third integer of LION_PRIV_INTS. */
#define LION_FLAG_SINGLEGROUP	0x01
#define LION_FLAG_SUMALL			0x02
#define LION_FLAG_GROUPIDX		0x04	/* an index drives the entry scan */

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
 *	8	IntList: LION_TL_* for each custom_scan_tlist column (added at plan
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
#define LION_PRIV_TLKINDS	8

/*
 * Shape of the list above: "RBI" and a shape version, and its length.  Shape
 * 2 dropped the group column member, which only the cross-partition hash
 * merge needed (DESIGN.md §16: the node emits partial aggregates now).  Shape
 * 3 moved the clause values out of member 3 and into custom_exprs, so that a
 * Param among them reaches setrefs.c and SS_finalize_plan() (DESIGN.md §10).
 * Shape 4 added the OR structure of DESIGN.md §19, and shape 5 the second
 * GROUP BY column of DESIGN.md §20.
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
#define LION_PRIV_MAGIC		0x52424906
#define LION_PRIV_NMEMBERS	9

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
	bool		singlegroup;	/* GROUP BY over constant columns only */
	bool		sumall;			/* no GROUP BY, but every entry of the group
								 * index is counted and summed (DESIGN.md §14,
								 * `col IS NOT NULL` with nothing else) */
	bool		hasgroupidx;	/* an index's entries drive the count */
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
	 * Directory pages this node's execution has read (DESIGN.md §21), as the
	 * difference of the process-wide counter across each ExecCustomScan call.
	 * It is what EXPLAIN ANALYZE prints as "Directory Pages Read", and what
	 * test/sql/directory.sql uses to prove that a sorted IN list costs one
	 * pass over the leaves it crosses instead of a descent per value.
	 */
	int64		dirpages;
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
 */
static bool
lion_type_equalimage(Oid typid, Oid collation)
{
	TypeCacheEntry *typentry;
	Oid			proc;

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

	if (OidIsValid(opno) &&
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
lion_agg_is_count(Aggref *agg, Index rti, RelOptInfo *rel,
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

	return bms_is_member(var->varattno, rel->notnullattnums);
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
 * How many containers a posting set of `members` members can span: one per
 * LION_BLOCKS_PER_CONTAINER heap pages, and never more than one per member.
 */
static double
lion_containers_for(double heap_pages, double members)
{
	return Max(1.0, Min(heap_pages / LION_BLOCKS_PER_CONTAINER, members));
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
 * Returns the clause index of the list, or -1 when neither applies.
 */
static int
lion_inlist_shape(IndexOptInfo *groupidx, AttrNumber groupcol,
				 IndexOptInfo *groupidx2,
				 List *whereidx, List *wherecol, List *whereclauses,
				 List *wherekinds,
				 List *ors, bool *sumshort, bool *groupdrive)
{
	int			nclause = list_length(whereclauses);
	bool	   *inor = lion_or_leaf_map(ors, nclause);
	IndexOptInfo *arrayidx = NULL;
	AttrNumber	arraycol = 1;
	int			arrayci = -1;
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
		if (!inor[ci] && LION_CLAUSE_IS_POSITIVE(lfirst_int(lc3)))
		{
			npos++;
			if (IsA((Node *) lfirst(lc2), ScalarArrayOpExpr))
			{
				if (arrayci >= 0)
					arrayci = -2;	/* two lists: neither is "the" one */
				else if (arrayci == -1)
				{
					arrayci = ci;
					arrayidx = (IndexOptInfo *) lfirst(lc1);
					arraycol = (AttrNumber) lfirst_int(lc4);
				}
			}
		}
		ci++;
	}
	pfree(inor);

	if (arrayci < 0)
		return -1;

	/*
	 * The list drives the groups only when its entries ARE the groups: the
	 * same index AND the same key column (DESIGN.md §24).  Two columns of one
	 * multicolumn index are two independent sets of entries, so a list on `b`
	 * says nothing about the groups of `a` - which is also how the executor
	 * decides it (lion_locate_where(), by index and by heap attno).
	 */
	if (groupidx == NULL && groupidx2 == NULL && ors == NIL && npos == 1)
		*sumshort = true;
	else if (groupidx != NULL && groupidx2 == NULL && arrayidx != NULL &&
			 arrayidx->indexoid == groupidx->indexoid &&
			 arraycol == groupcol)
		*groupdrive = true;
	else
		return -1;

	return arrayci;
}

static Cost
lion_cost_count_rel(PlannerInfo *root, RelOptInfo *rel,
				   IndexOptInfo *groupidx, AttrNumber groupcol,
				   IndexOptInfo *groupidx2, AttrNumber groupcol2,
				   List *whereidx, List *wherecol, List *whereclauses,
				   List *wherekinds,
				   List *ors, double numgroups,
				   double outer_entries, double inner_entries)
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
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	inlistci = lion_inlist_shape(groupidx, groupcol, groupidx2,
								whereidx, wherecol, whereclauses,
								wherekinds, ors, &sumshort, &groupdrive);

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

			nkeys = Max(estimate_array_length(root,
											  (Node *) lsecond(saop->args)),
						1.0);
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
		 * index is that column's share of it and not the whole relation.
		 */
		seq_pages += Max(1.0, (double) groupidx->pages *
						 lion_index_column_share(root, rel, groupidx,
												 groupcol));
		ncontainers += numgroups *
			lion_containers_for(heap_pages, matching / Max(numgroups, 1.0));
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

		seq_pages += Max(1.0, (double) groupidx2->pages *
						 lion_index_column_share(root, rel, groupidx2,
												 groupcol2));
		pair_cost = Max(oe * ie, numgroups) * cpu_tuple_cost;
		merge_ops += Min(oe, ie) * tuples;
		ncontainers += oe * ie * (lion_containers_for(heap_pages, tuples / oe) +
								  lion_containers_for(heap_pages, tuples / ie));
	}
	ncontainers = Max(ncontainers, 1.0);

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
	recheck_tids = matching * dirtyfrac;
	recheck_pages = Min(recheck_tids, dirty_pages);

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
					double inner_entries, double outrows)
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
								  outer_entries, inner_entries);
	}

	cpath->path.rows = outrows;
	cpath->path.disabled_nodes = 0;

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
 */
static bool
lion_analyze_leaf(Node *clause, Index rti, bool allow_negated,
				 LionLeafInfo *out)
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
lion_make_partial_target(PlannerInfo *root, PathTarget *grouping_target)
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
 * Decide whether count(*) over input_rel can be answered from roaring
 * posting sets and, if so, add a CustomPath to output_rel.  Every failed
 * check simply returns: the normal plan is always available.
 */
static void
lion_try_count_path(PlannerInfo *root, RelOptInfo *input_rel,
				   RelOptInfo *output_rel, GroupPathExtraData *extra)
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
	ListCell   *lc;

	/* ---- the query as a whole ---- */
	if (parse->commandType != CMD_SELECT)
		return;
	if (!parse->hasAggs)
		return;
	if (parse->groupingSets != NIL || parse->havingQual != NULL ||
		root->hasHavingQual)
		return;
	if (parse->hasWindowFuncs || parse->hasTargetSRFs ||
		parse->hasDistinctOn || parse->distinctClause != NIL)
		return;
	if (parse->rowMarks != NIL || root->rowMarks != NIL)
		return;
	if (extra != NULL && extra->havingQual != NULL)
		return;

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
	 */
	if (list_length(root->processed_groupClause) > LION_MAX_GROUPCOLS)
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
	else
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
											  &leaf))
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
					if (!lion_analyze_leaf(arm, rti, false, &leaf))
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

		if (!lion_analyze_leaf(clause, rti, true, &leaf))
			return;

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
			 * At most one positive clause per column: the same clause twice
			 * is just a duplicate, two different ones mean the query selects
			 * little or nothing and we would rather leave that to the normal
			 * plan.  `IS NOT NULL` is not subject to this - it constrains
			 * nothing by itself and is simply subtracted - and neither is a
			 * multi-key clause, whose sources intersect exactly as two
			 * clauses on different columns do (`tags @> '{a}' AND
			 * tags && '{b,c}'` is one AND of three key sets).  Nor is a leaf
			 * of an OR, which says nothing about the rows the OTHER arms
			 * select and so cannot be compared with a clause that does.
			 */
			if (list_member_int(posattnos, (int) leaf.var->varattno))
			{
				ListCell   *l1;
				ListCell   *l2;
				ListCell   *l3;
				ListCell   *l4;
				bool		same = false;

				forfour(l1, whereattnos, l2, whereconsts, l3, wherekinds,
						l4, whereinor)
				{
					if (lfirst_int(l4) != 0 ||
						lfirst_int(l1) != (int) leaf.var->varattno ||
						lfirst_int(l3) == LION_CLAUSE_NOTNULL)
						continue;
					same = (lfirst_int(l3) == leaf.kind &&
							equal(lfirst(l2), leaf.val));
					break;
				}
				if (!same)
					return;
				continue;
			}
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

	/* Something has to drive the count. */
	driveattno = groupattno[0];
	if (ngroup == 0 && !havepositive)
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

	/* ---- the grouped relation's target ---- */
	foreach(lc, output_rel->reltarget->exprs)
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
			if (!lion_agg_is_count((Aggref *) node, rti, input_rel,
								  groupattno, ngroup, nonnullattnos,
								  nullattnos))
				return;
			haveagg = true;
		}
		else
			return;
	}
	if (!haveagg)
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
	else if (driveattno != 0)
	{
		/* §14's sum-over-all: one index, and it groups nothing. */
		drive[0].attno = driveattno;
		drive[0].var = notnullvar;
		drive[0].collation = InvalidOid;
		drive[0].eqop = InvalidOid;
		drive[0].valueout = false;
		ndrive = 1;
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
	else if (sumall)
	{
		/* Every entry of the driving index is visited, one group or not. */
		Assert(notnullvar != NULL);
		numgroups = estimate_num_groups(root, list_make1(notnullvar),
										input_rel->rows, NULL, NULL);
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
		partialtarget = lion_make_partial_target(root, output_rel->reltarget);
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
						  (driveattno != 0 ? LION_FLAG_GROUPIDX : 0));
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
		bms_is_member(groupattno[0], input_rel->notnullattnums) &&
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
								 ors, &sumshort, &groupdrive);
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
	cpath->custom_restrictinfo = NIL;
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
	cpath->methods = &lion_count_path_methods;

	lion_cost_count_path(root, cpath, targets, whereclauses, wherekinds, ors,
						numgroups, groupest[0], (ngroup == 2) ? groupest[1] : 0,
						outrows);

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
								 NIL,	/* HAVING was refused above */
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

	lion_try_count_path(root, input_rel, output_rel,
					   (GroupPathExtraData *) extra);
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
	bool	   *inor;
	AttrNumber	groupattno;
	AttrNumber	groupattno2;
	ListCell   *lc;

	/*
	 * The name has to be resolvable before any CustomScan node of ours is
	 * written out or read back; plan time is the first moment that can
	 * happen, and registering twice is harmless.
	 */
	if (GetCustomScanMethods("LionCount", true) == NULL)
		RegisterCustomScanMethods(&lion_count_scan_methods);

	ints = (List *) list_nth(best_path->custom_private, LION_PRIV_INTS);
	ckinds = (List *) list_nth(best_path->custom_private, LION_PRIV_CLAUSEKINDS);
	groupattno = (AttrNumber) lsecond_int(ints);
	groupattno2 = (AttrNumber) lthird_int(ints);

	/*
	 * A leaf of an OR constrains no column of the result (DESIGN.md §19), so
	 * it neither pins a value the target list may print nor makes a
	 * `count(col)` zero: the other arms select rows it says nothing about.
	 */
	inor = lion_or_leaf_map((List *) list_nth(best_path->custom_private,
											  LION_PRIV_ORS),
							list_length(ckinds));

	foreach(lc, tlist)
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
			if (agg->args != NIL)
			{
				Node	   *arg = lion_strip((Node *)
											((TargetEntry *) linitial(agg->args))->expr);
				AttrNumber	attno;
				int			i;

				Assert(arg != NULL && IsA(arg, Var));
				attno = ((Var *) arg)->varattno;

				if (groupattno != 0 && attno == groupattno)
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
	cscan->scan.plan.qual = NIL;
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

static void
lion_open_relation(LionCountScanState *st, Oid heapoid, Oid groupidxoid,
				  Oid groupidxoid2, const Oid *clauseidxoid)
{
	int			i;

	Assert(st->heap == NULL);

	st->heap = table_open(heapoid, NoLock);
	Assert(CheckRelationLockedByMe(st->heap, AccessShareLock, true));

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
												  st->groupattno2));
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

	oids = (List *) list_nth(cscan->custom_private, LION_PRIV_OIDS);
	ints = (List *) list_nth(cscan->custom_private, LION_PRIV_INTS);
	ckinds = (List *) list_nth(cscan->custom_private, LION_PRIV_CLAUSEKINDS);
	exprs = cscan->custom_exprs;
	partlist = (List *) list_nth(cscan->custom_private, LION_PRIV_PARTS);
	clauseops = (List *) list_nth(cscan->custom_private, LION_PRIV_CLAUSEOPS);
	orlist = (List *) list_nth(cscan->custom_private, LION_PRIV_ORS);
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
	st->nclause = list_length(ckinds);

	/*
	 * One value expression per clause, in custom_exprs (see the shape marker
	 * above).  A mismatch is planner/executor drift, exactly like a wrong
	 * shape marker, and is said rather than decoded.
	 */
	if (list_length(exprs) != st->nclause)
		elog(ERROR, "LionCount: %d clauses but %d value expressions",
			 st->nclause, list_length(exprs));

	st->ntlist = list_length(kinds);
	st->tlkind = (int *) palloc(sizeof(int) * Max(st->ntlist, 1));
	for (i = 0; i < st->ntlist; i++)
		st->tlkind[i] = list_nth_int(kinds, i);

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
		st->driveattno = 0;
		for (i = 0; i < st->nclause; i++)
		{
			if (st->clause[i].kind == LION_CLAUSE_NOTNULL)
			{
				st->driveattno = st->clause[i].attno;
				break;
			}
		}
		if (st->driveattno == 0)
			elog(ERROR, "LionCount: sum-over-all without an IS NOT NULL clause");
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
		lion_open_relation(st, st->heapoid, st->groupidxoid, st->groupidxoid2,
						  NULL);

	/*
	 * Slot 0 is the (outer) group's posting set, 1 .. nitem the WHERE items,
	 * and - for a two-column GROUP BY (DESIGN.md §20) - slot nitem + 1 the
	 * inner group's.
	 */
	st->nsource = st->nitem + 1 + (st->groupattno2 != 0 ? 1 : 0);
	st->sources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * st->nsource);
	st->sources[0].nsets = 1;
	st->sources[0].sets = &st->groupset;
	st->sources[0].negated = false;
	if (st->groupattno2 != 0)
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

	MemoryContextSwitchTo(oldcxt);

	/*
	 * Can an IN list drive the groups instead of the index's entry scan
	 * (DESIGN.md §15)?  Only when the clause's index IS the index that would
	 * drive them - same relation, same column, so the entries are the same
	 * entries - and the grouping is the plain one-column form.  A two-column
	 * GROUP BY (§20) and a sum-over-all (§14) both walk the entries for
	 * reasons of their own and are left alone.
	 */
	st->ingroupitem = -1;
	st->ingroupset = 0;
	if (st->hasgroupidx && st->groupattno != 0 && st->groupattno2 == 0 &&
		!st->sumall)
	{
		for (k = 0; k < st->nitem; k++)
		{
			LionClauseState *cl;

			if (st->item[k].orno >= 0)
				continue;
			cl = &st->clause[st->item[k].clauseno];
			if (cl->kind != LION_CLAUSE_ARRAY ||
				cl->attno != st->groupattno ||
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
	 */
	st->sumallitem = -1;
	if (st->sumall && st->hasgroupidx)
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

	ExecClearTuple(slot);
	for (i = 0; i < st->ntlist; i++)
	{
		int			kind = st->tlkind[i];

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
									 &st->stats, st->viscache);
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

	lion_entry_scan_begin_col(&st->escan, st->groupidx, st->groupidxcol);
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
										  &st->stats, st->viscache);
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
										 &st->stats, st->viscache);
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
										 &st->stats, st->viscache);
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
										 &st->stats, st->viscache);
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
 * Whichever of the two the plan asks for.
 */
static TupleTableSlot *
lion_next_group_any(LionCountScanState *st, bool *exhausted)
{
	if (st->groupattno2 != 0)
		return lion_next_group2(st, exhausted);
	if (st->ingroupitem >= 0)
		return lion_next_group_inlist(st, exhausted);
	return lion_next_group(st, exhausted);
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
				lion_entry_scan_begin_col(&st->escan, st->groupidx,
										 st->groupidxcol);
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

	if (st->done)
		return NULL;

	slot = lion_exec_custom_scan_internal(node);
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
		/* A sum over all entries still has to report its one row. */
		if (st->sumall)
			return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, 0);
		return NULL;
	}

	/* ---- every entry of the index, summed into one row ---- */
	if (st->sumall)
	{
		int64		total = lion_sumall_relation(st);

		st->done = true;
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
		lion_entry_scan_begin_col(&st->escan, st->groupidx, st->groupidxcol);
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
}

static void
lion_end_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;

	lion_reset_run(st);

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
 * "tags @> {a,b}", "tsv @@ 'a' & 'b'".
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

					val = deparse_expression((Node *) cl->valexpr, context,
											 false, false);
				}
				else
				{
					getTypeOutputInfo(cl->con->consttype, &outfunc, &isvarlena);
					val = OidOutputFunctionCall(outfunc, cl->con->constvalue);
				}
				if (cl->kind == LION_CLAUSE_ARRAY)
					appendStringInfo(buf, "%s = ANY (%s)", attname, val);
				else if (cl->kind == LION_CLAUSE_MULTI)
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
		if (st->groupattno != 0)
			appendStringInfo(&buf, "(%s)",
							 get_attname(st->heapoid, st->groupattno, false));
		else
			appendStringInfoString(&buf, "(all keys)");

		/* The inner index of a two-column GROUP BY (DESIGN.md §20). */
		if (st->groupattno2 != 0)
		{
			appendStringInfoString(&buf, ", ");
			if (st->npart == 0)
				appendStringInfo(&buf, "%s%s ", get_rel_name(st->groupidxoid2),
								 lion_explain_col(st->groupidxoid2,
												  st->groupattno2));
			appendStringInfo(&buf, "(%s)",
							 get_attname(st->heapoid, st->groupattno2, false));
		}
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
	}
}
