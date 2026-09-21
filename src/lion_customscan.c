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
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
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
#define LION_TL_COUNT		1
#define LION_TL_COUNT_GROUPCOL	2	/* count(group column): 0 for the NULL
									 * group, the count otherwise (§14) */
#define LION_TL_COUNT_ZERO	3	/* count(col) where a clause pins col to NULL */
/* LION_TL_WHEREKEY + i: the key stored in the i'th clause's entry */
#define LION_TL_WHEREKEY		4

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
 *	1	OidList: heap Oid, group index Oid (InvalidOid if none), then one
 *		Oid per WHERE clause, in the same order as the other lists.  For a
 *		partitioned table the heap Oid is the PARENT's (EXPLAIN resolves
 *		column names against it) and every index Oid is InvalidOid: the real
 *		ones are per partition, in LION_PRIV_PARTS.
 *	2	IntList: base RT index, group attnum (0 if none), the LION_FLAG_* bits,
 *		then one attnum per WHERE clause.  Attnums are the PARENT's
 *		throughout; each partition's own numbering lives in its index Oids.
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
 *		table (DESIGN.md §16): heap Oid, group index Oid (InvalidOid if
 *		none), then one index Oid per WHERE clause
 *	6	OidList: the operator of each WHERE clause (InvalidOid for a null
 *		test), which is what EXPLAIN prints a multi-key clause with
 *	7	IntList: LION_TL_* for each custom_scan_tlist column (added at plan
 *		time, when the target list is known)
 */
#define LION_PRIV_VERSION	0
#define LION_PRIV_OIDS		1
#define LION_PRIV_INTS		2
#define LION_PRIV_CONSTS		3
#define LION_PRIV_CLAUSEKINDS 4
#define LION_PRIV_PARTS		5
#define LION_PRIV_CLAUSEOPS	6
#define LION_PRIV_TLKINDS	7

/*
 * Shape of the list above: "RBI" and a shape version, and its length.  Shape
 * 2 dropped the group column member, which only the cross-partition hash
 * merge needed (DESIGN.md §16: the node emits partial aggregates now).  Shape
 * 3 moved the clause values out of member 3 and into custom_exprs, so that a
 * Param among them reaches setrefs.c and SS_finalize_plan() (DESIGN.md §10).
 */
#define LION_PRIV_MAGIC		0x52424903
#define LION_PRIV_NMEMBERS	8

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
	AttrNumber	attno;

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
 * One relation the executor counts: a plain table, or one live leaf
 * partition (DESIGN.md §16).  The index Oids are that relation's own.
 */
typedef struct LionPartState
{
	Oid			heapoid;
	Oid			groupidxoid;	/* InvalidOid when no index drives the scan */
	Oid		   *clauseidxoid;	/* one per WHERE clause */
} LionPartState;

typedef struct LionCountScanState
{
	CustomScanState css;

	/* decoded from custom_private */
	Oid			heapoid;
	Index		scanrelid;
	Oid			groupidxoid;
	AttrNumber	groupattno;
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

	/*
	 * The inputs of the count: slot 0 is the group (or the driving index of
	 * a sumall), slots 1 .. nclause the WHERE clauses.  The clause sources
	 * are located once per node execution and keep their pins (DESIGN.md
	 * section 9) until the node is reset or closed; the group's set is
	 * located, counted and released one group at a time.
	 */
	LionCountSource *sources;
	LionPostingSet groupset;
	bool		located;
	bool		valsdone;		/* the clause values have been evaluated */
	bool		wheremissing;	/* a positive clause selects nothing at all */
	bool		scanning;
	bool		done;
	LionEntryScan escan;

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
} LionCountScanState;

static Plan *lion_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
								  CustomPath *best_path, List *tlist,
								  List *clauses, List *custom_plans);
static Node *lion_create_custom_scan_state(CustomScan *cscan);
static void lion_begin_custom_scan(CustomScanState *node, EState *estate,
								  int eflags);
static TupleTableSlot *lion_exec_custom_scan(CustomScanState *node);
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
 */
static IndexOptInfo *
lion_find_roaring_index(RelOptInfo *rel, AttrNumber attno, bool multikey)
{
	Oid			amoid = lion_get_am_oid();
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam != amoid)
			continue;
		if (idx->hypothetical)
			continue;
		if (idx->ncolumns != 1 || idx->nkeycolumns != 1)
			continue;
		if (idx->indpred != NIL || idx->indexprs != NIL)
			continue;
		if (idx->indexkeys[0] != attno)
			continue;
		if (lion_opfamily_is_multikey(idx->opfamily[0],
									 idx->opcintype[0]) != multikey)
			continue;

		return idx;
	}

	return NULL;
}

/*
 * The equality operator an index's opclass defines on its own key type: the
 * relation whose classes its entries are.  A scalar roaring opclass always
 * has it (the AM requires strategy 1), but an opfamily that only declares
 * cross-type members for (opcintype, opcintype) would not, and then nothing
 * below can be proved about the index.
 */
static Oid
lion_index_equality_op(IndexOptInfo *idx)
{
	return get_opfamily_member(idx->opfamily[0], idx->opcintype[0],
							   idx->opcintype[0], LION_STRAT_EQUAL);
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
lion_index_can_emit_value(IndexOptInfo *idx)
{
	Oid			typid = idx->opcintype[0];
	Oid			idxeq = lion_index_equality_op(idx);
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

	return lion_type_equalimage(typid, idx->indexcollations[0]);
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
				Oid exprcoll)
{
	bool		multikey = (kind == LION_CLAUSE_MULTI);
	IndexOptInfo *idx = lion_find_roaring_index(rel, attno, multikey);

	if (idx == NULL)
		return NULL;

	/*
	 * The planner's own rule, IndexCollMatchesExprColl(): a collation-
	 * sensitive clause may only use an index built under that collation.
	 * The index hashed and compared its keys with its own collation and the
	 * count never rechecks the predicate, so a mismatch (say a case-
	 * insensitive index under a case-sensitive query) would count rows the
	 * query does not select.
	 */
	if (OidIsValid(exprcoll) && idx->indexcollations[0] != exprcoll)
		return NULL;

	if (multikey)
	{
		if (get_op_opfamily_strategy(opno, idx->opfamily[0]) != strategy)
			return NULL;
		if (get_opfamily_proc(idx->opfamily[0], idx->opcintype[0],
							  idx->opcintype[0],
							  LION_EXTRACTQUERY_PROC) != extractquery)
			return NULL;
		return idx;
	}

	if (OidIsValid(opno) &&
		get_op_opfamily_strategy(opno, idx->opfamily[0]) != LION_STRAT_EQUAL)
		return NULL;

	if (OidIsValid(cmptype))
	{
		if (!OidIsValid(get_opfamily_member(idx->opfamily[0],
											idx->opcintype[0], cmptype,
											LION_STRAT_EQUAL)))
			return NULL;
		if (!OidIsValid(get_opfamily_proc(idx->opfamily[0], cmptype, cmptype,
										  LION_HASH_PROC)))
			return NULL;
	}

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
	IndexOptInfo *driveidx;		/* the index whose entries are scanned, or NULL */
	List	   *whereidx;		/* IndexOptInfo *, one per WHERE clause */
	Var		   *drivevar;		/* the driving column in THIS relation's own
								 * numbering, which is what a per-relation
								 * estimate_num_groups() needs; NULL when
								 * nothing drives the scan */
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
					const LionDriveInfo *drive, List *whereattnos,
					List *clauseinfos, List **targets)
{
	RangeTblEntry *rte;
	LionCountTarget *t;
	ListCell   *l1;
	ListCell   *l2;

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
			LionDriveInfo cdrive = *drive;
			List	   *cattnos = NIL;

			if (child == NULL || !bms_is_member(i, rel->live_parts))
				continue;		/* pruned at plan time */
			if (IS_DUMMY_REL(child))
				continue;		/* provably empty: it counts nothing */

			if (drive->attno != 0)
			{
				cdrive.var = lion_child_var(root, child->relid, drive->attno);
				if (cdrive.var == NULL)
					return false;
				cdrive.attno = cdrive.var->varattno;
			}
			foreach(l1, whereattnos)
			{
				AttrNumber	ca = lion_child_attno(root, child->relid,
												 (AttrNumber) lfirst_int(l1));

				if (ca == 0)
					return false;
				cattnos = lappend_int(cattnos, (int) ca);
			}

			if (!lion_collect_targets(root, child, &cdrive, cattnos,
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
	t->drivevar = drive->var;

	/*
	 * The driving index - a GROUP BY column's, or the one DESIGN.md §14 sums
	 * over - must be a scalar one: its entries have to be the column's
	 * values, one per row.
	 */
	if (drive->attno != 0)
	{
		t->driveidx = lion_find_roaring_index(rel, drive->attno, false);
		if (t->driveidx == NULL)
			return false;
		/* Grouping under one collation, index built under another: no. */
		if (OidIsValid(drive->collation) &&
			t->driveidx->indexcollations[0] != drive->collation)
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
		if (OidIsValid(drive->eqop) &&
			lion_index_equality_op(t->driveidx) != drive->eqop)
			return false;

		/*
		 * Printing the group key means printing a key this index stored, so
		 * it has to be a representation the rows really have (finding 4).
		 */
		if (drive->valueout && !lion_index_can_emit_value(t->driveidx))
			return false;
	}

	forboth(l1, whereattnos, l2, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(l2);
		IndexOptInfo *idx = lion_match_index(rel, (AttrNumber) lfirst_int(l1),
											ci->kind, ci->opno, ci->cmptype,
											ci->strategy, ci->extractquery,
											ci->collation);

		if (idx == NULL)
			return false;

		/* Same rule for a pinned column whose value the output prints. */
		if (ci->valueout && !lion_index_can_emit_value(idx))
			return false;

		t->whereidx = lappend(t->whereidx, idx);
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
				 AttrNumber groupattno, const List *nonnullattnos,
				 const List *nullattnos)
{
	TargetEntry *tle;
	Node	   *arg;
	Var		   *var;

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

	if (groupattno == var->varattno)
		return true;
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
 *	- for each WHERE key: ONE bucket page (a hash lookup), then that key's own
 *	  container chain, which was written sequentially and is a fraction of the
 *	  index's container pages proportional to the clause selectivity.  An IN
 *	  list is one such lookup per element (DESIGN.md §15);
 *	- for a GROUP BY: every page of the group index;
 *	- one O(1) step per container per participating source (the visibility map
 *	  is read per container);
 *	- the heap the visibility map cannot vouch for: the TIDs on blocks that
 *	  are not all-visible (pg_class.relallvisible via RelOptInfo.allvisfrac),
 *	  each resolved against the snapshot, on as many distinct blocks as there
 *	  can be - each of them fetched once per query.
 *
 * The bucket pages are NOT charged wholesale: an index carries at least
 * LION_DEFAULT_BUCKETS of them, which would price a single-key count on a
 * small table above a sequential scan of the whole table.  The bucket count
 * comes from the index's meta page (cached in rd_amcache), as
 * btcostestimate reads the tree height from the metapage.
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
static double
lion_index_bucket_pages(IndexOptInfo *idx)
{
	Relation	indexrel = index_open(idx->indexoid, AccessShareLock);
	LionState   *state = lion_get_state(indexrel);
	double		nbuckets = (double) state->meta.nbuckets;

	index_close(indexrel, AccessShareLock);
	return nbuckets;
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

static Cost
lion_cost_count_rel(PlannerInfo *root, RelOptInfo *rel,
				   IndexOptInfo *groupidx,
				   List *whereidx, List *whereclauses, double numgroups)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		dirty_pages;
	double		matching = Max(rel->rows, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		random_pages = 0;	/* bucket page lookups */
	double		seq_pages = 0;	/* container chains, read in order */
	double		ncontainers = 0;
	double		merge_ops = 0;	/* comparisons a union of k sets makes */
	double		recheck_tids;
	double		recheck_pages;
	Cost		run;
	ListCell   *lc1;
	ListCell   *lc2;

	forboth(lc1, whereidx, lc2, whereclauses)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Node	   *clause = (Node *) lfirst(lc2);
		Selectivity sel = clause_selectivity(root, clause, 0, JOIN_INNER, NULL);
		double		nbuckets = Max(lion_index_bucket_pages(idx), 1.0);
		double		container_pages;
		double		nkeys = 1.0;

		container_pages = (double) idx->pages - 1.0 - nbuckets;
		container_pages = Max(container_pages, 0.0);

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
		 */
		if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			nkeys = Max(estimate_array_length(root,
											  (Node *) lsecond(saop->args)),
						1.0);
		}

		random_pages += Min(nkeys, nbuckets);
		seq_pages += Max(nkeys, container_pages * sel);
		ncontainers += nkeys * lion_containers_for(heap_pages,
												  tuples * sel / nkeys);
		if (nkeys > 1.0)
			merge_ops += tuples * sel * log2(nkeys);
	}

	if (groupidx != NULL)
	{
		seq_pages += Max(1.0, (double) groupidx->pages);
		ncontainers += numgroups *
			lion_containers_for(heap_pages, matching / Max(numgroups, 1.0));
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
	run += seq_pages * seq_page_cost;
	run += ncontainers * cpu_operator_cost * 2.0;	/* block mask + VM mask */
	run += merge_ops * cpu_operator_cost;
	run += recheck_pages * lion_heap_page_cost(root, rel, recheck_pages,
											  heap_pages);
	run += recheck_tids * cpu_tuple_cost;
	run += numgroups * cpu_tuple_cost;

	return run;
}

/*
 * Sum the per-relation costs over every relation the node will count and put
 * the result on the path.  The WHERE clauses that do not select rows
 * (`IS NOT NULL`, DESIGN.md §14) are left out of the per-relation estimate,
 * as they were before partitions existed.
 */
static void
lion_cost_count_path(PlannerInfo *root, CustomPath *cpath, List *targets,
					List *whereclauses, List *wherekinds, double numgroups,
					double outrows)
{
	Cost		run = 0;
	ListCell   *lc;

	foreach(lc, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		List	   *costidx = NIL;
		List	   *costclauses = NIL;
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;

		forthree(l1, t->whereidx, l2, whereclauses, l3, wherekinds)
		{
			if (!LION_CLAUSE_IS_POSITIVE(lfirst_int(l3)))
				continue;
			costidx = lappend(costidx, lfirst(l1));
			costclauses = lappend(costclauses, lfirst(l2));
		}

		run += lion_cost_count_rel(root, t->rel, t->driveidx, costidx,
								  costclauses, numgroups);

		list_free(costidx);
		list_free(costclauses);
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
	Var		   *groupvar = NULL;
	AttrNumber	groupattno = 0;
	AttrNumber	driveattno = 0; /* column whose index drives the entry scan */
	Oid			groupeqop = InvalidOid;
	bool		singlegroup = false;
	bool		sumall = false;
	bool		partitioned = false;
	bool		groupvalueout = false;	/* the output prints the group key */
	List	   *valueattnos = NIL;	/* pinned columns the output prints */
	LionDriveInfo drive;
	PathTarget *partialtarget = NULL;	/* set for a partitioned GROUP BY */
	List	   *whereattnos = NIL;	/* its column, in the PARENT's numbering */
	List	   *clauseinfos = NIL;	/* LionClauseInfo, one per clause */
	List	   *whereclauses = NIL; /* the clause, for selectivity */
	List	   *whereconsts = NIL;	/* its value expression - a Const, a Param,
									 * or a NULL placeholder for a null test */
	List	   *wherekinds = NIL;	/* LION_CLAUSE_* */
	List	   *whereopnos = NIL;	/* the clause's operator (0 for a null test) */
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

	/* ---- GROUP BY: nothing, or one indexed column ---- */
	if (list_length(root->processed_groupClause) > 1)
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
		SortGroupClause *sgc;
		TargetEntry *tle;
		Node	   *expr;

		sgc = (SortGroupClause *) linitial(root->processed_groupClause);
		tle = get_sortgroupclause_tle(sgc, root->processed_tlist);
		if (tle == NULL)
			return;
		expr = lion_strip((Node *) tle->expr);
		if (expr == NULL || !IsA(expr, Var))
			return;
		groupvar = (Var *) expr;
		if (groupvar->varno != (int) rti || groupvar->varattno <= 0 ||
			groupvar->varlevelsup != 0)
			return;

		/*
		 * A nullable group column is fine now: NULL keys have an entry of
		 * their own, so the NULL group is produced like any other
		 * (DESIGN.md §14).
		 */
		groupattno = groupvar->varattno;

		/*
		 * The equality the planner chose for this column is what the index
		 * that drives the scan has to implement, whether there is one
		 * relation or many (lion_collect_targets(), finding 3 of the
		 * 2026-09-20 review), so a grouping clause without one is of no use
		 * here.
		 */
		groupeqop = sgc->eqop;
		if (!OidIsValid(groupeqop))
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
	}

	/* ---- every WHERE clause must be one the posting sets can answer ---- */
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *clause;
		Var		   *var = NULL;
		Node	   *val = NULL;	/* the value expression, or a placeholder */
		Oid			opno = InvalidOid;	/* operator the index must know */
		Oid			cmptype = InvalidOid;	/* type the index is compared with */
		StrategyNumber strategy = 0;	/* multi-key clauses only */
		Oid			extractquery = InvalidOid;
		LionClauseInfo *ci;
		int			kind;

		if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant)
			return;

		clause = (Node *) rinfo->clause;

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;
			Node	   *left;
			Node	   *right;
			Oid			opfamily;
			Oid			lefttype;

			if (list_length(op->args) != 2)
				return;
			if (!op_strict(op->opno))
				return;

			left = lion_strip((Node *) linitial(op->args));
			right = lion_strip((Node *) lsecond(op->args));
			if (left == NULL || right == NULL)
				return;

			strategy = lion_op_roaring_strategy(op->opno, &opfamily, &lefttype);

			if (strategy == LION_STRAT_EQUAL)
			{
				/* Equality commutes, so either side may hold the column. */
				if (IsA(left, Var) && lion_is_value_expr(right, false))
				{
					var = (Var *) left;
					val = right;
				}
				else if (lion_is_value_expr(left, false) && IsA(right, Var))
				{
					var = (Var *) right;
					val = left;
				}
				else
					return;

				/*
				 * A literal NULL equals nothing.  A parameter that turns out
				 * to be NULL is the same answer, but only the executor can
				 * see it, so it selects no rows there instead.
				 */
				if (IsA(val, Const) && ((Const *) val)->constisnull)
					return;

				opno = op->opno;
				cmptype = exprType(val);
				kind = LION_CLAUSE_EQ;
			}
			else if (strategy == LION_STRAT_CONTAINS ||
					 strategy == LION_STRAT_OVERLAP ||
					 strategy == LION_STRAT_MATCH)
			{
				/*
				 * A multi-key operator (DESIGN.md §17).  Unlike equality it
				 * does not commute - `'{a}' @> tags` is a containment the
				 * other way round, which is strategy 4 and not pushed down -
				 * so the column has to be the left operand.
				 */
				/*
				 * The QUERY, not just its value, decides whether the posting
				 * sets can answer this clause at all, so it has to be
				 * available now: a Param is refused here even though one is
				 * accepted for equality (DESIGN.md §17).  `tags @> $1` with
				 * `$1 = '{}'` extracts to ALL mode, which this node cannot
				 * answer - it has no way to recheck the operator against the
				 * heap - and by then there would be no plan left to fall back
				 * to.
				 */
				if (!IsA(left, Var) || !IsA(right, Const))
					return;
				var = (Var *) left;
				val = right;
				if (((Const *) val)->constisnull)
					return;

				/*
				 * Only an EXACT query is pushed down.  `tags @> '{}'`, `<@`,
				 * a tsquery with NOT/phrase/prefix/weights and anything with
				 * a NULL element all want every row rechecked in the heap,
				 * which is what the ordinary plan does anyway.
				 */
				if (!lion_multikey_query_is_exact(opfamily, lefttype, strategy,
												 (Const *) val, &extractquery))
					return;

				opno = op->opno;
				cmptype = InvalidOid;	/* the query is not a key */
				kind = LION_CLAUSE_MULTI;
			}
			else
				return;			/* strategy 4 (`<@`), or not ours at all */
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			Node	   *left;
			Node	   *right;
			int			nelems;

			/* `= ALL (...)` is not a union of keys (DESIGN.md §15). */
			if (!saop->useOr)
				return;
			if (list_length(saop->args) != 2)
				return;
			if (!op_strict(saop->opno))
				return;

			left = lion_strip((Node *) linitial(saop->args));
			right = lion_strip((Node *) lsecond(saop->args));
			if (left == NULL || right == NULL)
				return;
			if (!IsA(left, Var) || !lion_is_value_expr(right, true))
				return;
			var = (Var *) left;
			val = right;

			/*
			 * The length cap of DESIGN.md §15 applies to the lists whose
			 * length is known now: a literal array and the ARRAY[...] a
			 * generic plan keeps for `k IN ($1, $2)`.  A parameter that IS an
			 * array has no length until the executor has it, and by then
			 * there is no plan to decline in favour of, so it is answered
			 * whatever its length.
			 */
			if (IsA(val, Const))
			{
				if (((Const *) val)->constisnull)
					return;
				nelems = lion_array_const_nelems((Const *) val);
				if (nelems < 0 || nelems > LION_MAX_ARRAY_ELEMS)
					return;
			}
			else if (IsA(val, ArrayExpr))
			{
				nelems = list_length(((ArrayExpr *) val)->elements);
				if (nelems > LION_MAX_ARRAY_ELEMS)
					return;
			}

			/*
			 * `col op ANY (array)` is a union of single-key lookups, so the
			 * operator has to be equality; `tags @> ANY (...)` would be a
			 * union of multi-key queries, which nothing here builds.
			 */
			{
				Oid			opfamily;
				Oid			lefttype;

				if (lion_op_roaring_strategy(saop->opno, &opfamily,
											&lefttype) != LION_STRAT_EQUAL)
					return;
			}

			opno = saop->opno;
			cmptype = get_element_type(exprType(val));
			if (!OidIsValid(cmptype))
				return;
			kind = LION_CLAUSE_ARRAY;
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;
			Node	   *arg;

			if (nt->argisrow)
				return;
			arg = lion_strip((Node *) nt->arg);
			if (arg == NULL || !IsA(arg, Var))
				return;
			var = (Var *) arg;

			kind = (nt->nulltesttype == IS_NULL) ?
				LION_CLAUSE_NULL : LION_CLAUSE_NOTNULL;
			/* The executor needs no value; keep the lists in step. */
			val = (Node *) makeNullConst(var->vartype, var->vartypmod,
										 var->varcollid);
		}
		else
			return;

		if (var->varno != (int) rti || var->varattno <= 0 ||
			var->varlevelsup != 0)
			return;

		/*
		 * Which index answers the clause, whether its opfamily has the
		 * operator as strategy 1, and whether it can hash and compare the
		 * constant's type is settled per relation, in lion_match_index():
		 * with partitions there is one index per partition and they need not
		 * share an opclass (DESIGN.md §16).
		 */

		if (LION_CLAUSE_IS_POSITIVE(kind))
			havepositive = true;

		if (LION_CLAUSE_IS_POSITIVE(kind) && kind != LION_CLAUSE_MULTI)
		{
			/*
			 * At most one positive clause per column: the same clause twice
			 * is just a duplicate, two different ones mean the query selects
			 * little or nothing and we would rather leave that to the normal
			 * plan.  `IS NOT NULL` is not subject to this - it constrains
			 * nothing by itself and is simply subtracted - and neither is a
			 * multi-key clause, whose sources intersect exactly as two
			 * clauses on different columns do (`tags @> '{a}' AND
			 * tags && '{b,c}'` is one AND of three key sets).
			 */
			if (list_member_int(posattnos, (int) var->varattno))
			{
				ListCell   *l1;
				ListCell   *l2;
				ListCell   *l3;
				bool		same = false;

				forthree(l1, whereattnos, l2, whereconsts, l3, wherekinds)
				{
					if (lfirst_int(l1) != (int) var->varattno ||
						lfirst_int(l3) == LION_CLAUSE_NOTNULL)
						continue;
					same = (lfirst_int(l3) == kind &&
							equal(lfirst(l2), val));
					break;
				}
				if (!same)
					return;
				continue;
			}
			posattnos = lappend_int(posattnos, (int) var->varattno);
		}

		switch (kind)
		{
			case LION_CLAUSE_EQ:
				eqattnos = lappend_int(eqattnos, (int) var->varattno);
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case LION_CLAUSE_NOTNULL:
				if (notnullvar == NULL)
					notnullvar = var;
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case LION_CLAUSE_ARRAY:
			case LION_CLAUSE_MULTI:
				/* a strict operator with a non-NULL constant */
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case LION_CLAUSE_NULL:
				nullattnos = lappend_int(nullattnos, (int) var->varattno);
				break;
		}

		ci = (LionClauseInfo *) palloc0(sizeof(LionClauseInfo));
		ci->attno = var->varattno;
		ci->kind = kind;
		ci->opno = opno;
		ci->cmptype = cmptype;
		ci->strategy = strategy;
		ci->extractquery = extractquery;
		if (IsA(clause, OpExpr))
			ci->collation = ((OpExpr *) clause)->inputcollid;
		else if (IsA(clause, ScalarArrayOpExpr))
			ci->collation = ((ScalarArrayOpExpr *) clause)->inputcollid;
		else
			ci->collation = InvalidOid;

		whereattnos = lappend_int(whereattnos, (int) var->varattno);
		clauseinfos = lappend(clauseinfos, ci);
		whereclauses = lappend(whereclauses, clause);
		whereconsts = lappend(whereconsts, val);
		wherekinds = lappend_int(wherekinds, kind);
		whereopnos = lappend_oid(whereopnos, opno);
	}

	/* Something has to drive the count. */
	driveattno = groupattno;
	if (groupvar == NULL && !havepositive)
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
			if (groupvar != NULL && v->varattno == groupvar->varattno)
				groupvalueout = true;
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
								  groupattno, nonnullattnos, nullattnos))
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
	drive.attno = driveattno;
	drive.var = (driveattno != 0) ?
		((groupvar != NULL) ? groupvar : notnullvar) : NULL;
	drive.collation = (groupvar != NULL) ? groupvar->varcollid : InvalidOid;
	drive.eqop = (groupvar != NULL) ? groupeqop : InvalidOid;
	drive.valueout = groupvalueout;

	if (!lion_collect_targets(root, input_rel, &drive, whereattnos,
							 clauseinfos, &targets))
		return;
	if (targets == NIL)
		return;					/* everything was pruned: leave it to the planner */
	first = (LionCountTarget *) linitial(targets);

	/* ---- build the path ---- */
	if (groupvar != NULL)
		numgroups = estimate_num_groups(root, list_make1(groupvar),
										input_rel->rows, NULL, NULL);
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
	if (groupvar == NULL)
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

			if (t->drivevar != NULL)
				relgroups = estimate_num_groups(root,
												list_make1(t->drivevar),
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
	oids = list_make2_oid(rte->relid,
						  (!partitioned && first->driveidx != NULL) ?
						  first->driveidx->indexoid : InvalidOid);
	ints = list_make3_int((int) rti, (int) groupattno,
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

			one = list_make2_oid(t->heapoid,
								 t->driveidx ? t->driveidx->indexoid : InvalidOid);
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
	cpath->path.pathkeys = NIL;	/* groups come out in bucket order */
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
	cpath->methods = &lion_count_path_methods;

	lion_cost_count_path(root, cpath, targets, whereclauses, wherekinds,
						numgroups, outrows);

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
	AttrNumber	groupattno;
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
				else
				{
					for (i = 0; i < list_length(ckinds); i++)
					{
						if (list_nth_int(ckinds, i) == LION_CLAUSE_NULL &&
							list_nth_int(ints, 3 + i) == (int) attno)
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
				for (i = 3; i < list_length(ints); i++)
				{
					int			ckind = list_nth_int(ckinds, i - 3);

					if (ckind != LION_CLAUSE_EQ && ckind != LION_CLAUSE_NULL)
						continue;
					if (list_nth_int(ints, i) == (int) attno)
					{
						kind = LION_TL_WHEREKEY + (i - 3);
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
static void
lion_open_relation(LionCountScanState *st, Oid heapoid, Oid groupidxoid,
				  const Oid *clauseidxoid)
{
	int			i;

	Assert(st->heap == NULL);

	st->heap = table_open(heapoid, NoLock);
	Assert(CheckRelationLockedByMe(st->heap, AccessShareLock, true));

	for (i = 0; i < st->nclause; i++)
		st->clause[i].idx = index_open(clauseidxoid != NULL ?
									   clauseidxoid[i] : st->clause[i].idxoid,
									   AccessShareLock);

	if (OidIsValid(groupidxoid))
		st->groupidx = index_open(groupidxoid, AccessShareLock);

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
	}
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
	}
	for (i = 0; i < st->nclause; i++)
	{
		if (st->clause[i].idx != NULL)
		{
			index_close(st->clause[i].idx, AccessShareLock);
			st->clause[i].idx = NULL;
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
	List	   *kinds;
	int			flags;
	int			i;

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
	kinds = (List *) list_nth(cscan->custom_private, LION_PRIV_TLKINDS);

	st->heapoid = linitial_oid(oids);
	st->groupidxoid = lsecond_oid(oids);
	st->scanrelid = (Index) linitial_int(ints);
	st->groupattno = (AttrNumber) lsecond_int(ints);
	flags = lthird_int(ints);
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
		cl->idxoid = list_nth_oid(oids, 2 + i);
		cl->attno = (AttrNumber) list_nth_int(ints, 3 + i);
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
			st->part[i].clauseidxoid = (Oid *)
				palloc0(sizeof(Oid) * Max(st->nclause, 1));
			for (j = 0; j < st->nclause; j++)
				st->part[i].clauseidxoid[j] = list_nth_oid(one, 2 + j);
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
		lion_open_relation(st, st->heapoid, st->groupidxoid, NULL);

	/* slot 0 is the group's posting set, 1..nclause the WHERE clauses */
	st->sources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * (st->nclause + 1));
	st->sources[0].nsets = 1;
	st->sources[0].sets = &st->groupset;
	st->sources[0].negated = false;
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

/*
 * Locate the posting sets of one `col = ANY (array)` clause (DESIGN.md §15):
 * one set per distinct non-NULL element, which the count then unions.
 */
static void
lion_locate_array(LionCountScanState *st, LionClauseState *cl,
				 LionCountSource *src)
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
	int			nfound;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	src->sets = (LionPostingSet *)
		palloc0(sizeof(LionPostingSet) * Max(nelems, 1));

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
	nsets = lion_posting_set_lookup_many(cl->idx, elemtype, nelems,
										elems, nulls, src->sets, &nfound);
	src->nsets = nsets;

	/* An empty array, an all-NULL one, or no matching key: no rows at all. */
	if (nfound == 0)
		st->wheremissing = true;

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(cl->val))
		pfree(arr);
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
static void
lion_locate_multikey(LionCountScanState *st, LionClauseState *cl,
					LionCountSource *src)
{
	LionState   *istate = lion_get_state(cl->idx);
	LionQuery	q;
	int			i;

	lion_extract_query(istate, cl->val,
					  (StrategyNumber) get_op_opfamily_strategy(cl->opno,
																cl->idx->rd_opfamily[0]),
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

	src->sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * q.nkeys);
	src->nsets = q.nkeys;
	src->tree = q.tree;

	for (i = 0; i < q.nkeys; i++)
	{
		(void) lion_posting_set_lookup(cl->idx, q.keys[i], InvalidOid,
									  &src->sets[i]);
		CHECK_FOR_INTERRUPTS();
	}

	/* An AND over a key with no entry at all selects nothing anywhere. */
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

	istate = lion_get_state(cl->idx);
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
	int			i;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);

	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];
		LionCountSource *src = &st->sources[i + 1];

		src->negated = (cl->kind == LION_CLAUSE_NOTNULL);
		src->nsets = 0;
		src->sets = NULL;

		/*
		 * A parameter that came out NULL selects no rows at all, whatever the
		 * clause: `k = NULL`, `k = ANY (NULL)` and a NULL multi-key query are
		 * all never true (every one of those operators is strict).  The
		 * clause is then not looked up.
		 */
		if (cl->valisnull && LION_CLAUSE_IS_POSITIVE(cl->kind) &&
			cl->kind != LION_CLAUSE_NULL)
		{
			st->wheremissing = true;
			continue;
		}

		switch (cl->kind)
		{
			case LION_CLAUSE_EQ:
				src->sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet));
				src->nsets = 1;
				if (!lion_posting_set_lookup(cl->idx, cl->val, cl->valtype,
											&src->sets[0]))
					st->wheremissing = true;
				break;

			case LION_CLAUSE_ARRAY:
				lion_locate_array(st, cl, src);
				break;

			case LION_CLAUSE_MULTI:
				lion_locate_multikey(st, cl, src);
				break;

			case LION_CLAUSE_NULL:
			case LION_CLAUSE_NOTNULL:
				src->sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet));
				src->nsets = 1;
				if (!lion_posting_set_lookup_null(cl->idx, &src->sets[0]))
				{
					/*
					 * No NULL entry at all: `IS NULL` selects nothing, and
					 * `IS NOT NULL` has nothing to subtract.
					 */
					if (cl->kind == LION_CLAUSE_NULL)
						st->wheremissing = true;
					else
						src->nsets = 0;
				}
				break;

			default:
				elog(ERROR, "LionCount: unknown clause kind %d", cl->kind);
		}

		/*
		 * Only a clause that pins the column to ONE value can have its key
		 * printed, and those are the ones with a single set.
		 */
		if (LION_CLAUSE_PINS_VALUE(cl->kind) && src->nsets == 1)
			lion_save_clause_key(st, cl, &src->sets[0]);
	}

	MemoryContextSwitchTo(oldcxt);
	st->located = true;
}

static void
lion_release_where(LionCountScanState *st)
{
	int			i;
	int			j;

	if (st->sources == NULL)
		return;

	for (i = 0; i < st->nclause; i++)
	{
		LionCountSource *src = &st->sources[i + 1];

		for (j = 0; j < src->nsets; j++)
			lion_posting_set_release(&src->sets[j]);
		src->nsets = 0;
		src->sets = NULL;
		src->tree = NULL;		/* it lived in wherecxt, reset below */
	}
	if (st->wherecxt != NULL)
		MemoryContextReset(st->wherecxt);
	st->located = false;
	st->wheremissing = false;
}

static TupleTableSlot *
lion_emit_tuple(LionCountScanState *st, Datum key, bool keyisnull, int64 count)
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

			case LION_TL_COUNT:
				slot->tts_values[i] = Int64GetDatum(count);
				break;

			case LION_TL_COUNT_GROUPCOL:
				/* count(group column): 0 in the NULL group (DESIGN.md §14) */
				slot->tts_values[i] = Int64GetDatum(keyisnull ? 0 : count);
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

	Assert(st->nclause > 0);
	if (st->wheremissing)
		return 0;

	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);
	count = lion_count_sources_cached(st->heap, estate->es_snapshot,
									 st->nclause, &st->sources[1],
									 &st->stats, st->viscache);
	MemoryContextSwitchTo(oldcxt);

	return count;
}

/*
 * The sum over every entry of the driving index (DESIGN.md §14,
 * `col IS NOT NULL` with nothing else to drive the merge).
 */
static int64
lion_sumall_relation(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;
	int64		total = 0;
	Datum		key;

	if (st->wheremissing)
		return 0;

	lion_entry_scan_begin(&st->escan, st->groupidx);
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

		total += lion_count_sources_cached(st->heap, estate->es_snapshot,
										  st->nclause + 1, st->sources,
										  &st->stats, st->viscache);
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
										 st->nclause + 1, st->sources,
										 &st->stats, st->viscache);
		keyisnull = st->groupset.keyisnull;
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		return lion_emit_tuple(st, key, keyisnull, count);
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
					  st->part[p].clauseidxoid);
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
				lion_entry_scan_begin(&st->escan, st->groupidx);
				st->scanning = true;
			}
		}

		if (st->scanning)
		{
			bool		exhausted;
			TupleTableSlot *slot = lion_next_group(st, &exhausted);

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

	return lion_emit_tuple(st, (Datum) 0, true, total);
}

static TupleTableSlot *
lion_exec_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;
	EState	   *estate = node->ss.ps.state;
	MemoryContext oldcxt;
	int64		count;

	if (st->done)
		return NULL;

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

		return lion_emit_tuple(st, (Datum) 0, true, count);
	}

	/* ---- the group index drives the count ---- */
	if (st->wheremissing)
	{
		st->done = true;
		/* A sum over all entries still has to report its one row. */
		if (st->sumall)
			return lion_emit_tuple(st, (Datum) 0, true, 0);
		return NULL;
	}

	if (!st->scanning)
	{
		lion_entry_scan_begin(&st->escan, st->groupidx);
		st->scanning = true;
	}

	/* ---- every entry of the index, summed into one row ---- */
	if (st->sumall)
	{
		int64		total = 0;
		Datum		key;

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

			total += lion_count_sources_cached(st->heap, estate->es_snapshot,
											  st->nclause + 1, st->sources,
											  &st->stats, st->viscache);
			lion_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
		}

		st->done = true;
		return lion_emit_tuple(st, (Datum) 0, true, total);
	}

	/* ---- GROUP BY: one row per non-empty group ---- */
	{
		bool		exhausted;
		TupleTableSlot *slot = lion_next_group(st, &exhausted);

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
	lion_posting_set_release(&st->groupset);
	lion_release_where(st);

	if (st->npart > 0)
		lion_close_relation(st);

	if (st->pergroup != NULL)
		MemoryContextReset(st->pergroup);

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
			appendStringInfo(&buf, "%s ", get_rel_name(st->groupidxoid));
		if (st->groupattno != 0)
			appendStringInfo(&buf, "(%s)",
							 get_attname(st->heapoid, st->groupattno, false));
		else
			appendStringInfoString(&buf, "(all keys)");
	}

	for (i = 0; i < st->nclause; i++)
	{
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		if (st->npart == 0)
			appendStringInfo(&buf, "%s ", get_rel_name(st->clause[i].idxoid));
		appendStringInfoChar(&buf, '(');
		lion_explain_clause(st, &st->clause[i], ancestors, es, &buf);
		appendStringInfoChar(&buf, ')');
	}

	ExplainPropertyText("Lion Indexes", buf.data, es);
	pfree(buf.data);

	if (st->hasgroupidx && st->groupattno != 0)
		ExplainPropertyText("Group Key",
							get_attname(st->heapoid, st->groupattno, false),
							es);

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
	}
}
