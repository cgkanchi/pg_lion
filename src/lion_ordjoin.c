/*-------------------------------------------------------------------------
 *
 * lion_ordjoin.c
 *		LionOrderedJoin: a CustomScan for the join rel of
 *
 *			SELECT f.<cols>, d.<cols> FROM fact f JOIN dim d ON f.fk = d.pk
 *			WHERE <fact quals> AND <dim quals>
 *			  [AND f.fk IN (SELECT k FROM flt WHERE ...) ...]
 *			ORDER BY <columns a btree of the fact provides> LIMIT n
 *
 *		that walks the fact's btree in order, as LionOrdered does, over the
 *		fact's lion set; tests each member's key against the key sets of the
 *		tables that only filter the key; descends the dimension's unique
 *		btree for the key and tests the TID it finds against the dimension's
 *		own lion set BEFORE it touches the dimension's heap; and returns the
 *		two rows together, in the walk's order, until whatever is above it
 *		stops pulling.  DESIGN.md section 47 is the specification.
 *
 *		Each side is a LionOrdered walk, built by lion_ordered.c for the join
 *		and kept as a child of the join's own plan: the fact's is its ordered
 *		walk, with its set, its early stop and its fetch-and-sort switch; the
 *		dimension's a probe of its unique btree (§40.10), handed each key
 *		before its descent (lion_ordered_probe_key()).  EXPLAIN shows the
 *		join alone, its sides' lines in its own; a key set's table is
 *		scanned by a plan of core's, the join's child.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "nodes/multibitmapset.h"
#include "utils/datum.h"

#include "lion_customscan.h"
#include "lion_count.h"
#include "lion_compat.h"
#include "lion_fkjoin.h"

bool		lion_enable_ordered_join = true;
static set_join_pathlist_hook_type oj_prev_set_join_pathlist_hook = NULL;

/*
 * The most tables a join rel of the node may hold: the fact, the dimension
 * and the tables its key sets are made of.
 */
#define OJ_MAX_RELS			6

/*
 * custom_private of the plan is POSITIONAL, behind a shape marker:
 *
 *	OJ_PRIV_INTS	IntList (OJ_PRIV_MAGIC, the fact's columns in
 *					custom_scan_tlist, the fk's position among them, 1-based,
 *					the key sets)
 *	OJ_PRIV_INDEX	OidList: the dimension's unique btree, for EXPLAIN
 *	OJ_PRIV_KEYOPS	OidList: each key set's equality, hashable
 *	OJ_PRIV_KEYCOLL	OidList: ... its collation
 *	OJ_PRIV_KEYCOLS	IntList: ... the key's position in its plan's target
 *
 * custom_plans are the fact's walk, the dimension's probe and each key set's
 * plan, in that order; custom_scan_tlist is the fact's target list followed by
 * the dimension's - the rows the join puts together in its scan slot - and
 * then each key set's key, which no row holds, for EXPLAIN to name its table.
 */
#define OJ_PRIV_MAGIC		0x4c4f4a4e	/* "LOJN" */
#define OJ_PRIV_INTS		0
#define OJ_PRIV_INDEX		1
#define OJ_PRIV_KEYOPS		2
#define OJ_PRIV_KEYCOLL		3
#define OJ_PRIV_KEYCOLS		4
#define OJ_PRIV_NMEMBERS	5

static Plan *oj_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);
static Node *oj_create_state(CustomScan *cscan);
static void oj_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *oj_exec(CustomScanState *node);
static void oj_end(CustomScanState *node);
static void oj_rescan(CustomScanState *node);
static void oj_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);

const CustomPathMethods lion_ordjoin_path_methods = {
	.CustomName = "LionOrderedJoin",
	.PlanCustomPath = oj_plan_path,
};

/*
 * LionOrderedSplit (DESIGN.md §48): the join of an ORDER BY that leads with a
 * column of the dimension, run an arm at a time - one LionOrderedJoin, its
 * dimension narrowed to the rows of one entry of the column's lion index,
 * for each entry in the ORDER BY's order.
 *
 * custom_private of its path and plan is POSITIONAL:
 *
 *	OS_PRIV_INTS	IntList (OS_PRIV_MAGIC, the column as its lion index
 *					numbers it, 1-based, whether the walk is backward, where
 *					its NULL entry goes (LION_ORDER_NULLS_*), and
 *					pg_lion.ordered_split_max as it was planned)
 *	OS_PRIV_INDEX	OidList: the column's lion index
 *
 * custom_plans is the arms' LionOrderedJoin; custom_scan_tlist is its target
 * list, the rows it returns.
 */
int			lion_ordered_split_max = 8;

#define OS_PRIV_MAGIC		0x4c4f5350	/* "LOSP" */
#define OS_PRIV_INTS		0
#define OS_PRIV_INDEX		1
#define OS_PRIV_NMEMBERS	2

static Plan *os_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);
static Node *os_create_state(CustomScan *cscan);
static void os_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *os_exec(CustomScanState *node);
static void os_end(CustomScanState *node);
static void os_rescan(CustomScanState *node);
static void os_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);

const CustomPathMethods lion_ordsplit_path_methods = {
	.CustomName = "LionOrderedSplit",
	.PlanCustomPath = os_plan_path,
};

static const CustomScanMethods os_scan_methods = {
	.CustomName = "LionOrderedSplit",
	.CreateCustomScanState = os_create_state,
};

static const CustomExecMethods os_exec_methods = {
	.CustomName = "LionOrderedSplit",
	.BeginCustomScan = os_begin,
	.ExecCustomScan = os_exec,
	.EndCustomScan = os_end,
	.ReScanCustomScan = os_rescan,
	.ExplainCustomScan = os_explain,
};

static const CustomScanMethods oj_scan_methods = {
	.CustomName = "LionOrderedJoin",
	.CreateCustomScanState = oj_create_state,
};

static const CustomExecMethods oj_exec_methods = {
	.CustomName = "LionOrderedJoin",
	.BeginCustomScan = oj_begin,
	.ExecCustomScan = oj_exec,
	.EndCustomScan = oj_end,
	.ReScanCustomScan = oj_rescan,
	.ExplainCustomScan = oj_explain,
};

/* ---------------------------------------------------------------------
 * Planner (DESIGN.md §47.2)
 * --------------------------------------------------------------------- */

/*
 * A table that only filters the key (DESIGN.md §47.6): its key's values, of
 * the rows its own quals let through, become a set the fact's keys are
 * tested against.
 */
typedef struct OjKeySet
{
	RelOptInfo *rel;
	Var		   *keyvar;			/* its key column */
	Path	   *path;			/* the plan the set is built from */
	Oid			eqop;			/* the key type's equality, hashable */
	Oid			collation;
	double		rows;			/* the keys the set is expected to hold */
} OjKeySet;

/* The join the node answers: its two sides, its key and its key sets */
typedef struct OjJoin
{
	RelOptInfo *fact;
	RelOptInfo *dim;
	Var		   *fkvar;			/* the fact's key column */
	Var		   *pkvar;			/* the dimension's */
	IndexOptInfo *pkidx;		/* the unique btree on it */
	Expr	   *keyqual;		/* `pk = NULL`, the descent's qual */
	EquivalenceClass *keyec;	/* the key's class, or NULL: no key sets */
	List	   *keysets;		/* OjKeySet */
} OjJoin;

/*
 * Does rel's target hold plain columns of relids alone?  A PlaceHolderVar
 * evaluated at it - or at the join, in the join's target - is core's to
 * place; v1 leaves such queries to it.
 */
static bool
oj_target_ok(RelOptInfo *rel, Relids relids)
{
	ListCell   *lc;

	foreach(lc, rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (!IsA(var, Var) || var->varlevelsup != 0 ||
			!bms_is_member(var->varno, relids))
			return false;
	}
	return true;
}

/*
 * May rel be a side of the join (DESIGN.md §47.1): one LionOrdered may scan
 * (lion_ordered_rel_ok()), whose restriction clauses the side's own filter
 * can evaluate - none pseudoconstant, which core would gate the side's plan
 * with a Result for, and no subquery, whose plan EXPLAIN could not show under
 * the join - and whose target is its columns.
 */
static bool
oj_side_ok(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;

	if (!lion_ordered_rel_ok(root, rel))
		return false;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant ||
			contain_subplans((Node *) rinfo->clause))
			return false;
	}
	return oj_target_ok(rel, rel->relids);
}

/* A plain column of the current query level, under binary-compatible casts */
static Var *
oj_plain_var(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (node == NULL || !IsA(node, Var) || ((Var *) node)->varlevelsup != 0 ||
		((Var *) node)->varattno <= 0)
		return NULL;
	return (Var *) node;
}

/*
 * Is the join of fact and dim by `clauses` - every clause between the two -
 * one the node answers (DESIGN.md §47.1)?  One clause, `f.fk = d.pk` under an
 * equality of a unique single-column btree on d.pk, which each fact row's
 * key is then looked up in; both sides tables LionOrdered may scan.
 */
static bool
oj_key_join(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *dim,
			List *clauses, OjJoin *j)
{
	RestrictInfo *rinfo;
	OpExpr	   *op;
	Var		   *l;
	Var		   *r;
	Node	   *pkarg;
	Node	   *fkarg;
	Oid			opno;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;

	memset(j, 0, sizeof(OjJoin));

	/* one clause: f.fk = d.pk, between two columns */
	if (list_length(clauses) != 1)
		return false;
	rinfo = (RestrictInfo *) linitial(clauses);
	if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant ||
		!IsA(rinfo->clause, OpExpr))
		return false;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2 || op->opretset || !op_strict(op->opno))
		return false;
	l = oj_plain_var((Node *) linitial(op->args));
	r = oj_plain_var((Node *) lsecond(op->args));
	if (l == NULL || r == NULL)
		return false;
	if (l->varno == (int) dim->relid && r->varno == (int) fact->relid)
	{
		j->pkvar = l;
		j->fkvar = r;
		pkarg = (Node *) linitial(op->args);
		fkarg = (Node *) lsecond(op->args);
		opno = op->opno;
	}
	else if (l->varno == (int) fact->relid && r->varno == (int) dim->relid)
	{
		j->pkvar = r;
		j->fkvar = l;
		pkarg = (Node *) lsecond(op->args);
		fkarg = (Node *) linitial(op->args);
		opno = get_commutator(op->opno);
		if (!OidIsValid(opno))
			return false;
	}
	else
		return false;

	/*
	 * The dimension's key is unique under that equality, by a btree the
	 * descent reads (lion_column_unique_index()), the equality its with the
	 * key column on the left.
	 */
	j->pkidx = lion_column_unique_index(dim, j->pkvar->varattno, opno,
										op->inputcollid);
	if (j->pkidx == NULL || !op_in_opfamily(opno, j->pkidx->opfamily[0]))
		return false;
	get_op_opfamily_properties(opno, j->pkidx->opfamily[0], false, &strategy,
							   &lefttype, &righttype);
	if (strategy != BTEqualStrategyNumber ||
		lefttype != j->pkidx->opcintype[0] ||
		!IsBinaryCoercible(exprType(pkarg), lefttype) ||
		!IsBinaryCoercible(exprType(fkarg), righttype))
		return false;

	/* both sides scanned as LionOrdered scans them */
	if (!oj_side_ok(root, fact) || !oj_side_ok(root, dim))
		return false;

	j->fact = fact;
	j->dim = dim;
	j->keyec = rinfo->parent_ec;
	j->keyqual = make_opclause(opno, BOOLOID, false, (Expr *) copyObject(pkarg),
							   (Expr *) makeNullConst(righttype, -1,
													  InvalidOid),
							   InvalidOid, op->inputcollid);
	set_opfuncid((OpExpr *) j->keyqual);
	return true;
}

/*
 * The semi join whose inner side is x alone, minimal and syntactic - an
 * EXISTS or an IN of x - or NULL.  *other says another special join needs
 * x: x is in its minimal inner or outer side.  Only the syntactic outer side
 * is let pass: an IN after another has the first's table on it, which the
 * join does not need (oj_special_ok() asks the inner sides of them all).
 */
static SpecialJoinInfo *
oj_semi_of(PlannerInfo *root, RelOptInfo *x, bool *other)
{
	SpecialJoinInfo *semi = NULL;
	ListCell   *lc;

	*other = false;
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_SEMI &&
			bms_equal(sjinfo->min_righthand, x->relids) &&
			bms_equal(sjinfo->syn_righthand, x->relids))
			semi = sjinfo;
		else if (bms_overlap(sjinfo->min_lefthand, x->relids) ||
				 bms_overlap(sjinfo->min_righthand, x->relids) ||
				 bms_overlap(sjinfo->syn_righthand, x->relids))
			*other = true;
	}
	return semi;
}

/* Does the target carry the column `var` is of, as a plain Var? */
static bool
oj_target_has(PathTarget *target, Var *var)
{
	ListCell   *lc;

	foreach(lc, target->exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (IsA(v, Var) && v->varno == var->varno &&
			v->varattno == var->varattno && v->varlevelsup == 0)
			return true;
	}
	return false;
}

/*
 * May x, a table of joinrel, be folded into a key set of the join j
 * (DESIGN.md §47.6)?  Appends it to j->keysets when it may:
 *
 *	- a table with no LATERAL reference and no TABLESAMPLE, whose cheapest
 *	  path is unparameterized and carries the key;
 *	- the whole inner side of a semi join - an EXISTS or an IN - whose outer
 *	  side is in the join; or in no special join at all, inner-joined, and
 *	  then its key unique under the clause (lion_column_is_unique()), so that
 *	  no fact row is repeated;
 *	- nothing of it wanted above the join (the join rel's target);
 *	- exactly one clause joins it to the rest of the join, and that clause is
 *	  of the key's equivalence class: the key is the fact's key, f.fk = x.k,
 *	  whichever two columns of the class it was written between;
 *	- its key of the fact key's type, whose equality in the class is
 *	  hashable: the set is a hash table of that type's values;
 *	- the keys its path is expected to return fit work_mem: the set is held
 *	  in memory, and never spilled.
 */
static bool
oj_keyset_of(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j,
			 RelOptInfo *x)
{
	RangeTblEntry *rte = root->simple_rte_array[x->relid];
	SpecialJoinInfo *semi;
	bool		other;
	Relids		rest;
	List	   *clauses;
	RestrictInfo *rinfo;
	OpExpr	   *op;
	Var		   *l;
	Var		   *r;
	Var		   *key;
	Oid			type;
	Oid			eqop = InvalidOid;
	RegProcedure lhash;
	RegProcedure rhash;
	OjKeySet   *ks;
	double		bytes;
	int32		width;
	ListCell   *lc;

	if (j->keyec == NULL || j->keyec->ec_has_volatile || j->keyec->ec_broken)
		return false;
	if (x->reloptkind != RELOPT_BASEREL || rte == NULL ||
		rte->rtekind != RTE_RELATION || rte->tablesample != NULL ||
		!bms_is_empty(x->lateral_relids))
		return false;

	/* an EXISTS or an IN, or an inner join */
	semi = oj_semi_of(root, x, &other);
	if (other ||
		(semi != NULL &&
		 !bms_is_subset(semi->min_lefthand,
						bms_difference(joinrel->relids, x->relids))))
		return false;

	/* nothing of it wanted above */
	foreach(lc, joinrel->reltarget->exprs)
	{
		if (bms_overlap(pull_varnos(root, (Node *) lfirst(lc)), x->relids))
			return false;
	}

	/* one clause joins it to the rest, and it is the key's */
	rest = bms_difference(joinrel->relids, x->relids);
	clauses = generate_join_implied_equalities(root, joinrel->relids, rest, x,
											   NULL);
	foreach(lc, x->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (bms_is_subset(ri->required_relids, joinrel->relids))
			clauses = lappend(clauses, ri);
	}
	if (list_length(clauses) != 1)
		return false;
	rinfo = (RestrictInfo *) linitial(clauses);
	if (!IsA(rinfo, RestrictInfo) || rinfo->parent_ec != j->keyec ||
		!IsA(rinfo->clause, OpExpr))
		return false;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2)
		return false;
	l = oj_plain_var((Node *) linitial(op->args));
	r = oj_plain_var((Node *) lsecond(op->args));
	if (l == NULL || r == NULL)
		return false;
	key = (l->varno == (int) x->relid) ? l : r;
	if (key->varno != (int) x->relid ||
		(semi == NULL &&
		 !lion_column_is_unique(x, key->varattno, op->opno, op->inputcollid)))
		return false;

	/* a hash table of the fact key's type */
	type = exprType((Node *) j->fkvar);
	if (exprType((Node *) key) != type)
		return false;
	foreach(lc, j->keyec->ec_opfamilies)
	{
		Oid			opno = get_opfamily_member(lfirst_oid(lc), type, type,
											   BTEqualStrategyNumber);

		if (OidIsValid(opno) && op_hashjoinable(opno, type) &&
			get_op_hash_functions(opno, &lhash, &rhash) && lhash == rhash)
		{
			eqop = opno;
			break;
		}
	}
	if (!OidIsValid(eqop))
		return false;

	/* the cheapest path, with the key, and the set it makes in work_mem */
	if (x->cheapest_total_path == NULL ||
		x->cheapest_total_path->param_info != NULL ||
		!oj_target_has(x->reltarget, key))
		return false;
	width = get_attavgwidth(rte->relid, key->varattno);
	if (width <= 0)
		width = get_typavgwidth(type, key->vartypmod);
	bytes = x->cheapest_total_path->rows *
		(2.0 * (sizeof(Datum) + sizeof(uint32) + sizeof(char) + 3) +
		 (get_typbyval(type) ? 0.0 : MAXALIGN(width) + 16.0));
	if (bytes > (double) work_mem * 1024.0)
		return false;

	ks = (OjKeySet *) palloc0(sizeof(OjKeySet));
	ks->rel = x;
	ks->keyvar = key;
	ks->path = x->cheapest_total_path;
	ks->eqop = eqop;
	ks->collation = j->keyec->ec_collation;
	ks->rows = clamp_row_est(x->cheapest_total_path->rows);
	j->keysets = lappend(j->keysets, ks);
	return true;
}

/*
 * Is every special join with an inner side in joinrel the semi join of one of
 * j's key sets?  The fact and the dimension are then inner-joined, and so is
 * every key set that is not a semi join's.
 */
static bool
oj_special_ok(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j)
{
	ListCell   *lc;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		Relids		rh = bms_union(sjinfo->min_righthand,
								   sjinfo->syn_righthand);
		bool		mine = false;
		ListCell   *lc2;

		if (!bms_overlap(rh, joinrel->relids))
			continue;
		foreach(lc2, j->keysets)
		{
			if (sjinfo->jointype == JOIN_SEMI &&
				bms_equal(rh, ((OjKeySet *) lfirst(lc2))->rel->relids))
				mine = true;
		}
		if (!mine)
			return false;
	}
	return true;
}

/*
 * The core plan a LionOrderedJoin path competes with (DESIGN.md §47.3,
 * "Cold reads"), found before the first of its paths is added: add_path()
 * frees the paths a new one dominates.
 */
typedef struct OjCompetitor
{
	bool		nestloop;		/* a nested loop into an index scan, in the
								 * walk's order */
	bool		factwalk;		/* ... whose outer side is the fact's index
								 * scan */
	bool		indexonly;		/* ... index-only */
	bool		lionwalk;		/* ... or the fact's LionOrdered walk */
	double		walkreads;		/* that walk's reads over the LIMIT's share
								 * (lion_ordered_walk_reads()) */
	Cost		cost;			/* its price of the LIMIT's rows */
	Cost		memo;			/* what its Memoize's hits under the LIMIT
								 * leave out of that
								 * (lion_memoize_limit_extra()) */
} OjCompetitor;

/* The share of a path's rows the query's LIMIT takes: all without one */
static double
oj_limit_share(PlannerInfo *root, double rows)
{
	if (root->limit_tuples <= 0 || rows <= 0)
		return 1.0;
	return Min(1.0, root->limit_tuples / rows);
}

/*
 * Is p core's nested loop over the fact's LionOrdered walk - its outer side,
 * under a projection, a LionOrdered path of fact, and its inner side free of
 * lion's paths?  *walk is that walk.
 */
static bool
oj_loop_over_walk(Path *p, RelOptInfo *fact, Path **walk)
{
	JoinPath   *jp;
	Path	   *outer;

	*walk = NULL;
	while (p != NULL && IsA(p, ProjectionPath))
		p = ((ProjectionPath *) p)->subpath;
	if (p == NULL || !IsA(p, NestPath))
		return false;
	jp = (JoinPath *) p;
	if (lion_path_has_lion(jp->innerjoinpath))
		return false;
	outer = jp->outerjoinpath;
	while (outer != NULL && IsA(outer, ProjectionPath))
		outer = ((ProjectionPath *) outer)->subpath;
	if (outer == NULL || !IsA(outer, CustomPath) ||
		((CustomPath *) outer)->methods != &lion_ordered_path_methods ||
		outer->parent != fact)
		return false;
	*walk = outer;
	return true;
}

/*
 * The plan the LIMIT would take of core's paths of joinrel so far (core adds
 * them before it calls the hook): unparameterized, not disabled, with no lion
 * path inside (lion_path_has_lion()) but a nested loop's outer LionOrdered
 * walk of the fact (oj_loop_over_walk()), which a page core walks over
 * LionOrdered competes with; an ordered one - its first sort key the walk's -
 * at its price of the LIMIT's share of its rows, any other at its total,
 * which the Sort over it reads whole.  Only a nested loop into an index scan
 * in the walk's order is weighed: for what its Memoize's hits under the LIMIT
 * leave out of its price (lion_memoize_limit_extra()), and for its cold
 * reads, a walk over LionOrdered with that walk's own
 * (lion_ordered_walk_reads()).  A hash join and its Sort read the two sets'
 * heaps as bitmap scans, which §46 does not weigh either.
 */
static void
oj_competitor(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *fact,
			  List *pathkeys, OjCompetitor *c)
{
	Path	   *best = NULL;
	Path	   *bestwalk = NULL;
	bool		bestordered = false;
	double		share;
	ListCell   *lc;

	memset(c, 0, sizeof(OjCompetitor));
	foreach(lc, joinrel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		Path	   *walk = NULL;
		bool		ordered;
		Cost		cost;

		if (p->param_info != NULL || lion_path_disabled(p) ||
			(lion_path_has_lion(p) && !oj_loop_over_walk(p, fact, &walk)))
			continue;
		ordered = (p->pathkeys != NIL && pathkeys != NIL &&
				   linitial(p->pathkeys) == linitial(pathkeys));
		cost = ordered ?
			p->startup_cost + oj_limit_share(root, p->rows) *
			(p->total_cost - p->startup_cost) :
			p->total_cost;
		if (best == NULL || cost < c->cost)
		{
			best = p;
			bestwalk = walk;
			bestordered = ordered;
			c->cost = cost;
		}
	}
	if (best == NULL || !bestordered ||
		lion_competitor_kind(best) != LION_COMPETITOR_NESTLOOP)
		return;
	c->nestloop = true;
	share = oj_limit_share(root, best->rows);
	c->memo = lion_memoize_limit_extra(root, best, share);
	if (bestwalk != NULL)
	{
		c->lionwalk = lion_ordered_walk_reads(root, bestwalk, share,
											  &c->walkreads);
		return;
	}

	/* the loop's outermost side: the fact's walk, or something else */
	while (best != NULL && !IsA(best, IndexPath))
	{
		if (IsA(best, NestPath) || IsA(best, MergePath) ||
			IsA(best, HashPath))
			best = ((JoinPath *) best)->outerjoinpath;
		else if (IsA(best, ProjectionPath))
			best = ((ProjectionPath *) best)->subpath;
		else
			best = NULL;
	}
	if (best != NULL && best->parent == fact)
	{
		c->factwalk = true;
		c->indexonly = (best->pathtype == T_IndexOnlyScan);
	}
}

/*
 * The share of the fact's non-NULL keys that name a dimension row (DESIGN.md
 * §47.3, "The rows").  The dimension's key is unique, but nothing makes the
 * fact's a foreign key: of the fact key's nd distinct values at most
 * dim.tuples are the dimension's, so
 *
 *	found = min(1, dim.tuples / nd)
 *
 * as core's eqjoinsel_inner() takes the join with no MCV list to match:
 * keys x dimrows / max(nd, dim.tuples) rows.  1 where nd is a default.
 */
static double
oj_found_share(PlannerInfo *root, OjJoin *j, double dimtuples)
{
	VariableStatData vardata;
	double		nd;
	bool		isdefault;

	examine_variable(root, (Node *) j->fkvar, 0, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	if (isdefault || nd <= dimtuples)
		return 1.0;
	return dimtuples / nd;
}

/*
 * The price of one LionOrderedJoin (DESIGN.md §47.3): the fact's walk as
 * LionOrdered prices it with its set (lion_ordered_walk_price()); a test of
 * each fact row's key, and of each key against the key sets, built before the
 * walk from their tables' plans; for each key left a descent of the
 * dimension's unique btree and a test of the TID against its set; and for
 * each member found a heap fetch, its recheck and its filter; then the join's
 * own target.  A non-NULL key costs its descent whether or not it names a
 * dimension row; found of them do (oj_found_share()).  A key set's keys are
 * taken to be dimension keys: it lets through ks.rows / dim.tuples of the
 * keys that name a row, found x ks.rows / dim.tuples of them all - ks.rows /
 * max(nd, dim.tuples), core's eqjoinsel_semi() once nd passes dim.tuples -
 * and each key it lets through names one, so that the orphan share is taken
 * once, at the first key set or, with none, at the members and the rows.
 * *rows is what it returns: of the descents that find a dimension row, the
 * share of the dimension's rows that pass its quals (lion_probe_rel_rows()).
 * *share is the share of the walk the query's LIMIT is expected to take,
 * *probes the descents of the whole walk.  armshare is 1 for the join, and
 * for an arm of a LionOrderedSplit (§48) the share of the dimension's rows
 * its entry holds: the dimension's set narrowed to them holds that share of
 * its members and its rows, and every descent's TID is tested against it.
 *
 * Then, against comp, core's nested loop over the same walk, both over the
 * LIMIT's share s of the walk.  First its Memoize: the loop's price core
 * there, of the LIMIT's share of its rows, lacks memo, the hits core counted
 * that the share of its calls does not meet (lion_memoize_limit_extra()), and
 * the price is multiplied by
 *
 *	memoscale = core / (core + memo)
 *
 * unbounded, as lion_units_price() puts a path into the loop's units.  Then
 * §39's cold reads, as LionProbeJoin weighs them (lion_units_cold()): the
 * loop reads a leaf of the dimension for each key it keeps and a heap page
 * for each that names a row (found of them), and over the fact's index scan
 * a fact heap page for each entry it walks (in an index-only scan, those of
 * pages not all-visible), over a LionOrdered walk that walk's own reads
 * (lion_ordered_walk_reads(), none where its price carries a cold discount
 * of its own already); the node reads the fact's heap for its fetches
 * (factfetches over the whole walk, lo_cost()'s), a leaf for each descent,
 * the dimension's heap for each member, the lion index pages its sets'
 * builds read, and the key sets' tables.  Each page is charged perpage,
 * lion_units_cold_page() over core's random_page_cost in the
 * share of the tables effective_cache_size cannot hold, and the price
 * multiplied by
 *
 *	(own + ownreads * perpage) * (core + memo) /
 *		((core + memo + corereads * perpage) * own)
 *
 * - own the node's price of the LIMIT's share - at most 1 and at least
 * 1 / pg_lion.cold_max_discount, on top of memoscale.
 */
static void
oj_cost(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j, LionOrderedSet *ds,
		IndexPath *ord, Cost factstartup, Cost facttotal, double factrows,
		double factfetches, double factlionpages, const OjCompetitor *comp,
		double armshare, double *rows, double *probes, double *share,
		Cost *startup_p, Cost *total_p)
{
	RelOptInfo *dim = j->dim;
	IndexOptInfo *pkidx = j->pkidx;
	double		nonnull;
	double		keys;
	double		found;
	double		hit;
	double		descents;
	double		members;
	double		out;
	double		idxtuples = Max(pkidx->tuples, 1.0);
	double		idxpages = Max((double) pkidx->pages, 1.0);
	double		dimtuples = Max(dim->tuples, 1.0);
	double		spc_random;
	double		spc_seq;
	Cost		descent;
	Cost		startup;
	Cost		run;
	QualCost	resid;
	double		factor;
	ListCell   *lc;

	/* the fact's walk, and a test of each of its rows' keys */
	startup = factstartup;
	run = facttotal - factstartup;
	nonnull = nulltestsel(root, IS_NOT_NULL, (Node *) j->fkvar,
						  j->fact->relid, JOIN_INNER, NULL);
	run += factrows * cpu_operator_cost;
	keys = clamp_row_est(factrows * nonnull);
	found = oj_found_share(root, j, dimtuples);

	/*
	 * The key sets: built from their plans, and each key tested in turn.
	 * hit is the share of the keys left that name a dimension row: found
	 * until a key set has let through only its own keys, the dimension's.
	 */
	descents = keys;
	hit = found;
	foreach(lc, j->keysets)
	{
		OjKeySet   *ks = (OjKeySet *) lfirst(lc);

		startup += ks->path->total_cost + ks->rows * cpu_operator_cost;
		run += descents * cpu_operator_cost;
		descents *= Min(1.0, hit * ks->rows / dimtuples);
		hit = 1.0;
	}
	descents = clamp_row_est(descents);

	/*
	 * Each key's descent of the dimension's btree, priced as
	 * btcostestimate() prices one: its comparisons, the pages of the
	 * descent, an index tuple; and the leaf pages the descents read, as
	 * index_pages_fetched() spreads them over the index.  Then the set's
	 * test of the TID found, by the descents that find one.
	 */
	descent = ceil(log(idxtuples) / log(2.0)) * cpu_operator_cost +
		(Max(pkidx->tree_height, 0) + 1) * 50.0 * cpu_operator_cost +
		cpu_index_tuple_cost + cpu_operator_cost;
	get_tablespace_page_costs(pkidx->reltablespace, &spc_random, &spc_seq);
	run += descents * descent +
		index_pages_fetched(descents, (BlockNumber) idxpages, idxpages, root) *
		spc_random;
	if (ds->lion != NULL || armshare < 1.0)
		run += descents * hit * cpu_operator_cost;

	/*
	 * The set's build, and the members' heap fetches - the TIDs of the set
	 * among the keys looked up, each read at random - with their recheck and
	 * filter.
	 */
	startup += ds->build;
	members = clamp_row_est(descents * hit * ds->candsel * armshare);
	get_tablespace_page_costs(dim->reltablespace, &spc_random, &spc_seq);
	cost_qual_eval(&resid, ds->residual, root);
	startup += resid.startup;
	run += index_pages_fetched(members, dim->pages, idxpages, root) *
		spc_random +
		members * (cpu_tuple_cost + ds->recheck + resid.per_tuple);

	/* the rows it returns, and the join's target */
	out = clamp_row_est(descents * hit * armshare *
						Min(1.0, lion_probe_rel_rows(root, dim) / dimtuples));
	startup += joinrel->reltarget->cost.startup;
	run += out * (cpu_tuple_cost + joinrel->reltarget->cost.per_tuple);

	*rows = out;
	*probes = descents;

	/*
	 * The share of the walk the LIMIT takes, which decides whether each
	 * side's set is lazy or built at once (§30.4); it is not a stop.  The
	 * LIMIT bounds the join's output only through what sits above it, and
	 * a join above that filters lets the walk go on further.  But a lazy set
	 * that goes on further builds itself once its probes have cost what the
	 * build would, so going lazy wrongly costs about one more build, where a
	 * set built at once for a walk that stops after a few hundred entries
	 * costs the whole build.  The cheaper error is the lazy one, and the
	 * share is applied with joins above the node too: the left joins and
	 * NOT EXISTS the node sits below keep its rows about one to one with the
	 * LIMIT's.  The plain LionOrdered keeps its own stricter rule (lo_limit(),
	 * the node alone under the LIMIT) for its hard stop, which this is not.
	 */
	*share = oj_limit_share(root, out);

	/* the loop's Memoize and the cold reads, over the LIMIT's share */
	factor = 1.0;
	if (comp->nestloop)
	{
		double		s = *share;
		double		ordpages = Max((double) ord->indexinfo->pages, 1.0);
		Cost		perpage = lion_units_cold_page(root, ordpages + idxpages,
												   j->fact->reltablespace);
		LionUnits	u;

		memset(&u, 0, sizeof(LionUnits));
		u.kind = LION_COMPETITOR_NESTLOOP;
		u.rate = 1.0;
		u.margin = 1.0;
		u.skewscale = 1.0;
		u.nlcost = comp->cost;
		u.memoscale = (comp->memo > 0.0 && comp->cost > 0.0) ?
			comp->cost / (comp->cost + comp->memo) : 1.0;
		if (perpage > 0.0)
		{
			double		walked = clamp_row_est(ord->indexselectivity *
											   j->fact->tuples);
			double		corereads;
			double		ownreads;

			corereads = index_pages_fetched(s * keys, (BlockNumber) idxpages,
											idxpages, root) +
				index_pages_fetched(s * keys * found, dim->pages, idxpages,
									root);
			if (comp->lionwalk)
				corereads += comp->walkreads;
			else if (comp->factwalk)
				corereads += index_pages_fetched(s * walked, j->fact->pages,
												 ordpages, root) *
					(comp->indexonly ? 1.0 - j->fact->allvisfrac : 1.0);
			ownreads = index_pages_fetched(s * factfetches, j->fact->pages,
										   ordpages, root) +
				index_pages_fetched(s * descents, (BlockNumber) idxpages,
									idxpages, root) +
				index_pages_fetched(s * members, dim->pages, idxpages, root) +
				factlionpages + ds->lionpages;
			foreach(lc, j->keysets)
				ownreads += (double) ((OjKeySet *) lfirst(lc))->rel->pages;

			u.nlcold = corereads * perpage;
			u.coldpage = perpage;
			lion_units_cold(&u, startup + s * run, ownreads);
		}
		factor = u.rate * u.memoscale;
	}
	*startup_p = startup * factor;
	*total_p = (startup + run) * factor;
}

/*
 * A LionOrderedJoin path of the join j (DESIGN.md §47.2), at the price given:
 * the fact's walk of ord with its set factlion, index-only by iocols; the
 * dimension's probe with its set dimlion; the key sets.  factweight is the
 * share of the walk the query's LIMIT is expected to take, dimweight the
 * descents it is expected to make (lion_ordered_side_path()); factrows,
 * factstartup and facttotal the walk's own, dimbuild the dimension's set's.
 */
static CustomPath *
oj_join_path(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j,
			 IndexPath *ord, List *pathkeys, Path *factlion, Path *dimlion,
			 List *iocols, double factweight, double dimweight,
			 double factrows, Cost factstartup, Cost facttotal,
			 Cost dimbuild, double rows, Cost startup, Cost total)
{
	CustomPath *cp;
	CustomPath *factpath;
	CustomPath *dimpath;
	IndexPath  *pkpath;
	List	   *keyinfo = NIL;
	List	   *paths;
	ListCell   *lc;

	factpath = lion_ordered_side_path(j->fact, ord, factlion, iocols, false,
									  factweight, NULL, factrows, factstartup,
									  facttotal);
	pkpath = create_index_path(root, j->pkidx, NIL, NIL, NIL, NIL,
							   ForwardScanDirection, false, NULL, 1.0, false);
	dimpath = lion_ordered_side_path(j->dim, pkpath, dimlion, NIL, true,
									 dimweight, j->keyqual, rows, dimbuild,
									 dimbuild);
	paths = list_make2(factpath, dimpath);
	foreach(lc, j->keysets)
	{
		OjKeySet   *ks = (OjKeySet *) lfirst(lc);

		paths = lappend(paths, ks->path);
		keyinfo = lappend(keyinfo, list_make3(ks->keyvar,
											  list_make1_oid(ks->eqop),
											  list_make1_oid(ks->collation)));
	}

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = joinrel;
	cp->path.pathtarget = joinrel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup;
	cp->path.total_cost = total;
	cp->path.pathkeys = pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = paths;
	cp->custom_private = list_make3(j->fkvar,
									list_make1_oid(j->pkidx->indexoid),
									keyinfo);
	cp->methods = &lion_ordjoin_path_methods;
	return cp;
}

/*
 * One LionOrderedJoin path (DESIGN.md §47.2): the fact's walk of ord with its
 * set factlion, the dimension's probe with its set dimlion, the key sets,
 * priced and offered to the join rel at pg_lion.pushdown_margin (§39), as
 * LionOrdered's paths are offered to their relation.
 */
static void
oj_add_path(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j,
			IndexPath *ord, List *pathkeys, const OjCompetitor *comp,
			Path *factlion, Path *dimlion)
{
	CustomPath *cp;
	LionOrderedSet ds;
	List	   *iocols;
	double		factrows = lion_probe_rel_rows(root, j->fact);
	double		rows;
	double		probes;
	double		share;
	double		margin = lion_units_margin();
	Cost		factstartup;
	Cost		facttotal;
	double		factfetches;
	double		factlionpages;
	Cost		startup;
	Cost		total;

	/* the fact's walk, index-only where its btree has every column it needs */
	lion_ordered_walk_price(root, j->fact, ord, factlion, factrows, true,
							&iocols, &factstartup, &facttotal, &factfetches,
							&factlionpages);
	lion_ordered_set_info(root, j->dim, dimlion, &ds);
	oj_cost(root, joinrel, j, &ds, ord, factstartup, facttotal, factrows,
			factfetches, factlionpages, comp, 1.0, &rows, &probes, &share,
			&startup, &total);

	/* the dimension's weight: the descents the LIMIT's share of the walk makes */
	cp = oj_join_path(root, joinrel, j, ord, pathkeys, factlion, dimlion,
					  iocols, share, clamp_row_est(probes * share), factrows,
					  factstartup, facttotal, ds.build, rows,
					  startup / margin, total / margin);
	elog(DEBUG2, "lion: a LionOrderedJoin path over %s, %.2f..%.2f for %.0f rows",
		 get_rel_name(ord->indexinfo->indexoid), cp->path.startup_cost,
		 cp->path.total_cost, rows);

	/* a walk of an index and descents of another, as index scans are */
	lion_path_core_switches(&cp->path, LION_CORE_INDEXSCAN);
	add_path(joinrel, &cp->path);
}

/* ---------------------------------------------------------------------
 * LionOrderedSplit (DESIGN.md §48): the join an arm at a time, one an entry
 * of a column of the dimension that leads the ORDER BY
 * --------------------------------------------------------------------- */

/*
 * The arms a split is expected to run, in the walk's order (os_arms()): each
 * an entry of the column, its share of the dimension's rows, and its share of
 * the rows the walk's entries hold together - the share of the rows that
 * pass the dimension's quals its narrowed set is taken to hold.
 */
typedef struct OsArms
{
	int			n;
	double	   *freq;			/* [n] */
	double	   *share;			/* [n] */
} OsArms;

/* The split of a join: the column that leads the ORDER BY, and its walk */
typedef struct OsSplit
{
	PathKey    *pathkey;		/* the ORDER BY's first: the column's */
	IndexOptInfo *index;		/* the column's lion index */
	int			indexcol;		/* ... the column in it, 0-based */
	Var		   *col;
	bool		backward;
	int			nulls;			/* LION_ORDER_NULLS_* */
	OsArms		arms;
} OsSplit;

/* A most common value of the column and its share, sorted by the column */
typedef struct OsValue
{
	Datum		value;
	double		freq;
} OsValue;

typedef struct OsValueSort
{
	FmgrInfo   *cmp;
	Oid			collation;
} OsValueSort;

static int
os_value_cmp(const void *a, const void *b, void *arg)
{
	OsValueSort *s = (OsValueSort *) arg;

	return DatumGetInt32(FunctionCall2Coll(s->cmp, s->collation,
										   ((const OsValue *) a)->value,
										   ((const OsValue *) b)->value));
}

/*
 * Where the walk of the split's column takes its NULL entry: where the
 * pathkey puts NULLs, or nowhere when a restriction clause of the dimension
 * is strict in the column, which no NULL passes.
 */
static int
os_nulls(RelOptInfo *dim, Var *col, PathKey *pathkey)
{
	ListCell   *lc;

	foreach(lc, dim->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (mbms_is_member(col->varno,
						   col->varattno - FirstLowInvalidHeapAttributeNumber,
						   find_nonnullable_vars((Node *) rinfo->clause)))
			return LION_ORDER_NULLS_NONE;
	}
	return pathkey->pk_nulls_first ? LION_ORDER_NULLS_FIRST :
		LION_ORDER_NULLS_LAST;
}

/*
 * The arms a split of the column col is expected to run (DESIGN.md §48.3),
 * in the walk's order: an entry each of the column's distinct values -
 * lion's count of its entries where it has one (§33), ANALYZE's otherwise -
 * and its NULL entry where the walk takes it and the column has NULLs.  A
 * most common value's share of the rows is its frequency, the list sorted by
 * the column's order and walked in the walk's direction; the values past the
 * list share what is left evenly, and are taken after it.  False - no split
 * - when the arms are more than pg_lion.ordered_split_max, or the count is a
 * default.
 */
static bool
os_arms(PlannerInfo *root, Var *col, bool backward, int nulls, OsArms *arms)
{
	VariableStatData vardata;
	AttStatsSlot sslot;
	bool		hasmcv = false;
	bool		isdefault;
	double		nd;
	double		nullfrac = 0.0;
	double		mcvsum = 0.0;
	double		walked = 0.0;
	double		rest;
	OsValue    *mcv = NULL;
	int			nvals;
	int			nmcv = 0;
	bool		nullarm;
	bool		ok = false;
	int			i;
	int			k = 0;

	memset(arms, 0, sizeof(OsArms));
	examine_variable(root, (Node *) col, 0, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	if (isdefault || nd > (double) lion_ordered_split_max)
		goto done;
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats = (Form_pg_statistic)
			GETSTRUCT(vardata.statsTuple);

		nullfrac = stats->stanullfrac;
		hasmcv = get_attstatsslot(&sslot, vardata.statsTuple,
								  STATISTIC_KIND_MCV, InvalidOid,
								  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	}
	if (hasmcv)
	{
		TypeCacheEntry *typentry = lookup_type_cache(col->vartype,
													 TYPECACHE_CMP_PROC_FINFO);
		OsValueSort sort;

		if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
			goto done;
		nmcv = Min(sslot.nvalues, sslot.nnumbers);
		mcv = (OsValue *) palloc(sizeof(OsValue) * Max(nmcv, 1));
		for (i = 0; i < nmcv; i++)
		{
			mcv[i].value = sslot.values[i];
			mcv[i].freq = sslot.numbers[i];
			mcvsum += sslot.numbers[i];
		}
		sort.cmp = &typentry->cmp_proc_finfo;
		sort.collation = col->varcollid;
		qsort_arg(mcv, nmcv, sizeof(OsValue), os_value_cmp, &sort);
	}
	nvals = Max((int) ceil(nd), nmcv);
	nullarm = (nulls != LION_ORDER_NULLS_NONE && nullfrac > 0.0);
	arms->n = nvals + (nullarm ? 1 : 0);
	if (arms->n > lion_ordered_split_max)
		goto done;

	arms->freq = (double *) palloc(sizeof(double) * arms->n);
	arms->share = (double *) palloc(sizeof(double) * arms->n);
	rest = (nvals > nmcv) ?
		Max(0.0, 1.0 - nullfrac - mcvsum) / (double) (nvals - nmcv) : 0.0;
	if (nullarm && nulls == LION_ORDER_NULLS_FIRST)
		arms->freq[k++] = nullfrac;
	for (i = 0; i < nmcv; i++)
		arms->freq[k++] = mcv[backward ? nmcv - 1 - i : i].freq;
	for (i = nmcv; i < nvals; i++)
		arms->freq[k++] = rest;
	if (nullarm && nulls == LION_ORDER_NULLS_LAST)
		arms->freq[k++] = nullfrac;
	Assert(k == arms->n);
	for (i = 0; i < arms->n; i++)
		walked += arms->freq[i];
	for (i = 0; i < arms->n; i++)
		arms->share[i] = (walked > 0.0) ? arms->freq[i] / walked :
			1.0 / (double) arms->n;
	ok = true;

done:
	if (hasmcv)
		free_attstatsslot(&sslot);
	ReleaseVariableStats(vardata);
	return ok;
}

/*
 * One entry of the column's walk (DESIGN.md §30.11, "Cost"): the entry, each
 * of its TIDs handed out and gathered into the arm's set, and the pages its
 * TIDs take, two bytes a row; and the arm's set of `members`, held with the
 * dimension's set of `setbytes` within hash_mem and past it spilled
 * (lion_ordered_spill_price()), over a dimension of `pages` pages.
 */
static Cost
os_entry_cost(double tids, double members, double setbytes, double pages)
{
	double		armbytes = lion_ordered_set_bytes(members, pages);

	return LION_RANGE_UNION_ENTRY_COST + tids * 2.0 * cpu_operator_cost +
		ceil(tids * 2.0 / BLCKSZ) * seq_page_cost +
		lion_ordered_spill_price(setbytes + armbytes) -
		lion_ordered_spill_price(setbytes);
}

/*
 * The price of a split of the join j (DESIGN.md §48.3).  Each arm is priced
 * as §47.3 prices the join (oj_cost(), its competitor and cold reads with
 * it), the dimension's set narrowed to the arm's share of the rows; the arms
 * run in the walk's order, each a walk of the fact from its start, until the
 * LIMIT's rows are met.  The first arm's start-up is paid once - the sets'
 * builds, the key sets', the column walk's descent - and each later arm's
 * walk restarts with a descent of the fact's btree; each arm reads its entry
 * first, and holds its narrowed set with the dimension's within hash_mem,
 * the bytes past it spilled.  So, with out_i the rows of arm i and run_i its
 * price past its start-up, the price of the LIMIT's n rows is
 *
 *	C(n) = S + sum of run_i over the arms before the m-th, the first whose
 *		   rows reach n, + run_m x (n - rows before it) / out_m
 *
 * plus the entries and restarts, and without a LIMIT every arm's.  The path
 * is shaped for core's Limit, which takes startup + (total - startup) x n /
 * rows of a path: its start-up S, the first arm's; its rows R the sum of the
 * arms'; and its total S + (C(n) - S) x R / n, at which the Limit's price is
 * C(n).  Without a LIMIT, or one the arms never meet, the total is every
 * arm's.  *factweight is the shares of the fact's walk the arms take, summed,
 * which weigh the fact's lazy set (§47.4), and *dimweight the descents those
 * shares make, which weigh nothing at run time: under a split the
 * dimension's set is built at once (lion_ordered_narrow_begin()), as
 * oj_cost() prices it; *armstartup and *armtotal an arm's, for its
 * LionOrderedJoin's plan.
 */
static void
os_cost(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j, OsSplit *sp,
		LionOrderedSet *ds, IndexPath *ord, Cost factstartup,
		Cost facttotal, double factrows, double factfetches,
		double factlionpages, const OjCompetitor *comp, double *rows_p,
		double *factweight, double *dimweight, Cost *startup_p,
		Cost *total_p, Cost *armstartup, Cost *armtotal)
{
	double		dimtuples = Max(j->dim->tuples, 1.0);
	double		dimpages = (double) j->dim->pages;
	double		setbytes = 0.0;
	double		need = root->limit_tuples;
	double		rows = 0.0;
	double		spc_random;
	double		spc_seq;
	Cost		restart;
	Cost		once = 0.0;
	Cost		full = 0.0;
	Cost		atlimit = -1.0;
	Cost		startup = 0.0;
	int			i;

	get_tablespace_page_costs(ord->indexinfo->reltablespace, &spc_random,
							  &spc_seq);
	restart = (Max(ord->indexinfo->tree_height, 0) + 1) * 50.0 *
		cpu_operator_cost + spc_random;
	if (ds->lion != NULL)
		setbytes = lion_ordered_set_bytes(dimtuples * ds->candsel, dimpages);
	*factweight = 0.0;
	*dimweight = 0.0;
	for (i = 0; i < sp->arms.n; i++)
	{
		double		out;
		double		probes;
		double		share;
		double		frac = 1.0;
		Cost		st;
		Cost		tot;
		Cost		run;
		Cost		entry;

		oj_cost(root, joinrel, j, ds, ord, factstartup, facttotal, factrows,
				factfetches, factlionpages, comp, sp->arms.share[i], &out,
				&probes, &share, &st, &tot);
		run = tot - st;
		entry = os_entry_cost(dimtuples * sp->arms.freq[i],
							  ds->lion != NULL ?
							  dimtuples * ds->candsel * sp->arms.share[i] :
							  dimtuples * sp->arms.freq[i],
							  setbytes, dimpages);
		if (i == 0)
		{
			/* the column's walk descends to its first entry (§30.11) */
			once = st + 2.0 * random_page_cost;
			full = once;
			startup = once + entry;
		}
		else
			full += restart;
		full += entry + run;
		rows += out;
		if (atlimit >= 0.0)
			continue;			/* past the arm the LIMIT is met in */
		if (need > 0.0 && out >= need)
		{
			frac = need / out;
			atlimit = full - run * (1.0 - frac);
		}
		else if (need > 0.0)
			need -= out;
		*factweight += frac;
		*dimweight += probes * frac;
	}

	*rows_p = clamp_row_est(rows);
	*startup_p = startup;
	if (root->limit_tuples > 0.0 && atlimit >= 0.0)
		*total_p = startup + (atlimit - startup) * *rows_p /
			root->limit_tuples;
	else
		*total_p = full;
	*armstartup = startup;
	*armtotal = startup + (full - startup) / (double) sp->arms.n;
}

/*
 * One LionOrderedSplit path (DESIGN.md §48.2): the split sp of the join j,
 * its arms a LionOrderedJoin of the fact's walk of ord in the order joinkeys
 * - its set factlion - and the dimension's probe with its set dimlion,
 * narrowed per arm; priced (os_cost()) and offered to the join rel at
 * pg_lion.pushdown_margin, as the join's paths are.
 */
static void
os_add_path(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j, OsSplit *sp,
			IndexPath *ord, List *joinkeys, List *pathkeys,
			const OjCompetitor *comp, Path *factlion, Path *dimlion)
{
	CustomPath *cp;
	CustomPath *child;
	LionOrderedSet ds;
	List	   *iocols;
	double		factrows = lion_probe_rel_rows(root, j->fact);
	double		margin = lion_units_margin();
	double		rows;
	double		factweight;
	double		dimweight;
	double		factfetches;
	double		factlionpages;
	Cost		factstartup;
	Cost		facttotal;
	Cost		startup;
	Cost		total;
	Cost		armstartup;
	Cost		armtotal;

	lion_ordered_walk_price(root, j->fact, ord, factlion, factrows, true,
							&iocols, &factstartup, &facttotal, &factfetches,
							&factlionpages);
	lion_ordered_set_info(root, j->dim, dimlion, &ds);
	os_cost(root, joinrel, j, sp, &ds, ord, factstartup, facttotal, factrows,
			factfetches, factlionpages, comp, &rows, &factweight, &dimweight,
			&startup, &total, &armstartup, &armtotal);

	child = oj_join_path(root, joinrel, j, ord, joinkeys, factlion, dimlion,
						 iocols, factweight, clamp_row_est(dimweight),
						 factrows, factstartup, facttotal, ds.build,
						 clamp_row_est(rows / sp->arms.n),
						 armstartup / margin, armtotal / margin);

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = joinrel;
	cp->path.pathtarget = joinrel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup / margin;
	cp->path.total_cost = total / margin;
	cp->path.pathkeys = pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make1(child);
	cp->custom_private =
		list_make2(list_make5_int(OS_PRIV_MAGIC, sp->indexcol + 1,
								  (int) sp->backward, sp->nulls,
								  lion_ordered_split_max),
				   list_make1_oid(sp->index->indexoid));
	cp->methods = &lion_ordsplit_path_methods;
	elog(DEBUG2, "lion: a LionOrderedSplit path over %s by %s, %d arms, %.2f..%.2f for %.0f rows",
		 get_rel_name(ord->indexinfo->indexoid),
		 get_rel_name(sp->index->indexoid), sp->arms.n,
		 cp->path.startup_cost, cp->path.total_cost, rows);

	/* a walk of two indexes and descents of a third, as index scans are */
	lion_path_core_switches(&cp->path, LION_CORE_INDEXSCAN);
	add_path(joinrel, &cp->path);
}

/*
 * Is the ORDER BY of the join j one a split answers (DESIGN.md §48.1): its
 * first sort key a column of the dimension whose lion index walks it in that
 * order (lion_ordered_column_order(), §30.11's rule - a plain column, in its
 * type's btree order under its own collation), at most
 * pg_lion.ordered_split_max arms expected of it (os_arms()), and more sort
 * keys after it?  *sp is the split.
 */
static bool
os_split_of(PlannerInfo *root, OjJoin *j, OsSplit *sp)
{
	memset(sp, 0, sizeof(OsSplit));
	if (lion_ordered_split_max <= 0 || list_length(root->query_pathkeys) < 2)
		return false;
	sp->pathkey = (PathKey *) linitial(root->query_pathkeys);
	if (!bms_is_subset(j->dim->relids, sp->pathkey->pk_eclass->ec_relids))
		return false;			/* no column of the dimension's */
	sp->index = lion_ordered_column_order(root, j->dim, sp->pathkey,
										  &sp->indexcol, &sp->col);
	if (sp->index == NULL)
		return false;
#if PG_VERSION_NUM >= 180000
	sp->backward = (sp->pathkey->pk_cmptype == COMPARE_GT);
#else
	sp->backward = (sp->pathkey->pk_strategy == BTGreaterStrategyNumber);
#endif
	sp->nulls = os_nulls(j->dim, sp->col, sp->pathkey);
	return os_arms(root, sp->col, sp->backward, sp->nulls, &sp->arms);
}

/*
 * The paths of the split sp of the join j (DESIGN.md §48.1): for each order
 * of the fact's btrees the rest of the ORDER BY begins with
 * (lion_ordered_orders_for(): core builds none for an order the query's does
 * not lead with), each of the fact's lion accesses and each of the
 * dimension's, one path.  What the ORDER BY has past the fact's order is
 * core's to sort, an Incremental Sort above the split.  The competitors are
 * found before the first path is added (oj_competitor()).
 */
static void
os_add_paths(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j, OsSplit *sp,
			 List *factcands, List *dimcands)
{
	List	   *rest = list_copy_tail(root->query_pathkeys, 1);
	List	   *ords = lion_ordered_orders_for(root, j->fact, rest);
	List	   *keys = NIL;
	List	   *joinkeys = NIL;
	OjCompetitor *comps;
	ListCell   *lc;
	ListCell   *lck;
	ListCell   *lcj;

	if (ords == NIL)
		return;
	comps = (OjCompetitor *) palloc0(sizeof(OjCompetitor) *
									 list_length(ords));
	foreach(lc, ords)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *jk = NIL;
		List	   *pk = NIL;
		int			ncommon = 0;
		ListCell   *lc2;

		(void) pathkeys_count_contained_in(rest, ord->path.pathkeys,
										   &ncommon);
		foreach(lc2, ord->path.pathkeys)
		{
			if (((PathKey *) lfirst(lc2))->pk_eclass == sp->pathkey->pk_eclass)
				ncommon = 0;	/* the column's class: no order of the fact */
		}
		if (ncommon > 0)
		{
			jk = list_copy_head(ord->path.pathkeys, ncommon);
			pk = lcons(sp->pathkey, list_copy(jk));
			oj_competitor(root, joinrel, j->fact, pk,
						  &comps[foreach_current_index(lc)]);
		}
		joinkeys = lappend(joinkeys, jk);
		keys = lappend(keys, pk);
	}

	forthree(lc, ords, lck, keys, lcj, joinkeys)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *pathkeys = (List *) lfirst(lck);
		ListCell   *lc2;
		ListCell   *lc3;

		if (pathkeys == NIL)
			continue;
		foreach(lc2, factcands)
		{
			foreach(lc3, dimcands)
				os_add_path(root, joinrel, j, sp, ord, (List *) lfirst(lcj),
							pathkeys, &comps[foreach_current_index(lc)],
							(Path *) lfirst(lc2), (Path *) lfirst(lc3));
		}
	}
}

/*
 * The paths of a join j recognized in joinrel: one for each ordered btree of
 * the fact whose order the query wants, with each of the fact's lion accesses
 * and each of the dimension's - every pair is priced and add_path() keeps
 * what it keeps.  A side without a lion access is walked or probed with no
 * set, every row it meets filtered; one side has a set, or a key set
 * filters.  An ORDER BY that leads with a column of the dimension is the
 * split's (os_add_paths(), §48), whose arms narrow the dimension by the
 * column's entries, a set of their own; where a column of the fact is in the
 * same class, both are offered and add_path() keeps what it keeps.
 */
static void
oj_add_paths(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j)
{
	List	   *ords;
	List	   *factcands;
	List	   *dimcands;
	List	   *keys = NIL;
	OjCompetitor *comps;
	OsSplit		sp;
	bool		split;
	bool		anyset;
	ListCell   *lc;
	ListCell   *lck;

	if (!bms_is_empty(joinrel->lateral_relids) ||
		!oj_target_ok(joinrel, bms_union(j->fact->relids, j->dim->relids)))
		return;
	ords = lion_ordered_orders(j->fact);
	split = os_split_of(root, j, &sp);
	if (ords == NIL && !split)
		return;
	factcands = lion_ordered_accesses(root, j->fact);
	dimcands = lion_ordered_accesses(root, j->dim);
	anyset = (factcands != NIL || dimcands != NIL || j->keysets != NIL);
	if (factcands == NIL)
		factcands = list_make1(NULL);
	if (dimcands == NIL)
		dimcands = list_make1(NULL);

	if (split)
		os_add_paths(root, joinrel, j, &sp, factcands, dimcands);
	if (ords == NIL || !anyset)
		return;

	/*
	 * Each order the query wants, as far as it wants it, and the core plan
	 * it competes with, before the first path is added (oj_competitor())
	 */
	comps = (OjCompetitor *) palloc0(sizeof(OjCompetitor) *
									 Max(list_length(ords), 1));
	foreach(lc, ords)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *pathkeys = build_join_pathkeys(root, joinrel, JOIN_INNER,
												   ord->path.pathkeys);
		int			ncommon = 0;

		if (pathkeys != NIL)
			(void) pathkeys_count_contained_in(root->query_pathkeys, pathkeys,
											   &ncommon);
		if (ncommon == 0)
			pathkeys = NIL;
		else
			oj_competitor(root, joinrel, j->fact, pathkeys,
						  &comps[foreach_current_index(lc)]);
		keys = lappend(keys, pathkeys);
	}

	forboth(lc, ords, lck, keys)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *pathkeys = (List *) lfirst(lck);
		ListCell   *lc2;
		ListCell   *lc3;

		if (pathkeys == NIL)
			continue;
		foreach(lc2, factcands)
		{
			foreach(lc3, dimcands)
				oj_add_path(root, joinrel, j, ord, pathkeys,
							&comps[foreach_current_index(lc)],
							(Path *) lfirst(lc2), (Path *) lfirst(lc3));
		}
	}
}

/*
 * set_join_pathlist_hook: LionOrderedJoin's paths (DESIGN.md §47.2).
 *
 * The join of two tables, outerrel the fact and innerrel the dimension,
 * inner-joined by their clauses - the other orientation is the hook's other
 * call.  Or the join of outerrel, the fact and the dimension with key sets
 * folded already, and innerrel, a table that only filters the key: taken
 * apart whole (oj_keyset_of()), in the one call where innerrel is the key
 * set's table of the highest relid, so that each join rel is offered its
 * paths once.
 */
static void
oj_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 JoinType jointype, JoinPathExtraData *extra)
{
	Query	   *parse = root->parse;
	OjJoin		j;
	RelOptInfo *rels[OJ_MAX_RELS];
	int			nrels = 0;
	int			a;
	int			b;
	int			relid;

	if (oj_prev_set_join_pathlist_hook != NULL)
		oj_prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
									   jointype, extra);

	if (!lion_enable_ordered_join || root->query_pathkeys == NIL)
		return;

	/* a plain query's join, nothing locked or written */
	if (parse->commandType != CMD_SELECT || parse->rowMarks != NIL ||
		root->rowMarks != NIL || extra->sjinfo == NULL)
		return;
	if (innerrel->reloptkind != RELOPT_BASEREL)
		return;

	if (outerrel->reloptkind == RELOPT_BASEREL)
	{
		/* the fact and the dimension, inner-joined */
		if (jointype != JOIN_INNER || extra->sjinfo->jointype != JOIN_INNER ||
			!bms_equal(joinrel->relids,
					   bms_union(outerrel->relids, innerrel->relids)))
			return;
		if (oj_key_join(root, outerrel, innerrel, extra->restrictlist, &j))
			oj_add_paths(root, joinrel, &j);
		return;
	}

	/* a key set's table, inner-joined or the inner side of a semi join */
	if (!((jointype == JOIN_INNER && extra->sjinfo->jointype == JOIN_INNER) ||
		  (jointype == JOIN_SEMI && extra->sjinfo->jointype == JOIN_SEMI)))
		return;
	if (!bms_equal(joinrel->relids,
				   bms_union(outerrel->relids, innerrel->relids)))
		return;
	relid = -1;
	while ((relid = bms_next_member(outerrel->relids, relid)) >= 0)
	{
		RelOptInfo *rel = (relid < root->simple_rel_array_size) ?
			root->simple_rel_array[relid] : NULL;

		/* base relations alone: an outer join's relid has none */
		if (rel == NULL || rel->reloptkind != RELOPT_BASEREL ||
			nrels == OJ_MAX_RELS - 1)
			return;
		rels[nrels++] = rel;
	}

	/* every way of taking two tables of outerrel as the fact and dimension */
	for (a = 0; a < nrels; a++)
	{
		for (b = 0; b < nrels; b++)
		{
			List	   *clauses;
			int			i;
			bool		ok = true;

			if (a == b)
				continue;
			clauses = (rels[a]->relid < rels[b]->relid) ?
				lion_fkjoin_pair_clauses(root, rels[a], rels[b]) :
				lion_fkjoin_pair_clauses(root, rels[b], rels[a]);
			if (!oj_key_join(root, rels[a], rels[b], clauses, &j))
				continue;
			for (i = 0; i < nrels && ok; i++)
			{
				if (i != a && i != b)
					ok = rels[i]->relid < innerrel->relid &&
						oj_keyset_of(root, joinrel, &j, rels[i]);
			}
			if (ok && oj_keyset_of(root, joinrel, &j, innerrel) &&
				oj_special_ok(root, joinrel, &j))
				oj_add_paths(root, joinrel, &j);
		}
	}
}

/*
 * PlanCustomPath: the scan slot is the fact's row and then the dimension's,
 * as the sides' plans return them - each its relation's target, in that
 * target's order - and the join's target list is evaluated over it; then each
 * key set's key, for EXPLAIN.
 */
static Plan *
oj_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Var		   *fkvar = (Var *) linitial(best_path->custom_private);
	List	   *index = (List *) lsecond(best_path->custom_private);
	List	   *keyinfo = (List *) lthird(best_path->custom_private);
	List	   *sctlist = NIL;
	List	   *keyops = NIL;
	List	   *keycolls = NIL;
	List	   *keycols = NIL;
	AttrNumber	resno = 1;
	int			nfact = 0;
	int			fkresno = 0;
	ListCell   *lc;
	ListCell   *lck;
	int			side;

	if (list_length(custom_plans) != 2 + list_length(keyinfo) ||
		clauses != NIL)
		elog(ERROR, "LionOrderedJoin: unexpected plan shape");
	for (side = 0; side < 2; side++)
	{
		Plan	   *child = (Plan *) list_nth(custom_plans, side);

		foreach(lc, child->targetlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			Var		   *var = (Var *) tle->expr;

			if (side == 0 && IsA(var, Var) && var->varno == fkvar->varno &&
				var->varattno == fkvar->varattno)
				fkresno = resno;
			sctlist = lappend(sctlist,
							  makeTargetEntry((Expr *) copyObject(tle->expr),
											  resno++, NULL, false));
		}
		if (side == 0)
			nfact = list_length(sctlist);
	}
	if (fkresno == 0)
		elog(ERROR, "LionOrderedJoin: the fact's key is not in its target");

	/* each key set: its key's position in its plan's target */
	foreach(lck, keyinfo)
	{
		List	   *info = (List *) lfirst(lck);
		Var		   *key = (Var *) linitial(info);
		Plan	   *child = (Plan *) list_nth(custom_plans,
											  2 + foreach_current_index(lck));
		int			col = 0;
		int			i = 1;
		ListCell   *lct;

		foreach(lct, child->targetlist)
		{
			Var		   *var = (Var *) lfirst_node(TargetEntry, lct)->expr;

			if (IsA(var, Var) && var->varno == key->varno &&
				var->varattno == key->varattno)
			{
				col = i;
				break;
			}
			i++;
		}
		if (col == 0)
			elog(ERROR, "LionOrderedJoin: a key set's key is not in its plan's target");
		keycols = lappend_int(keycols, col);
		keyops = lappend_oid(keyops, linitial_oid((List *) lsecond(info)));
		keycolls = lappend_oid(keycolls, linitial_oid((List *) lthird(info)));
		sctlist = lappend(sctlist,
						  makeTargetEntry((Expr *) copyObject(key), resno++,
										  NULL, true));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_scan_tlist = sctlist;
	cscan->custom_exprs = NIL;
	cscan->custom_private = list_make5(list_make4_int(OJ_PRIV_MAGIC, nfact,
													  fkresno,
													  list_length(keyinfo)),
									   list_copy(index), keyops, keycolls,
									   keycols);
	cscan->methods = &oj_scan_methods;
	return &cscan->scan.plan;
}

/* ---------------------------------------------------------------------
 * Key sets (DESIGN.md §47.6): a hash table of a key type's values
 * --------------------------------------------------------------------- */

typedef struct OjKeyFuncs
{
	FmgrInfo	hash;
	FmgrInfo	eq;
	Oid			collation;
} OjKeyFuncs;

typedef struct OjKeyEnt
{
	Datum		key;
	uint32		hash;
	char		status;
} OjKeyEnt;

static inline uint32
oj_key_hash(OjKeyFuncs *f, Datum key)
{
	return DatumGetUInt32(FunctionCall1Coll(&f->hash, f->collation, key));
}

static inline bool
oj_key_equal(OjKeyFuncs *f, Datum a, Datum b)
{
	return DatumGetBool(FunctionCall2Coll(&f->eq, f->collation, a, b));
}

#define SH_PREFIX		ojkeys
#define SH_ELEMENT_TYPE OjKeyEnt
#define SH_KEY_TYPE		Datum
#define SH_KEY			key
#define SH_HASH_KEY(tb, k) oj_key_hash((OjKeyFuncs *) (tb)->private_data, k)
#define SH_EQUAL(tb, a, b) oj_key_equal((OjKeyFuncs *) (tb)->private_data, a, b)
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) ((a)->hash)
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

typedef struct OjKeySetState
{
	PlanState  *child;			/* the plan the set is built from */
	AttrNumber	col;			/* the key in its target */
	OjKeyFuncs	funcs;
	bool		byval;
	int16		typlen;
	MemoryContext cxt;			/* the set; reset per build */
	ojkeys_hash *keys;			/* NULL: not built for this scan */
	uint64		nkeys;			/* EXPLAIN ANALYZE: the last build's keys */
	uint64		builds;
} OjKeySetState;

/* ---------------------------------------------------------------------
 * Executor (DESIGN.md §47.4)
 * --------------------------------------------------------------------- */

typedef struct OjState
{
	CustomScanState css;
	PlanState  *fact;			/* the fact's walk */
	PlanState  *dim;			/* the dimension's probe */
	int			nfact;			/* the fact's columns in the scan slot */
	int			ndim;			/* ... the dimension's, after them */
	AttrNumber	fkresno;		/* the key among the fact's */
	Oid			pkindex;		/* the dimension's unique btree */
	int			nkeysets;
	OjKeySetState *keysets;
	uint64		rejected;		/* EXPLAIN ANALYZE: keys no key set holds */
} OjState;

static Node *
oj_create_state(CustomScan *cscan)
{
	OjState    *st = (OjState *) newNode(sizeof(OjState), T_CustomScanState);

	st->css.methods = &oj_exec_methods;
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

static void
oj_begin(CustomScanState *node, EState *estate, int eflags)
{
	OjState    *st = (OjState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *ints;
	List	   *index;
	List	   *keyops;
	List	   *keycolls;
	List	   *keycols;
	int			ncols = list_length(cscan->custom_scan_tlist);
	int			i;

	if (list_length(cscan->custom_private) != OJ_PRIV_NMEMBERS)
		elog(ERROR, "LionOrderedJoin: unrecognized custom_private shape");
	ints = (List *) list_nth(cscan->custom_private, OJ_PRIV_INTS);
	index = (List *) list_nth(cscan->custom_private, OJ_PRIV_INDEX);
	keyops = (List *) list_nth(cscan->custom_private, OJ_PRIV_KEYOPS);
	keycolls = (List *) list_nth(cscan->custom_private, OJ_PRIV_KEYCOLL);
	keycols = (List *) list_nth(cscan->custom_private, OJ_PRIV_KEYCOLS);
	if (ints == NIL || !IsA(ints, IntList) || list_length(ints) != 4 ||
		linitial_int(ints) != OJ_PRIV_MAGIC ||
		index == NIL || !IsA(index, OidList) || list_length(index) != 1)
		elog(ERROR, "LionOrderedJoin: unrecognized custom_private shape");
	st->nfact = lsecond_int(ints);
	st->fkresno = (AttrNumber) lthird_int(ints);
	st->nkeysets = lfourth_int(ints);
	st->pkindex = linitial_oid(index);
	st->ndim = ncols - st->nfact - st->nkeysets;
	if (list_length(cscan->custom_plans) != 2 + st->nkeysets ||
		st->nfact < 1 || st->ndim < 0 || st->nkeysets < 0 ||
		st->fkresno < 1 || st->fkresno > st->nfact ||
		list_length(((Plan *) linitial(cscan->custom_plans))->targetlist) !=
		st->nfact ||
		list_length(((Plan *) lsecond(cscan->custom_plans))->targetlist) !=
		st->ndim ||
		(keyops != NIL && !IsA(keyops, OidList)) ||
		(keycolls != NIL && !IsA(keycolls, OidList)) ||
		(keycols != NIL && !IsA(keycols, IntList)) ||
		list_length(keyops) != st->nkeysets ||
		list_length(keycolls) != st->nkeysets ||
		list_length(keycols) != st->nkeysets)
		elog(ERROR, "LionOrderedJoin: malformed plan");

	/*
	 * The sides, run by the join alone: neither scans backward nor marks.
	 * They are not among custom_ps, which EXPLAIN would show as nodes of
	 * their own; the join shows their lines in its own.  The key sets' plans
	 * are core's, and EXPLAIN shows them under the join.
	 */
	eflags &= ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);
	st->fact = ExecInitNode((Plan *) linitial(cscan->custom_plans), estate,
							eflags);
	st->dim = ExecInitNode((Plan *) lsecond(cscan->custom_plans), estate,
						   eflags);
	st->keysets = (OjKeySetState *)
		palloc0(sizeof(OjKeySetState) * Max(st->nkeysets, 1));
	for (i = 0; i < st->nkeysets; i++)
	{
		OjKeySetState *ks = &st->keysets[i];
		Plan	   *plan = (Plan *) list_nth(cscan->custom_plans, 2 + i);
		Oid			eqop = list_nth_oid(keyops, i);
		RegProcedure lhash;
		RegProcedure rhash;
		TargetEntry *tle;
		Oid			type;

		ks->child = ExecInitNode(plan, estate, eflags);
		node->custom_ps = lappend(node->custom_ps, ks->child);
		ks->col = (AttrNumber) list_nth_int(keycols, i);
		if (ks->col < 1 || ks->col > list_length(plan->targetlist))
			elog(ERROR, "LionOrderedJoin: malformed key set");
		tle = (TargetEntry *) list_nth(plan->targetlist, ks->col - 1);
		type = exprType((Node *) tle->expr);
		if (!get_op_hash_functions(eqop, &lhash, &rhash) || lhash != rhash)
			elog(ERROR, "LionOrderedJoin: the key set's equality %u is not hashable",
				 eqop);
		fmgr_info(lhash, &ks->funcs.hash);
		fmgr_info(get_opcode(eqop), &ks->funcs.eq);
		ks->funcs.collation = list_nth_oid(keycolls, i);
		get_typlenbyval(type, &ks->typlen, &ks->byval);
		ks->cxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionOrderedJoin key set",
										ALLOCSET_DEFAULT_SIZES);
	}
}

/*
 * Build a key set from its plan's rows: each non-NULL key once, its value
 * copied into the set's memory.  A NULL key matches nothing.  The key type's
 * hash and equality run in econtext's per-tuple memory, reset a row, as a
 * hash join's do (ExecHashGetHashValue()): a type's own functions may leave
 * what they allocate behind.  The table and its keys are the set's.
 */
static void
oj_keyset_build(OjKeySetState *ks, ExprContext *econtext)
{
	MemoryContextReset(ks->cxt);
	ks->keys = ojkeys_create(ks->cxt, 256, &ks->funcs);
	ks->nkeys = 0;
	for (;;)
	{
		TupleTableSlot *slot;
		MemoryContext oldcxt;
		Datum		key;
		bool		isnull;
		bool		found;
		OjKeyEnt   *ent;

		CHECK_FOR_INTERRUPTS();
		ResetExprContext(econtext);
		slot = ExecProcNode(ks->child);
		if (TupIsNull(slot))
			break;
		key = slot_getattr(slot, ks->col, &isnull);
		if (isnull)
			continue;
		oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
		ent = ojkeys_insert(ks->keys, key, &found);
		if (!found)
		{
			MemoryContextSwitchTo(ks->cxt);
			ent->key = datumCopy(key, ks->byval, ks->typlen);
			ks->nkeys++;
		}
		MemoryContextSwitchTo(oldcxt);
	}
	ks->builds++;
}

/*
 * The next joined row: the fact's walk's next row whose key is not NULL, is
 * in every key set and names a dimension row that is a member of its set,
 * visible and passes its filter.  The key is unique, so one descent finds
 * that row or none.  The scan slot's values are the two sides' slots', which
 * neither touches until the join asks it for its next row.
 */
static TupleTableSlot *
oj_exec(CustomScanState *node)
{
	OjState    *st = (OjState *) node;
	TupleTableSlot *scanslot = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			i;

	/* the key sets, before the walk: one that is empty ends the scan */
	for (i = 0; i < st->nkeysets; i++)
	{
		if (st->keysets[i].keys == NULL)
			oj_keyset_build(&st->keysets[i], econtext);
		if (st->keysets[i].nkeys == 0)
			return NULL;
	}

	for (;;)
	{
		TupleTableSlot *fslot;
		TupleTableSlot *dslot;
		MemoryContext oldcxt;
		Datum		key;
		bool		isnull;

		CHECK_FOR_INTERRUPTS();

		/*
		 * The row this returned last has been read by now, and its
		 * projection with it; the key sets' hash and equality run in the
		 * memory reset here, as oj_keyset_build()'s do.
		 */
		ResetExprContext(econtext);
		ExecClearTuple(scanslot);
		fslot = ExecProcNode(st->fact);
		if (TupIsNull(fslot))
			return NULL;
		key = slot_getattr(fslot, st->fkresno, &isnull);
		if (isnull)
			continue;			/* the clause is strict: no row matches */
		oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
		for (i = 0; i < st->nkeysets; i++)
		{
			if (ojkeys_lookup(st->keysets[i].keys, key) == NULL)
				break;
		}
		MemoryContextSwitchTo(oldcxt);
		if (i < st->nkeysets)
		{
			st->rejected++;
			continue;
		}

		lion_ordered_probe_key(st->dim, key);
		dslot = ExecProcNode(st->dim);
		if (TupIsNull(dslot))
			continue;

		slot_getsomeattrs(fslot, st->nfact);
		memcpy(scanslot->tts_values, fslot->tts_values,
			   sizeof(Datum) * st->nfact);
		memcpy(scanslot->tts_isnull, fslot->tts_isnull,
			   sizeof(bool) * st->nfact);
		if (st->ndim > 0)
		{
			slot_getsomeattrs(dslot, st->ndim);
			memcpy(scanslot->tts_values + st->nfact, dslot->tts_values,
				   sizeof(Datum) * st->ndim);
			memcpy(scanslot->tts_isnull + st->nfact, dslot->tts_isnull,
				   sizeof(bool) * st->ndim);
		}
		/* the key sets' keys, which no row holds */
		for (i = st->nfact + st->ndim;
			 i < scanslot->tts_tupleDescriptor->natts; i++)
		{
			scanslot->tts_values[i] = (Datum) 0;
			scanslot->tts_isnull[i] = true;
		}
		ExecStoreVirtualTuple(scanslot);

		if (node->ss.ps.ps_ProjInfo == NULL)
			return scanslot;
		econtext->ecxt_scantuple = scanslot;
		return ExecProject(node->ss.ps.ps_ProjInfo);
	}
}

/*
 * The walk restarts; the dimension is rescanned at its next descent anyway.
 * A changed Param reaches each side, whose rescan rebuilds its set only when
 * its lion quals use the Param (DESIGN.md §30.4), and each key set's plan,
 * whose set is rebuilt only when the plan depends on it.
 */
static void
oj_rescan(CustomScanState *node)
{
	OjState    *st = (OjState *) node;
	int			i;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (node->ss.ps.chgParam != NULL)
	{
		UpdateChangedParamSet(st->fact, node->ss.ps.chgParam);
		UpdateChangedParamSet(st->dim, node->ss.ps.chgParam);
		for (i = 0; i < st->nkeysets; i++)
			UpdateChangedParamSet(st->keysets[i].child, node->ss.ps.chgParam);
	}
	if (st->fact->chgParam == NULL)
		ExecReScan(st->fact);
	for (i = 0; i < st->nkeysets; i++)
	{
		/* rescanned by its next ExecProcNode(), as the set is rebuilt */
		if (st->keysets[i].child->chgParam != NULL)
			st->keysets[i].keys = NULL;
	}
}

static void
oj_end(CustomScanState *node)
{
	OjState    *st = (OjState *) node;
	int			i;

	if (st->fact != NULL)
		ExecEndNode(st->fact);
	if (st->dim != NULL)
		ExecEndNode(st->dim);
	for (i = 0; i < st->nkeysets; i++)
	{
		if (st->keysets[i].child != NULL)
			ExecEndNode(st->keysets[i].child);
		st->keysets[i].child = NULL;
		st->keysets[i].keys = NULL;
	}
	st->fact = NULL;
	st->dim = NULL;
}

/* A relation as EXPLAIN names it, its alias after it when that differs */
static void
oj_explain_rel(StringInfo buf, Index rti, ExplainState *es)
{
	RangeTblEntry *rte = rt_fetch(rti, es->rtable);
	char	   *refname = (char *) list_nth(es->rtable_names, rti - 1);
	char	   *relname = get_rel_name(rte->relid);

	appendStringInfoString(buf, quote_identifier(relname != NULL ?
												 relname : "???"));
	if (refname != NULL && (relname == NULL || strcmp(refname, relname) != 0))
		appendStringInfo(buf, " %s", quote_identifier(refname));
}

/*
 * EXPLAIN (DESIGN.md §47.5): the fact's walk's lines, as LionOrdered prints
 * them; the dimension, by its relation and its unique btree, and its probe's
 * lines behind "Dimension "; the key sets' tables; and under ANALYZE the
 * counters of each.
 */
static void
oj_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	OjState    *st = (OjState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	/*
	 * The sides' expressions are deparsed with the join as their parent, in a
	 * copy of the ancestors: lcons() extends the list in place, and the
	 * caller's list is the one the nodes after this one are deparsed in.  A
	 * nested loop's inner side above the join finds its Params' outer Vars
	 * through it, and with the join left at its head it found none: "$0",
	 * and an assertion failure in an assert-enabled build.
	 */
	List	   *anc = lcons(node->ss.ps.plan, list_copy(ancestors));
	Scan	   *dscan = (Scan *) st->dim->plan;
	char	   *idxname = get_rel_name(st->pkindex);
	StringInfoData buf;
	int			i;

	lion_ordered_explain_side(st->fact, anc, es, NULL, false);

	initStringInfo(&buf);
	oj_explain_rel(&buf, dscan->scanrelid, es);
	appendStringInfo(&buf, " via %s",
					 quote_identifier(idxname != NULL ? idxname : "???"));
	ExplainPropertyText("Dimension", buf.data, es);
	lion_ordered_explain_side(st->dim, anc, es, "Dimension ", false);

	if (st->nkeysets > 0)
	{
		resetStringInfo(&buf);
		for (i = 0; i < st->nkeysets; i++)
		{
			TargetEntry *tle = (TargetEntry *)
				list_nth(cscan->custom_scan_tlist, st->nfact + st->ndim + i);
			Var		   *key = (Var *) tle->expr;

			if (i > 0)
				appendStringInfoString(&buf, ", ");
			if (IsA(key, Var))
				oj_explain_rel(&buf, key->varno, es);
			else
				appendStringInfoString(&buf, "???");
		}
		ExplainPropertyText("Key Sets", buf.data, es);
	}

	if (es->analyze)
	{
		lion_ordered_explain_side(st->fact, anc, es, NULL, true);
		if (st->nkeysets > 0)
		{
			uint64		nkeys = 0;
			uint64		builds = 0;

			for (i = 0; i < st->nkeysets; i++)
			{
				nkeys += st->keysets[i].nkeys;
				builds += st->keysets[i].builds;
			}
			ExplainPropertyInteger("Key Set Keys", NULL, (int64) nkeys, es);
			ExplainPropertyInteger("Key Set Builds", NULL, (int64) builds, es);
			ExplainPropertyInteger("Keys Rejected by Key Sets", NULL,
								   (int64) st->rejected, es);
		}
		lion_ordered_explain_side(st->dim, anc, es, "Dimension ", true);
	}
	pfree(buf.data);
}

/* ---------------------------------------------------------------------
 * LionOrderedSplit's plan and executor (DESIGN.md §48.2, §48.4)
 * --------------------------------------------------------------------- */

/*
 * PlanCustomPath: the scan slot is the row the arm's LionOrderedJoin returns,
 * its target list's, and the split's target list is evaluated over it.
 */
static Plan *
os_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *child;
	List	   *sctlist = NIL;
	AttrNumber	resno = 1;
	ListCell   *lc;

	if (list_length(custom_plans) != 1 || clauses != NIL ||
		list_length(best_path->custom_private) != OS_PRIV_NMEMBERS)
		elog(ERROR, "LionOrderedSplit: unexpected plan shape");
	child = (Plan *) linitial(custom_plans);
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		sctlist = lappend(sctlist,
						  makeTargetEntry((Expr *) copyObject(tle->expr),
										  resno++, NULL, false));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_scan_tlist = sctlist;
	cscan->custom_exprs = NIL;
	cscan->custom_private = copyObject(best_path->custom_private);
	cscan->methods = &os_scan_methods;
	return &cscan->scan.plan;
}

typedef struct OsState
{
	CustomScanState css;
	PlanState  *child;			/* the arms' LionOrderedJoin */
	PlanState  *probe;			/* ... its dimension, narrowed per arm */
	Oid			colindex;		/* the column's lion index */
	AttrNumber	colattno;		/* ... the column, as the index numbers it */
	bool		backward;
	int			nulls;			/* LION_ORDER_NULLS_* */
	int			cap;			/* pg_lion.ordered_split_max, planned at */
	Relation	colrel;
	MemoryContext walkcxt;		/* the column's walk; reset per scan */
	LionOrderWalk *walk;		/* NULL: not begun in this scan */
	ItemPointerData next;		/* the TID the walk handed out past the arm */
	bool		hasnext;
	bool		inarm;			/* the child is running an arm */
	bool		childran;		/* ... has run since it was last rescanned */
	uint64		arms;			/* EXPLAIN ANALYZE: arms run */
	uint64		skipped;		/* ... entries with no row of the set */
} OsState;

static Node *
os_create_state(CustomScan *cscan)
{
	OsState    *st = (OsState *) newNode(sizeof(OsState), T_CustomScanState);

	st->css.methods = &os_exec_methods;
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

static void
os_begin(CustomScanState *node, EState *estate, int eflags)
{
	OsState    *st = (OsState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *ints;
	List	   *index;

	if (list_length(cscan->custom_private) != OS_PRIV_NMEMBERS ||
		list_length(cscan->custom_plans) != 1)
		elog(ERROR, "LionOrderedSplit: unrecognized plan shape");
	ints = (List *) list_nth(cscan->custom_private, OS_PRIV_INTS);
	index = (List *) list_nth(cscan->custom_private, OS_PRIV_INDEX);
	if (ints == NIL || !IsA(ints, IntList) || list_length(ints) != 5 ||
		linitial_int(ints) != OS_PRIV_MAGIC ||
		index == NIL || !IsA(index, OidList) || list_length(index) != 1)
		elog(ERROR, "LionOrderedSplit: unrecognized custom_private shape");
	st->colattno = (AttrNumber) lsecond_int(ints);
	st->backward = lthird_int(ints) != 0;
	st->nulls = lfourth_int(ints);
	st->cap = list_nth_int(ints, 4);
	st->colindex = linitial_oid(index);
	if (st->colattno < 1 ||
		(st->nulls != LION_ORDER_NULLS_NONE &&
		 st->nulls != LION_ORDER_NULLS_FIRST &&
		 st->nulls != LION_ORDER_NULLS_LAST))
		elog(ERROR, "LionOrderedSplit: malformed plan");

	/*
	 * The arms' join, run by the split alone and rescanned for each arm: it
	 * scans neither backward nor marks.  EXPLAIN shows it beneath the split.
	 */
	eflags &= ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);
	st->child = ExecInitNode((Plan *) linitial(cscan->custom_plans), estate,
							 eflags);
	node->custom_ps = list_make1(st->child);
	if (!IsA(st->child, CustomScanState) ||
		((CustomScanState *) st->child)->methods != &oj_exec_methods)
		elog(ERROR, "LionOrderedSplit: its arms are not a LionOrderedJoin");
	st->probe = ((OjState *) st->child)->dim;

	st->colrel = index_open(st->colindex, AccessShareLock);
	st->walkcxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionOrderedSplit walk",
										ALLOCSET_DEFAULT_SIZES);
}

/*
 * The next arm, its child rescanned and its dimension narrowed: the column's
 * next entry - in the walk's order, its NULL entry where the ORDER BY puts
 * NULLs (DESIGN.md §30.11) - that has a row of the dimension's set.  An entry
 * with none is passed over without running its arm.  false past the last.
 *
 * The child is rescanned BEFORE the entry is narrowed, when it has run or a
 * Param of the split's has changed: its rescan hands the Param to the probe
 * (oj_rescan()), and lion_ordered_narrow_begin() then rescans the probe,
 * which makes the dimension's set again for the new value, once, before the
 * entry is ANDed with it; the child's descents find the probe's Param
 * consumed and the set current.  Rescanned after the narrowing, the child
 * would hand the Param on again, and the probe's first descent would drop
 * and build the set a second time.  An entry passed over costs a rescan of
 * the child at most once, and that rescan reads nothing.
 */
static bool
os_next_arm(OsState *st)
{
	if (st->walk == NULL)
	{
		st->walk = lion_order_walk_begin(st->colrel, st->colattno, NULL, 0,
										 st->backward, st->nulls,
										 st->walkcxt);
		pgstat_count_index_scan(st->colrel);
		st->hasnext = lion_order_walk_next(st->walk, &st->next);
	}
	while (st->hasnext)
	{
		int64		entry = lion_order_walk_entryno(st->walk);

		CHECK_FOR_INTERRUPTS();
		if (st->childran || st->child->chgParam != NULL)
		{
			ExecReScan(st->child);
			st->childran = false;
		}
		lion_ordered_narrow_begin(st->probe);
		do
		{
			lion_ordered_narrow_add(st->probe, &st->next);
			st->hasnext = lion_order_walk_next(st->walk, &st->next);
		} while (st->hasnext && lion_order_walk_entryno(st->walk) == entry);
		if (lion_ordered_narrow_end(st->probe) == 0)
		{
			st->skipped++;
			continue;
		}
		st->childran = true;
		st->arms++;
		return true;
	}
	return false;
}

/*
 * The next row: the arm's next, in the fact's walk's order, and past an
 * arm's last the next arm's first.  The scan slot's values are the child's,
 * which it does not touch until it is asked for its next row.
 */
static TupleTableSlot *
os_exec(CustomScanState *node)
{
	OsState    *st = (OsState *) node;
	TupleTableSlot *scanslot = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			natts = scanslot->tts_tupleDescriptor->natts;

	for (;;)
	{
		TupleTableSlot *slot;

		if (!st->inarm)
		{
			if (!os_next_arm(st))
				return NULL;
			st->inarm = true;
		}
		ResetExprContext(econtext);
		ExecClearTuple(scanslot);
		slot = ExecProcNode(st->child);
		if (TupIsNull(slot))
		{
			st->inarm = false;
			continue;
		}
		slot_getsomeattrs(slot, natts);
		memcpy(scanslot->tts_values, slot->tts_values, sizeof(Datum) * natts);
		memcpy(scanslot->tts_isnull, slot->tts_isnull, sizeof(bool) * natts);
		ExecStoreVirtualTuple(scanslot);
		if (node->ss.ps.ps_ProjInfo == NULL)
			return scanslot;
		econtext->ecxt_scantuple = scanslot;
		return ExecProject(node->ss.ps.ps_ProjInfo);
	}
}

/*
 * The walk of the column starts again; the child is rescanned at the next
 * arm, before its narrowing (os_next_arm()), a changed Param handed to it
 * first, which its rescan hands to its sides: they keep their sets unless
 * their lion quals use the Param (§47.4).
 */
static void
os_rescan(CustomScanState *node)
{
	OsState    *st = (OsState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (st->walk != NULL)
		lion_order_walk_end(st->walk);
	st->walk = NULL;
	MemoryContextReset(st->walkcxt);
	st->hasnext = false;
	st->inarm = false;
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(st->child, node->ss.ps.chgParam);
}

static void
os_end(CustomScanState *node)
{
	OsState    *st = (OsState *) node;

	if (st->child != NULL)
		ExecEndNode(st->child);
	st->child = NULL;
	if (st->walk != NULL)
		lion_order_walk_end(st->walk);
	st->walk = NULL;
	if (st->colrel != NULL)
		index_close(st->colrel, AccessShareLock);
	st->colrel = NULL;
}

/*
 * EXPLAIN (DESIGN.md §48.5): the column walked, by its index, as LionOrdered
 * prints a lion column's walk - its direction and where its NULL entry goes
 * - and the most arms the planner would split into; under ANALYZE the arms
 * run, the entries passed over, and - where any did - the arms whose
 * narrowed set spilled, with the most of one in the arena.  The arms'
 * LionOrderedJoin is printed beneath, once, its counters summed over the
 * arms and its loops theirs.
 */
static void
os_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	OsState    *st = (OsState *) node;
	char	   *idxname = get_rel_name(st->colindex);
	char	   *colname = get_attname(st->colindex, st->colattno, true);
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s (%s%s%s)",
					 quote_identifier(idxname != NULL ? idxname : "???"),
					 quote_identifier(colname != NULL ? colname : "???"),
					 st->backward ? ", backward" : "",
					 st->nulls == LION_ORDER_NULLS_FIRST ? ", nulls first" :
					 st->nulls == LION_ORDER_NULLS_LAST ? ", nulls last" : "");
	ExplainPropertyText("Split By", buf.data, es);
	ExplainPropertyInteger("Arms Max", NULL, (int64) st->cap, es);
	if (es->analyze)
	{
		uint64		spills = 0;
		Size		spillbytes = 0;

		ExplainPropertyInteger("Arms Run", NULL, (int64) st->arms, es);
		ExplainPropertyInteger("Entries Skipped", NULL, (int64) st->skipped,
							   es);
		if (st->probe != NULL)
			lion_ordered_narrow_spill(st->probe, &spills, &spillbytes);
		if (spills > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%llu, %zu kB",
							 (unsigned long long) spills,
							 (spillbytes + 1023) / 1024);
			ExplainPropertyText("Arm Sets Spilled", buf.data, es);
		}
	}
	pfree(buf.data);
}

/* ---------------------------------------------------------------------
 * Initialisation, from _PG_init (lion_am.c)
 * --------------------------------------------------------------------- */

void
lion_ordjoin_init(void)
{
	DefineCustomBoolVariable("pg_lion.enable_ordered_join",
							 "Offer LionOrderedJoin: an ordered walk of a table's btree across a join to a unique key, both sides filtered by lion sets.",
							 NULL,
							 &lion_enable_ordered_join,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_lion.ordered_split_max",
							"The most arms a LionOrderedSplit is planned for: the distinct values of the dimension's column that leads the ORDER BY, and its NULLs.",
							"0 offers no split.  At run time every entry of the column is an arm, however many.",
							&lion_ordered_split_max,
							8,
							0,
							1024,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	RegisterCustomScanMethods(&oj_scan_methods);
	RegisterCustomScanMethods(&os_scan_methods);

	oj_prev_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = oj_set_join_pathlist;
}
