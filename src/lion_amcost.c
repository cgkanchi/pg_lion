/*-------------------------------------------------------------------------
 *
 * lion_amcost.c
 *		Costing a lion index scan: amcostestimate (lioncostestimate()) and the
 *		shape of the walk a plain or bitmap scan makes.
 *
 * Part of the lion access method, which lion.h declares.  See DESIGN.md
 * section 6.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "lion.h"
#include "lion_costs.h"
#include "lion_count.h"

/*
 * Peel binary-coercion relabels off an expression, so that a varchar column
 * compared with a text constant presents the constant the opclass will see.
 */
static Node *
lion_cost_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * Does this index's opclass extract many keys from one value (DESIGN.md §17)?
 * The presence of support function 2 is the same test lion_fill_state() makes.
 */
static bool
lion_index_is_multikey(IndexOptInfo *index, int col)
{
	return OidIsValid(get_opfamily_proc(index->opfamily[col],
										index->opcintype[col],
										index->opcintype[col],
										LION_EXTRACTVALUE_PROC));
}

/*
 * Extract one multi-key query at plan time and say whether answering it means
 * emitting every posting of every entry (DESIGN.md §17's LION_QMODE_ALL).
 *
 * lion_extract_query() wants an LionState, but only for the extractQuery
 * FmgrInfo and the collation; nothing here touches an index.  A missing
 * extraction function means the question cannot be answered, which is costed
 * as the expensive answer.
 */
static bool
lion_query_is_full_scan(IndexOptInfo *index, int col, StrategyNumber strategy,
					   Datum query)
{
	Oid			proc;
	FmgrInfo	flinfo;
	LionState	state;
	LionQuery	q;
	MemoryContext cxt;
	MemoryContext oldcxt;
	bool		full;

	proc = get_opfamily_proc(index->opfamily[col], index->opcintype[col],
							 index->opcintype[col], LION_EXTRACTQUERY_PROC);
	if (!OidIsValid(proc))
		return true;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring cost query extract",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&state, 0, sizeof(state));
	state.multikey = true;
	state.collation = index->indexcollations[col];
	fmgr_info(proc, &flinfo);
	state.extractquery = flinfo;

	/*
	 * The scan answers what the superset can bound and rechecks it
	 * (lion_scan.c, lion_emit_query()), so only a query no key narrows -
	 * `!a`, `@> '{}'`, `<@` - reads the whole index.
	 */
	if (strategy == LION_STRAT_MATCH)
		query = lion_tsquery_strip_prefixes(query);	/* §17, "Prefix lexemes" */
	lion_extract_query_superset(&state, query, strategy, &q);
	full = (q.mode == LION_QMODE_ALL);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return full;
}

/*
 * `col op ANY (array)`: each element is a query of its own and their answers
 * go into the same bitmap, so one element that needs the whole index makes
 * the scan a full one.  An array that is not available at plan time has to be
 * assumed to contain such an element.
 */
static bool
lion_array_query_is_full_scan(IndexOptInfo *index, int col,
							 StrategyNumber strategy, Node *arraynode)
{
	Const	   *con = (Const *) arraynode;
	ArrayType  *arr;
	Oid			elemtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			i;
	bool		full = false;

	if (arraynode == NULL || !IsA(arraynode, Const))
		return true;
	if (con->constisnull)
		return false;			/* `col op ANY (NULL)` is never true */

	arr = DatumGetArrayTypeP(con->constvalue);
	elemtype = ARR_ELEMTYPE(arr);
	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	for (i = 0; i < nelems && !full; i++)
	{
		if (nulls[i])
			continue;			/* never true, nothing is scanned for it */
		full = lion_query_is_full_scan(index, col, strategy, elems[i]);
	}

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(con->constvalue))
		pfree(arr);

	return full;
}

/*
 * Is this index qual one of the range comparisons of DESIGN.md §28 - `<`,
 * `<=`, `>=` or `>`, strategies 6 .. 9 of a SCALAR column's opfamily - or a
 * `<>` (10, §35), which the scan answers with the same walk, a hole in it?
 */
static bool
lion_cost_is_range_op(IndexOptInfo *index, int col, Oid opno)
{
	return !lion_index_is_multikey(index, col) &&
		LION_STRAT_IS_WALK(get_op_opfamily_strategy(opno,
													index->opfamily[col]));
}

/* ... and is it the `<>` - the hole - among them? */
static bool
lion_cost_is_hole(IndexOptInfo *index, int col, RestrictInfo *rinfo)
{
	Node	   *clause = (Node *) rinfo->clause;
	Oid			opno;

	if (IsA(clause, OpExpr))
		opno = ((OpExpr *) clause)->opno;
	else if (IsA(clause, ScalarArrayOpExpr))
		opno = ((ScalarArrayOpExpr *) clause)->opno;
	else
		return false;
	return !lion_index_is_multikey(index, col) &&
		get_op_opfamily_strategy(opno, index->opfamily[col]) == LION_STRAT_NE;
}

static bool
lion_cost_is_range(IndexOptInfo *index, int col, OpExpr *op)
{
	return lion_cost_is_range_op(index, col, op->opno);
}

/*
 * What the scan answers of key column c, from the path's index quals: the
 * classification lion_scan_choose() makes of the scan keys (lion_scan.c,
 * DESIGN.md §5 SCAN step 5, §29.2), made here so that every function below
 * prices the scan the executor will run.
 *
 *	sets	every equality, list, `IS NULL` and multi-key query: a set tree
 *			each, ANDed with one another and with the other columns';
 *	walk	for a column without sets: `op ANY (array)` with a range strategy
 *			(ONE walk to its widest element), else the column's plain range
 *			comparisons, which ranges lists and which are all one walk, else
 *			`IS NOT NULL`;
 *	ndropped	the quals left to the recheck: a column's range comparisons
 *			beside its sets, and a second array range or the plain range
 *			comparisons beside an array range;
 *	nomatch	`IS NULL` beside a strict qual of the column or `IS NOT NULL`,
 *			which the scan answers with no row at all.
 *
 * `IS NOT NULL` beside a strict qual of its column - any qual but a null test
 * - is implied by it and is none of these, nor is a second `IS NOT NULL`.  A
 * set tree the scan cannot build after all, a multi-key query in mode ALL
 * (§17), is among the sets; lion_cost_qual_is_full() says which.  The lists
 * are the caller's to free.
 */
typedef struct LionCostCol
{
	List	   *sets;			/* RestrictInfos, in the path's order */
	List	   *ranges;			/* the plain range comparisons of the walk */
	RestrictInfo *walk;			/* the array range, the first range
								 * comparison, or `IS NOT NULL` */
	int			nquals;			/* index quals on the column at all */
	int			ndropped;
	bool		nomatch;
} LionCostCol;

static void
lion_cost_col_quals(IndexPath *path, int c, LionCostCol *cc)
{
	IndexOptInfo *index = path->indexinfo;
	RestrictInfo *arrayrange = NULL;
	RestrictInfo *notnull = NULL;
	int			narray = 0;
	int			nisnull = 0;
	int			nstrict = 0;
	ListCell   *lc;

	memset(cc, 0, sizeof(LionCostCol));

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		ListCell   *lc2;

		if (iclause->indexcol != c)
			continue;
		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Node	   *clause = (Node *) rinfo->clause;

			cc->nquals++;
			if (IsA(clause, NullTest))
			{
				if (((NullTest *) clause)->nulltesttype == IS_NULL)
				{
					nisnull++;
					cc->sets = lappend(cc->sets, rinfo);
				}
				else if (notnull == NULL)
					notnull = rinfo;
				continue;
			}

			nstrict++;
			if (IsA(clause, OpExpr) &&
				lion_cost_is_range(index, c, (OpExpr *) clause))
				cc->ranges = lappend(cc->ranges, rinfo);
			else if (IsA(clause, ScalarArrayOpExpr) &&
					 lion_cost_is_range_op(index, c,
										   ((ScalarArrayOpExpr *) clause)->opno))
			{
				if (arrayrange == NULL)
					arrayrange = rinfo;
				narray++;
			}
			else
				cc->sets = lappend(cc->sets, rinfo);
		}
	}

	if (nisnull > 0 && (nstrict > 0 || notnull != NULL))
		cc->nomatch = true;

	if (cc->sets != NIL)
	{
		/* the recheck bounds the sets */
		cc->ndropped = list_length(cc->ranges) + narray;
		list_free(cc->ranges);
		cc->ranges = NIL;
	}
	else if (arrayrange != NULL)
	{
		cc->ndropped = narray - 1 + list_length(cc->ranges);
		list_free(cc->ranges);
		cc->ranges = NIL;
		cc->walk = arrayrange;
	}
	else if (cc->ranges != NIL)
		cc->walk = linitial_node(RestrictInfo, cc->ranges);
	else
		cc->walk = notnull;
}

static void
lion_cost_col_free(LionCostCol *cc)
{
	list_free(cc->sets);
	list_free(cc->ranges);
	cc->sets = NIL;
	cc->ranges = NIL;
}

/*
 * Would the scan read the whole of key column c to answer this one set qual
 * (lion_cost_col_quals())?  *all says whether it then emits every indexed
 * row as a candidate, too, for the heap to recheck.
 *
 * Only a multi-key query does: one the extractor answers with
 * LION_QMODE_ALL - a phrase, a prefix, a NOT, a weight mask, `<@`,
 * `@> '{}'`, a NULL element, or more than LION_MAX_QUERY_KEYS keys (DESIGN.md
 * §17) - and one whose value is not a plan-time Const (a Param), because the
 * MODE follows the query's shape, not just its value, and an unknown value
 * has to be priced as the expensive shape.  `op ANY (array)` is the union of
 * its elements' answers, so one element that needs every row is enough, and
 * an array not available at plan time has to be assumed to hold one.  A
 * scalar column's sets - an equality, a list, `IS NULL` - look up keys, and
 * never read the column whole.
 */
static bool
lion_cost_qual_is_full(IndexOptInfo *index, int c, Node *cl, bool *all)
{
	*all = false;
	if (IsA(cl, NullTest) || !lion_index_is_multikey(index, c))
		return false;

	if (IsA(cl, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) cl;
		StrategyNumber strategy;
		Node	   *arg;

		if (list_length(op->args) == 2 &&
			(strategy = (StrategyNumber)
			 get_op_opfamily_strategy(op->opno, index->opfamily[c])) != 0 &&
			(arg = lion_cost_strip((Node *) lsecond(op->args))) != NULL &&
			IsA(arg, Const))
		{
			/* a strict operator with NULL is never true: nothing is scanned */
			if (((Const *) arg)->constisnull ||
				!lion_query_is_full_scan(index, c, strategy,
										 ((Const *) arg)->constvalue))
				return false;
		}
		*all = true;
		return true;
	}

	if (IsA(cl, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) cl;
		StrategyNumber strategy;

		if (list_length(saop->args) == 2 &&
			(strategy = (StrategyNumber)
			 get_op_opfamily_strategy(saop->opno,
									  index->opfamily[c])) != 0 &&
			(strategy == LION_STRAT_EQUAL ||
			 !lion_array_query_is_full_scan(index, c, strategy,
											lion_cost_strip((Node *) lsecond(saop->args)))))
			return false;		/* a union of single-key lookups, or of exact queries */
		*all = true;
		return true;
	}

	return false;
}

/*
 * Will the scan have to walk the WHOLE index for this path - read every
 * bucket, every entry and every posting of every entry?
 *
 * The scan answers every set qual of a column and ANDs them with each other
 * and with the other columns' (lion_scan_choose(), lion_cost_col_quals()),
 * and the cost of the scan is the cost of what it answers, so the
 * classification is mirrored here.  A column is read whole only when
 * NOTHING it answers selects from its posting sets: every set qual of it
 * needs the whole column (lion_cost_qual_is_full()), or it is walked for
 * `IS NOT NULL`, which is every entry but the reserved NULL one (DESIGN.md
 * §14).  One qual that looks keys up is enough to keep the column off the
 * full walk - the scan drops the others and rechecks them.  A range walks
 * only the entries between its bounds, so it never makes the scan a full
 * one; what it costs per entry is lion_range_walk_cost()'s.
 *
 * *emits_all_rows additionally says whether the CANDIDATES the walk produces
 * are every indexed row, which is what makes the heap side a full recheck
 * rather than a selective fetch.  The multi-key fallback asks for a superset
 * and the operator is re-applied to every row; `IS NOT NULL` does not - the
 * rows it emits are exactly the rows it selects, so its heap side is still
 * the clause's own selectivity.
 *
 * Handing genericcostestimate() an empty GenericCosts instead priced these
 * as selective lookups - a prefix query estimated at 192 cost units against
 * the sequential scan's 9156, for a scan that emitted 1.18M posting TIDs and
 * rechecked 198k rows and ran 2.6x slower than that sequential scan (the
 * 2026-09-21 follow-up review).  And ranking ONE qual per column, as this
 * function did while the scan answered one, priced `tags @> '{}' AND tags &&
 * '{t1}'` as the full walk of the first and a whole-table recheck, where the
 * second one's sets answer it.
 */
static bool
lion_scan_walks_whole_index(IndexPath *path, bool *emits_all_rows)
{
	IndexOptInfo *index = path->indexinfo;
	int			ncols = index->nkeycolumns;
	int			nchosen = 0;
	int			firstcol = -1;
	bool		anyselective = false;
	int			c;

	*emits_all_rows = false;

	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		return false;

	/*
	 * Per column, would that column alone make the scan walk the whole
	 * column's entries?  The answers combine the way the scan does
	 * (DESIGN.md §24): the columns are INTERSECTED, so one column that
	 * selects from its posting sets keeps the scan off the full walk however
	 * the others are answered - the scan drops those and rechecks.  When
	 * none does, the scan walks the FIRST column, by its first qual.
	 */
	for (c = 0; c < ncols; c++)
	{
		LionCostCol cc;
		bool		colfull = true;
		bool		colall = false;
		ListCell   *lc;

		lion_cost_col_quals(path, c, &cc);
		if (cc.nquals == 0)
		{
			lion_cost_col_free(&cc);
			continue;
		}
		nchosen++;
		if (firstcol < 0)
			firstcol = c;

		if (cc.nomatch)
			colfull = false;	/* nothing is read at all */
		else if (cc.sets != NIL)
		{
			foreach(lc, cc.sets)
			{
				bool		all;

				if (!lion_cost_qual_is_full(index, c,
											(Node *) lfirst_node(RestrictInfo, lc)->clause,
											&all))
					colfull = false;
				else if (lc == list_head(cc.sets))
					colall = all;
			}
		}
		else
			colfull = IsA(cc.walk->clause, NullTest);	/* ranges are not */

		if (!colfull)
			anyselective = true;
		else if (c == firstcol)
			*emits_all_rows = colall;
		lion_cost_col_free(&cc);
	}

	/*
	 * No clause at all: a partial index whose predicate the query implies,
	 * which the scan answers by emitting every row it holds.  The rows are
	 * exactly the ones the scan selects, so the heap side keeps the
	 * predicate's own selectivity (*emits_all_rows stays false).
	 */
	if (nchosen == 0)
		return true;
	if (anyselective)
	{
		*emits_all_rows = false;
		return false;
	}

	return true;
}

/* Does a column other than col have an index qual at all? */
static bool
lion_cost_other_column(IndexPath *path, int col)
{
	ListCell   *lc;

	foreach(lc, path->indexclauses)
	{
		if (((IndexClause *) lfirst(lc))->indexcol != col)
			return true;
	}
	return false;
}

/*
 * Does a column other than col have a qual the scan answers with a set tree
 * (lion_cost_col_quals(), lion_scan_col_tree()), which a range walk on col is
 * then ANDed with?  An equality, a list, `IS NULL` or a multi-key query is a
 * set; another range is a second walk and `IS NOT NULL` is dropped, neither of
 * which the scan streams beside this one, and so is a multi-key query that
 * needs every row (lion_cost_qual_is_full()).
 */
static bool
lion_cost_sets_beside(IndexPath *path, int col)
{
	IndexOptInfo *index = path->indexinfo;
	bool		beside = false;
	int			c;

	for (c = 0; c < index->nkeycolumns && !beside; c++)
	{
		LionCostCol cc;
		ListCell   *lc;

		if (c == col)
			continue;
		lion_cost_col_quals(path, c, &cc);
		foreach(lc, cc.sets)
		{
			bool		all;

			if (!lion_cost_qual_is_full(index, c,
										(Node *) lfirst_node(RestrictInfo, lc)->clause,
										&all))
			{
				beside = true;
				break;
			}
		}
		lion_cost_col_free(&cc);
	}
	return beside;
}

/*
 * The range quals of key column c that one walk answers (lion_cost_col_quals(),
 * DESIGN.md §28): none when the column has sets - an equality, a list, `IS
 * NULL`, a multi-key query, beside which a range is only rechecked - or
 * selects nothing; else an `op ANY (array)`, which for a range operator is
 * ONE walk to the widest element (lion_emit_array_range()), alone; else the
 * column's plain range comparisons, all of them one walk.
 */
static List *
lion_cost_col_ranges(IndexPath *path, int c)
{
	LionCostCol cc;
	List	   *ranges = NIL;

	lion_cost_col_quals(path, c, &cc);
	if (!cc.nomatch && cc.walk != NULL && !IsA(cc.walk->clause, NullTest))
	{
		if (cc.ranges != NIL)
		{
			ranges = cc.ranges;
			cc.ranges = NIL;
		}
		else
			ranges = list_make1(cc.walk);
	}
	lion_cost_col_free(&cc);
	return ranges;
}

/*
 * n_distinct of the heap column key column c indexes, which is how many
 * entries a scalar column has; DEFAULT_NUM_DISTINCT for an expression.
 */
static double
lion_cost_col_ndistinct(PlannerInfo *root, IndexOptInfo *index, int c)
{
	RangeTblEntry *rte;
	VariableStatData vardata;
	bool		isdefault;
	double		ndistinct;
	Var		   *var;

	if (index->indexkeys[c] <= 0)
		return DEFAULT_NUM_DISTINCT;

	rte = planner_rt_fetch(index->rel->relid, root);
	var = makeVar(index->rel->relid, index->indexkeys[c],
				  get_atttype(rte->relid, index->indexkeys[c]),
				  -1, index->indexcollations[c], 0);
	examine_variable(root, (Node *) var, index->rel->relid, &vardata);
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	return ndistinct;
}

/*
 * How many entries ONE walk of key column c's range reads (DESIGN.md §28):
 * the keys between its bounds, about n_distinct(col) x sel of them - or, on
 * a column with SUMMARY posting sets (§32), the keys of the buckets at the
 * range's two ends one by one and ONE summary for every bucket it covers
 * whole, which is what lion_walk_begin() hands the bitmap and the plain scan
 * alike.  The model is the count's (lion_cost_range_side(),
 * lion_plan_cost.c): a bucket holds max(summary_tids, rows per key) rows -
 * or what the column's first summaries hold on average, once that is more
 * than twice as many (lion_summary_shape(): keys that arrive out of order
 * grow the buckets past the reloption) - and so that many rows' worth of
 * keys, and a run of n keys covers about n / keys per bucket - 1 buckets
 * whole, half a bucket being left over at each end on average.  With no
 * whole bucket the walk is the keys', as the executor's is.
 */
static double
lion_cost_walk_entries(PlannerInfo *root, IndexPath *path, int c,
					   Selectivity sel, int nholes)
{
	IndexOptInfo *index = path->indexinfo;
	double		nkeys = Max(1.0, lion_cost_col_ndistinct(root, index, c) * sel);
	Relation	indexrel;
	LionState  *col;
	LionSumShape shape;
	uint32		bucket_tids;
	bool		summarized;
	double		rowsper;
	double		bucketrows;
	double		keysper;
	double		whole;

	/*
	 * A walk with holes (`<>`, DESIGN.md §35) - sel is then its RANGE's, the
	 * holes left out - reads every key of the range but theirs, however many
	 * rows the holes hold, and reads them one by one: a bucket's summary may
	 * hold a hole's rows (lion_entry_scan_plan_sum()).
	 */
	if (nholes > 0)
		return Max(1.0, nkeys - nholes);

	if (index->hypothetical)
		return nkeys;			/* nothing to read the options off */

	indexrel = index_open(index->indexoid, AccessShareLock);
	col = lion_index_column_state(indexrel, (AttrNumber) (c + 1));
	summarized = col->summarized && col->ordered;
	bucket_tids = lion_get_index_state(indexrel)->meta.summary_tids;
	memset(&shape, 0, sizeof(shape));
	if (summarized && bucket_tids > 0)
		lion_summary_shape(indexrel, col, &shape);
	index_close(indexrel, AccessShareLock);

	/* `on` over an empty table: no summary yet, and a walk of the keys */
	if (!summarized || bucket_tids == 0 || shape.nsummaries <= 0)
		return nkeys;

	rowsper = Max(Max(index->rel->tuples, 1.0) * sel / nkeys, 1.0);
	bucketrows = Max((double) bucket_tids, rowsper);
	if (shape.rows / shape.nsummaries > 2.0 * bucketrows)
		bucketrows = shape.rows / shape.nsummaries;
	keysper = Max(1.0, bucketrows / rowsper);
	whole = Max(0.0, nkeys / keysper - 1.0);
	if (whole < 1.0)
		return nkeys;
	return Max(0.0, nkeys - whole * keysper) + whole;
}

/*
 * ... for the range quals of key column c (lion_cost_col_ranges()), whose
 * selectivity together is sel: a walk with holes (`<>`, DESIGN.md §35) reads
 * the keys of its RANGE, the quals that are not holes, but the holes'.
 */
static double
lion_cost_ranges_entries(PlannerInfo *root, IndexPath *path, int c,
						 List *ranges, Selectivity sel)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *bounds = NIL;
	int			nholes = 0;
	ListCell   *lc;
	double		entries;

	foreach(lc, ranges)
	{
		if (lion_cost_is_hole(index, c, lfirst_node(RestrictInfo, lc)))
			nholes++;
		else
			bounds = lappend(bounds, lfirst(lc));
	}
	if (nholes > 0)
		sel = (bounds == NIL) ? 1.0 :
			clauselist_selectivity(root, bounds, index->rel->relid,
								   JOIN_INNER, NULL);
	entries = lion_cost_walk_entries(root, path, c, sel, nholes);
	list_free(bounds);
	return entries;
}

/*
 * What the ENTRIES of the range walks of this path cost (DESIGN.md §28), on
 * top of the pages and the postings genericcostestimate() prorates by the
 * selectivity.
 *
 * A range is one walk per key column, over the entries between its bounds
 * (lion_cost_walk_entries()), each decoded and compared with every bound.  On
 * a low- or mid-cardinality column that is a few hundred entries, which is
 * nothing; on a near-unique one every row is an entry, and this is the term -
 * with the index's own size, an entry header per row against btree's tuple -
 * that leaves such a column to btree, unless the column has summaries (§32),
 * whose walk is a summary per bucket.  Only a column the scan walks for its
 * range pays it (lion_cost_col_ranges()): not one with sets of its own.
 *
 * Beside another column's sets, a plain scan walks a long range once per
 * WINDOW of those sets' containers (DESIGN.md §29.3, lion_walk_window()), so
 * the entries are paid once per window: the heap's container keys over the
 * window, which is 1 below 32768 heap blocks at the default floor and an
 * upper bound above it (the sets may have containers at fewer keys).  The
 * bitmap scan, which shares the path, walks once per window of the same
 * memory too (lion_emit_intersect()), whose containers are held at their own
 * size rather than as bitset images, so it makes at most as many walks.  The
 * plain scan used to restart the other columns' stream for every entry
 * instead, a descent of each of their posting trees, and that was never
 * charged at all (2026-09-25 review).
 *
 * *walkrows is what the walks read beyond that, for lioncostestimate() to
 * charge: on a MULTICOLUMN path genericcostestimate() prorates the index by
 * the selectivity of every column's quals together, which is what an AND of
 * set trees - leapfrogging each other (§22) - reads about, but a range
 * column is walked whole whatever the others select (§28, §29.3), so it
 * reads the postings of every row in its range: its own selectivity's share.
 * A one-column path is prorated by that already and adds nothing.
 */
static Cost
lion_range_walk_cost(PlannerInfo *root, IndexPath *path, double *walkrows)
{
	IndexOptInfo *index = path->indexinfo;
	Cost		cost = 0;
	int			c;

	for (c = 0; c < index->nkeycolumns; c++)
	{
		List	   *ranges = lion_cost_col_ranges(path, c);

		if (ranges != NIL)
		{
			Selectivity sel = clauselist_selectivity(root, ranges,
													 index->rel->relid,
													 JOIN_INNER, NULL);
			double		entries = lion_cost_ranges_entries(root, path, c,
														   ranges, sel);

			if (lion_cost_sets_beside(path, c))
			{
				double		containers = ceil((double) index->rel->pages /
											  LION_BLOCKS_PER_CONTAINER);

				entries *= Max(1.0, ceil(containers / lion_walk_window()));
			}
			cost += entries * (cpu_index_tuple_cost +
							   list_length(ranges) * cpu_operator_cost);
			if (lion_cost_other_column(path, c))
				*walkrows += sel * Max(index->rel->tuples, 0.0);
			list_free(ranges);
		}
	}

	return cost;
}

/*
 * The sum of the squared frequencies of a column's values among its non-NULL
 * rows, from its MCV list and, for the values not in it, an even share of
 * the rest: what ANALYZE's correlation comes out at when the values are
 * placed at random (lion_var_heap_correlation()).  1 when nothing is known,
 * which leaves no correlation at all.
 */
static double
lion_stats_sum_sq_freq(VariableStatData *vardata)
{
	Form_pg_statistic stats;
	AttStatsSlot sslot;
	double		nonnull;
	double		summcv = 0.0;
	double		sumsq = 0.0;
	double		rest;
	double		ndistinct;
	bool		isdefault;
	int			nmcv = 0;
	int			i;

	if (!HeapTupleIsValid(vardata->statsTuple))
		return 1.0;
	stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
	nonnull = 1.0 - stats->stanullfrac;
	if (nonnull <= 0.0)
		return 1.0;

	if (get_attstatsslot(&sslot, vardata->statsTuple, STATISTIC_KIND_MCV,
						 InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		for (i = 0; i < sslot.nnumbers; i++)
		{
			double		f = sslot.numbers[i] / nonnull;

			summcv += sslot.numbers[i];
			sumsq += f * f;
		}
		nmcv = sslot.nnumbers;
		free_attstatsslot(&sslot);
	}

	rest = Max(nonnull - summcv, 0.0) / nonnull;
	if (rest > 0.0)
	{
		ndistinct = get_variable_numdistinct(vardata, &isdefault);
		sumsq += rest * rest / Max(ndistinct - nmcv, 1.0);
	}
	return Min(sumsq, 1.0);
}

/*
 * The correlation of a heap column's values with the heap order, as the cost
 * model reads it (DESIGN.md §29.11): the ANALYZE correlation for the type's
 * default `<`, which is what ANALYZE computed it with, less the part that
 * says nothing about the heap.  0 whenever there is no statistic.
 *
 * ANALYZE sorts its sample by value and equal values by their place in the
 * heap, so a column of few values placed at random correlates by exactly the
 * sum of its values' squared frequencies: 1/k for k equally common values,
 * 0.82 for a 90/10 boolean.  cost_index() reads that as the rows of one value
 * packed on fewer pages than they touch - and lion is made for such columns.
 * So the correlation is taken beyond that baseline: (corr - S) / (1 - S), S
 * the sum of the squares, clamped to [-1, 1], which is 0 for values placed at
 * random and still 1 for a column stored in value order (and -1 for one
 * stored in reverse, whose values are packed as well).  Measured on 8M rows
 * with the values placed at random: 0.4406 for a column of three values at
 * 60/20/20% (S = 0.44), 0.8171 for 90/10 (S = 0.82), 0.6852 for 80/20
 * (S = 0.68).
 *
 * Shared with the count pushdown, which prices the heap pages its recheck
 * visits by the same number (lion_plan_cost.c, lion_var_correlation()): read
 * raw, a low-cardinality column placed at random looked packed there too.
 */
double
lion_var_heap_correlation(PlannerInfo *root, Index relid, Node *var)
{
	VariableStatData vardata;
	TypeCacheEntry *tce;
	double		corr = 0.0;

	examine_variable(root, var, relid, &vardata);
	tce = lookup_type_cache(exprType(var), TYPECACHE_LT_OPR);
	if (HeapTupleIsValid(vardata.statsTuple) && OidIsValid(tce->lt_opr))
	{
		AttStatsSlot sslot;

		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_CORRELATION, tce->lt_opr,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0)
			{
				double		sumsq = lion_stats_sum_sq_freq(&vardata);

				corr = (sumsq < 1.0) ?
					(sslot.numbers[0] - sumsq) / (1.0 - sumsq) : 0.0;
				corr = Max(-1.0, Min(corr, 1.0));
			}
			free_attstatsslot(&sslot);
		}
	}
	ReleaseVariableStats(vardata);

	return corr;
}

/*
 * The correlation a PLAIN index scan's heap fetches have with the heap order
 * (DESIGN.md §29.11), which only cost_index() reads - a bitmap heap scan
 * sorts its pages whatever the index says.  It starts from btcostestimate()'s
 * answer: the correlation of the first index column the path has a clause on
 * (of column 1 when it has none), times 0.75 for a multicolumn index, taken
 * as lion_var_heap_correlation() takes it.  Within one key a lion scan
 * returns TIDs in heap order, as btree does since its heap-TID tiebreaker, so
 * how that key's rows are spread over the heap is what the column's
 * correlation describes.  0 for a multi-key column (a row is under several of
 * its entries and the column's statistics are the ARRAY's) and for an
 * expression column.
 */
static double
lion_index_correlation(PlannerInfo *root, IndexPath *path)
{
	IndexOptInfo *index = path->indexinfo;
	RangeTblEntry *rte;
	double		corr;
	int			col = -1;
	ListCell   *lc;
	Var		   *var;

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);

		if (col < 0 || iclause->indexcol < col)
			col = iclause->indexcol;
	}
	if (col < 0)
		col = 0;
	if (col >= index->nkeycolumns || index->indexkeys[col] <= 0 ||
		lion_index_is_multikey(index, col))
		return 0.0;

	rte = planner_rt_fetch(index->rel->relid, root);
	var = makeVar(index->rel->relid, index->indexkeys[col],
				  get_atttype(rte->relid, index->indexkeys[col]), -1,
				  index->indexcollations[col], 0);
	corr = lion_var_heap_correlation(root, index->rel->relid, (Node *) var);

	if (index->nkeycolumns > 1)
		corr *= 0.75;
	return corr;
}

/*
 * The order a plain scan of this path hands out its TIDs in (DESIGN.md §29.3,
 * lion_source_build()):
 *
 *	LION_PLAIN_SORTED	heap order: the SETS, UNION and WINDOW sources
 *						(lion_source_sorted());
 *	LION_PLAIN_WALK		one column's entries in key order, each entry's TIDs
 *						in heap order: *walkcol is that column;
 *	LION_PLAIN_LIST		an IN list longer than a batch, located and streamed a
 *						batch at a time, each batch in heap order.
 *
 * Mirrors lion_source_build(): every set qual of every column is answered
 * (lion_cost_col_quals()); a long list outranks a walk; the first column
 * that walks a range walks, and a second one is rechecked; an `IS NOT NULL`
 * walks only when nothing else answers.  Beside another column's sets a
 * range is taken for a WINDOW, as lion_range_walk_cost() takes it: a walk
 * too short for one restarts the other columns' stream a few times at most
 * (lion_source_walk_is_long()), each restart in heap order.  No key at all
 * is a partial index read whole, by a walk of column 1 or, when that column
 * is multi-key, by a union.
 *
 * *rechecks says whether the scan sets xs_recheck (§29.6), and so evaluates
 * its index quals on every row it fetches: when it leaves a qual unanswered
 * (a range beside its column's sets, a second walk or long list, a walk
 * beside a list, an `IS NOT NULL` beside anything that answers), when it
 * answers a multi-key column, and for a UNION.
 */
#define LION_PLAIN_SORTED	0
#define LION_PLAIN_WALK		1
#define LION_PLAIN_LIST		2

static int
lion_plain_scan_shape(PlannerInfo *root, IndexPath *path, int *walkcol,
					  bool *rechecks)
{
	IndexOptInfo *index = path->indexinfo;
	int			ncols = index->nkeycolumns;
	int			nsets = 0;
	int			nlists = 0;
	int			rangecol = -1;
	int			notnullcol = -1;
	int			nnotnull = 0;
	int			nrangecols = 0;
	int			ndropped = 0;
	bool		anychosen = false;
	int			c;

	*walkcol = 0;
	*rechecks = false;
	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		return LION_PLAIN_LIST;

	for (c = 0; c < ncols; c++)
	{
		LionCostCol cc;
		ListCell   *lc;

		lion_cost_col_quals(path, c, &cc);
		if (cc.nquals == 0)
		{
			lion_cost_col_free(&cc);
			continue;
		}
		anychosen = true;
		ndropped += cc.ndropped;

		/*
		 * A column that selects nothing is the NONE source, and a multi-key
		 * column a set tree or a UNION: all of them in heap order.
		 */
		if (cc.nomatch || lion_index_is_multikey(index, c))
		{
			nsets++;
			if (!cc.nomatch)
				*rechecks = true;
			lion_cost_col_free(&cc);
			continue;
		}

		foreach(lc, cc.sets)
		{
			Node	   *cl = (Node *) lfirst_node(RestrictInfo, lc)->clause;

			if (IsA(cl, ScalarArrayOpExpr) &&
				get_op_opfamily_strategy(((ScalarArrayOpExpr *) cl)->opno,
										 index->opfamily[c]) == LION_STRAT_EQUAL &&
#if PG_VERSION_NUM >= 170000
				estimate_array_length(root,
									  (Node *) lsecond(((ScalarArrayOpExpr *) cl)->args))
#else
				estimate_array_length((Node *) lsecond(((ScalarArrayOpExpr *) cl)->args))
#endif
				> lion_scan_list_batch())
				nlists++;
			else
				nsets++;
		}

		if (cc.walk != NULL && IsA(cc.walk->clause, NullTest))
		{
			nnotnull++;
			if (notnullcol < 0)
				notnullcol = c;
		}
		else if (cc.walk != NULL)
		{
			nrangecols++;
			if (rangecol < 0)
				rangecol = c;
		}
		lion_cost_col_free(&cc);
	}

	if (ndropped > 0 || nrangecols > 1 || nlists > 1 ||
		(nlists > 0 && nrangecols > 0) || nnotnull > 1 ||
		(nnotnull > 0 && (nsets > 0 || nlists > 0 || nrangecols > 0)))
		*rechecks = true;

	if (!anychosen)
	{
		*rechecks = lion_index_is_multikey(index, 0);
		return lion_index_is_multikey(index, 0) ? LION_PLAIN_SORTED :
			LION_PLAIN_WALK;
	}
	if (nlists > 0)
		return LION_PLAIN_LIST;
	if (nsets > 0)
		return LION_PLAIN_SORTED;
	if (rangecol >= 0 || notnullcol >= 0)
	{
		*walkcol = (rangecol >= 0) ? rangecol : notnullcol;
		return LION_PLAIN_WALK;
	}
	return LION_PLAIN_SORTED;
}

/*
 * How many entries a WALK of key column c reads (LION_PLAIN_WALK): the
 * column's n_distinct, or the entries of its range's walk when a range bounds
 * it (lion_cost_walk_entries(), summaries and all) - the same count
 * lion_range_walk_cost() prices the entries by, and the same caveat: it is
 * the share of the column's values for a column whose rows spread evenly over
 * them.
 */
static double
lion_plain_walk_entries(PlannerInfo *root, IndexPath *path, int c)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *ranges = lion_cost_col_ranges(path, c);
	double		entries;

	if (ranges == NIL)
		return Max(1.0, lion_cost_col_ndistinct(root, index, c));
	entries = lion_cost_ranges_entries(root, path, c, ranges,
									   clauselist_selectivity(root, ranges,
															  index->rel->relid,
															  JOIN_INNER, NULL));
	list_free(ranges);
	return Max(1.0, entries);
}

/*
 * The plain scan's own prices below are planner settings (lion_costs.c):
 * each macro is the multiplier a setting holds - LION_PLAIN_FETCH_ROW_COST is
 * pg_lion.plain_fetch_row_cost - of the unit its comment names, and each
 * comment is why the setting's default is what it is.
 */

/*
 * What a plain scan pays to fetch a row past the first on its heap page, in
 * cpu_tuple_cost, beyond what a bitmap heap scan pays for the same row: an
 * amgettuple call, a buffer lock and a HOT search per row, where the bitmap
 * heap scan takes the lock and walks the page's matches once.  Measured on
 * the release build (backend CPU time, heap in shared buffers, the two scans
 * interleaved): 11.5 ns a row more for 40,000 numeric rows stored in value
 * order, 21 ns for 75,000 rows of 30 days stored in order, 20 ns for 43% of a
 * 200k-row table on every page (plaincost.sql) - one cpu_tuple_cost at the
 * 500 units a millisecond core's scans run at there (DESIGN.md §10, "The
 * units").  It was 0.5, from 20 to 50 ns on an assert build.  Where the rows
 * are scattered the plain scan spends 0.5 us a page LESS than the bitmap heap
 * scan (30% less over 25,000 rows at 0.4 a page), which is not credited.
 */
#define LION_PLAIN_FETCH_ROW_COST	lion_plain_fetch_row_cost

/*
 * cost_bitmap_tree_node()'s charge for a row's bitmap entry, in
 * cpu_operator_cost: part of the bitmap heap scan's per-row price, which the
 * plain scan is charged as well (lion_plain_heap_correlation()).
 */
#define LION_BITMAP_ROW_COST		lion_bitmap_row_cost

/*
 * What a WALK pays for an entry past the first, in cpu_tuple_cost, beyond what
 * the bitmap scan's walk of the same entry pays: the plain scan streams each
 * entry through a stream of its own, the other columns' sets beside it
 * (lion_source_next()), where the bitmap scan adds the entry's members to its
 * bitmap.  Measured on an assert-enabled build over 4,000 to 390,000 one-row
 * entries of a timestamp stored in heap order, the heap in memory: 0.30 to
 * 0.35 us an entry more than the bitmap scan, against 55 ns for a row of a
 * sequential scan.
 */
#define LION_WALK_PASS_COST		lion_walk_pass_cost

/*
 * Does a plain scan whose rows lie on `pages` heap pages compete with a
 * PARALLEL bitmap heap scan of them (DESIGN.md §29.11, "One process against
 * several")?  core's own answer, create_partial_bitmap_paths()'s: the
 * relation may be scanned in parallel and compute_parallel_worker() gives
 * that many pages a worker - never below min_parallel_table_scan_size, at a
 * max_parallel_workers_per_gather of 0 or for a table whose parallel_workers
 * is 0.  An inheritance child is given one whatever its size, for a Parallel
 * Append to combine; below that size a child's plain scan is taken as a base
 * relation's is, against one process's bitmap heap scan.
 */
static bool
lion_plain_meets_workers(RelOptInfo *rel, double pages)
{
	if (!rel->consider_parallel || max_parallel_workers_per_gather <= 0)
		return false;
	if (rel->reloptkind != RELOPT_BASEREL &&
		pages < (double) min_parallel_table_scan_size)
		return false;
	return compute_parallel_worker(rel, pages, -1,
								   max_parallel_workers_per_gather) > 0;
}

/*
 * The correlation handed to cost_index() for a plain scan (DESIGN.md
 * §29.11).  A LIST, which restarts the heap for every batch, gets the
 * column's, as btree's does (lion_index_correlation()).
 *
 * For a scan in heap order it is not a statistic but the value that makes
 * cost_index()'s heap side come out at what the scan reads and does, which
 * is what a bitmap heap scan of the same rows reads and does, and a little
 * more.  cost_index() interpolates its I/O by the correlation squared between
 * rows packed on sel x heap pages and read in order, and rows scattered at
 * random over Mackert and Lohman's count of pages, each a random read.  A
 * scan in heap order has the packed end but not the scattered one: scattered
 * rows are read as a bitmap heap scan reads them, each page once and in block
 * order, which cost_bitmap_heap_scan() prices (compute_bitmap_pages():
 * Cardenas's count of the pages the rows touch, at random_page_cost falling
 * to seq_page_cost as those approach the whole heap).  So the I/O is that,
 * interpolated towards the packed end by the column's correlation.  Priced
 * as a random read a page, a count of 2% of 8M rows scattered over 108k of
 * the heap's 190k pages came to 2.3 times the bitmap scan's price and ran 1.6
 * to 2.5 times faster than it (2026-09-27).
 *
 * Per row, cost_index() charges cpu_tuple_cost and the quals it leaves to the
 * heap, and cost_bitmap_heap_scan() that, every index qual as well (the
 * recheck) and a bitmap entry.  The plain scan is charged the bitmap heap
 * scan's price - it rechecks its index quals whenever it drops one or reads
 * a multi-key column, which cost_index() never charges - and, for every row
 * past the first on its page, LION_PLAIN_FETCH_ROW_COST more.  In the
 * measurement behind that constant the plain scan spent 0.4 us a page LESS
 * than the bitmap heap scan, which is not credited: with one row to a page
 * the two cost about the same.
 *
 * That is one process against one process.  Against a PARALLEL bitmap heap
 * scan - where core would give the bitmap heap scan of the same pages
 * workers (lion_plain_meets_workers()) - it is not.  The plain scan is one
 * process (amcanparallel is false: cost_index() divides none of its price)
 * that reads a page when it needs it; the parallel scan reads its pages in
 * several processes, each reading ahead.  core credits that scan with its CPU
 * divided and nothing for its reads, so the plain scan gives back the credit
 * it took from cost_bitmap_heap_scan() for reading in block order: a page is
 * priced as one process's read, random_page_cost but for the pages that
 * follow the one read before them, which the kernel's read-ahead of a
 * consecutive run serves - their share of the heap, where the bitmap heap
 * scan's price takes its square root.  Below that size, and wherever no
 * parallel plan can be made, it is one process against one, priced alike,
 * which is what keeps a selective result on the plain scan (DESIGN.md
 * §29.11, "One process against several").
 *
 * A WALK hands out one entry's TIDs in heap order, then the next entry's:
 * it is a pass over the heap per entry, each reading its entry's pages in
 * block order - a bitmap heap scan of one entry's rows, done once per entry
 * (lion_plain_walk_entries()).  So its pages are what compute_bitmap_pages()
 * counts for scans repeated that often, Mackert and Lohman's for all the rows
 * together (a page a later pass reads again comes from the cache while the
 * cache holds the heap), each at the price of a pass's own pages - a pass of
 * a few pages pays random_page_cost for each, and one over most of the heap
 * seq_page_cost - and its fetches are counted per pass, and each pass past
 * the first costs its stream (LION_WALK_PASS_COST).  One entry is a sorted
 * scan; entries of a row each are btree's uncorrelated end, one random read a
 * row, and a stream each.  It used to be priced as btree's whatever its
 * entries held: `flag >= true`, one entry over 80% of 1M rows on every page,
 * came to 2.4 times the bitmap scan's price for 1.14 to 1.41 times its time,
 * and two entries of 40% to 2.9 times for 1.2 to 1.5 (2026-09-27).
 *
 * cost_index() charges no more than its uncorrelated end, so at a
 * random_page_cost near seq_page_cost the heap side of a result on nearly
 * every page is clamped there, about the bitmap heap scan's, and the fetches
 * that make the plain scan dearer are lost; what the correlation cannot carry
 * is charged to the path once it is built (lion_plain_note_remainder()).
 */
static double
lion_plain_heap_correlation(PlannerInfo *root, IndexPath *path,
							double loop_count, Selectivity sel)
{
	IndexOptInfo *index = path->indexinfo;
	RelOptInfo *baserel = index->rel;
	double		corr = lion_index_correlation(root, path);
	double		T;
	double		tuples;
	double		pages;
	double		max_pages;
	double		min_pages;
	double		spc_random_page_cost;
	double		spc_seq_page_cost;
	QualCost	qcost;
	Cost		target;
	Cost		max_io;
	Cost		min_io;
	double		csquared;
	bool		rechecks;
	double		passes = 1.0;	/* heap passes: the entries of a WALK */
	double		passpages;		/* the pages one pass reads */
	double		visits;			/* the page visits of all the passes */
	double		bmpages;		/* the pages a bitmap heap scan reads */
	double		share;			/* of the price between random and seq */
	bool		serial;			/* against a parallel bitmap heap scan */
	int			walkcol;
	int			shape;

	/*
	 * An index-only scan is left as it was: lion's are of queries that need no
	 * column, so they have no qual and read a whole column, and a multi-key
	 * one - the only kind in heap order - fetches every TID it hands out
	 * (liongettuple()), which the all-visible fraction cost_index() applies
	 * would not describe.
	 */
	if (path->path.pathtype == T_IndexOnlyScan)
		return corr;
	shape = lion_plain_scan_shape(root, path, &walkcol, &rechecks);
	if (shape == LION_PLAIN_LIST)
		return corr;

	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;
	tuples = clamp_row_est(sel * baserel->tuples);
	get_tablespace_page_costs(baserel->reltablespace, &spc_random_page_cost,
							  &spc_seq_page_cost);
	if (shape == LION_PLAIN_WALK)
		passes = Min(lion_plain_walk_entries(root, path, walkcol), tuples);

	/*
	 * compute_bitmap_pages() and cost_bitmap_heap_scan(), for `passes` scans
	 * of tuples / passes rows each
	 */
	if (loop_count > 1 || passes > 1.0)
		pages = index_pages_fetched(tuples * loop_count, baserel->pages,
									(double) index->pages, root) / loop_count;
	else
		pages = (2.0 * T * tuples) / (2.0 * T + tuples);
	pages = (pages >= T) ? T : ceil(pages);
	if (passes > 1.0)
	{
		double		perpass = tuples / passes;

		passpages = Min((2.0 * T * perpass) / (2.0 * T + perpass), T);
	}
	else
		passpages = pages;
	visits = Min(passes * passpages, tuples);

	/*
	 * The price of a page.  Against one process's bitmap heap scan, that
	 * scan's: random_page_cost falling to seq_page_cost with the square root
	 * of the share of the heap a pass reads.  Against a parallel one, over
	 * the pages compute_bitmap_pages() counts for it, one process's reading a
	 * page when it needs it: random_page_cost, and seq_page_cost for a page
	 * that follows the one read before it - the share itself, not its root.
	 */
	bmpages = Min(ceil((2.0 * T * tuples) / (2.0 * T + tuples)), T);
	serial = loop_count <= 1.0 && path->path.param_info == NULL &&
		lion_plain_meets_workers(baserel, bmpages);
	if (passpages < 2.0)
		share = 0.0;
	else if (serial)
		share = passpages / T;
	else
		share = sqrt(passpages / T);
	target = pages * (spc_random_page_cost -
					  (spc_random_page_cost - spc_seq_page_cost) * share);

	/* cost_index()'s ends: uncorrelated (max_io) and packed (min_io) */
	if (loop_count > 1)
	{
		max_pages = index_pages_fetched(tuples * loop_count, baserel->pages,
										(double) index->pages, root);
		min_pages = index_pages_fetched(ceil(sel * (double) baserel->pages) *
										loop_count, baserel->pages,
										(double) index->pages, root);
		max_io = max_pages * spc_random_page_cost / loop_count;
		min_io = min_pages * spc_random_page_cost / loop_count;
	}
	else
	{
		max_pages = index_pages_fetched(tuples, baserel->pages,
										(double) index->pages, root);
		min_pages = ceil(sel * (double) baserel->pages);
		max_io = max_pages * spc_random_page_cost;
		min_io = (min_pages > 0) ? spc_random_page_cost +
			Max(min_pages - 1.0, 0.0) * spc_seq_page_cost : 0.0;
	}

	/* from scattered as the bitmap reads them to packed */
	target += corr * corr * (min_io - target);

	/*
	 * The rows: the bitmap entry, the index quals when the scan rechecks them
	 * (§29.6) - cost_bitmap_heap_scan() charges them always, but an exact
	 * scan evaluates none, and charged them it lost `lion_pdn`'s count of
	 * 40,000 of 60,000 numeric rows (4.3 ms) to the sequential scan (10 ms),
	 * whose one numeric `=` a row core prices as an int's - and the fetches.
	 */
	cost_qual_eval(&qcost, get_quals_from_indexclauses(path->indexclauses),
				   root);
	target += tuples * ((rechecks ? qcost.per_tuple : 0.0) +
						LION_BITMAP_ROW_COST * cpu_operator_cost);
	target += LION_PLAIN_FETCH_ROW_COST * cpu_tuple_cost *
		Max(tuples - visits, 0.0);
	target += LION_WALK_PASS_COST * cpu_tuple_cost * Max(passes - 1.0, 0.0);

	/* what the uncorrelated end cannot carry is charged to the path later */
	if (target > max_io && loop_count <= 1.0 && path->path.param_info == NULL)
		lion_plain_note_remainder(root, path, target - max_io);

	if (max_io <= min_io)
		return 0.0;
	csquared = (max_io - target) / (max_io - min_io);
	csquared = Max(0.0, Min(csquared, 1.0));
	return sqrt(csquared);
}

/*
 * The quals of a scan's AND of SETS (DESIGN.md §29.11, "One price for the
 * AND of sets"): every set qual the scan answers, of every key column and as
 * many as a column has (lion_cost_col_quals()) - an equality, `IS NULL`, a
 * multi-key query, or an IN list, whose sets are united - as lion_scan_choose()
 * answers them.  Two `&&` on one array column are two sets as much as two
 * columns are.  A multi-key query that needs every row is none: the scan
 * drops it beside the others and rechecks it (lion_cost_qual_is_full()).  A
 * range or `IS NOT NULL` is a walk, priced by lion_range_walk_cost(), and
 * every other qual of a column is left to the heap recheck.
 *
 * Returns how many there are, with their key columns (1-based) in *cols and
 * their RestrictInfos in *quals, in column order, both palloc'd; 0 when a
 * column selects nothing, since the scan then reads no set at all.
 */
static int
lion_scan_set_quals(IndexPath *path, AttrNumber **cols, RestrictInfo ***quals)
{
	IndexOptInfo *index = path->indexinfo;
	int			ncols = index->nkeycolumns;
	int			maxsets = 0;
	int			nsets = 0;
	bool		nomatch = false;
	ListCell   *lc;
	int			c;

	*cols = NULL;
	*quals = NULL;
	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		return 0;

	foreach(lc, path->indexclauses)
		maxsets += list_length(((IndexClause *) lfirst(lc))->indexquals);
	if (maxsets < 2)
		return 0;
	*cols = (AttrNumber *) palloc(sizeof(AttrNumber) * maxsets);
	*quals = (RestrictInfo **) palloc(sizeof(RestrictInfo *) * maxsets);

	for (c = 0; c < ncols && !nomatch; c++)
	{
		LionCostCol cc;

		lion_cost_col_quals(path, c, &cc);
		nomatch = cc.nomatch;
		foreach(lc, cc.sets)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
			bool		all;

			if (lion_cost_qual_is_full(index, c, (Node *) rinfo->clause, &all))
				continue;
			(*cols)[nsets] = (AttrNumber) (c + 1);
			(*quals)[nsets] = rinfo;
			nsets++;
		}
		lion_cost_col_free(&cc);
	}

	return nomatch ? 0 : nsets;
}

/*
 * What genericcostestimate() charged for the index pages it prorated by the
 * selectivity of the quals (costs->numIndexPages at random_page_cost, spread
 * over the repeated scans of a nested loop or a list as index_pages_fetched()
 * spreads them): the part of its estimate that the AND of sets replaces.
 */
static Cost
lion_generic_page_cost(PlannerInfo *root, IndexPath *path, double loop_count,
					   const GenericCosts *costs)
{
	IndexOptInfo *index = path->indexinfo;
	double		num_scans = costs->num_sa_scans * loop_count;

	if (num_scans > 1)
		return index_pages_fetched(costs->numIndexPages * num_scans,
								   index->pages, (double) index->pages,
								   root) *
			costs->spc_random_page_cost / loop_count;
	return costs->numIndexPages * costs->spc_random_page_cost;
}

/*
 * Cost estimate: the generic estimate, with four corrections and the heap
 * correlation.  A scan that has to walk the whole index is priced as one
 * rather than as the selective lookup its predicate's output selectivity
 * suggests, a range pays for the entries it walks (lion_range_walk_cost()),
 * an AND of two sets or more is priced as the count pushdown prices the same
 * sets, from a selectivity the intersection probe may have measured
 * (lion_cost_set_and(), lion_isect_factor()), and a plain index scan's heap
 * side is priced by the correlation: for a scan in heap order, the bitmap
 * heap scan's price for the same pages and the plain scan's per-row work
 * (lion_plain_heap_correlation()), otherwise the column's correlation as
 * btree's is (lion_index_correlation(), DESIGN.md §29.11); a bitmap path,
 * which shares this estimate, does not read that last number.  With
 * pg_lion.enable_plain_scan off there is no plain path to price.
 */
void
lioncostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	GenericCosts costs = {0};
	bool		emits_all_rows;
	bool		fullscan = lion_scan_walks_whole_index(path, &emits_all_rows);
	double		walkrows = 0.0;

	/*
	 * pg_lion.enable_plain_scan off (DESIGN.md §29.11, "Plain scans switched
	 * off"): the planner is to plan this index as it plans a GIN index, for
	 * bitmap scans only.  get_index_paths() offers an IndexPath as a plain or
	 * index-only scan only when the index's amhasgettuple says so, and asks
	 * once the path is built and costed - here - so clearing the flag now
	 * keeps every plain path of the index, parameterized ones included, out
	 * of this planning run before add_path() has compared one with anything
	 * else.  The bitmap paths are built from the same IndexPaths and do not
	 * change, nor does what reads them: LionOrdered takes its lion side from
	 * a bitmap path when there is no plain one (lion_ordered.c).  The next
	 * planning run reads the flag from the handler again.
	 *
	 * That order inside get_index_paths() - cost first, the flag read after
	 * - is core's implementation, not a promise: test/sql/plainscan.sql
	 * pins it on every major CI builds, and it is to be checked again when
	 * a new major is added.
	 */
	if (!lion_enable_plain_scan)
		path->indexinfo->amhasgettuple = false;

	/*
	 * A full walk visits every index tuple, and genericcostestimate() then
	 * prorates the index's tuples into every index page.  The planner's
	 * count of those is ROWS: the table's, or for a partial index its
	 * reltuples, which VACUUM and ANALYZE both set to rows (DESIGN.md §18,
	 * "Statistics").
	 */
	if (fullscan)
		costs.numIndexTuples = Max(path->indexinfo->tuples, 1.0);

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * genericcostestimate() charges every index tuple it expects to visit an
	 * operator call per index qual, as a btree evaluates its quals on its
	 * tuples.  A lion scan evaluates none: its "index tuples" are the members
	 * of the posting sets its quals located, turned into TIDs a container at a
	 * time (DESIGN.md §29.11).  cpu_index_tuple_cost a TID is left, which is
	 * about what a TID costs on the release build (5 to 15 ns a TID put in a
	 * bitmap, against the 10 ns cpu_index_tuple_cost stands for at 500 units a
	 * millisecond); with the operator on top the bitmap scan of 40,000 of
	 * `lion_pdn`'s 60,000 numeric rows lost to the sequential scan at 1,185
	 * against 1,116, for 2.5 ms against 5.3.
	 */
	costs.indexTotalCost -= costs.numIndexTuples * costs.num_sa_scans *
		cpu_operator_cost *
		list_length(get_quals_from_indexclauses(path->indexclauses));
	costs.indexTotalCost = Max(costs.indexTotalCost, costs.indexStartupCost);

	if (fullscan)
	{
		double		allpages = Max((double) path->indexinfo->pages, 1.0);

		/*
		 * Rows, not postings: a row is posted once per key column and once
		 * per element of a multi-key one, so the prorated page count can come
		 * out short of the index the scan really reads.  Charge the rest of
		 * it.
		 */
		if (costs.numIndexPages < allpages)
		{
			costs.indexTotalCost += (allpages - costs.numIndexPages) *
				costs.spc_random_page_cost;
			costs.numIndexPages = allpages;
		}

		/*
		 * And, for the multi-key fallback, nothing is filtered out before the
		 * heap either: every indexed row is emitted as a candidate, fetched
		 * and rechecked there, so the heap side is the cost of a full recheck
		 * and not of the predicate's own selectivity.
		 */
		if (emits_all_rows)
			costs.indexSelectivity = 1.0;
	}

	costs.indexTotalCost += lion_range_walk_cost(root, path, &walkrows);

	/*
	 * The AND of several sets (DESIGN.md §29.11, "One price for the AND of
	 * sets"): of several columns, or several of one.  genericcostestimate()
	 * prorated the index by the selectivity of the quals together, which
	 * prices an AND by what it RETURNS; what the scan reads is the sets,
	 * located, united where a list is, and ANDed as a count ANDs them.  So the
	 * prorated pages give way to the price the count pushdown charges for the
	 * same sets (lion_cost_set_and()), and what is left of
	 * genericcostestimate() is the scan's own: a TID a result row
	 * (cpu_index_tuple_cost), and the quals' arguments.  A nested loop's inner
	 * scan repeats the AND, and its pages are spread over the repetitions as
	 * genericcostestimate() spreads its own (index_pages_fetched()).
	 *
	 * The selectivity is corrected first, by what the intersection probe
	 * measured of the same sets ("Correlated sets"): a factor on core's
	 * estimate, 1 unless the probe found it three times off or more.  It
	 * moves the TIDs charged here and everything priced from the selectivity:
	 * the heap side of a plain or bitmap path, and LionOrdered's lion side.
	 */
	if (!fullscan)
	{
		IndexOptInfo *index = path->indexinfo;
		IndexOptInfo **setidx;
		AttrNumber *setcols;
		RestrictInfo **setquals;
		int			nsets = lion_scan_set_quals(path, &setcols, &setquals);

		if (nsets >= 2)
		{
			LionAndCost setand;
			Cost		setcost;
			Cost		pagecost = lion_generic_page_cost(root, path, loop_count,
														  &costs);
			double		factor;
			int			i;

			setidx = (IndexOptInfo **) palloc(sizeof(IndexOptInfo *) * nsets);
			for (i = 0; i < nsets; i++)
				setidx[i] = index;

			factor = lion_isect_factor(root, index->rel, index, nsets, setcols,
									   (Node **) setquals);
			if (factor != 1.0)
			{
				Selectivity sel = Min(costs.indexSelectivity * factor, 1.0);
				double		tuples = rint(sel * index->rel->tuples /
										  Max(costs.num_sa_scans, 1.0));

				tuples = Max(Min(tuples, index->tuples), 1.0);
				costs.indexTotalCost += (tuples - costs.numIndexTuples) *
					costs.num_sa_scans * cpu_index_tuple_cost;
				costs.numIndexTuples = tuples;
				costs.indexSelectivity = sel;
			}

			setcost = lion_cost_set_and(root, index->rel, nsets, setidx,
										setcols, (Node **) setquals, factor,
										&setand);
			if (loop_count > 1)
				setcost = setand.cpu +
					index_pages_fetched((setand.leafpages + setand.setpages) *
										loop_count,
										index->pages, (double) index->pages,
										root) *
					costs.spc_random_page_cost / loop_count;
			costs.indexTotalCost += setcost - pagecost;
			costs.indexTotalCost = Max(costs.indexTotalCost,
									   costs.indexStartupCost);
			costs.numIndexPages = Max(setand.leafpages + setand.setpages, 1.0);
			pfree(setidx);
		}
		if (setcols != NULL)
		{
			pfree(setcols);
			pfree(setquals);
		}
	}

	/*
	 * A range walked beside other columns reads the postings of its own
	 * range, which genericcostestimate() prorated by every column's
	 * selectivity together (lion_range_walk_cost()): charge the rest of
	 * them, a share of the index's pages and a tuple cost each.  The pages
	 * are those of a walk - directory leaves in key order through their right
	 * links, which ambuild lays out in that order - so they are charged as
	 * sequential reads.  Without the term `a BETWEEN 1 AND 900000 AND b = 5`
	 * over a million unique a was priced at two thirds of the sequential scan
	 * and ran 1.4 to 1.6 times as long as it; charged at random_page_cost,
	 * the 400000-row range went to the sequential scan at 2.5 times the plain
	 * scan's time (2026-09-25 review).
	 */
	if (walkrows > costs.numIndexTuples)
	{
		IndexOptInfo *index = path->indexinfo;
		double		extra = walkrows - costs.numIndexTuples;
		double		pages = ceil(extra * (double) index->pages /
								 Max(index->tuples, 1.0));
		double		spc_random_page_cost;
		double		spc_seq_page_cost;

		get_tablespace_page_costs(index->reltablespace, &spc_random_page_cost,
								  &spc_seq_page_cost);
		pages = Min(pages, Max((double) index->pages - costs.numIndexPages, 0.0));
		costs.indexTotalCost += pages * spc_seq_page_cost +
			extra * cpu_index_tuple_cost;
		costs.numIndexPages += pages;
		costs.numIndexTuples = walkrows;
	}

	/*
	 * A plain scan that reads a multi-key column whole streams the union of
	 * its entries a window of container keys at a time, and walks every entry
	 * again for each window (DESIGN.md §29.3, lion_union_window()): charge
	 * each window past the first another read of the index.  The IndexPath is
	 * shared with the bitmap scan, which reads the index once, so that side
	 * is overcharged - which only matters on a heap of more than 65536 blocks
	 * per window and a qual that reads the whole column anyway.
	 */
	if (fullscan)
	{
		IndexOptInfo *index = path->indexinfo;
		int			col = 0;
		ListCell   *lc;

		foreach(lc, path->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (lc == list_head(path->indexclauses) || iclause->indexcol < col)
				col = iclause->indexcol;
		}
		if (lion_index_is_multikey(index, col))
		{
			double		containers = ceil((double) index->rel->pages /
										  LION_BLOCKS_PER_CONTAINER);
			double		windows = ceil(containers / lion_union_window());

			if (windows > 1.0)
				costs.indexTotalCost += (windows - 1.0) *
					Max((double) index->pages, 1.0) * seq_page_cost;
		}
	}

	/*
	 * No lion index may be read while PostgreSQL 16's old_snapshot_threshold
	 * is set (DESIGN.md §9, lion_check_old_snapshot()): price every path on
	 * one out of the running, the way 16 prices a disabled scan.  A plan that
	 * has nothing else left still gets it, and stops at the scan's ERROR.
	 */
	if (lion_old_snapshot_threshold_active())
	{
		costs.indexStartupCost += disable_cost;
		costs.indexTotalCost += disable_cost;
	}

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = lion_enable_plain_scan ?
		lion_plain_heap_correlation(root, path, loop_count,
									costs.indexSelectivity) : 0.0;
	*indexPages = costs.numIndexPages;
}
