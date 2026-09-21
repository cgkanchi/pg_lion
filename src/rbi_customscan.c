/*-------------------------------------------------------------------------
 *
 * rbi_customscan.c
 *		A CustomScan that answers
 *
 *			SELECT count(*) [, k] FROM t WHERE <clause> [AND <clause> ...]
 *			[GROUP BY k]
 *
 *		out of roaring posting sets, visiting the heap only for pages the
 *		visibility map does not vouch for.  DESIGN.md section 10 is the
 *		specification; the counting itself lives in rbi_count.c and follows
 *		the pin/visibility-map rule of DESIGN.md section 9.
 *
 * A clause is `k = const` (§10), `k = ANY (const array)` (§15), `k IS NULL`
 * or `k IS NOT NULL` (§14), each on a column with a usable roaring index.
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
 * with the column numbers translated per partition - and adds the results up.
 * Everything below that says "the relation" therefore means "the relation the
 * node is currently counting": one table, or one partition of many.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

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
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "storage/predicate.h"

#include "rbi.h"
#include "rbi_count.h"

/* GUC and the previous hook, both owned here and installed by _PG_init. */
bool		rbi_enable_count_pushdown = true;
create_upper_paths_hook_type rbi_prev_create_upper_paths_hook = NULL;

/* Kinds of column in custom_scan_tlist. */
#define RBI_TL_GROUPKEY		0
#define RBI_TL_COUNT		1
#define RBI_TL_COUNT_GROUPCOL	2	/* count(group column): 0 for the NULL
									 * group, the count otherwise (§14) */
#define RBI_TL_COUNT_ZERO	3	/* count(col) where a clause pins col to NULL */
/* RBI_TL_WHEREKEY + i: the key stored in the i'th clause's entry */
#define RBI_TL_WHEREKEY		4

/*
 * Kinds of WHERE clause the pushdown understands.  EQ, ARRAY and NULL select
 * rows (they are positive sources of the count); NOTNULL removes them
 * (DESIGN.md §14: the NULL rows are exactly the members of the index's
 * reserved NULL entry, so `IS NOT NULL` is their complement).
 */
#define RBI_CLAUSE_EQ		0	/* col = const */
#define RBI_CLAUSE_ARRAY	1	/* col = ANY (const array), DESIGN.md §15 */
#define RBI_CLAUSE_NULL		2	/* col IS NULL */
#define RBI_CLAUSE_NOTNULL	3	/* col IS NOT NULL */
#define RBI_CLAUSE_MULTI	4	/* col @> / && / @@ const, DESIGN.md §17 */

#define RBI_CLAUSE_IS_POSITIVE(k)	((k) != RBI_CLAUSE_NOTNULL)

/*
 * Which clause kinds pin their column to ONE value, so that a target list
 * asking for that column can be answered with the key the entry stored.  A
 * multi-key clause pins nothing: `tags @> '{a}'` says what the array
 * contains, not what it is.
 */
#define RBI_CLAUSE_PINS_VALUE(k) \
	((k) == RBI_CLAUSE_EQ || (k) == RBI_CLAUSE_NULL)

/*
 * An `IN` list longer than this is not pushed down: every listed value needs
 * its own posting set, and each of those may hold a buffer pin for as long as
 * the node runs (DESIGN.md §9).  A long list is also exactly the case where
 * the ordinary bitmap plan does well.
 */
#define RBI_MAX_ARRAY_ELEMS		1000

/* Flag bits of the third integer of RBI_PRIV_INTS. */
#define RBI_FLAG_SINGLEGROUP	0x01
#define RBI_FLAG_SUMALL			0x02
#define RBI_FLAG_GROUPIDX		0x04	/* an index drives the entry scan */

/*
 * What the planner decided, in a form the executor can be handed through
 * custom_private.  Everything in there has to be a copyable/serialisable
 * node, so it is seven plain lists plus one filled in at plan time, behind a
 * shape marker:
 *
 *	0	IntList: RBI_PRIV_MAGIC and RBI_PRIV_NMEMBERS.  The list is
 *		positional, so the executor checks this before reading anything else:
 *		a plan made by a differently shaped build of this library (a cached
 *		plan across an upgrade, a hand-built node) is then an error and not a
 *		list silently read at the wrong offsets.  Bump RBI_PRIV_MAGIC whenever
 *		the meaning of a member changes without its position doing so.
 *	1	OidList: heap Oid, group index Oid (InvalidOid if none), then one
 *		Oid per WHERE clause, in the same order as the other lists.  For a
 *		partitioned table the heap Oid is the PARENT's (EXPLAIN resolves
 *		column names against it) and every index Oid is InvalidOid: the real
 *		ones are per partition, in RBI_PRIV_PARTS.
 *	2	IntList: base RT index, group attnum (0 if none), the RBI_FLAG_* bits,
 *		then one attnum per WHERE clause.  Attnums are the PARENT's
 *		throughout; each partition's own numbering lives in its index Oids.
 *	3	List of Const: one per WHERE clause - the compared value, the array of
 *		an IN list, or a NULL placeholder for a null test
 *	4	IntList: RBI_CLAUSE_* for each WHERE clause
 *	5	List of OidList, one per live leaf partition and empty for a plain
 *		table (DESIGN.md §16): heap Oid, group index Oid (InvalidOid if
 *		none), then one index Oid per WHERE clause
 *	6	The group column, when there is one: a two-element list holding the
 *		group Var (for its type, typmod and collation) and a one-element
 *		OidList holding the equality operator the planner chose for it.  That
 *		operator is what the cross-partition TupleHashTable groups by, and it
 *		is the equality every driving index had to agree with at plan time.
 *	7	OidList: the operator of each WHERE clause (InvalidOid for a null
 *		test), which is what EXPLAIN prints a multi-key clause with
 *	8	IntList: RBI_TL_* for each custom_scan_tlist column (added at plan
 *		time, when the target list is known)
 */
#define RBI_PRIV_VERSION	0
#define RBI_PRIV_OIDS		1
#define RBI_PRIV_INTS		2
#define RBI_PRIV_CONSTS		3
#define RBI_PRIV_CLAUSEKINDS 4
#define RBI_PRIV_PARTS		5
#define RBI_PRIV_GROUPKEY	6
#define RBI_PRIV_CLAUSEOPS	7
#define RBI_PRIV_TLKINDS	8

/* Shape of the list above: "RBI" and a shape version, and its length. */
#define RBI_PRIV_MAGIC		0x52424901
#define RBI_PRIV_NMEMBERS	9

/*
 * One WHERE clause of the pushdown, as the executor sees it.
 *
 * storedkey is the key the clause's entry holds, which is what a target list
 * that prints the pinned column has to report (see rbi_emit_tuple()).  It is
 * remembered here rather than read out of the posting set, because with
 * partitions the set is released before the row is emitted; the first
 * partition that has the key wins, and any other partition's key compares
 * equal to it by the opclass equality.
 */
typedef struct RBIClauseState
{
	int			kind;			/* RBI_CLAUSE_* */
	Oid			idxoid;
	Oid			opno;			/* the clause's operator (0 for a null test) */
	AttrNumber	attno;
	Const	   *con;			/* value, array, query, or a NULL placeholder */
	StrategyNumber strategy;	/* RBI_CLAUSE_MULTI: 2, 3 or 5 */
	Relation	idx;
	Datum		storedkey;
	bool		hasstoredkey;
	bool		keyisnull;
} RBIClauseState;

/*
 * One relation the executor counts: a plain table, or one live leaf
 * partition (DESIGN.md §16).  The index Oids are that relation's own.
 */
typedef struct RBIPartState
{
	Oid			heapoid;
	Oid			groupidxoid;	/* InvalidOid when no index drives the scan */
	Oid		   *clauseidxoid;	/* one per WHERE clause */
} RBIPartState;

typedef struct RBICountScanState
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
	RBIClauseState *clause;
	int			ntlist;
	int		   *tlkind;

	/*
	 * The relations to count.  npart is 0 for a plain table, whose heap and
	 * indexes are opened once for the life of the node; a partitioned one
	 * (DESIGN.md §16) has one RBIPartState per live leaf partition and opens
	 * them one partition at a time, so that no partition's buffer pin ever
	 * outlives that partition's processing.
	 */
	int			npart;
	RBIPartState *part;

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
	RBICountSource *sources;
	RBIPostingSet groupset;
	bool		located;
	bool		wheremissing;	/* a positive clause selects nothing at all */
	bool		scanning;
	bool		done;
	RBIEntryScan escan;

	/*
	 * GROUP BY over a partitioned table: the groups of the partitions are
	 * merged here, by the equality and hash operators of the grouping column
	 * (DESIGN.md §16).  Every partition is counted before the first row comes
	 * out, because a group may have rows in any of them.  Each entry's
	 * "additional" bytes hold the int64 running count.
	 */
	Oid			grouptype;
	int32		grouptypmod;
	Oid			groupcollid;
	Oid			groupeqop;
	TupleHashTable hashtab;
	TupleTableSlot *hashslot;	/* virtual, for probing */
	TupleTableSlot *hashoutslot;	/* minimal, for reading entries back */
	TupleDesc	hashdesc;
	MemoryContext hashmetacxt;
	MemoryContext hashtuplescxt;
	MemoryContext hashtempcxt;
	bool		hashfilled;
	TupleHashIterator hashiter;

	MemoryContext pergroup;		/* reset before each group is counted */
	MemoryContext wherecxt;		/* the located WHERE payload copies */
	MemoryContext keycxt;		/* the clause keys a target list may print */
	RBICountStats stats;
} RBICountScanState;

static Plan *rbi_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
								  CustomPath *best_path, List *tlist,
								  List *clauses, List *custom_plans);
static Node *rbi_create_custom_scan_state(CustomScan *cscan);
static void rbi_begin_custom_scan(CustomScanState *node, EState *estate,
								  int eflags);
static TupleTableSlot *rbi_exec_custom_scan(CustomScanState *node);
static void rbi_end_custom_scan(CustomScanState *node);
static void rbi_rescan_custom_scan(CustomScanState *node);
static void rbi_explain_custom_scan(CustomScanState *node, List *ancestors,
									ExplainState *es);

static const CustomPathMethods rbi_count_path_methods = {
	.CustomName = "RoaringCount",
	.PlanCustomPath = rbi_plan_custom_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods rbi_count_scan_methods = {
	.CustomName = "RoaringCount",
	.CreateCustomScanState = rbi_create_custom_scan_state,
};

static const CustomExecMethods rbi_count_exec_methods = {
	.CustomName = "RoaringCount",
	.BeginCustomScan = rbi_begin_custom_scan,
	.ExecCustomScan = rbi_exec_custom_scan,
	.EndCustomScan = rbi_end_custom_scan,
	.ReScanCustomScan = rbi_rescan_custom_scan,
	.ExplainCustomScan = rbi_explain_custom_scan,
};


/* =====================================================================
 * Planner
 * ===================================================================== */

/*
 * Peel binary-coercion relabels off an expression.  A varchar column
 * compared with a text constant arrives as RelabelType(Var) = Const, and the
 * roaring index on that column is a text_ops index, so the relabelled form is
 * exactly what we want to match.
 */
static Node *
rbi_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * Does this opfamily extract many keys from one value (DESIGN.md §17)?  The
 * presence of support function 2 is the same test rbi_fill_state() makes.
 */
static bool
rbi_opfamily_is_multikey(Oid opfamily, Oid opcintype)
{
	return OidIsValid(get_opfamily_proc(opfamily, opcintype, opcintype,
										RBI_EXTRACTVALUE_PROC));
}

/*
 * A usable roaring index on one plain column of rel, or NULL.  Only indexes
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
rbi_find_roaring_index(RelOptInfo *rel, AttrNumber attno, bool multikey)
{
	Oid			amoid = rbi_get_am_oid();
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
		if (rbi_opfamily_is_multikey(idx->opfamily[0],
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
rbi_index_equality_op(IndexOptInfo *idx)
{
	return get_opfamily_member(idx->opfamily[0], idx->opcintype[0],
							   idx->opcintype[0], RBI_STRAT_EQUAL);
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
rbi_type_equalimage(Oid typid, Oid collation)
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
rbi_index_can_emit_value(IndexOptInfo *idx)
{
	Oid			typid = idx->opcintype[0];
	Oid			idxeq = rbi_index_equality_op(idx);
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

	return rbi_type_equalimage(typid, idx->indexcollations[0]);
}

/*
 * The same, but for a column a particular clause is applied to.
 *
 * For a scalar clause (`=`, `= ANY`, a null test) opno has to be strategy 1
 * of the index's opfamily (the index's opfamily and the operator Oid, so
 * cross-type integer equality is fine), and cmptype - the type the column is
 * compared with, InvalidOid for a null test - has to be one the opfamily can
 * compare with the indexed type and can hash, or the lookup in rbi_count.c
 * would fail at run time.
 *
 * For a multi-key clause (DESIGN.md §17) the index must be a multi-key one
 * whose opfamily gives opno the strategy the planner decided on, and whose
 * extractQuery function is the very one the plan-time extraction used: the
 * plan was only made because that function called the query exact, and a
 * different function might not.
 *
 * Every partition is checked separately, because nothing stops one of them
 * from carrying a roaring index built with a different opclass.
 */
static IndexOptInfo *
rbi_match_index(RelOptInfo *rel, AttrNumber attno, int kind, Oid opno,
				Oid cmptype, StrategyNumber strategy, Oid extractquery,
				Oid exprcoll)
{
	bool		multikey = (kind == RBI_CLAUSE_MULTI);
	IndexOptInfo *idx = rbi_find_roaring_index(rel, attno, multikey);

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
							  RBI_EXTRACTQUERY_PROC) != extractquery)
			return NULL;
		return idx;
	}

	if (OidIsValid(opno) &&
		get_op_opfamily_strategy(opno, idx->opfamily[0]) != RBI_STRAT_EQUAL)
		return NULL;

	if (OidIsValid(cmptype))
	{
		if (!OidIsValid(get_opfamily_member(idx->opfamily[0],
											idx->opcintype[0], cmptype,
											RBI_STRAT_EQUAL)))
			return NULL;
		if (!OidIsValid(get_opfamily_proc(idx->opfamily[0], cmptype, cmptype,
										  RBI_HASH_PROC)))
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
 * an index is safe because rbi_match_index() checks the strategy again
 * against the index that will really answer the clause, per partition.
 */
static StrategyNumber
rbi_op_roaring_strategy(Oid opno, Oid *opfamily, Oid *lefttype)
{
	Oid			amoid = rbi_get_am_oid();
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
 * *extractquery receives the support function used, which rbi_match_index()
 * then insists on finding on every index that will answer the clause, so that
 * the run-time extraction cannot come out differently from this one.
 */
static bool
rbi_multikey_query_is_exact(Oid opfamily, Oid lefttype,
							StrategyNumber strategy, Const *con,
							Oid *extractquery)
{
	FmgrInfo	flinfo;
	RBIQuery	q;
	RBIState	state;
	MemoryContext cxt;
	MemoryContext oldcxt;
	bool		exact;

	*extractquery = get_opfamily_proc(opfamily, lefttype, lefttype,
									  RBI_EXTRACTQUERY_PROC);
	if (!OidIsValid(*extractquery))
		return false;

	/*
	 * rbi_extract_query() wants an RBIState, but only for the extractQuery
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

	rbi_extract_query(&state, con->constvalue, strategy, &q);
	exact = (q.mode == RBI_QMODE_KEYS);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return exact;
}

/*
 * One relation the executor will count, with the indexes it will use: a
 * plain table, or one live leaf partition (DESIGN.md §16).
 */
typedef struct RBICountTarget
{
	RelOptInfo *rel;			/* for the per-relation cost */
	Oid			heapoid;
	IndexOptInfo *driveidx;		/* the index whose entries are scanned, or NULL */
	List	   *whereidx;		/* IndexOptInfo *, one per WHERE clause */
} RBICountTarget;

/*
 * The attribute number a parent column has in one child, or 0 when the child
 * does not have it (a column dropped in that partition).  Partitions may
 * number their columns differently, so every attnum the pushdown carries
 * across a partition boundary goes through here (DESIGN.md §16).
 */
static AttrNumber
rbi_child_attno(PlannerInfo *root, Index childrelid, AttrNumber parentattno)
{
	AppendRelInfo *appinfo;
	Var		   *cvar;

	if (parentattno <= 0)
		return 0;
	if (childrelid == 0 || childrelid >= (Index) root->simple_rel_array_size)
		return 0;
	if (root->append_rel_array == NULL)
		return 0;
	appinfo = root->append_rel_array[childrelid];
	if (appinfo == NULL)
		return 0;
	if ((int) parentattno > list_length(appinfo->translated_vars))
		return 0;

	cvar = (Var *) list_nth(appinfo->translated_vars, parentattno - 1);
	if (cvar == NULL || !IsA(cvar, Var) || cvar->varattno <= 0)
		return 0;

	return cvar->varattno;
}

/*
 * Collect one RBICountTarget per relation the node will count: just rel when
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
 * without a usable roaring index on one of the columns.  An empty *targets
 * means everything was pruned away; the caller leaves that to the planner's
 * own dummy-rel handling.
 */
/*
 * Everything rbi_match_index() needs about one clause, gathered once by the
 * clause analysis and reused for every relation.
 */
typedef struct RBIClauseInfo
{
	AttrNumber	attno;			/* in the PARENT's numbering */
	int			kind;			/* RBI_CLAUSE_* */
	Oid			opno;			/* 0 for a null test */
	Oid			cmptype;		/* the type the column is compared with */
	StrategyNumber strategy;	/* multi-key clauses only */
	Oid			extractquery;	/* multi-key clauses only */
	Oid			collation;		/* clause input collation; InvalidOid if the operator ignores it */
	bool		valueout;		/* the target list prints this column's value,
								 * so the index has to be able to produce it
								 * (rbi_index_can_emit_value()) */
} RBIClauseInfo;

/*
 * Everything the driving index of one relation has to satisfy, gathered once
 * by rbi_try_count_path() and applied to every partition's own index.
 */
typedef struct RBIDriveInfo
{
	AttrNumber	attno;			/* in the PARENT's numbering, 0 for none */
	Oid			collation;		/* the grouping column's collation, or none */
	Oid			eqop;			/* GROUP BY: the equality the index must have
								 * as strategy 1 of its opfamily; InvalidOid
								 * when nothing groups (the sum-over-all of
								 * DESIGN.md §14 does not care how the entries
								 * partition the rows) */
	bool		valueout;		/* the group key appears in the output */
} RBIDriveInfo;

static bool
rbi_collect_targets(PlannerInfo *root, RelOptInfo *rel,
					const RBIDriveInfo *drive, List *whereattnos,
					List *clauseinfos, List **targets)
{
	RangeTblEntry *rte;
	RBICountTarget *t;
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
			RBIDriveInfo cdrive = *drive;
			List	   *cattnos = NIL;

			if (child == NULL || !bms_is_member(i, rel->live_parts))
				continue;		/* pruned at plan time */
			if (IS_DUMMY_REL(child))
				continue;		/* provably empty: it counts nothing */

			if (drive->attno != 0)
			{
				cdrive.attno = rbi_child_attno(root, child->relid,
											   drive->attno);
				if (cdrive.attno == 0)
					return false;
			}
			foreach(l1, whereattnos)
			{
				AttrNumber	ca = rbi_child_attno(root, child->relid,
												 (AttrNumber) lfirst_int(l1));

				if (ca == 0)
					return false;
				cattnos = lappend_int(cattnos, (int) ca);
			}

			if (!rbi_collect_targets(root, child, &cdrive, cattnos,
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

	t = (RBICountTarget *) palloc0(sizeof(RBICountTarget));
	t->rel = rel;
	t->heapoid = rte->relid;

	/*
	 * The driving index - a GROUP BY column's, or the one DESIGN.md §14 sums
	 * over - must be a scalar one: its entries have to be the column's
	 * values, one per row.
	 */
	if (drive->attno != 0)
	{
		t->driveidx = rbi_find_roaring_index(rel, drive->attno, false);
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
		 * merged groups that no amount of cross-partition merging can take
		 * apart.  So strategy 1 of this index's opfamily, on its own key
		 * type, has to be the very operator the planner chose for the
		 * grouping column (the 2026-09-20 review, finding 3).  This also
		 * covers the count(col) cases of DESIGN.md §14 that read the group
		 * column's entries (a real group is count(*), the NULL group is 0),
		 * because they are only reached through a grouping index.
		 */
		if (OidIsValid(drive->eqop) &&
			rbi_index_equality_op(t->driveidx) != drive->eqop)
			return false;

		/*
		 * Printing the group key means printing a key this index stored, so
		 * it has to be a representation the rows really have (finding 4).
		 */
		if (drive->valueout && !rbi_index_can_emit_value(t->driveidx))
			return false;
	}

	forboth(l1, whereattnos, l2, clauseinfos)
	{
		RBIClauseInfo *ci = (RBIClauseInfo *) lfirst(l2);
		IndexOptInfo *idx = rbi_match_index(rel, (AttrNumber) lfirst_int(l1),
											ci->kind, ci->opno, ci->cmptype,
											ci->strategy, ci->extractquery,
											ci->collation);

		if (idx == NULL)
			return false;

		/* Same rule for a pinned column whose value the output prints. */
		if (ci->valueout && !rbi_index_can_emit_value(idx))
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
rbi_agg_is_count(Aggref *agg, Index rti, RelOptInfo *rel,
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
	arg = rbi_strip((Node *) tle->expr);
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
 *	  index's container pages proportional to the clause selectivity;
 *	- for a GROUP BY: every page of the group index;
 *	- one O(1) step per container (the visibility map is read per container);
 *	- the heap the visibility map cannot vouch for: the TIDs on blocks that
 *	  are not all-visible (pg_class.relallvisible via RelOptInfo.allvisfrac),
 *	  each rechecked in the heap, on as many distinct blocks as there can be.
 *
 * The bucket pages are NOT charged wholesale: an index carries at least
 * RBI_DEFAULT_BUCKETS of them, which would price a single-key count on a
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
rbi_index_bucket_pages(IndexOptInfo *idx)
{
	Relation	indexrel = index_open(idx->indexoid, AccessShareLock);
	RBIState   *state = rbi_get_state(indexrel);
	double		nbuckets = (double) state->meta.nbuckets;

	index_close(indexrel, AccessShareLock);
	return nbuckets;
}

static Cost
rbi_cost_count_rel(PlannerInfo *root, RelOptInfo *rel,
				   IndexOptInfo *groupidx,
				   List *whereidx, List *whereclauses, double numgroups)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		dirty_pages;
	double		matching = Max(rel->rows, 1.0);
	double		containers_per_key;
	double		random_pages = 0;	/* bucket page lookups */
	double		seq_pages = 0;	/* container chains, read in order */
	double		ncontainers;
	double		recheck_tids;
	double		recheck_pages;
	Cost		run;
	ListCell   *lc1;
	ListCell   *lc2;

	/*
	 * A key's posting set has at most one container per
	 * RBI_BLOCKS_PER_CONTAINER heap pages and at most one per matching row.
	 */
	containers_per_key = Min(heap_pages / RBI_BLOCKS_PER_CONTAINER, matching);
	containers_per_key = Max(containers_per_key, 1.0);

	forboth(lc1, whereidx, lc2, whereclauses)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Selectivity sel = clause_selectivity(root, (Node *) lfirst(lc2),
											 0, JOIN_INNER, NULL);
		double		container_pages;

		container_pages = (double) idx->pages - 1.0 - rbi_index_bucket_pages(idx);
		container_pages = Max(container_pages, 0.0);

		random_pages += 1.0;
		seq_pages += Max(1.0, container_pages * sel);
	}
	if (groupidx != NULL)
		seq_pages += Max(1.0, (double) groupidx->pages);

	if (groupidx != NULL)
		ncontainers = numgroups * Max(1.0, Min(heap_pages / RBI_BLOCKS_PER_CONTAINER,
											   matching / Max(numgroups, 1.0)));
	else
		ncontainers = containers_per_key;
	ncontainers *= Max(1, list_length(whereidx) + (groupidx != NULL ? 1 : 0));

	/*
	 * Rechecking is what makes the pushdown expensive, and the estimate has
	 * to say so: every TID whose heap block the visibility map cannot vouch
	 * for is fetched from the heap.  Charge cpu_tuple_cost for each of them
	 * and one random page fetch per distinct block they are expected to
	 * touch.
	 *
	 * The blocks that can be touched are only the ones the visibility map
	 * cannot vouch for - heap_pages * dirtyfrac, from the same
	 * relallvisible/relpages the TID estimate comes from - and a single count
	 * visits each of them at most once, because it walks the merged result in
	 * TID order.  Charging random reads across the WHOLE heap instead made
	 * the model refuse the pushdown on freshly vacuumed tables, where it is
	 * at its best (the 2026-09-20 review, finding 5).
	 *
	 * A GROUP BY is the case that really does return to a block repeatedly:
	 * every group whose rows include a tuple on a dirty block pins it again,
	 * so the visits are numgroups per dirty block, bounded by the number of
	 * rechecked TIDs (a block cannot be visited more often than it has
	 * candidate TIDs on it).  That keeps the case the model exists for - a
	 * thousand groups of one TID per block on a heap the visibility map
	 * cannot vouch for - losing to the sequential scan that would do the
	 * same work once.
	 */
	dirty_pages = Min(heap_pages * dirtyfrac, heap_pages);
	recheck_tids = matching * dirtyfrac;
	if (groupidx != NULL)
		recheck_pages = Min(numgroups * dirty_pages, recheck_tids);
	else
		recheck_pages = Min(recheck_tids, dirty_pages);

	run = random_pages * random_page_cost;
	run += seq_pages * seq_page_cost;
	run += ncontainers * cpu_operator_cost * 2.0;	/* block mask + VM mask */
	run += recheck_pages * random_page_cost;
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
rbi_cost_count_path(PlannerInfo *root, CustomPath *cpath, List *targets,
					List *whereclauses, List *wherekinds, double numgroups,
					double outrows, double hashentrysize)
{
	Cost		run = 0;
	ListCell   *lc;

	foreach(lc, targets)
	{
		RBICountTarget *t = (RBICountTarget *) lfirst(lc);
		List	   *costidx = NIL;
		List	   *costclauses = NIL;
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;

		forthree(l1, t->whereidx, l2, whereclauses, l3, wherekinds)
		{
			if (!RBI_CLAUSE_IS_POSITIVE(lfirst_int(l3)))
				continue;
			costidx = lappend(costidx, lfirst(l1));
			costclauses = lappend(costclauses, lfirst(l2));
		}

		run += rbi_cost_count_rel(root, t->rel, t->driveidx, costidx,
								  costclauses, numgroups);

		list_free(costidx);
		list_free(costclauses);
	}

	cpath->path.rows = outrows;
	cpath->path.disabled_nodes = 0;

	if (hashentrysize > 0.0)
	{
		/*
		 * A partitioned GROUP BY merges the partitions' groups in a hash
		 * table (DESIGN.md §16) and emits nothing until the last partition
		 * has been counted: the whole scan is startup work, and the merge
		 * itself is one materialized entry of hashentrysize bytes per group.
		 * Charging that makes a plan whose only cost was per-group memory
		 * comparable with the Agg it replaces (the 2026-09-20 review,
		 * finding 5); the hard budget is at plan time, in
		 * rbi_try_count_path().
		 */
		run += numgroups * hashentrysize * cpu_operator_cost;
		cpath->path.startup_cost = run;
	}
	else
	{
		/* Without GROUP BY the single output row needs the whole scan first. */
		cpath->path.startup_cost = (outrows <= 1.0) ? run : 0.0;
	}
	cpath->path.total_cost = run;
}

/*
 * Number of elements of a Const array, or -1 when it is not a plain array.
 */
static int
rbi_array_const_nelems(Const *con)
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
 * Decide whether count(*) over input_rel can be answered from roaring
 * posting sets and, if so, add a CustomPath to output_rel.  Every failed
 * check simply returns: the normal plan is always available.
 */
static void
rbi_try_count_path(PlannerInfo *root, RelOptInfo *input_rel,
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
	RBIDriveInfo drive;
	double		hashentrysize = 0.0;
	List	   *whereattnos = NIL;	/* its column, in the PARENT's numbering */
	List	   *clauseinfos = NIL;	/* RBIClauseInfo, one per clause */
	List	   *whereclauses = NIL; /* the clause, for selectivity */
	List	   *whereconsts = NIL;	/* its Const (a placeholder for a null test) */
	List	   *wherekinds = NIL;	/* RBI_CLAUSE_* */
	List	   *whereopnos = NIL;	/* the clause's operator (0 for a null test) */
	List	   *posattnos = NIL;	/* columns with a positive clause */
	List	   *eqattnos = NIL;		/* columns pinned to one value */
	List	   *nonnullattnos = NIL;	/* columns a clause proves non-null */
	List	   *nullattnos = NIL;	/* columns a clause pins to NULL */
	List	   *targets = NIL;		/* RBICountTarget, one per counted relation */
	RBICountTarget *first;
	Var		   *notnullvar = NULL;	/* the first `IS NOT NULL` column */
	List	   *oids;
	List	   *ints;
	List	   *consts = NIL;
	List	   *ckinds = NIL;
	List	   *parts = NIL;
	List	   *groupkey = NIL;
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
	if (extra != NULL && extra->patype != PARTITIONWISE_AGGREGATE_NONE)
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
		 * that for a column with an equality qual against a Const), so the
		 * query has exactly one group - but, unlike a plain aggregate, it
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
		expr = rbi_strip((Node *) tle->expr);
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
		 * relation or many (rbi_collect_targets(), finding 3 of the
		 * 2026-09-20 review), so a grouping clause without one is of no use
		 * here.
		 */
		groupeqop = sgc->eqop;
		if (!OidIsValid(groupeqop))
			return;

		/*
		 * The groups of all the partitions are merged in a TupleHashTable
		 * (DESIGN.md §16) built from that operator, so with more than one
		 * relation the column also has to be hashable.  One table needs no
		 * merging and does not care.
		 */
		if (partitioned && !sgc->hashable)
			return;
	}

	/* ---- every WHERE clause must be one the posting sets can answer ---- */
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *clause;
		Var		   *var = NULL;
		Const	   *con = NULL;
		Oid			opno = InvalidOid;	/* operator the index must know */
		Oid			cmptype = InvalidOid;	/* type the index is compared with */
		StrategyNumber strategy = 0;	/* multi-key clauses only */
		Oid			extractquery = InvalidOid;
		RBIClauseInfo *ci;
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

			left = rbi_strip((Node *) linitial(op->args));
			right = rbi_strip((Node *) lsecond(op->args));
			if (left == NULL || right == NULL)
				return;

			strategy = rbi_op_roaring_strategy(op->opno, &opfamily, &lefttype);

			if (strategy == RBI_STRAT_EQUAL)
			{
				/* Equality commutes, so either side may hold the column. */
				if (IsA(left, Var) && IsA(right, Const))
				{
					var = (Var *) left;
					con = (Const *) right;
				}
				else if (IsA(left, Const) && IsA(right, Var))
				{
					var = (Var *) right;
					con = (Const *) left;
				}
				else
					return;

				if (con->constisnull)
					return;

				opno = op->opno;
				cmptype = con->consttype;
				kind = RBI_CLAUSE_EQ;
			}
			else if (strategy == RBI_STRAT_CONTAINS ||
					 strategy == RBI_STRAT_OVERLAP ||
					 strategy == RBI_STRAT_MATCH)
			{
				/*
				 * A multi-key operator (DESIGN.md §17).  Unlike equality it
				 * does not commute - `'{a}' @> tags` is a containment the
				 * other way round, which is strategy 4 and not pushed down -
				 * so the column has to be the left operand.
				 */
				if (!IsA(left, Var) || !IsA(right, Const))
					return;
				var = (Var *) left;
				con = (Const *) right;
				if (con->constisnull)
					return;

				/*
				 * Only an EXACT query is pushed down.  `tags @> '{}'`, `<@`,
				 * a tsquery with NOT/phrase/prefix/weights and anything with
				 * a NULL element all want every row rechecked in the heap,
				 * which is what the ordinary plan does anyway.
				 */
				if (!rbi_multikey_query_is_exact(opfamily, lefttype, strategy,
												 con, &extractquery))
					return;

				opno = op->opno;
				cmptype = InvalidOid;	/* the query is not a key */
				kind = RBI_CLAUSE_MULTI;
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

			left = rbi_strip((Node *) linitial(saop->args));
			right = rbi_strip((Node *) lsecond(saop->args));
			if (left == NULL || right == NULL)
				return;
			if (!IsA(left, Var) || !IsA(right, Const))
				return;
			var = (Var *) left;
			con = (Const *) right;
			if (con->constisnull)
				return;

			nelems = rbi_array_const_nelems(con);
			if (nelems < 0 || nelems > RBI_MAX_ARRAY_ELEMS)
				return;

			/*
			 * `col op ANY (array)` is a union of single-key lookups, so the
			 * operator has to be equality; `tags @> ANY (...)` would be a
			 * union of multi-key queries, which nothing here builds.
			 */
			{
				Oid			opfamily;
				Oid			lefttype;

				if (rbi_op_roaring_strategy(saop->opno, &opfamily,
											&lefttype) != RBI_STRAT_EQUAL)
					return;
			}

			opno = saop->opno;
			cmptype = get_element_type(con->consttype);
			if (!OidIsValid(cmptype))
				return;
			kind = RBI_CLAUSE_ARRAY;
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;
			Node	   *arg;

			if (nt->argisrow)
				return;
			arg = rbi_strip((Node *) nt->arg);
			if (arg == NULL || !IsA(arg, Var))
				return;
			var = (Var *) arg;

			kind = (nt->nulltesttype == IS_NULL) ?
				RBI_CLAUSE_NULL : RBI_CLAUSE_NOTNULL;
			/* The executor needs no value; keep the lists in step. */
			con = makeNullConst(var->vartype, var->vartypmod, var->varcollid);
		}
		else
			return;

		if (var->varno != (int) rti || var->varattno <= 0 ||
			var->varlevelsup != 0)
			return;

		/*
		 * Which index answers the clause, whether its opfamily has the
		 * operator as strategy 1, and whether it can hash and compare the
		 * constant's type is settled per relation, in rbi_match_index():
		 * with partitions there is one index per partition and they need not
		 * share an opclass (DESIGN.md §16).
		 */

		if (RBI_CLAUSE_IS_POSITIVE(kind))
			havepositive = true;

		if (RBI_CLAUSE_IS_POSITIVE(kind) && kind != RBI_CLAUSE_MULTI)
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
						lfirst_int(l3) == RBI_CLAUSE_NOTNULL)
						continue;
					same = (lfirst_int(l3) == kind &&
							equal((Const *) lfirst(l2), con));
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
			case RBI_CLAUSE_EQ:
				eqattnos = lappend_int(eqattnos, (int) var->varattno);
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case RBI_CLAUSE_NOTNULL:
				if (notnullvar == NULL)
					notnullvar = var;
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case RBI_CLAUSE_ARRAY:
			case RBI_CLAUSE_MULTI:
				/* a strict operator with a non-NULL constant */
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case RBI_CLAUSE_NULL:
				nullattnos = lappend_int(nullattnos, (int) var->varattno);
				break;
		}

		ci = (RBIClauseInfo *) palloc0(sizeof(RBIClauseInfo));
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
		whereconsts = lappend(whereconsts, con);
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
			if (!rbi_agg_is_count((Aggref *) node, rti, input_rel,
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
		RBIClauseInfo *ci = (RBIClauseInfo *) lfirst(lc);

		ci->valueout = (ci->kind == RBI_CLAUSE_EQ &&
						list_member_int(valueattnos, (int) ci->attno));
	}

	/*
	 * ---- the relations to count, and the indexes on each of them ----
	 *
	 * One table, or one live leaf partition at a time (DESIGN.md §16).  The
	 * column numbers above are the parent's; rbi_collect_targets() maps them
	 * onto each partition through its AppendRelInfo before looking an index
	 * up, because partitions may number their columns differently.
	 */
	drive.attno = driveattno;
	drive.collation = (groupvar != NULL) ? groupvar->varcollid : InvalidOid;
	drive.eqop = (groupvar != NULL) ? groupeqop : InvalidOid;
	drive.valueout = groupvalueout;

	if (!rbi_collect_targets(root, input_rel, &drive, whereattnos,
							 clauseinfos, &targets))
		return;
	if (targets == NIL)
		return;					/* everything was pruned: leave it to the planner */
	first = (RBICountTarget *) linitial(targets);

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

	outrows = (groupvar != NULL) ? numgroups : 1.0;

	/*
	 * A partitioned GROUP BY keeps every group of every partition in a
	 * TupleHashTable until the last partition has been counted (DESIGN.md
	 * §16), and that table has no spill path: the node cannot fall back to a
	 * sorted merge the way HashAggregate falls back to disk.  So decline at
	 * plan time when the estimated table does not fit in the budget
	 * HashAggregate itself respects - work_mem * hash_mem_multiplier, via
	 * get_hash_memory_limit() - and let the ordinary Agg, which can spill,
	 * have the query (the 2026-09-20 review, finding 6).
	 *
	 * The estimate is one entry per group: the key (its average width, or its
	 * length when it is fixed) plus the minimal tuple's header, the hash
	 * table's own per-entry bookkeeping and the int64 count, for which 64
	 * bytes is the round number.
	 */
	if (partitioned && groupvar != NULL)
	{
		int32		width = get_attavgwidth(rte->relid, groupattno);

		if (width <= 0)
			width = get_typavgwidth(groupvar->vartype, groupvar->vartypmod);

		hashentrysize = (double) (MAXALIGN(width) + 64);
		if (numgroups * hashentrysize > (double) get_hash_memory_limit())
			return;
	}

	/*
	 * A plain table's own Oids go in RBI_PRIV_OIDS, which is where the
	 * executor and EXPLAIN have always read them.  A partitioned one leaves
	 * them invalid - there is no single index - and fills RBI_PRIV_PARTS
	 * instead, one OidList per partition in the same clause order.
	 */
	oids = list_make2_oid(rte->relid,
						  (!partitioned && first->driveidx != NULL) ?
						  first->driveidx->indexoid : InvalidOid);
	ints = list_make3_int((int) rti, (int) groupattno,
						  (singlegroup ? RBI_FLAG_SINGLEGROUP : 0) |
						  (sumall ? RBI_FLAG_SUMALL : 0) |
						  (driveattno != 0 ? RBI_FLAG_GROUPIDX : 0));
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
			consts = lappend(consts, copyObject((Const *) lfirst(l2)));
			ckinds = lappend_int(ckinds, lfirst_int(l3));
			i++;
		}
	}

	if (partitioned)
	{
		foreach(lc, targets)
		{
			RBICountTarget *t = (RBICountTarget *) lfirst(lc);
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
	 * The group column's type, typmod and collation (from the Var) and the
	 * equality operator the planner chose for it: what the cross-partition
	 * TupleHashTable of DESIGN.md §16 is built from.
	 */
	if (groupvar != NULL)
		groupkey = list_make2(copyObject(groupvar), list_make1_oid(groupeqop));

	/*
	 * The strategy of a multi-key clause travels with its operator: the
	 * executor re-extracts the query and EXPLAIN prints the operator's name,
	 * and both need the Oid.
	 */

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.pathkeys = NIL;	/* groups come out in bucket order */
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_restrictinfo = NIL;
	/*
	 * The shape marker comes first, so that rbi_begin_custom_scan() can
	 * refuse a list it does not recognise instead of reading it positionally.
	 */
	cpath->custom_private = list_make1(list_make2_int(RBI_PRIV_MAGIC,
													  RBI_PRIV_NMEMBERS));
	cpath->custom_private = lappend(cpath->custom_private, oids);
	cpath->custom_private = lappend(cpath->custom_private, ints);
	cpath->custom_private = lappend(cpath->custom_private, consts);
	cpath->custom_private = lappend(cpath->custom_private, ckinds);
	cpath->custom_private = lappend(cpath->custom_private, parts);
	cpath->custom_private = lappend(cpath->custom_private, groupkey);
	cpath->custom_private = lappend(cpath->custom_private, whereopnos);
	cpath->methods = &rbi_count_path_methods;

	rbi_cost_count_path(root, cpath, targets, whereclauses, wherekinds,
						numgroups, outrows, hashentrysize);

	add_path(output_rel, &cpath->path);
}

void
rbi_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	if (rbi_prev_create_upper_paths_hook != NULL)
		rbi_prev_create_upper_paths_hook(root, stage, input_rel, output_rel,
										 extra);

	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (!rbi_enable_count_pushdown)
		return;

	rbi_try_count_path(root, input_rel, output_rel,
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
rbi_plan_custom_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *ctlist = NIL;
	List	   *kinds = NIL;
	List	   *ints;
	List	   *ckinds;
	AttrNumber	groupattno;
	ListCell   *lc;

	/*
	 * The name has to be resolvable before any CustomScan node of ours is
	 * written out or read back; plan time is the first moment that can
	 * happen, and registering twice is harmless.
	 */
	if (GetCustomScanMethods("RoaringCount", true) == NULL)
		RegisterCustomScanMethods(&rbi_count_scan_methods);

	ints = (List *) list_nth(best_path->custom_private, RBI_PRIV_INTS);
	ckinds = (List *) list_nth(best_path->custom_private, RBI_PRIV_CLAUSEKINDS);
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
			kind = RBI_TL_COUNT;
			if (agg->args != NIL)
			{
				Node	   *arg = rbi_strip((Node *)
											((TargetEntry *) linitial(agg->args))->expr);
				AttrNumber	attno;
				int			i;

				Assert(arg != NULL && IsA(arg, Var));
				attno = ((Var *) arg)->varattno;

				if (groupattno != 0 && attno == groupattno)
					kind = RBI_TL_COUNT_GROUPCOL;
				else
				{
					for (i = 0; i < list_length(ckinds); i++)
					{
						if (list_nth_int(ckinds, i) == RBI_CLAUSE_NULL &&
							list_nth_int(ints, 3 + i) == (int) attno)
						{
							kind = RBI_TL_COUNT_ZERO;
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
				kind = RBI_TL_GROUPKEY;
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

					if (ckind != RBI_CLAUSE_EQ && ckind != RBI_CLAUSE_NULL)
						continue;
					if (list_nth_int(ints, i) == (int) attno)
					{
						kind = RBI_TL_WHEREKEY + (i - 3);
						break;
					}
				}
				if (kind < 0)
					elog(ERROR, "RoaringCount: column %d is neither grouped nor constrained",
						 attno);
			}
		}
		else
			elog(ERROR, "unexpected expression in RoaringCount target list");

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
	cscan->custom_exprs = NIL;
	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->custom_private = lappend(list_copy(best_path->custom_private),
									kinds);
	cscan->methods = &rbi_count_scan_methods;

	return &cscan->scan.plan;
}


/* =====================================================================
 * Executor
 * ===================================================================== */

static Node *
rbi_create_custom_scan_state(CustomScan *cscan)
{
	RBICountScanState *st = (RBICountScanState *)
		newNode(sizeof(RBICountScanState), T_CustomScanState);

	st->css.methods = &rbi_count_exec_methods;
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
rbi_open_relation(RBICountScanState *st, Oid heapoid, Oid groupidxoid,
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
rbi_close_relation(RBICountScanState *st)
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

/*
 * Build the TupleHashTable that merges the groups of the partitions
 * (DESIGN.md §16).  One column - the grouping column - hashed and compared
 * with the operators the planner chose for the GROUP BY clause, so that a
 * NULL group merges like any other and a type whose equality is not bytewise
 * (citext, say) groups the way the query says it should.
 */
static void
rbi_build_group_hash(RBICountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	Oid		   *eqfuncoids;
	FmgrInfo   *hashfunctions;
	AttrNumber *keycol;
	Oid		   *collations;
	double		nelements;

	Assert(OidIsValid(st->grouptype) && OidIsValid(st->groupeqop));

	st->hashdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(st->hashdesc, (AttrNumber) 1, "groupkey",
					   st->grouptype, st->grouptypmod, 0);
	TupleDescInitEntryCollation(st->hashdesc, (AttrNumber) 1, st->groupcollid);
	TupleDescFinalize(st->hashdesc);

	st->hashslot = MakeSingleTupleTableSlot(st->hashdesc, &TTSOpsVirtual);
	st->hashoutslot = MakeSingleTupleTableSlot(st->hashdesc, &TTSOpsMinimalTuple);

	st->hashmetacxt = AllocSetContextCreate(estate->es_query_cxt,
											"RoaringCount group hash meta",
											ALLOCSET_SMALL_SIZES);
	st->hashtuplescxt = AllocSetContextCreate(estate->es_query_cxt,
											  "RoaringCount group hash tuples",
											  ALLOCSET_DEFAULT_SIZES);
	st->hashtempcxt = AllocSetContextCreate(estate->es_query_cxt,
											"RoaringCount group hash temp",
											ALLOCSET_SMALL_SIZES);

	execTuplesHashPrepare(1, &st->groupeqop, &eqfuncoids, &hashfunctions);

	keycol = (AttrNumber *) palloc(sizeof(AttrNumber));
	keycol[0] = 1;
	collations = (Oid *) palloc(sizeof(Oid));
	collations[0] = st->groupcollid;

	nelements = st->css.ss.ps.plan->plan_rows;
	if (!(nelements >= 16.0))
		nelements = 16.0;

	st->hashtab = BuildTupleHashTable(&st->css.ss.ps,
									  st->hashdesc,
									  &TTSOpsVirtual,
									  1,
									  keycol,
									  eqfuncoids,
									  hashfunctions,
									  collations,
									  nelements,
									  sizeof(int64),
									  st->hashmetacxt,
									  st->hashtuplescxt,
									  st->hashtempcxt,
									  false);
}

static void
rbi_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
	RBICountScanState *st = (RBICountScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *shape;
	List	   *oids;
	List	   *ints;
	List	   *consts;
	List	   *ckinds;
	List	   *partlist;
	List	   *groupkey;
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
	shape = (list_length(cscan->custom_private) == RBI_PRIV_NMEMBERS) ?
		(List *) list_nth(cscan->custom_private, RBI_PRIV_VERSION) : NIL;
	if (shape == NIL || !IsA(shape, IntList) || list_length(shape) != 2 ||
		linitial_int(shape) != RBI_PRIV_MAGIC ||
		lsecond_int(shape) != RBI_PRIV_NMEMBERS)
		elog(ERROR, "RoaringCount: unrecognized custom_private shape (%d members)",
			 list_length(cscan->custom_private));

	oids = (List *) list_nth(cscan->custom_private, RBI_PRIV_OIDS);
	ints = (List *) list_nth(cscan->custom_private, RBI_PRIV_INTS);
	consts = (List *) list_nth(cscan->custom_private, RBI_PRIV_CONSTS);
	ckinds = (List *) list_nth(cscan->custom_private, RBI_PRIV_CLAUSEKINDS);
	partlist = (List *) list_nth(cscan->custom_private, RBI_PRIV_PARTS);
	groupkey = (List *) list_nth(cscan->custom_private, RBI_PRIV_GROUPKEY);
	clauseops = (List *) list_nth(cscan->custom_private, RBI_PRIV_CLAUSEOPS);
	kinds = (List *) list_nth(cscan->custom_private, RBI_PRIV_TLKINDS);

	st->heapoid = linitial_oid(oids);
	st->groupidxoid = lsecond_oid(oids);
	st->scanrelid = (Index) linitial_int(ints);
	st->groupattno = (AttrNumber) lsecond_int(ints);
	flags = lthird_int(ints);
	st->singlegroup = (flags & RBI_FLAG_SINGLEGROUP) != 0;
	st->sumall = (flags & RBI_FLAG_SUMALL) != 0;
	st->hasgroupidx = (flags & RBI_FLAG_GROUPIDX) != 0;
	st->nclause = list_length(consts);

	st->ntlist = list_length(kinds);
	st->tlkind = (int *) palloc(sizeof(int) * Max(st->ntlist, 1));
	for (i = 0; i < st->ntlist; i++)
		st->tlkind[i] = list_nth_int(kinds, i);

	st->clause = (RBIClauseState *)
		palloc0(sizeof(RBIClauseState) * Max(st->nclause, 1));
	for (i = 0; i < st->nclause; i++)
	{
		st->clause[i].kind = list_nth_int(ckinds, i);
		st->clause[i].idxoid = list_nth_oid(oids, 2 + i);
		st->clause[i].attno = (AttrNumber) list_nth_int(ints, 3 + i);
		st->clause[i].con = (Const *) list_nth(consts, i);
		st->clause[i].opno = list_nth_oid(clauseops, i);
		st->clause[i].strategy = 0;
	}

	/* One target per live leaf partition, in the planner's order. */
	st->npart = list_length(partlist);
	if (st->npart > 0)
	{
		st->part = (RBIPartState *) palloc0(sizeof(RBIPartState) * st->npart);
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

	if (groupkey != NIL)
	{
		Var		   *gv = (Var *) linitial(groupkey);

		st->grouptype = gv->vartype;
		st->grouptypmod = gv->vartypmod;
		st->groupcollid = gv->varcollid;
		st->groupeqop = linitial_oid((List *) lsecond(groupkey));
	}

	st->located = false;
	st->wheremissing = false;
	st->scanning = false;
	st->done = false;
	st->hashfilled = false;
	memset(&st->stats, 0, sizeof(st->stats));

	st->pergroup = AllocSetContextCreate(estate->es_query_cxt,
										 "RoaringCount per-group",
										 ALLOCSET_SMALL_SIZES);
	st->wherecxt = AllocSetContextCreate(estate->es_query_cxt,
										 "RoaringCount where keys",
										 ALLOCSET_SMALL_SIZES);
	st->keycxt = AllocSetContextCreate(estate->es_query_cxt,
									   "RoaringCount clause keys",
									   ALLOCSET_SMALL_SIZES);

	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0)
		return;

	/*
	 * A plain table is opened once and stays open.  The executor already
	 * holds locks on every range table entry, so the heap is opened without
	 * taking another one.  The indexes are not range table entries, so they
	 * get their own AccessShareLock.
	 *
	 * A partitioned one opens nothing here: rbi_open_relation() opens one
	 * partition at a time (DESIGN.md §16).
	 */
	if (st->npart == 0)
		rbi_open_relation(st, st->heapoid, st->groupidxoid, NULL);

	/* slot 0 is the group's posting set, 1..nclause the WHERE clauses */
	st->sources = (RBICountSource *)
		palloc0(sizeof(RBICountSource) * (st->nclause + 1));
	st->sources[0].nsets = 1;
	st->sources[0].sets = &st->groupset;
	st->sources[0].negated = false;

	/* The cross-partition group merge (DESIGN.md §16). */
	if (st->npart > 0 && st->hasgroupidx && st->groupattno != 0)
		rbi_build_group_hash(st);
}

/*
 * Locate the posting sets of one `col = ANY (array)` clause (DESIGN.md §15):
 * one set per distinct non-NULL element, which the count then unions.
 */
static void
rbi_locate_array(RBICountScanState *st, RBIClauseState *cl,
				 RBICountSource *src)
{
	ArrayType  *arr = DatumGetArrayTypeP(cl->con->constvalue);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			nsets = 0;
	int			nfound = 0;
	int			i;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	src->sets = (RBIPostingSet *)
		palloc0(sizeof(RBIPostingSet) * Max(nelems, 1));

	for (i = 0; i < nelems; i++)
	{
		bool		dup = false;
		int			j;

		if (nulls[i])
			continue;			/* `col = NULL` is never true */

		/*
		 * Looking a value up twice would only make the union do the same work
		 * again; it could not change the answer, because a union of a set
		 * with itself is that set.  The test is the bytewise one, so two
		 * values that compare equal without being identical still get a set
		 * each, which is equally harmless.  The planner caps the list at
		 * RBI_MAX_ARRAY_ELEMS, which bounds this loop.
		 */
		for (j = 0; j < i; j++)
		{
			if (!nulls[j] &&
				datumIsEqual(elems[i], elems[j], elmbyval, elmlen))
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		if (rbi_posting_set_lookup(cl->idx, elems[i], elemtype,
								   &src->sets[nsets]))
			nfound++;
		nsets++;
	}

	src->nsets = nsets;

	/* An empty array, an all-NULL one, or no matching key: no rows at all. */
	if (nfound == 0)
		st->wheremissing = true;

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(cl->con->constvalue))
		pfree(arr);
}

/*
 * Locate the posting sets of one multi-key clause (DESIGN.md §17).
 *
 * The query is extracted again here, with the index's OWN extractQuery
 * function - which rbi_match_index() has already insisted is the one the
 * planner used - so the keys and the boolean tree are the same the plan was
 * costed with.  The tree becomes the source's combining expression; the
 * merge in rbi_count.c evaluates it over the sets with the same cursors it
 * uses for an IN list, so the DESIGN.md §9 pin discipline is unchanged.
 */
static void
rbi_locate_multikey(RBICountScanState *st, RBIClauseState *cl,
					RBICountSource *src)
{
	RBIState   *istate = rbi_get_state(cl->idx);
	RBIQuery	q;
	int			i;

	rbi_extract_query(istate, cl->con->constvalue,
					  (StrategyNumber) get_op_opfamily_strategy(cl->opno,
																cl->idx->rd_opfamily[0]),
					  &q);

	/*
	 * The plan was only made because this extraction came out exact
	 * (rbi_multikey_query_is_exact()), against this very function and this
	 * very constant.  A different answer now would mean the count could
	 * silently miss rows, so say so instead.
	 */
	if (q.mode != RBI_QMODE_KEYS)
		elog(ERROR, "roaring count: query for index \"%s\" is no longer exact",
			 RelationGetRelationName(cl->idx));

	src->sets = (RBIPostingSet *) palloc0(sizeof(RBIPostingSet) * q.nkeys);
	src->nsets = q.nkeys;
	src->tree = q.tree;

	for (i = 0; i < q.nkeys; i++)
	{
		(void) rbi_posting_set_lookup(cl->idx, q.keys[i], InvalidOid,
									  &src->sets[i]);
		CHECK_FOR_INTERRUPTS();
	}

	/* An AND over a key with no entry at all selects nothing anywhere. */
	if (!rbi_sets_satisfiable(src->nsets, src->sets, src->tree))
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
rbi_save_clause_key(RBICountScanState *st, RBIClauseState *cl,
					const RBIPostingSet *ps)
{
	MemoryContext oldcxt;
	RBIState   *istate;

	if (cl->hasstoredkey || !ps->found || !ps->hasstoredkey)
		return;

	if (ps->keyisnull)
	{
		cl->storedkey = (Datum) 0;
		cl->keyisnull = true;
		cl->hasstoredkey = true;
		return;
	}

	istate = rbi_get_state(cl->idx);
	oldcxt = MemoryContextSwitchTo(st->keycxt);
	cl->storedkey = datumCopy(ps->storedkey, istate->typbyval, istate->typlen);
	MemoryContextSwitchTo(oldcxt);
	cl->keyisnull = false;
	cl->hasstoredkey = true;
}

/*
 * Locate the posting sets of every WHERE clause of the relation the node is
 * counting.  They keep their pins (for INLINE entries) until
 * rbi_release_where(), which is exactly the DESIGN.md section 9 discipline
 * applied for the length of that relation's processing rather than for one
 * container.  With partitions that is one partition's turn; with a plain
 * table it is the whole node execution.
 */
static void
rbi_locate_where(RBICountScanState *st)
{
	MemoryContext oldcxt;
	int			i;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);

	for (i = 0; i < st->nclause; i++)
	{
		RBIClauseState *cl = &st->clause[i];
		RBICountSource *src = &st->sources[i + 1];

		src->negated = (cl->kind == RBI_CLAUSE_NOTNULL);
		src->nsets = 0;
		src->sets = NULL;

		switch (cl->kind)
		{
			case RBI_CLAUSE_EQ:
				src->sets = (RBIPostingSet *) palloc0(sizeof(RBIPostingSet));
				src->nsets = 1;
				if (!rbi_posting_set_lookup(cl->idx, cl->con->constvalue,
											cl->con->consttype, &src->sets[0]))
					st->wheremissing = true;
				break;

			case RBI_CLAUSE_ARRAY:
				rbi_locate_array(st, cl, src);
				break;

			case RBI_CLAUSE_MULTI:
				rbi_locate_multikey(st, cl, src);
				break;

			case RBI_CLAUSE_NULL:
			case RBI_CLAUSE_NOTNULL:
				src->sets = (RBIPostingSet *) palloc0(sizeof(RBIPostingSet));
				src->nsets = 1;
				if (!rbi_posting_set_lookup_null(cl->idx, &src->sets[0]))
				{
					/*
					 * No NULL entry at all: `IS NULL` selects nothing, and
					 * `IS NOT NULL` has nothing to subtract.
					 */
					if (cl->kind == RBI_CLAUSE_NULL)
						st->wheremissing = true;
					else
						src->nsets = 0;
				}
				break;

			default:
				elog(ERROR, "RoaringCount: unknown clause kind %d", cl->kind);
		}

		/*
		 * Only a clause that pins the column to ONE value can have its key
		 * printed, and those are the ones with a single set.
		 */
		if (RBI_CLAUSE_PINS_VALUE(cl->kind) && src->nsets == 1)
			rbi_save_clause_key(st, cl, &src->sets[0]);
	}

	MemoryContextSwitchTo(oldcxt);
	st->located = true;
}

static void
rbi_release_where(RBICountScanState *st)
{
	int			i;
	int			j;

	if (st->sources == NULL)
		return;

	for (i = 0; i < st->nclause; i++)
	{
		RBICountSource *src = &st->sources[i + 1];

		for (j = 0; j < src->nsets; j++)
			rbi_posting_set_release(&src->sets[j]);
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
rbi_emit_tuple(RBICountScanState *st, Datum key, bool keyisnull, int64 count)
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
			case RBI_TL_GROUPKEY:
				slot->tts_values[i] = key;
				slot->tts_isnull[i] = keyisnull;
				break;

			case RBI_TL_COUNT:
				slot->tts_values[i] = Int64GetDatum(count);
				break;

			case RBI_TL_COUNT_GROUPCOL:
				/* count(group column): 0 in the NULL group (DESIGN.md §14) */
				slot->tts_values[i] = Int64GetDatum(keyisnull ? 0 : count);
				break;

			case RBI_TL_COUNT_ZERO:
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
					RBIClauseState *cl = &st->clause[kind - RBI_TL_WHEREKEY];

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
 * rbi_open_relation() and located its WHERE clauses, and every posting set
 * they take is released before they return, so no pin of this relation
 * outlives its turn.
 * --------------------------------------------------------------------- */

/*
 * The intersection of the WHERE clauses, with no index driving the count.
 */
static int64
rbi_count_relation(RBICountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;
	int64		count;

	Assert(st->nclause > 0);
	if (st->wheremissing)
		return 0;

	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);
	count = rbi_count_sources(st->heap, estate->es_snapshot,
							  st->nclause, &st->sources[1], &st->stats);
	MemoryContextSwitchTo(oldcxt);

	return count;
}

/*
 * The sum over every entry of the driving index (DESIGN.md §14,
 * `col IS NOT NULL` with nothing else to drive the merge).
 */
static int64
rbi_sumall_relation(RBICountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;
	int64		total = 0;
	Datum		key;

	if (st->wheremissing)
		return 0;

	rbi_entry_scan_begin(&st->escan, st->groupidx);
	st->scanning = true;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (!rbi_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}

		total += rbi_count_sources(st->heap, estate->es_snapshot,
								   st->nclause + 1, st->sources, &st->stats);
		rbi_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
	}

	rbi_entry_scan_end(&st->escan);
	st->scanning = false;
	return total;
}

/*
 * Add one group's count to the cross-partition hash table.
 */
static void
rbi_hash_add_group(RBICountScanState *st, Datum key, bool keyisnull,
				   int64 count)
{
	TupleHashEntry entry;
	bool		isnew;
	int64	   *slotcount;

	ExecClearTuple(st->hashslot);
	st->hashslot->tts_values[0] = key;
	st->hashslot->tts_isnull[0] = keyisnull;
	ExecStoreVirtualTuple(st->hashslot);

	entry = LookupTupleHashEntry(st->hashtab, st->hashslot, &isnew, NULL);
	slotcount = (int64 *) TupleHashEntryGetAdditional(st->hashtab, entry);
	if (isnew)
		*slotcount = count;
	else
		*slotcount += count;

	/*
	 * The entry (and the copy of the key inside it) lives in the table's own
	 * context; everything the hash and equality functions allocated is in the
	 * temp one and can go.
	 */
	ExecClearTuple(st->hashslot);
	MemoryContextReset(st->hashtempcxt);
}

/*
 * Count every group of one relation into the hash table.
 */
static void
rbi_group_relation_into_hash(RBICountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;

	if (st->wheremissing)
		return;

	rbi_entry_scan_begin(&st->escan, st->groupidx);
	st->scanning = true;

	for (;;)
	{
		Datum		key;
		bool		keyisnull;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (!rbi_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}

		count = rbi_count_sources(st->heap, estate->es_snapshot,
								  st->nclause + 1, st->sources, &st->stats);
		keyisnull = st->groupset.keyisnull;
		rbi_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		/* The key still lives in pergroup; the hash table takes a copy. */
		rbi_hash_add_group(st, key, keyisnull, count);
	}

	rbi_entry_scan_end(&st->escan);
	st->scanning = false;
}

/*
 * Walk one partition: open it, locate its clauses, hand it to one of the
 * three above, then let go of everything it owns (DESIGN.md §16).
 */
static int64
rbi_run_partition(RBICountScanState *st, int p, bool intohash)
{
	int64		count = 0;

	rbi_open_relation(st, st->part[p].heapoid, st->part[p].groupidxoid,
					  st->part[p].clauseidxoid);
	rbi_locate_where(st);

	if (intohash)
		rbi_group_relation_into_hash(st);
	else if (st->hasgroupidx)
	{
		/* No group key of its own: the only driver left is a sum-over-all. */
		Assert(st->sumall);
		count = rbi_sumall_relation(st);
	}
	else
		count = rbi_count_relation(st);

	rbi_release_where(st);
	rbi_close_relation(st);

	return count;
}

/*
 * Return the next merged group, or NULL when the table has been drained.
 */
static TupleTableSlot *
rbi_next_hash_group(RBICountScanState *st)
{
	TupleHashEntry entry;

	while ((entry = ScanTupleHashTable(st->hashtab, &st->hashiter)) != NULL)
	{
		int64	   *count = (int64 *) TupleHashEntryGetAdditional(st->hashtab,
																 entry);
		Datum		key;
		bool		isnull;

		if (*count <= 0)
			continue;			/* only positive counts are ever inserted */

		/*
		 * The minimal tuple belongs to the hash table's context, so the key
		 * stays valid for as long as the node does.
		 */
		ExecClearTuple(st->hashoutslot);
		ExecStoreMinimalTuple(TupleHashEntryGetTuple(entry), st->hashoutslot,
							  false);
		key = slot_getattr(st->hashoutslot, 1, &isnull);

		return rbi_emit_tuple(st, key, isnull, *count);
	}

	st->done = true;
	return NULL;
}

/*
 * The partitioned form of the node (DESIGN.md §16).  Nothing comes out until
 * every partition has been counted: without GROUP BY because the one row is
 * the sum over all of them, with GROUP BY because a group may have rows in
 * any partition.
 */
static TupleTableSlot *
rbi_exec_partitioned(RBICountScanState *st)
{
	int64		total = 0;
	int			p;

	if (st->hasgroupidx && st->groupattno != 0)
	{
		if (!st->hashfilled)
		{
			for (p = 0; p < st->npart; p++)
				(void) rbi_run_partition(st, p, true);
			st->hashfilled = true;
			InitTupleHashIterator(st->hashtab, &st->hashiter);
		}
		return rbi_next_hash_group(st);
	}

	for (p = 0; p < st->npart; p++)
		total += rbi_run_partition(st, p, false);

	st->done = true;

	/*
	 * A plain aggregate always produces its one row; a GROUP BY whose columns
	 * the planner folded to constants produces one only if the group exists.
	 */
	if (total == 0 && st->singlegroup)
		return NULL;

	return rbi_emit_tuple(st, (Datum) 0, true, total);
}

static TupleTableSlot *
rbi_exec_custom_scan(CustomScanState *node)
{
	RBICountScanState *st = (RBICountScanState *) node;
	EState	   *estate = node->ss.ps.state;
	MemoryContext oldcxt;
	int64		count;

	if (st->done)
		return NULL;

	/* A partitioned table counts one partition at a time. */
	if (st->npart > 0)
		return rbi_exec_partitioned(st);

	if (!st->located)
		rbi_locate_where(st);

	/* ---- no index to iterate: exactly one row ---- */
	if (!st->hasgroupidx)
	{
		/* Without a group index a clause has to drive the count. */
		st->done = true;
		count = rbi_count_relation(st);

		/*
		 * A plain aggregate always produces its one row; a GROUP BY whose
		 * columns the planner folded to constants produces one only if the
		 * group exists.
		 */
		if (count == 0 && st->singlegroup)
			return NULL;

		return rbi_emit_tuple(st, (Datum) 0, true, count);
	}

	/* ---- the group index drives the count ---- */
	if (st->wheremissing)
	{
		st->done = true;
		/* A sum over all entries still has to report its one row. */
		if (st->sumall)
			return rbi_emit_tuple(st, (Datum) 0, true, 0);
		return NULL;
	}

	if (!st->scanning)
	{
		rbi_entry_scan_begin(&st->escan, st->groupidx);
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

			if (!rbi_entry_scan_next(&st->escan, &key, &st->groupset))
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}

			total += rbi_count_sources(st->heap, estate->es_snapshot,
									   st->nclause + 1, st->sources,
									   &st->stats);
			rbi_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
		}

		st->done = true;
		return rbi_emit_tuple(st, (Datum) 0, true, total);
	}

	/* ---- GROUP BY: one row per non-empty group ---- */
	for (;;)
	{
		Datum		key;

		CHECK_FOR_INTERRUPTS();

		/*
		 * The key of the group we returned last time lives in pergroup, and
		 * the caller is done with it by now: a scan node's tuple is only
		 * guaranteed until its next call.
		 */
		ExecClearTuple(node->ss.ss_ScanTupleSlot);
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (!rbi_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			st->done = true;
			return NULL;
		}

		count = rbi_count_sources(st->heap, estate->es_snapshot,
								  st->nclause + 1, st->sources, &st->stats);
		{
			bool		keyisnull = st->groupset.keyisnull;

			rbi_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);

			/* A group exists only if at least one of its rows is visible. */
			if (count == 0)
				continue;

			return rbi_emit_tuple(st, key, keyisnull, count);
		}
	}
}

/*
 * Everything the node built while running, undone.  A partitioned scan may be
 * standing in the middle of a partition (a LIMIT above it, an error being
 * unwound), so the relation it has open is closed here too; every posting set
 * has been released before any of them, which is what DESIGN.md §9 requires.
 */
static void
rbi_reset_run(RBICountScanState *st)
{
	int			i;

	if (st->scanning)
	{
		rbi_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	rbi_posting_set_release(&st->groupset);
	rbi_release_where(st);

	if (st->npart > 0)
		rbi_close_relation(st);

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

	if (st->hashtab != NULL)
		ResetTupleHashTable(st->hashtab);
	if (st->hashtempcxt != NULL)
		MemoryContextReset(st->hashtempcxt);
	if (st->hashslot != NULL)
		ExecClearTuple(st->hashslot);
	if (st->hashoutslot != NULL)
		ExecClearTuple(st->hashoutslot);
	st->hashfilled = false;
}

static void
rbi_rescan_custom_scan(CustomScanState *node)
{
	RBICountScanState *st = (RBICountScanState *) node;

	rbi_reset_run(st);
	st->done = false;
}

static void
rbi_end_custom_scan(CustomScanState *node)
{
	RBICountScanState *st = (RBICountScanState *) node;

	rbi_reset_run(st);

	/* A plain table's relations were opened once and are closed once. */
	if (st->npart == 0)
		rbi_close_relation(st);

	if (st->hashslot != NULL)
	{
		ExecDropSingleTupleTableSlot(st->hashslot);
		st->hashslot = NULL;
	}
	if (st->hashoutslot != NULL)
	{
		ExecDropSingleTupleTableSlot(st->hashoutslot);
		st->hashoutslot = NULL;
	}
	st->hashtab = NULL;

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
	if (st->hashmetacxt != NULL)
	{
		MemoryContextDelete(st->hashmetacxt);
		st->hashmetacxt = NULL;
	}
	if (st->hashtuplescxt != NULL)
	{
		MemoryContextDelete(st->hashtuplescxt);
		st->hashtuplescxt = NULL;
	}
	if (st->hashtempcxt != NULL)
	{
		MemoryContextDelete(st->hashtempcxt);
		st->hashtempcxt = NULL;
	}
}

/*
 * "col = 3", "col = ANY ('{1,2,3}')", "col IS NULL", "col IS NOT NULL",
 * "tags @> {a,b}", "tsv @@ 'a' & 'b'".
 */
static void
rbi_explain_clause(RBICountScanState *st, RBIClauseState *cl, StringInfo buf)
{
	const char *attname = get_attname(st->heapoid, cl->attno, false);

	switch (cl->kind)
	{
		case RBI_CLAUSE_NULL:
			appendStringInfo(buf, "%s IS NULL", attname);
			break;
		case RBI_CLAUSE_NOTNULL:
			appendStringInfo(buf, "%s IS NOT NULL", attname);
			break;
		default:
			{
				Oid			outfunc;
				bool		isvarlena;
				char	   *val;

				getTypeOutputInfo(cl->con->consttype, &outfunc, &isvarlena);
				val = OidOutputFunctionCall(outfunc, cl->con->constvalue);
				if (cl->kind == RBI_CLAUSE_ARRAY)
					appendStringInfo(buf, "%s = ANY (%s)", attname, val);
				else if (cl->kind == RBI_CLAUSE_MULTI)
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
rbi_explain_custom_scan(CustomScanState *node, List *ancestors,
						ExplainState *es)
{
	RBICountScanState *st = (RBICountScanState *) node;
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
		rbi_explain_clause(st, &st->clause[i], &buf);
		appendStringInfoChar(&buf, ')');
	}

	ExplainPropertyText("Roaring Indexes", buf.data, es);
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
	}
}
