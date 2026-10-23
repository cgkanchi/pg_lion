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
 * The price of one LionOrderedJoin (DESIGN.md §47.3): the fact's walk as
 * LionOrdered prices it with its set (lion_ordered_walk_price()); a test of
 * each fact row's key, and of each key against the key sets, built before the
 * walk from their tables' plans; for each key left a descent of the
 * dimension's unique btree and a test of the TID against its set; and for
 * each member found a heap fetch, its recheck and its filter; then the join's
 * own target.  Each non-NULL key is taken to name a dimension row - the join
 * is on a foreign key - and a key set to let through the share of them its
 * keys are of the dimension's rows.  *rows is what it returns: the keys' share
 * of the dimension's rows pass its quals (lion_probe_rel_rows()).  *share is
 * the share of the walk the query's LIMIT is expected to take, *probes the
 * descents of the whole walk.
 */
static void
oj_cost(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j, LionOrderedSet *ds,
		Cost factstartup, Cost facttotal, double factrows, double *rows,
		double *probes, double *share, Cost *startup_p, Cost *total_p)
{
	RelOptInfo *dim = j->dim;
	IndexOptInfo *pkidx = j->pkidx;
	double		nonnull;
	double		keys;
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
	ListCell   *lc;

	/* the fact's walk, and a test of each of its rows' keys */
	startup = factstartup;
	run = facttotal - factstartup;
	nonnull = nulltestsel(root, IS_NOT_NULL, (Node *) j->fkvar,
						  j->fact->relid, JOIN_INNER, NULL);
	run += factrows * cpu_operator_cost;
	keys = clamp_row_est(factrows * nonnull);

	/* the key sets: built from their plans, and each key tested in turn */
	descents = keys;
	foreach(lc, j->keysets)
	{
		OjKeySet   *ks = (OjKeySet *) lfirst(lc);

		startup += ks->path->total_cost + ks->rows * cpu_operator_cost;
		run += descents * cpu_operator_cost;
		descents *= Min(1.0, ks->rows / dimtuples);
	}
	descents = clamp_row_est(descents);

	/*
	 * Each key's descent of the dimension's btree, priced as
	 * btcostestimate() prices one: its comparisons, the pages of the
	 * descent, an index tuple; and the leaf pages the descents read, as
	 * index_pages_fetched() spreads them over the index.  Then the set's
	 * test of the TID found.
	 */
	descent = ceil(log(idxtuples) / log(2.0)) * cpu_operator_cost +
		(Max(pkidx->tree_height, 0) + 1) * 50.0 * cpu_operator_cost +
		cpu_index_tuple_cost + cpu_operator_cost;
	get_tablespace_page_costs(pkidx->reltablespace, &spc_random, &spc_seq);
	run += descents * descent +
		index_pages_fetched(descents, (BlockNumber) idxpages, idxpages, root) *
		spc_random;
	if (ds->lion != NULL)
		run += descents * cpu_operator_cost;

	/*
	 * The set's build, and the members' heap fetches - the TIDs of the set
	 * among the keys looked up, each read at random - with their recheck and
	 * filter.
	 */
	startup += ds->build;
	members = clamp_row_est(descents * ds->candsel);
	get_tablespace_page_costs(dim->reltablespace, &spc_random, &spc_seq);
	cost_qual_eval(&resid, ds->residual, root);
	startup += resid.startup;
	run += index_pages_fetched(members, dim->pages, idxpages, root) *
		spc_random +
		members * (cpu_tuple_cost + ds->recheck + resid.per_tuple);

	/* the rows it returns, and the join's target */
	out = clamp_row_est(descents * Min(1.0, lion_probe_rel_rows(root, dim) /
									   dimtuples));
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
	*share = 1.0;
	if (root->limit_tuples > 0)
		*share = Min(1.0, root->limit_tuples / out);
	*startup_p = startup;
	*total_p = startup + run;
}

/*
 * One LionOrderedJoin path (DESIGN.md §47.2): the fact's walk of ord with its
 * set factlion, the dimension's probe with its set dimlion, the key sets,
 * priced and offered to the join rel at pg_lion.pushdown_margin (§39), as
 * LionOrdered's paths are offered to their relation.
 */
static void
oj_add_path(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j,
			IndexPath *ord, List *pathkeys, Path *factlion, Path *dimlion)
{
	CustomPath *cp;
	CustomPath *factpath;
	CustomPath *dimpath;
	IndexPath  *pkpath;
	LionOrderedSet ds;
	List	   *iocols;
	List	   *keyinfo = NIL;
	List	   *paths;
	double		factrows = lion_probe_rel_rows(root, j->fact);
	double		rows;
	double		probes;
	double		share;
	double		margin = lion_units_margin();
	Cost		factstartup;
	Cost		facttotal;
	Cost		startup;
	Cost		total;
	ListCell   *lc;

	/* the fact's walk, index-only where its btree has every column it needs */
	lion_ordered_walk_price(root, j->fact, ord, factlion, factrows, true,
							&iocols, &factstartup, &facttotal);
	lion_ordered_set_info(root, j->dim, dimlion, &ds);
	oj_cost(root, joinrel, j, &ds, factstartup, facttotal, factrows, &rows,
			&probes, &share, &startup, &total);

	factpath = lion_ordered_side_path(j->fact, ord, factlion, iocols, false,
									  share, NULL, factrows, factstartup,
									  facttotal);
	pkpath = create_index_path(root, j->pkidx, NIL, NIL, NIL, NIL,
							   ForwardScanDirection, false, NULL, 1.0, false);
	/* the dimension's weight: the descents the LIMIT's share of the walk makes */
	dimpath = lion_ordered_side_path(j->dim, pkpath, dimlion, NIL, true,
									 clamp_row_est(probes * share),
									 j->keyqual, rows, ds.build, ds.build);
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
	cp->path.startup_cost = startup / margin;
	cp->path.total_cost = total / margin;
	cp->path.pathkeys = pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = paths;
	cp->custom_private = list_make3(j->fkvar,
									list_make1_oid(j->pkidx->indexoid),
									keyinfo);
	cp->methods = &lion_ordjoin_path_methods;

	/* a walk of an index and descents of another, as index scans are */
	lion_path_core_switches(&cp->path, LION_CORE_INDEXSCAN);
	add_path(joinrel, &cp->path);
}

/*
 * The paths of a join j recognized in joinrel: one for each ordered btree of
 * the fact whose order the query wants, with each of the fact's lion accesses
 * and each of the dimension's - every pair is priced and add_path() keeps
 * what it keeps.  A side without a lion access is walked or probed with no
 * set, every row it meets filtered; one side has a set, or a key set
 * filters.
 */
static void
oj_add_paths(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j)
{
	List	   *ords;
	List	   *factcands;
	List	   *dimcands;
	ListCell   *lc;

	if (!bms_is_empty(joinrel->lateral_relids) ||
		!oj_target_ok(joinrel, bms_union(j->fact->relids, j->dim->relids)))
		return;
	ords = lion_ordered_orders(j->fact);
	if (ords == NIL)
		return;
	factcands = lion_ordered_accesses(root, j->fact);
	dimcands = lion_ordered_accesses(root, j->dim);
	if (factcands == NIL && dimcands == NIL && j->keysets == NIL)
		return;
	if (factcands == NIL)
		factcands = list_make1(NULL);
	if (dimcands == NIL)
		dimcands = list_make1(NULL);

	foreach(lc, ords)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *pathkeys = build_join_pathkeys(root, joinrel, JOIN_INNER,
												   ord->path.pathkeys);
		int			ncommon = 0;
		ListCell   *lc2;
		ListCell   *lc3;

		/* the walk's order, as far as the query wants it */
		if (pathkeys == NIL)
			continue;
		(void) pathkeys_count_contained_in(root->query_pathkeys, pathkeys,
										   &ncommon);
		if (ncommon == 0)
			continue;
		foreach(lc2, factcands)
		{
			foreach(lc3, dimcands)
				oj_add_path(root, joinrel, j, ord, pathkeys,
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
 * copied into the set's memory.  A NULL key matches nothing.
 */
static void
oj_keyset_build(OjKeySetState *ks)
{
	MemoryContextReset(ks->cxt);
	ks->keys = ojkeys_create(ks->cxt, 256, &ks->funcs);
	ks->nkeys = 0;
	for (;;)
	{
		TupleTableSlot *slot;
		Datum		key;
		bool		isnull;
		bool		found;
		OjKeyEnt   *ent;

		CHECK_FOR_INTERRUPTS();
		slot = ExecProcNode(ks->child);
		if (TupIsNull(slot))
			break;
		key = slot_getattr(slot, ks->col, &isnull);
		if (isnull)
			continue;
		ent = ojkeys_insert(ks->keys, key, &found);
		if (!found)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(ks->cxt);

			ent->key = datumCopy(key, ks->byval, ks->typlen);
			MemoryContextSwitchTo(oldcxt);
			ks->nkeys++;
		}
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
			oj_keyset_build(&st->keysets[i]);
		if (st->keysets[i].nkeys == 0)
			return NULL;
	}

	ResetExprContext(econtext);
	for (;;)
	{
		TupleTableSlot *fslot;
		TupleTableSlot *dslot;
		Datum		key;
		bool		isnull;

		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(scanslot);
		fslot = ExecProcNode(st->fact);
		if (TupIsNull(fslot))
			return NULL;
		key = slot_getattr(fslot, st->fkresno, &isnull);
		if (isnull)
			continue;			/* the clause is strict: no row matches */
		for (i = 0; i < st->nkeysets; i++)
		{
			if (ojkeys_lookup(st->keysets[i].keys, key) == NULL)
				break;
		}
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

	RegisterCustomScanMethods(&oj_scan_methods);

	oj_prev_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = oj_set_join_pathlist;
}
