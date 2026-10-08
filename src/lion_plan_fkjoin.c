/*-------------------------------------------------------------------------
 *
 * lion_plan_fkjoin.c
 *		Planning the FK-side join (DESIGN.md §27): its aggregate rewrite, its
 *		paths, and the semi and anti join paths.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_plan_private.h"

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
 * May the FK-side join's rows carry the fact's join column as a value - a
 * grouping column, printed (DESIGN.md §27, "Grouped by the join key")?  The
 * node reads no fact row: it emits the dimension's key where the fact's
 * column stands, as it does for count(DISTINCT f.fk) above.  A DISTINCT only
 * compares, so there the key has only to be EQUAL to every fact value it
 * stands for; a GROUP BY prints its column, so here it has to BE each of
 * them, which holds when
 *
 *	- the join is an inner join, or the forward semi join over its distinct
 *	  keys: a semi or anti join's rows are the dimension's, which the query
 *	  above them cannot name the fact's column in;
 *	- the two columns are of one type, so the key's datum is one of the fact
 *	  column's type;
 *	- the join compares them with that type's own equality - the one its
 *	  GROUP BY and DISTINCT compare with - and that equality implies an
 *	  identical representation under the join's collation: §10's
 *	  value-representation rule (lion_type_equalimage()), which the count
 *	  pushdown asks before it prints any key it did not read from a row.
 *	  numeric's 1.0 and 1.00 are equal and print differently, and so are a
 *	  citext key's spellings and a text key's under a case-insensitive
 *	  collation: a fact row's spelling could then differ from the key's.
 *
 * lion_fkjoin_fk_groups_ok() asks the rest, of the GROUP BY itself.
 */
static bool
lion_fkjoin_fk_is_key(const LionFkJoin *fj)
{
	TypeCacheEntry *typentry;

	/*
	 * ... and a semi join path's row, which is an outer row with a match: the
	 * key stands for the matching fact rows' values there too (DESIGN.md §27,
	 * "The semi and anti join as a join path").
	 */
	if (fj->jointype != JOIN_INNER && fj->jointype != JOIN_UNIQUE_INNER &&
		!(fj->joinpath && fj->jointype == JOIN_SEMI))
		return false;
	if (fj->fkvar->vartype != fj->pkvar->vartype)
		return false;
	typentry = lookup_type_cache(fj->fkvar->vartype,
								 TYPECACHE_EQ_OPR | TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(typentry->eq_opr) || typentry->eq_opr != fj->opno ||
		!OidIsValid(typentry->btree_opintype))
		return false;
	return lion_type_equalimage(typentry->btree_opintype, fj->collation);
}

/*
 * Does the GROUP BY take the fact's join column as the FK-side join compares
 * it (DESIGN.md §27, "Grouped by the join key")?  The rule of
 * lion_fkjoin_agg_is_distinct(), for each grouping column the fact's column
 * is in: the column itself, grouped by an equality compatible with the join
 * operator (equality_ops_are_compatible()), under the join's collation - so
 * that the fact values a dimension row joins are one group, and the key the
 * node emits for them is in it.  An expression of the column (`GROUP BY
 * f.fk + 1`) is not taken; one that the target computes from the grouping
 * column (`SELECT f.fk + 1 ... GROUP BY f.fk`) is the Agg's, above the node.
 */
static bool
lion_fkjoin_fk_groups_ok(PlannerInfo *root, PathTarget *target,
						 const LionFkJoin *fj)
{
	int			i = 0;
	ListCell   *lc;

	foreach(lc, target->exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(target, i++);
		SortGroupClause *sgc;
		Node	   *stripped;
		List	   *vars;
		ListCell   *lv;
		bool		hasfk = false;

		if (sgref == 0)
			continue;
		sgc = get_sortgroupref_clause_noerr(sgref, root->parse->groupClause);
		if (sgc == NULL)
			continue;

		vars = pull_var_clause(expr, PVC_RECURSE_AGGREGATES |
							   PVC_RECURSE_WINDOWFUNCS |
							   PVC_INCLUDE_PLACEHOLDERS);
		foreach(lv, vars)
		{
			Var		   *v = (Var *) lfirst(lv);

			if (IsA(v, Var) && v->varno == fj->fkvar->varno &&
				v->varattno == fj->fkvar->varattno && v->varlevelsup == 0)
				hasfk = true;
		}
		list_free(vars);
		if (!hasfk)
			continue;

		stripped = lion_strip(expr);
		if (stripped == NULL || !IsA(stripped, Var))
			return false;
		if (!OidIsValid(sgc->eqop) ||
			!equality_ops_are_compatible(sgc->eqop, fj->opno))
			return false;
		if (exprCollation(expr) != fj->collation)
			return false;
	}
	return true;
}

/*
 * The fact column the query's GROUP BY groups an FK-side join by, other than
 * its join key (DESIGN.md §27, "Grouped by a fact column"), or NULL - and in
 * *drive what lion_collect_targets() is to find for it in each relation
 * counted.
 *
 * A dimension row's count is one number, the rows of one posting set ANDed
 * with the fact filters, which says nothing of how the rows it counts divide
 * among the fact column's values; so the node counts each dimension row once
 * per GROUP of the column instead, ANDing in that group's own posting set -
 * an entry of a lion index on the column - and hands up a partial count per
 * dimension row and group, which the Finalize Agg adds up per group, as it
 * adds up a dimension column's.  The column is then printed from the entry's
 * stored key, so it takes what a GROUP BY's own driving index takes: a scalar
 * lion index whose strategy 1 is the grouping equality, under the grouping
 * collation, that can print its keys (§10) - or, in a partition whose bounds
 * give the column one value, no index at all (lion_leaf_bound_value()).
 *
 * Only for an inner join, and the forward semi join over its distinct keys:
 * the rows of a semi or anti join are the dimension's, whose query sees no
 * fact column.  One such column, grouped by itself - a plain Var of the
 * GROUP BY, hashed by the Finalize Agg - beside any dimension columns; the
 * join key has its own rules (lion_fkjoin_fk_is_key()).
 */
static Var *
lion_fkjoin_fact_group(PlannerInfo *root, PathTarget *target,
					   const LionFkJoin *fj, LionDriveInfo *drive)
{
	Var		   *found = NULL;
	List	   *vars;
	ListCell   *lc;
	int			i = 0;

	if (fj->jointype != JOIN_INNER && fj->jointype != JOIN_UNIQUE_INNER)
		return NULL;

	vars = pull_var_clause((Node *) target->exprs,
						   PVC_INCLUDE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varno != fj->fkvar->varno ||
			v->varlevelsup != 0 || v->varattno <= 0 ||
			v->varattno == fj->fkvar->varattno)
			continue;
		if (found != NULL && found->varattno != v->varattno)
			return NULL;
		found = v;
	}
	list_free(vars);
	if (found == NULL)
		return NULL;

	foreach(lc, target->exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(target, i++);
		SortGroupClause *sgc;
		Var		   *v;

		if (sgref == 0)
			continue;
		v = (Var *) lion_strip(expr);
		if (v == NULL || !IsA(v, Var) || v->varno != found->varno ||
			v->varattno != found->varattno || v->varlevelsup != 0)
			continue;
		sgc = get_sortgroupref_clause_noerr(sgref, root->parse->groupClause);
		if (sgc == NULL || !OidIsValid(sgc->eqop) || !sgc->hashable ||
			exprCollation(expr) != v->varcollid)
			return NULL;

		memset(drive, 0, sizeof(*drive));
		drive->attno = v->varattno;
		drive->var = v;
		drive->collation = v->varcollid;
		drive->eqop = sgc->eqop;
		drive->valueout = true;
		drive->bound = true;
		return v;
	}
	return NULL;
}

/*
 * Is each group of the query's GROUP BY one row of the FK-side join's
 * (DESIGN.md §27, "Grouped by the join key")?  It is when one of the grouping
 * columns is a key the node's rows never repeat, grouped by an equality
 * compatible with the join operator under the join's collation - the
 * equality the key is unique under:
 *
 *	- the dimension's key in an inner join, which an index proves unique
 *	  under that equality and collation (lion_column_is_unique(), which
 *	  recognising the join asked), or in a semi join where one does; a NULL
 *	  key is no row of either.  An anti join's rows include NULL keys, which
 *	  would be one group of several rows, and it is not asked;
 *	- the fact's join column standing for it (lion_fkjoin_fk_is_key()): in an
 *	  inner join each dimension row's key once, and in the forward semi join
 *	  over a non-unique key each DISTINCT key once, which the node makes
 *	  distinct itself by the join operator's family.
 *
 * Every row is then its own group, whatever the other grouping columns hold,
 * and the Agg over the rows needs neither a hash table nor a Sort: a sorted
 * Agg over them as they come finds each row a group of its own.  Nothing
 * else about the rows' order is claimed.  The index is what core relies on
 * for a unique inner join, and dropping it invalidates the plan as it does
 * core's.
 */
static bool
lion_fkjoin_groups_per_row(PlannerInfo *root, const LionFkJoin *fj,
						   bool fkkey)
{
	ListCell   *lc;

	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		Node	   *expr = (Node *) get_sortgroupclause_expr(sgc,
															  root->processed_tlist);
		Node	   *stripped = lion_strip(expr);
		Var		   *v;

		if (stripped == NULL || !IsA(stripped, Var))
			continue;
		v = (Var *) stripped;
		if (v->varlevelsup != 0)
			continue;
		if (v->varno == fj->pkvar->varno && v->varattno == fj->pkvar->varattno)
		{
			if (fj->jointype != JOIN_INNER &&
				!(fj->jointype == JOIN_SEMI &&
				  lion_column_is_unique(fj->dimrel, fj->pkvar->varattno,
										fj->opno, fj->collation)))
				continue;
		}
		else if (v->varno == fj->fkvar->varno &&
				 v->varattno == fj->fkvar->varattno)
		{
			if (!fkkey)
				continue;
		}
		else
			continue;
		if (!OidIsValid(sgc->eqop) ||
			!equality_ops_are_compatible(sgc->eqop, fj->opno))
			continue;
		if (exprCollation(expr) != fj->collation)
			continue;
		return true;
	}
	return false;
}

/*
 * How many groups a GROUP BY the FK-side join's rows each make one of has
 * (lion_fkjoin_groups_per_row()): the rows that come out, which are the
 * dimension rows that join.  Of an inner join's `inrows` dimension rows - or
 * the forward semi join's distinct keys - those are the ones at least one of
 * the join's pairs falls on: 1 - e^(-pairs/rows) of them when the pairs fall
 * on the rows at random, which is no more than either.  A semi join's rows
 * are its result rows already.  And no more than core's own estimate of the
 * groups over the join's pairs, which for the fact's key counts its distinct
 * values.
 */
static double
lion_fkjoin_per_row_groups(PlannerInfo *root, const LionFkJoin *fj,
						   double inrows)
{
	double		groups = inrows;
	double		joined;

	if (fj->jointype == JOIN_INNER || fj->jointype == JOIN_UNIQUE_INNER)
		groups = inrows * (1.0 - exp(-fj->joinrel->rows / Max(inrows, 1.0)));
	joined = estimate_num_groups(root,
								 get_sortgrouplist_exprs(root->processed_groupClause,
														 root->processed_tlist),
								 fj->joinrel->rows, NULL, NULL);
	return clamp_row_est(Min(groups, joined));
}

/*
 * How the FK-side join answers one aggregate of the query (DESIGN.md §27,
 * "Every aggregate over the node's rows").  A count of join pairs is a sum of
 * the dimension rows' counts, which is a partial aggregate when every
 * aggregate is one; the others need the node's ROWS - one per dimension row
 * that joins - and a plain core Agg over them, and once one of them does,
 * every aggregate of the query is answered there:
 *
 *	COUNT		count(*), count(1), count of the key: over rows that carry
 *				their counts n (an inner join's), lion_join_count(n); over a
 *				semi or anti join's rows, which are the join's own rows, as
 *				it stands;
 *	DISTINCT	count(DISTINCT x), as it stands;
 *	ROWS		min, max, bool_and, bool_or, every, bit_and and bit_or of an
 *				expression of dimension columns, as they stand: each is
 *				blind to how often a value comes - min(v, v) is v, v AND v is
 *				v - and skips NULLs, so over the rows it sees the values the
 *				join's pairs hold, each once;
 *	COUNTCOL	count(x) of an expression of dimension columns: over counted
 *				rows lion_join_count(n) FILTER (WHERE x IS NOT NULL), each
 *				pair of a row having that row's x; over a semi or anti join's
 *				rows as it stands;
 *	SUM			sum(x) of such an int2 or int4 expression: over counted rows
 *				lion_join_sum(x * n) - int8 arithmetic that fails where the
 *				int8 sum fails, and NULL for no non-NULL x as sum() is - and
 *				as it stands over a semi or anti join's.
 *
 * sum of an int8 or numeric column is left out: its product needs numeric
 * arithmetic a row, against the 128-bit sum core does, to answer the same.
 * No other aggregate is taken, and none of these with DISTINCT (but count's),
 * ORDER BY or FILTER, or over a fact column but the key.
 */
typedef enum LionFkAgg
{
	LION_FKAGG_NONE = 0,		/* not answered: the join declines */
	LION_FKAGG_COUNT,
	LION_FKAGG_DISTINCT,
	LION_FKAGG_ROWS,
	LION_FKAGG_COUNTCOL,
	LION_FKAGG_SUM
} LionFkAgg;

static LionFkAgg
lion_fkjoin_agg_kind(Aggref *agg, const LionFkJoin *fj)
{
	TargetEntry *tle;
	List	   *vars;
	ListCell   *lc;
	HeapTuple	tup;
	Oid			sortop;

	if (lion_fkjoin_agg_is_count(agg, fj))
		return LION_FKAGG_COUNT;
	if (lion_fkjoin_agg_is_distinct(agg, fj))
		return LION_FKAGG_DISTINCT;

	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->aggvariadic ||
		agg->aggdirectargs != NIL || agg->aggstar)
		return LION_FKAGG_NONE;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE ||
		agg->aggkind != AGGKIND_NORMAL)
		return LION_FKAGG_NONE;
	if (list_length(agg->args) != 1)
		return LION_FKAGG_NONE;
	tle = (TargetEntry *) linitial(agg->args);
	if (!IsA(tle, TargetEntry))
		return LION_FKAGG_NONE;

	/*
	 * An expression of the dimension's columns, which the rows carry: the
	 * Agg computes it over them as it would over the join's pairs, each pair
	 * holding its dimension row's values.  The volatile ones went earlier.
	 */
	vars = pull_var_clause((Node *) tle->expr, PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varno != (int) fj->dimrel->relid ||
			v->varattno <= 0 || v->varlevelsup != 0)
			return LION_FKAGG_NONE;
	}
	list_free(vars);

	switch (agg->aggfnoid)
	{
		case F_COUNT_ANY:
			return LION_FKAGG_COUNTCOL;
		case F_SUM_INT2:
			return (exprType((Node *) tle->expr) == INT2OID) ?
				LION_FKAGG_SUM : LION_FKAGG_NONE;
		case F_SUM_INT4:
			return (exprType((Node *) tle->expr) == INT4OID) ?
				LION_FKAGG_SUM : LION_FKAGG_NONE;
		case F_BOOL_AND:
		case F_BOOL_OR:
		case F_EVERY:
		case F_BIT_AND_INT2:
		case F_BIT_AND_INT4:
		case F_BIT_AND_INT8:
		case F_BIT_AND_BIT:
		case F_BIT_OR_INT2:
		case F_BIT_OR_INT4:
		case F_BIT_OR_INT8:
		case F_BIT_OR_BIT:
			return LION_FKAGG_ROWS;
		default:
			break;
	}

	/*
	 * min and max, of every type: core's aggregates with a sort operator,
	 * which are the ones planagg.c answers as the first value in that
	 * operator's order, NULLs skipped - a value, however often it comes.
	 */
	if (get_func_namespace(agg->aggfnoid) != PG_CATALOG_NAMESPACE)
		return LION_FKAGG_NONE;
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(agg->aggfnoid));
	if (!HeapTupleIsValid(tup))
		return LION_FKAGG_NONE;
	sortop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
	ReleaseSysCache(tup);
	return OidIsValid(sortop) ? LION_FKAGG_ROWS : LION_FKAGG_NONE;
}

/*
 * What rewriting a join's aggregates over its counted rows needs (DESIGN.md
 * §27, "Every aggregate over the node's rows"): the rows' count column, and
 * the extension's aggregates and core's operators the rewrites are made of.
 */
typedef struct LionFkAggRewrite
{
	const LionFkJoin *fj;
	Aggref	   *n;				/* the rows' count: a partial count(*) */
	Oid			countfn;		/* lion_join_count(int8) */
	Oid			sumfn;			/* lion_join_sum(int8) */
	Oid			mul2op;			/* int2 * int8 */
	Oid			mul4op;			/* int4 * int8 */
} LionFkAggRewrite;

/*
 * One of the extension's aggregates, found in the extension's own schema and
 * checked to be the extension's - no namesake another schema holds - and one
 * the current user may run, as the Agg will ask (the executor asks again, of
 * whoever runs the plan).  It is read from the catalog cache, not looked up
 * by name as a query's function is: that asks USAGE on the schema, and fails
 * for a user who has none, where running the aggregate does not ask it.  A
 * database whose pg_lion was created by a script without it gets the
 * ordinary plan.
 */
static Oid
lion_fkjoin_ext_agg(Oid extoid, Oid nsp, const char *name)
{
	Oid			argtype = INT8OID;
	Oid			fn;

	fn = GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
						 CStringGetDatum(name),
						 PointerGetDatum(buildoidvector(&argtype, 1)),
						 ObjectIdGetDatum(nsp));
	if (!OidIsValid(fn) || get_func_prokind(fn) != PROKIND_AGGREGATE ||
		getExtensionOfObject(ProcedureRelationId, fn) != extoid)
		return InvalidOid;
	if (object_aclcheck(ProcedureRelationId, fn, GetUserId(),
						ACL_EXECUTE) != ACLCHECK_OK)
		return InvalidOid;
	return fn;
}

/* core's `*` of an int2 or int4 by an int8, from the catalog cache too */
static Oid
lion_fkjoin_mul_op(Oid lefttype)
{
	return GetSysCacheOid4(OPERNAMENSP, Anum_pg_operator_oid,
						   CStringGetDatum("*"),
						   ObjectIdGetDatum(lefttype),
						   ObjectIdGetDatum(INT8OID),
						   ObjectIdGetDatum(PG_CATALOG_NAMESPACE));
}

static bool
lion_fkjoin_rewrite_init(LionFkAggRewrite *cx, const LionFkJoin *fj,
						 bool needcount, bool needsum)
{
	Oid			extoid = get_extension_oid("pg_lion", true);
	Oid			nsp;
	Aggref	   *n;

	memset(cx, 0, sizeof(LionFkAggRewrite));
	cx->fj = fj;
	if (!OidIsValid(extoid))
		return false;
	nsp = get_extension_schema(extoid);
	if (!OidIsValid(nsp))
		return false;
	if (needcount)
	{
		cx->countfn = lion_fkjoin_ext_agg(extoid, nsp, "lion_join_count");
		if (!OidIsValid(cx->countfn))
			return false;
	}
	if (needsum)
	{
		cx->sumfn = lion_fkjoin_ext_agg(extoid, nsp, "lion_join_sum");
		cx->mul2op = lion_fkjoin_mul_op(INT2OID);
		cx->mul4op = lion_fkjoin_mul_op(INT4OID);
		if (!OidIsValid(cx->sumfn) || !OidIsValid(cx->mul2op) ||
			!OidIsValid(cx->mul4op))
			return false;
	}

	/*
	 * The count column: what the partial count(*) of a dimension row's join
	 * pairs is, and is printed as - "(PARTIAL count(*))" - wherever an
	 * FK-side join hands such counts up.
	 */
	n = makeNode(Aggref);
	n->aggfnoid = F_COUNT_;
	n->aggtype = INT8OID;
	n->aggtranstype = INT8OID;
	n->aggstar = true;
	n->aggkind = AGGKIND_NORMAL;
	n->aggsplit = AGGSPLIT_SIMPLE;
	n->location = -1;
	mark_partial_aggref(n, AGGSPLIT_INITIAL_SERIAL);
	cx->n = n;
	return true;
}

/*
 * An int8 aggregate of the extension's in place of `orig`: the same aggregate
 * number and transition number, which the executor finds its state by, so
 * that the Agg's aggregates and the HAVING's are one and the same where they
 * were (two Aggrefs share a number only when they are the same aggregate of
 * the same arguments, which rewrite alike).
 */
static Aggref *
lion_fkjoin_make_agg(Oid aggfnoid, Expr *arg, Expr *filter, Aggref *orig)
{
	Aggref	   *agg = makeNode(Aggref);

	agg->aggfnoid = aggfnoid;
	agg->aggtype = INT8OID;
	agg->aggcollid = InvalidOid;
	agg->inputcollid = InvalidOid;
	agg->aggtranstype = INT8OID;
	agg->aggargtypes = list_make1_oid(INT8OID);
	agg->args = list_make1(makeTargetEntry(arg, 1, NULL, false));
	agg->aggfilter = filter;
	agg->aggkind = AGGKIND_NORMAL;
	agg->aggsplit = AGGSPLIT_SIMPLE;
	agg->aggno = orig->aggno;
	agg->aggtransno = orig->aggtransno;
	agg->location = orig->location;
	return agg;
}

/*
 * The target list or HAVING of the Agg over counted rows: every aggregate
 * that sees how many pairs a dimension row makes rewritten over the rows'
 * count column n, and the others left as they stand (lion_fkjoin_agg_kind()).
 */
static Node *
lion_fkjoin_rewrite_mutator(Node *node, LionFkAggRewrite *cx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;
		Expr	   *x;
		Expr	   *filter;
		OpExpr	   *product;

		switch (lion_fkjoin_agg_kind(agg, cx->fj))
		{
			case LION_FKAGG_COUNT:
				return (Node *) lion_fkjoin_make_agg(cx->countfn,
													 (Expr *) copyObject(cx->n),
													 NULL, agg);
			case LION_FKAGG_COUNTCOL:
				{
					NullTest   *nt = makeNode(NullTest);

					/* a NULL datum, as count() sees it: not a row's fields */
					nt->arg = (Expr *) copyObject(((TargetEntry *) linitial(agg->args))->expr);
					nt->nulltesttype = IS_NOT_NULL;
					nt->argisrow = false;
					nt->location = -1;
					filter = (Expr *) nt;
				}
				return (Node *) lion_fkjoin_make_agg(cx->countfn,
													 (Expr *) copyObject(cx->n),
													 filter, agg);
			case LION_FKAGG_SUM:
				x = (Expr *) copyObject(((TargetEntry *) linitial(agg->args))->expr);
				product = (OpExpr *) make_opclause(agg->aggfnoid == F_SUM_INT2 ?
												   cx->mul2op : cx->mul4op,
												   INT8OID, false, x,
												   (Expr *) copyObject(cx->n),
												   InvalidOid, InvalidOid);
				set_opfuncid(product);
				return (Node *) lion_fkjoin_make_agg(cx->sumfn,
													 (Expr *) product, NULL,
													 agg);
			case LION_FKAGG_DISTINCT:
			case LION_FKAGG_ROWS:
				return node;
			case LION_FKAGG_NONE:
				break;
		}
		elog(ERROR, "LionCount: an aggregate the join cannot answer");
	}
	return expression_tree_mutator(node, lion_fkjoin_rewrite_mutator,
								   (void *) cx);
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
		cost += 2.0 * ceil(bytes / BLCKSZ) * LION_FKJOIN_SORT_PAGE_COST;
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
 * The core Agg over the rows an FK-side join emits (DESIGN.md §27): the
 * query's own target and HAVING, or - when the rows carry their counts
 * (LION_JOINFLAG_COUNTS) - both rewritten over them
 * (lion_fkjoin_rewrite_mutator()), the Agg then going into the grouped rel
 * below a LionJoinAgg; and whether it may hash.
 */
typedef struct LionFkJoinAgg
{
	bool		counts;			/* the rows carry their counts, and the
								 * aggregates are rewritten over them */
	PathTarget *target;			/* the Agg's target */
	List	   *having;			/* ... and its HAVING */
	bool		hashable;		/* no DISTINCT aggregate */
} LionFkJoinAgg;

/*
 * The Agg over an FK-side join's rows, into the grouped rel: as it is, or,
 * when its aggregates were rewritten over counted rows, below a LionJoinAgg
 * whose target is the grouped rel's own (DESIGN.md §27, "Every aggregate over
 * the node's rows").
 *
 * The rewritten aggregates cannot be the grouped rel's: what core puts above
 * a grouped path - a Sort for ORDER BY count(*), a projection - finds the
 * query's aggregates in its input's target list by equal() (createplan.c's
 * prepare_sort_from_pathkeys(), setrefs.c's fix_upper_expr()), and would not
 * find count(*) in an Agg that computes lion_join_count() instead - or worse,
 * would add a count(*) of the Agg's own, which over the rows counts dimension
 * rows.  So the grouped rel gets this node, whose target list IS the query's
 * own (in custom_scan_tlist, which setrefs matches the parents against), and
 * which hands up the Agg's rows unchanged: the Agg's target is the grouped
 * rel's expression for expression, each aggregate rewritten in its place, so
 * column i of one is column i of the other, of the same type.  Nothing above
 * the node sees the rewritten aggregates, and nothing below it sees the
 * query's.  It costs nothing of its own: it projects nothing and passes a
 * slot through, a group at a time.
 */
static void
lion_add_join_agg_path(PlannerInfo *root, RelOptInfo *output_rel,
					   Path *aggpath, const LionFkJoinAgg *rowagg)
{
	CustomPath *cpath;

	if (!rowagg->counts)
	{
		add_path(output_rel, aggpath);
		return;
	}

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = aggpath->parallel_safe;
	cpath->path.parallel_workers = 0;
	cpath->path.pathkeys = aggpath->pathkeys;
	cpath->path.rows = aggpath->rows;
	cpath->path.startup_cost = aggpath->startup_cost;
	cpath->path.total_cost = aggpath->total_cost;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = aggpath->disabled_nodes;
#endif
	cpath->flags = 0;
	cpath->custom_paths = list_make1(aggpath);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = NIL;
	cpath->methods = &lion_join_agg_path_methods;
	add_path(output_rel, &cpath->path);
}

/*
 * The chance that leaf partition `rel` of an FK-side join's fact table has a
 * turn (lion_join_count_parts()): that its fact filters select any row.  None
 * when its bound refutes them at their plan-time values
 * (lion_leaf_refuted_now()); otherwise the rows its fact filters are
 * expected to select - with the ends of the columns they bound read from its
 * lion index (lion_probed_selectivity()) - taken as that chance while they
 * are less than a row, which clamp_row_est() rounds up to one: a leaf
 * expected to hold a row or more has its turn.
 */
double
lion_leaf_turn_share(PlannerInfo *root, RelOptInfo *rel)
{
	double		rows;

	if (rel->baserestrictinfo == NIL)
		return 1.0;
	if (lion_leaf_refuted_now(root, rel, rel->baserestrictinfo))
		return 0.0;
	rows = Max(rel->tuples, 0.0) *
		lion_probed_selectivity(root, rel, rel->baserestrictinfo);
	return Min(rows, 1.0);
}

/*
 * How many groups of the fact column a GROUP BY groups an FK-side join by
 * (DESIGN.md §27, "Grouped by a fact column") relation t counts each of its
 * keys in: every entry of the column's lion index there - the relation's
 * distinct values, whether or not the fact filters leave them any row - or
 * one, where the partition's bounds give the column its value, or nothing is
 * grouped by.
 */
double
lion_fact_groups(PlannerInfo *root, const LionCountTarget *t)
{
	if (t->driveidx[0] == NULL || t->drivevar[0] == NULL)
		return 1.0;
	return clamp_row_est(estimate_num_groups(root, list_make1(t->drivevar[0]),
											 Max(t->rel->tuples, 1.0), NULL,
											 NULL));
}

/* ... summed over the relations: the groups a key is counted in, all told */
static double
lion_fact_groups_all(PlannerInfo *root, List *targets)
{
	double		groups = 0;
	ListCell   *lc;

	foreach(lc, targets)
		groups += lion_fact_groups(root, (LionCountTarget *) lfirst(lc));
	return groups;
}

/*
 * Do the child's rows carry the dimension's column `var`?  The node reads
 * every dimension column it emits, and the key, from its child's row
 * (lion_child_resno()).  A dimension of one table always carries the columns
 * the query needs of it above the scan; a dimension that is a join (DESIGN.md
 * §27, "A dimension that is a join") carries those its join rel's target
 * holds, which are the ones needed above the join - every one the node's own
 * target can name - and that is checked here rather than assumed.
 */
static bool
lion_fkjoin_child_has(const LionFkJoin *fj, Var *var)
{
	ListCell   *lc;

	foreach(lc, fj->dimpath->pathtarget->exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (IsA(v, Var) && v->varno == var->varno &&
			v->varattno == var->varattno && v->varlevelsup == 0)
			return true;
	}
	return false;
}

/*
 * Could the dimension's rows differ from one run of its plan to the next -
 * something volatile in a qual of one of its tables, in a join condition
 * inside it (a semi or anti join of a dimension that is a join, DESIGN.md
 * §27, "A dimension that is a join"), or inside a table of it that is no
 * table: a subquery, a function or a VALUES list?  The forward semi join's
 * parallel plan has every participant run the dimension whole and needs them
 * all to see the same rows.
 */
static bool
lion_fkjoin_dim_volatile(PlannerInfo *root, const LionFkJoin *fj)
{
	Relids		dimrels = fj->dimchild->relids;
	int			i = -1;

	while ((i = bms_next_member(dimrels, i)) >= 0)
	{
		RelOptInfo *r = root->simple_rel_array[i];
		RangeTblEntry *rte = root->simple_rte_array[i];
		ListCell   *lc;

		if (contain_volatile_functions((Node *) r->baserestrictinfo))
			return true;
		foreach(lc, r->joininfo)
		{
			RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

			if (bms_is_subset(ri->required_relids, dimrels) &&
				contain_volatile_functions((Node *) ri->clause))
				return true;
		}
		if (rte->rtekind != RTE_RELATION &&
			(contain_volatile_functions((Node *) rte->subquery) ||
			 contain_volatile_functions((Node *) rte->functions) ||
			 contain_volatile_functions((Node *) rte->values_lists)))
			return true;
	}
	return false;
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
 * the sort are charged to every participant in full, and the lookups and
 * counts are one participant's share of the keys, as the making of the fact
 * filters' copy is.
 *
 * The node streams one row per dimension row that joins (a partial count, or
 * the row itself for count(DISTINCT)), so it starts when its child does, and
 * it costs the child plus what it does per dimension row.  A target of counts
 * alone is summed instead (LION_JOINFLAG_SUM): one row per participant and
 * run, which is what core's Gather and Finalize Agg above it then handle.
 * In a parallel plan the child hands each participant its share of the
 * dimension rows, the dimension rows divided as core divides a partial path's
 * (the Gather may start more workers than the child planned, "Parallel");
 * every participant locates the fact filters, whose cost is charged in full
 * to each, and they make ONE copy of them together, whose cost is divided
 * among them as a Parallel Hash divides its build ("One copy per query") -
 * the elapsed cost of the parallel plan is one participant's.
 *
 * Above a partial path goes a Gather; above that, or above the node itself,
 * core's Finalize Agg over the partial counts - or, for emitted rows
 * (`rowagg`), a Sort by the query's group pathkeys (the GROUP BY columns and
 * the DISTINCT arguments core may have chosen to presort) and core's plain
 * Agg, and beside it a hashed Agg where no aggregate is a DISTINCT one.  An
 * Agg whose aggregates were rewritten over counted rows goes into the grouped
 * rel below a LionJoinAgg (lion_add_join_agg_path()).
 *
 * When each group is one of the node's rows (`perrow`,
 * lion_fkjoin_groups_per_row()) - a GROUP BY of the dimension's key or of the
 * fact's key standing for it - the Agg, Finalize or plain, is a sorted one
 * straight over the rows as they come, with no Sort and no hash table: each
 * row differs from the one before it and is a group of its own, which is
 * what the Agg is charged for, a comparison and a group a row.  Hashing the
 * rows or sorting them first would group nothing, and their cost - a hash
 * table that may spill, a sort of every row - is not charged either.
 */
static void
lion_add_fkjoin_paths(PlannerInfo *root, RelOptInfo *rel,
					  RelOptInfo *output_rel, const LionFkJoin *fj,
					  List *having, List *targets, Path *child,
					  int workers, PathTarget *nodetarget, List *base,
					  int joinclause, int jointype,
					  const LionFkJoinAgg *rowagg, bool perrow,
					  List *whereclauses, List *wherekinds, List *ors,
					  double dimrows, double found, bool parallel_safe)
{
	Query	   *parse = root->parse;
	CustomPath *cpath;
	Path	   *input;
	Path	   *aggpath;
	bool		partial = (workers > 0);
	bool		unique = (fj->jointype == JOIN_UNIQUE_INNER);
	bool		emitrows = (rowagg != NULL);
	bool		counts = (rowagg != NULL && rowagg->counts);
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
	Cost		sortcost = 0;
	Cost		startup;
	int			flags;
	AggStrategy aggstrategy;
	AggClauseCosts agg_costs;
	LionUnits	units;

	/*
	 * What a child row takes of a batch looked up in key order: its columns
	 * and the batch's own bookkeeping - or, for the distinct keys of a forward
	 * semi join, the key alone.
	 */
	rowbytes = (double) LION_FKJOIN_BATCH_ENT_BYTES +
		MAXALIGN(unique ?
				 get_typavgwidth(fj->pkvar->vartype, fj->pkvar->vartypmod) :
				 child->pathtarget->width);

	/*
	 * What the node does over the fact side, and the sort of a forward semi
	 * join's keys, are its own price, in the units of the cheapest core path
	 * of the grouped rel (DESIGN.md §39); the child is core's, priced by core.
	 */
	lion_units_for(output_rel, &units);
	run = lion_cost_fkjoin_path(root, rel, targets, fj->fkvar, joinclause,
								whereclauses, wherekinds, ors, childrows,
								childfound,
								jointype != LION_JOIN_INNER ||
								(emitrows && !counts),
								rowbytes, workers, &collect, &walk, false);
	if (unique)
		sortcost = lion_fkjoin_sort_cost(child->rows,
										 child->pathtarget->width);
	run = lion_units_price(&units, run);
	sortcost = lion_units_price(&units, sortcost);

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
		(sum ? LION_JOINFLAG_SUM : 0) |
		(counts ? LION_JOINFLAG_COUNTS : 0);

	/*
	 * The node starts when its child does - or, when it sorts the child's
	 * keys first, once the whole child has run and the keys are sorted.
	 */
	startup = child->startup_cost;
	if (unique)
	{
		startup = child->total_cost + sortcost;
		run += sortcost;
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

	/*
	 * ... or one per group of a fact column grouped by, in each relation
	 * counted, that has rows: no more than the join's own ("Grouped by a
	 * fact column")
	 */
	if (lion_count_priv_fact_group_attno(base) != 0)
		rows = clamp_row_est(Min(rows * lion_fact_groups_all(root, targets),
								 fj->joinrel->rows * share));

	/* ... or one, their sum, when that is all the target wants */
	if (sum)
		rows = 1.0;
	run += lion_units_price(&units, rows * LION_FKJOIN_ROW_COST);
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
		/*
		 * ---- the Finalize Agg that groups the partial counts ----
		 *
		 * Hashed, or - when each group is one row - sorted over the rows as
		 * they come, which groups nothing and only finishes each row's count.
		 */
		if (perrow)
		{
			aggstrategy = AGG_SORTED;
			numgroups = lion_fkjoin_per_row_groups(root, fj, inrows);
		}
		else if (root->processed_groupClause != NIL)
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
	 * core's own sorted Agg would be given.  Its aggregates are the query's,
	 * or those rewritten over counted rows (rowagg).
	 */
	MemSet(&agg_costs, 0, sizeof(agg_costs));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &agg_costs);

	/*
	 * Each group one row: the sorted Agg straight over the rows, which need
	 * no order since no two of them are one group - neither a Sort nor a
	 * hashed Agg.  A DISTINCT aggregate's values are one per group then, as
	 * sorted as a presorted one needs them.
	 */
	if (perrow)
	{
		aggpath = (Path *) create_agg_path(root, output_rel, input,
										   rowagg->target,
										   AGG_SORTED, AGGSPLIT_SIMPLE,
										   root->processed_groupClause,
										   rowagg->having,
										   &agg_costs,
										   lion_fkjoin_per_row_groups(root, fj,
																	  inrows));
		lion_add_join_agg_path(root, output_rel, aggpath, rowagg);
		return;
	}

	if (root->processed_groupClause != NIL)
		numgroups = estimate_num_groups(root,
										get_sortgrouplist_exprs(root->processed_groupClause,
																root->processed_tlist),
										inrows, NULL, NULL);
	else
		numgroups = 1.0;

	/*
	 * ... or hashed, which needs no Sort, where no aggregate is a DISTINCT
	 * one - which core's Agg computes over sorted input, or by sorting each
	 * group's values itself - and the GROUP BY can be hashed.
	 */
	if (rowagg->hashable && root->processed_groupClause != NIL &&
		grouping_is_hashable(root->processed_groupClause))
	{
		aggpath = (Path *) create_agg_path(root, output_rel, input,
										   rowagg->target,
										   AGG_HASHED, AGGSPLIT_SIMPLE,
										   root->processed_groupClause,
										   rowagg->having,
										   &agg_costs,
										   numgroups);
		lion_add_join_agg_path(root, output_rel, aggpath, rowagg);
	}

	if (root->group_pathkeys != NIL)
		input = (Path *) create_sort_path(root, output_rel, input,
										  root->group_pathkeys, -1.0);
	if (root->processed_groupClause != NIL)
		aggstrategy = AGG_SORTED;
	else
		aggstrategy = (parse->groupClause != NIL) ? AGG_SORTED : AGG_PLAIN;

	aggpath = (Path *) create_agg_path(root, output_rel, input,
									   rowagg->target,
									   aggstrategy, AGGSPLIT_SIMPLE,
									   root->processed_groupClause,
									   rowagg->having,
									   &agg_costs,
									   numgroups);
	lion_add_join_agg_path(root, output_rel, aggpath, rowagg);
}

/*
 * What every FK-side join path carries of the fact side (DESIGN.md §27), the
 * aggregate over the query's join and the semi or anti join path alike
 * ("The semi and anti join as a join path"): the join key appended to the
 * fact filters as one more LION_CLAUSE_EQ clause, whose value expression is
 * the dimension's (the outer side's) column; the relations to count - the
 * fact table, or each of its live leaf partitions - each with a lion index
 * for the key and for every fact filter it keeps (lion_collect_targets());
 * and custom_private but for its join member.  `tlexprs` and `having` are
 * what the node's target and HAVING evaluate, for the EXECUTE checks
 * (lion_replaced_functions()).  False when the fact side declines.
 */
typedef struct LionFkJoinSetup
{
	int			joinclause;		/* the key's clause */
	List	   *targets;		/* LionCountTarget, one per relation counted */
	bool		partitioned;
	double		fkpages;		/* the fk index's pages, in every partition */
	List	   *whereclauses;	/* the clauses, the key's appended */
	List	   *wherekinds;		/* ... and their kinds */
	List	   *consts;			/* ... and values */
	List	   *base;			/* custom_private, its join member NIL */
} LionFkJoinSetup;

static bool
lion_fkjoin_setup(PlannerInfo *root, RelOptInfo *rel, const LionFkJoin *fj,
				  List *whereattnos, List *clauseinfos, List *whereclauses,
				  List *whereconsts, List *wherekinds, List *whereopnos,
				  List *whereinor, List *ors, const LionImply *imply,
				  List *tlexprs, List *having, const LionDriveInfo *fgdrive,
				  LionFkJoinSetup *out)
{
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	LionLeafInfo leaf;
	LionDriveInfo nodrive[LION_MAX_GROUPCOLS];
	int			joinclause;
	List	   *targets = NIL;
	bool		partitioned;
	double		fkpages;
	List	   *parts = NIL;
	List	   *oids;
	List	   *ints;
	List	   *consts = NIL;
	List	   *ckinds = NIL;
	List	   *base;
	ListCell   *lc;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	int			i;

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
	 * partitioned fact rel is one target per live leaf partition, each with
	 * its own fk index and its own fact filters, less the ones its bounds
	 * imply (DESIGN.md §27, "A partitioned fact table"); all of them pruned
	 * away leaves the join to the planner, which knows it is empty.  A fact
	 * column the GROUP BY groups by is a driving column of each relation
	 * (fgdrive, "Grouped by a fact column"): the lion index whose entries are
	 * its groups there - one of the rules of a GROUP BY's own driver, which
	 * prints the key it stored - or, in a partition whose bounds give it one
	 * value, that value.
	 */
	memset(nodrive, 0, sizeof(nodrive));
	if (!lion_collect_targets(root, rel, fgdrive != NULL ? fgdrive : nodrive,
							 fgdrive != NULL ? 1 : 0, whereattnos,
							 clauseinfos, imply, &targets))
		return false;
	if (targets == NIL)
		return false;
	partitioned = rte->inh;
	fkpages = 0;
	foreach(lc, targets)
		fkpages += (double)
			((IndexOptInfo *) list_nth(((LionCountTarget *) lfirst(lc))->whereidx,
									   joinclause))->pages;

	/*
	 * A partitioned fact table's own index Oids are InvalidOid, its
	 * partitions' in LION_PRIV_PARTS - InvalidOid where a partition leaves a
	 * fact filter out - as for a count (DESIGN.md §16).
	 */
	oids = list_make3_oid(rte->relid, InvalidOid, InvalidOid);
	ints = list_make4_int((int) rel->relid, 0, 0, 0);
	i = 0;
	forthree(l1, whereattnos, l2, whereconsts, l3, wherekinds)
	{
		oids = lappend_oid(oids, partitioned ? InvalidOid :
						   ((IndexOptInfo *) list_nth(((LionCountTarget *) linitial(targets))->whereidx,
													  i))->indexoid);
		ints = lappend_int(ints, lfirst_int(l1));
		consts = lappend(consts, copyObject((Node *) lfirst(l2)));
		ckinds = lappend_int(ckinds, lfirst_int(l3));
		i++;
	}
	if (partitioned)
	{
		foreach(lc, targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);
			List	   *one = list_make3_oid(t->heapoid, InvalidOid, InvalidOid);

			foreach(l1, t->whereidx)
				one = lappend_oid(one, lfirst(l1) != NULL ?
								  ((IndexOptInfo *) lfirst(l1))->indexoid :
								  InvalidOid);
			parts = lappend(parts, one);
		}
	}

	base = list_make1(list_make2_int(LION_PRIV_MAGIC, LION_PRIV_NMEMBERS));
	base = lappend(base, oids);
	base = lappend(base, ints);
	base = lappend(base, consts);
	base = lappend(base, ckinds);
	base = lappend(base, parts);
	base = lappend(base, whereopnos);
	base = lappend(base, ors);
	base = lappend(base, NIL);	/* having */
	base = lappend(base, NIL);	/* distinct */
	base = lappend(base, NIL);	/* join: lion_fkjoin_private() */
	/* the dimension's GROUP BY is the Agg's, which checks it */
	base = lappend(base, lion_replaced_functions(rel, tlexprs, having, NIL,
												 fj));
	base = lappend(base, NIL);	/* coalesce: the fact side groups nothing */
	base = lappend(base, (imply != NULL) ? imply->skipped : NIL);

	/* a fact column grouped by: each relation's index for it, or its value */
	if (fgdrive != NULL)
	{
		List	   *fgoids = NIL;
		List	   *fgconsts = NIL;

		foreach(lc, targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);

			if (t->driveconst[0] != NULL)
			{
				fgoids = lappend_oid(fgoids, InvalidOid);
				fgconsts = lappend(fgconsts, copyObject(t->driveconst[0]));
			}
			else
			{
				Assert(t->driveidx[0] != NULL);
				fgoids = lappend_oid(fgoids, t->driveidx[0]->indexoid);
				fgconsts = lappend(fgconsts, makeBoolConst(false, true));
			}
		}
		base = lappend(base, list_make3(list_make1_int((int) fgdrive->attno),
										fgoids, fgconsts));
	}
	else
		base = lappend(base, NIL);
	base = lappend(base, NIL);	/* decoded walk: the dimension's groups */
	base = lappend(base, NIL);	/* every row: the join key drives */
	base = lappend(base, NIL);	/* top k: the Agg above groups */
	base = lappend(base, NIL);	/* aggregates over a column's entries */

	out->joinclause = joinclause;
	out->targets = targets;
	out->partitioned = partitioned;
	out->fkpages = fkpages;
	out->whereclauses = whereclauses;
	out->wherekinds = wherekinds;
	out->consts = consts;
	out->base = base;
	return true;
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
 *	  of the key or of a dimension column has no partial form, nor have min,
 *	  max and the other aggregates of lion_fkjoin_agg_kind(); then the node
 *	  emits the dimension ROWS that join, one each, and core's own Agg - over
 *	  a Sort by the query's group pathkeys, which is what its DISTINCT and a
 *	  GROUP BY of it expect, or hashed where there is no DISTINCT - computes
 *	  the aggregates as it would over the join.  A semi or anti join's rows
 *	  are the join's own, so every aggregate is computed over them as it
 *	  stands.  An inner join's stand for its dimension rows, not for its
 *	  pairs: a count of pairs beside them, or a count or sum of a dimension
 *	  column, needs each row's count as well, so then the rows carry it
 *	  (LION_JOINFLAG_COUNTS) and those aggregates are rewritten into sums of
 *	  it (lion_fkjoin_rewrite_mutator()), below a LionJoinAgg that gives the
 *	  grouped rel back the query's own.  Either way core forms the groups,
 *	  of the dimension's columns, so neither the grouping-equality rule nor
 *	  the value-representation rule of §10 has anything to check - but for
 *	  the fact's join column, which the rows may carry as the dimension's
 *	  key where the GROUP BY takes it as the join compares it and the key
 *	  is spelled as the fact's values are (lion_fkjoin_fk_is_key()).  Grouped
 *	  by a key the rows never repeat, each row is a group and the Agg,
 *	  partial or not, sorts nothing and hashes nothing
 *	  (lion_fkjoin_groups_per_row());
 *	- the paths: the node over the dimension's cheapest path, and - when the
 *	  query may run in parallel and the dimension has a partial path - the
 *	  node over that partial path under a Gather, each participant counting
 *	  the dimension rows its share of the child returns ("Parallel").
 */
void
lion_try_fkjoin_path(PlannerInfo *root, RelOptInfo *rel,
					 RelOptInfo *output_rel, GroupPathExtraData *extra,
					 const LionFkJoin *fj, List *having,
					 List *whereattnos, List *clauseinfos, List *whereclauses,
					 List *whereconsts, List *wherekinds, List *whereopnos,
					 List *whereinor, List *ors, const LionImply *imply)
{
	List	   *exprs;
	List	   *items;
	LionFkJoinSetup setup;
	int			joinclause;
	List	   *targets;
	double		fkpages;		/* the fk index's pages, in every partition */
	PathTarget *nodetarget;
	List	   *consts;
	List	   *base;
	double		dimrows;
	double		found;
	int			jointype;
	bool		parallel;
	int			nagg[LION_FKAGG_SUM + 1] = {0};
	LionFkJoinAgg rowaggdata;
	LionFkJoinAgg *rowagg = NULL;
	bool		fkkey;
	bool		perrow;
	Var		   *fgvar;
	LionDriveInfo fgdrive;
	ListCell   *lc;

	/*
	 * The fact's join column may stand in the rows as the dimension's key
	 * where the GROUP BY takes it as the join compares it (DESIGN.md §27,
	 * "Grouped by the join key").
	 */
	fkkey = lion_fkjoin_fk_is_key(fj) &&
		lion_fkjoin_fk_groups_ok(root, output_rel->reltarget, fj);

	/*
	 * ... and another fact column may be grouped by, each row of the node a
	 * dimension row's count in one of its groups ("Grouped by a fact
	 * column").
	 */
	fgvar = lion_fkjoin_fact_group(root, output_rel->reltarget, fj, &fgdrive);

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
			 * rows, which have to carry them (lion_fkjoin_child_has()) - but
			 * for the fact's join column where it stands for the key, and a
			 * fact column the GROUP BY groups by, whose groups the node counts
			 * each dimension row in (fgvar).
			 */
			if (v->varattno <= 0 || v->varlevelsup != 0)
				return;
			if (v->varno == (int) fj->dimrel->relid)
			{
				if (!lion_fkjoin_child_has(fj, v))
					return;
				continue;
			}
			if (fkkey && v->varno == fj->fkvar->varno &&
				v->varattno == fj->fkvar->varattno)
				continue;
			if (fgvar != NULL && v->varno == fgvar->varno &&
				v->varattno == fgvar->varattno)
				continue;
			return;
		}
		else if (IsA(node, Aggref))
		{
			LionFkAgg	kind = lion_fkjoin_agg_kind((Aggref *) node, fj);

			if (kind == LION_FKAGG_NONE)
				return;
			nagg[kind]++;
		}
		else
			return;
	}
	if (nagg[LION_FKAGG_COUNT] + nagg[LION_FKAGG_DISTINCT] +
		nagg[LION_FKAGG_ROWS] + nagg[LION_FKAGG_COUNTCOL] +
		nagg[LION_FKAGG_SUM] == 0)
		return;

	/*
	 * Counts alone are partial counts, as they always were; anything else
	 * makes the node emit rows (DESIGN.md §27, "Every aggregate over the
	 * node's rows").  A semi or anti join's rows are its result rows, one
	 * each, and every aggregate is computed over them as it stands.  An
	 * inner join's - and the forward semi join's distinct keys - stand for
	 * dimension rows, and when an aggregate sees how many pairs each makes,
	 * the rows carry their counts and the Agg is given the aggregates
	 * rewritten over them.
	 */
	if (nagg[LION_FKAGG_DISTINCT] + nagg[LION_FKAGG_ROWS] +
		nagg[LION_FKAGG_COUNTCOL] + nagg[LION_FKAGG_SUM] > 0)
	{
		/*
		 * The rows of a fact column's groups are partial counts; a dimension
		 * row's row per group, with anything but its count, is not built.
		 */
		if (fgvar != NULL)
			return;
		memset(&rowaggdata, 0, sizeof(rowaggdata));
		rowagg = &rowaggdata;
		rowagg->target = output_rel->reltarget;
		rowagg->having = having;
		rowagg->hashable = (nagg[LION_FKAGG_DISTINCT] == 0);
		rowagg->counts = (fj->jointype == JOIN_INNER ||
						  fj->jointype == JOIN_UNIQUE_INNER) &&
			nagg[LION_FKAGG_COUNT] + nagg[LION_FKAGG_COUNTCOL] +
			nagg[LION_FKAGG_SUM] > 0;
	}

	if (rowagg == NULL)
	{
		/*
		 * The grouping is done above the node, from partial counts, so the
		 * planner has to consider the aggregates splittable and a GROUP BY
		 * has to be hashable (a sorted Finalize Agg over groups of several
		 * rows would need a Sort over the node, which is not built for
		 * partial counts; groups of one row each need none, but are asked
		 * the same).
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
		 * or the fact's join column that a DISTINCT count reads or the GROUP
		 * BY takes (fkkey, checked above), which the node emits as the key.
		 * A DISTINCT aggregate groups by sorting, so a GROUP BY must be
		 * sortable.
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
			{
				if (!lion_fkjoin_child_has(fj, v))
					return;
				continue;
			}
			if (v->varno == fj->fkvar->varno &&
				v->varattno == fj->fkvar->varattno &&
				(fj->jointype == JOIN_INNER ||
				 fj->jointype == JOIN_UNIQUE_INNER))
				continue;
			return;
		}

		/*
		 * Counted rows: each carries its count of join pairs as one more
		 * column, a partial count(*), and the Agg's target and HAVING are
		 * the query's with every aggregate that sees multiplicity rewritten
		 * over it, in place - which needs the extension's aggregates.
		 */
		if (rowagg->counts)
		{
			LionFkAggRewrite cx;

			if (!lion_fkjoin_rewrite_init(&cx, fj,
										  nagg[LION_FKAGG_COUNT] +
										  nagg[LION_FKAGG_COUNTCOL] > 0,
										  nagg[LION_FKAGG_SUM] > 0))
				return;
			nodetarget = copy_pathtarget(nodetarget);
			add_column_to_pathtarget(nodetarget, (Expr *) cx.n, 0);
			nodetarget->width += sizeof(int64);
			rowagg->target = copy_pathtarget(output_rel->reltarget);
			rowagg->target->exprs = (List *)
				lion_fkjoin_rewrite_mutator((Node *) rowagg->target->exprs,
											&cx);
			rowagg->having = (List *)
				lion_fkjoin_rewrite_mutator((Node *) having, &cx);
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

	/*
	 * ---- the join key, the fact rel's indexes, and what every path carries
	 * of them (lion_fkjoin_setup()) ----
	 */
	if (!lion_fkjoin_setup(root, rel, fj, whereattnos, clauseinfos,
						   whereclauses, whereconsts, wherekinds, whereopnos,
						   whereinor, ors, imply, output_rel->reltarget->exprs,
						   having, fgvar != NULL ? &fgdrive : NULL, &setup))
		return;
	joinclause = setup.joinclause;
	targets = setup.targets;
	fkpages = setup.fkpages;
	whereclauses = setup.whereclauses;
	wherekinds = setup.wherekinds;
	consts = setup.consts;
	base = setup.base;

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
				fj->dimchild->consider_parallel &&
				is_parallel_safe(root, (Node *) consts));

	/*
	 * A GROUP BY of a key the rows never repeat makes each row its own group,
	 * and the Agg above them a trivial one (lion_add_fkjoin_paths()) - which
	 * the rows of a fact column's groups never are: a dimension row has one
	 * in each of its groups, and a partitioned fact's in each partition.
	 */
	perrow = (fgvar == NULL) && lion_fkjoin_groups_per_row(root, fj, fkkey);

	lion_add_fkjoin_paths(root, rel, output_rel, fj, having, targets,
						  fj->dimpath, 0, nodetarget, base, joinclause,
						  jointype, rowagg, perrow, whereclauses, wherekinds,
						  ors, dimrows, found,
						  parallel && fj->dimpath->parallel_safe);
	if (parallel && fj->jointype == JOIN_UNIQUE_INNER)
	{
		Path	   *whole = NULL;
		int			workers;

		/*
		 * A forward semi join over a non-unique key is parallel over its
		 * whole child, which every participant runs and sorts: the sorted,
		 * distinct keys are what they divide (lion_add_fkjoin_paths()).  So
		 * the child has to give every participant the same rows - it may run
		 * in a worker, and nothing in the dimension's quals is volatile,
		 * which could keep a row in one participant and drop it in another.
		 * It is the dimension's cheapest path that may run in a worker, which
		 * is not its cheapest path when that is a Gather - of its table, or of
		 * the join rel of a dimension that is a join, whose semi and anti
		 * joins and the tables inside them are asked the same.  The workers are
		 * what a parallel scan of the fk index pages the distinct keys read
		 * would get, as for the other joins; there is no partial child whose
		 * own count could be more.
		 */
		foreach(lc, fj->dimchild->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);

			if (p->parallel_safe && p->param_info == NULL &&
				(whole == NULL || compare_path_costs(p, whole, TOTAL_COST) < 0))
				whole = p;
		}
		workers = compute_parallel_worker(fj->dimrel, -1,
										  fkpages *
										  Min(dimrows /
											  lion_fkjoin_fk_ndistinct(root, rel,
																	   fj->fkvar),
											  1.0),
										  max_parallel_workers_per_gather);

		if (whole != NULL && workers > 0 && !lion_fkjoin_dim_volatile(root, fj))
			lion_add_fkjoin_paths(root, rel, output_rel, fj, having, targets,
								  whole, workers, nodetarget, base,
								  joinclause, jointype, rowagg, perrow,
								  whereclauses, wherekinds, ors, dimrows,
								  found, true);
	}
	else if (parallel && fj->dimchild->partial_pathlist != NIL)
	{
		Path	   *partial = (Path *) linitial(fj->dimchild->partial_pathlist);
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
		workers = compute_parallel_worker(fj->dimrel, -1,
										  fkpages *
										  Min(dimrows /
											  lion_fkjoin_fk_ndistinct(root, rel,
																	   fj->fkvar),
											  1.0),
										  max_parallel_workers_per_gather);
		if (fj->dimrel->rel_parallel_workers == -1)
			workers = Max(workers, partial->parallel_workers);

		if (partial->param_info == NULL && workers > 0)
			lion_add_fkjoin_paths(root, rel, output_rel, fj, having, targets,
								  partial, workers, nodetarget, base,
								  joinclause, jointype, rowagg, perrow,
								  whereclauses, wherekinds, ors, dimrows,
								  dimrows, true);
	}
}

/* Does a path's target carry the column `var` is of, as a plain Var? */
static bool
lion_path_has_var(Path *path, Var *var)
{
	ListCell   *lc;

	foreach(lc, path->pathtarget->exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (IsA(v, Var) && v->varno == var->varno &&
			v->varattno == var->varattno && v->varlevelsup == 0)
			return true;
	}
	return false;
}

/*
 * How many child rows a batch holds, as lion_join_fill_batch() fills it: rows
 * until the batch's memory context has work_mem allocated.  That is its first
 * block, the array of entries - 1,024 of them at first, doubled whenever it is
 * full - and each row's copy, `rowbytes` less its entry.  At a small work_mem
 * the first array is most of it: 64 kB holds some 300 rows of one column where
 * rowbytes alone would say 700.
 */
static double
lion_join_batch_rows(double rowbytes)
{
	double		limit = (double) work_mem * 1024.0;
	double		ent = (double) sizeof(LionJoinEnt);
	double		tup = Max(rowbytes - ent, 1.0);
	double		base = (double) ALLOCSET_DEFAULT_INITSIZE;
	double		prev = 0.0;
	double		cap;
	double		n = (double) LION_JOIN_BATCH_MAX;

	for (cap = 1024.0; cap < (double) LION_JOIN_BATCH_MAX; cap *= 2.0)
	{
		/* the rows beside an array of `cap` entries, while it holds them */
		n = ceil((limit - base - ent * cap) / tup);
		if (n <= cap)
		{
			/* the row that made the array grow is in the batch */
			n = Max(n, prev + 1.0);
			break;
		}
		prev = cap;
	}
	return Min(Max(n, 1.0), (double) LION_JOIN_BATCH_MAX);
}

/*
 * One semi or anti join path (DESIGN.md §27, "The semi and anti join as a
 * join path") over `child`, a path of the outer side: a serial one, or with
 * `workers` above zero a partial one over a partial child, each participant
 * testing the outer rows its share of the child returns.  `run` is what the
 * node does over the fact side for `childrows` outer rows (one participant's
 * share, in a partial path), and `collect` and `walk` its two choices, all of
 * them lion_cost_fkjoin_path()'s: they do not depend on which of the outer
 * side's paths the child is.
 *
 * The path's rows are the join rel's, core's estimate of the semi or anti
 * join - a participant's share of them, in a partial path - and it costs the
 * child, the lookups and the existence tests, and cpu_tuple_cost and the
 * target's cost per row it emits, as a join of core's does.
 *
 * STARTUP.  No row goes up before the first batch is tested whole: the
 * child's first `firstrows` rows, read at their share of its run, and
 * `startrun`, what the fact side does for them - the fact filters located
 * and collected, and the batch's lookups and existence tests - so that a
 * LIMIT above prices the wait.  A plan that looks the keys up a row at a time
 * has a batch of one row.
 *
 * ORDER.  A plan that looks the keys up a row at a time emits the child's
 * rows in the child's order.  One that walks the fk index in key order
 * (LION_JOINFLAG_WALK), and every plan over a partitioned fact, reads the
 * child a batch at a time and sorts the batch into the index's key order;
 * the join path counts the batch whole and hands its surviving rows up in
 * their own order again (LION_JOINFLAG_ORDERED, lion_join_batch_unsort()) -
 * so it claims the child's pathkeys, as a nested loop claims its outer
 * side's, where the join rel has a use for them (build_join_pathkeys()).
 */
static void
lion_add_semijoin_path(PlannerInfo *root, const LionFkJoin *fj,
					   const LionFkJoinSetup *setup, Path *child, int workers,
					   int jointype, Cost run, Cost startrun, double firstrows,
					   bool collect, bool walk, double childrows,
					   bool parallel_safe)
{
	RelOptInfo *joinrel = fj->joinrel;
	PathTarget *target = joinrel->reltarget;
	bool		partial = (workers > 0);
	CustomPath *cpath;
	List	   *pathkeys;
	double		rows;
	double		childshare;
	Cost		startup;
	Cost		total;
	int			flags;
	ListCell   *lc;

	/* the child's rows carry every outer column the join rel's do, and the key */
	if (!lion_path_has_var(child, fj->pkvar))
		return;
	foreach(lc, target->exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (bms_is_member(v->varno, fj->dimchild->relids) &&
			!lion_path_has_var(child, v))
			return;
	}

	pathkeys = build_join_pathkeys(root, joinrel, fj->jointype,
								   child->pathkeys);
	flags = LION_JOINFLAG_ROWS | LION_JOINFLAG_OUTER |
		(collect ? LION_JOINFLAG_COLLECT : 0) |
		(walk ? LION_JOINFLAG_WALK : 0) |
		(pathkeys != NIL ? LION_JOINFLAG_ORDERED : 0);

	rows = partial ?
		clamp_row_est(Min(joinrel->rows / lion_parallel_divisor(workers),
						  childrows)) :
		joinrel->rows;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = joinrel;
	cpath->path.pathtarget = target;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = partial;
	cpath->path.parallel_safe = parallel_safe;
	cpath->path.parallel_workers = workers;
	cpath->path.pathkeys = pathkeys;
	cpath->path.rows = rows;
	total = child->total_cost + run + target->cost.startup +
		rows * (cpu_tuple_cost + target->cost.per_tuple);
	childshare = Min(firstrows / Max(child->rows, 1.0), 1.0);
	startup = child->startup_cost +
		(child->total_cost - child->startup_cost) * childshare +
		startrun + target->cost.startup;
	cpath->path.startup_cost = Min(startup, total);
	cpath->path.total_cost = total;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = child->disabled_nodes;
#endif

	/*
	 * It projects, as core's joins do: a target list other than the join
	 * rel's - the query's own, over the topmost join - is computed by the
	 * node from its row (lion_plan_fkjoin_path()), with no Result above it.
	 */
	cpath->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cpath->custom_paths = list_make1(child);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = lion_fkjoin_private(setup->base,
												setup->joinclause, jointype,
												flags, InvalidOid, InvalidOid);
	cpath->methods = (jointype == LION_JOIN_ANTI) ?
		&lion_antijoin_path_methods : &lion_semijoin_path_methods;

	if (partial)
		add_partial_path(joinrel, &cpath->path);
	else
		add_path(joinrel, &cpath->path);
}

/*
 * THE SEMI OR ANTI JOIN AS A JOIN PATH (DESIGN.md §27, "The semi and anti
 * join as a join path"): the rest of lion_try_count_path() for a join rel of
 * an outer side semi- or anti-joined to the fact `rel` on one key
 * (lion_fkjoin_recognize_join(), fj->joinpath).  The caller has analysed the
 * fact's filters exactly as for the upper node; this adds the key, finds the
 * fact's indexes (lion_fkjoin_setup()) and adds a path of the join rel over
 * each path of the outer side, and a partial one over its partial paths.
 *
 * The node is the upper node's machinery with the upper node's rows: per
 * child row, the key looked up in the fact's fk index and tested for a
 * visible row under the fact filters (their collected copy or the probes;
 * key-order batches; a partitioned fact's leaves each looked up, any or none
 * of them matching; the §9 interlock per leaf), and the row emitted when the
 * semi join has a match, or when the anti join has none - a NULL key, which
 * joins nothing, included.  Its target is the join rel's, which holds only
 * columns of the outer side, read from the child's row, and in a semi join
 * the fact's key (below).  No uniqueness is asked: each outer row is tested
 * and emitted on its own, however many share its key.
 */
void
lion_try_semijoin_path(PlannerInfo *root, RelOptInfo *rel,
					   const LionFkJoin *fj, List *whereattnos,
					   List *clauseinfos, List *whereclauses,
					   List *whereconsts, List *wherekinds, List *whereopnos,
					   List *whereinor, List *ors, const LionImply *imply)
{
	RelOptInfo *joinrel = fj->joinrel;
	RelOptInfo *outerrel = fj->dimchild;
	PathTarget *target = joinrel->reltarget;
	LionFkJoinSetup setup;
	int			jointype;
	double		dimrows;
	double		rowbytes;
	double		perbatch;
	double		firstrows;
	bool		parallel;
	bool		partitioned;
	bool		collect;
	bool		walk;
	Cost		run;
	Cost		startrun;
	LionUnits	units;
	ListCell   *lc;

	/*
	 * ---- what the join rel's rows carry ----
	 *
	 * Columns of the outer side, which the node reads from its child's row -
	 * of any kind (a system column, a whole-row Var): it passes them through
	 * as they come.  A semi join's rows may carry the fact's join column too:
	 * core's semi join hands up its first matching fact row's, which an
	 * equivalence class of the key needs above the join - `EXISTS` on two
	 * facts correlated to one key makes one class of the three columns, and
	 * the join with the second fact may compare the first fact's column with
	 * the second's.  The node emits the outer row's key in its place, which is
	 * every matching fact row's value when the join operator is the type's own
	 * equality and implies one representation (lion_fkjoin_fk_is_key(), §10's
	 * value rule).  Anything else - a PlaceHolderVar, another fact column, an
	 * anti join's fact column, which has no row to come from - declines.
	 */
	foreach(lc, target->exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0)
			return;
		if (bms_is_member(v->varno, outerrel->relids))
			continue;
		if (v->varno == fj->fkvar->varno &&
			v->varattno == fj->fkvar->varattno &&
			lion_fkjoin_fk_is_key(fj))
			continue;
		return;
	}

	/* ---- the join key and the fact's indexes ---- */
	if (!lion_fkjoin_setup(root, rel, fj, whereattnos, clauseinfos,
						   whereclauses, whereconsts, wherekinds, whereopnos,
						   whereinor, ors, imply, target->exprs, NIL, NULL,
						   &setup))
		return;
	jointype = (fj->jointype == JOIN_SEMI) ? LION_JOIN_SEMI : LION_JOIN_ANTI;

	/*
	 * May it run in a worker: the rules of the upper node (lion_try_fkjoin_path()),
	 * with the join rel in the grouped rel's place.
	 */
	parallel = (joinrel->consider_parallel && rel->consider_parallel &&
				outerrel->consider_parallel &&
				is_parallel_safe(root, (Node *) setup.consts));

	/*
	 * ---- serial: over each path of the outer side ----
	 *
	 * Every outer row is tested: an existence test per row, priced as the
	 * upper node prices a reverse semi join's (found = every row, "Cost,
	 * revisited"), with nothing taken off for the rows core's estimate says
	 * have no match.  What the node does over the fact side is the same over
	 * every path of the outer side, which differ in their own cost and order
	 * only.
	 */
	dimrows = clamp_row_est(outerrel->rows);
	rowbytes = (double) LION_FKJOIN_BATCH_ENT_BYTES +
		MAXALIGN(outerrel->reltarget->width);

	/*
	 * Priced in the units of the cheapest of core's joins the join rel has
	 * (DESIGN.md §39): the node's own work over the fact side, `run` and
	 * `startrun`; the child and the rows it emits are core's prices.
	 */
	lion_units_for(joinrel, &units);
	run = lion_cost_fkjoin_path(root, rel, setup.targets, fj->fkvar,
								setup.joinclause, setup.whereclauses,
								setup.wherekinds, ors, dimrows, dimrows, true,
								rowbytes, 0, &collect, &walk, false);

	/*
	 * The first batch, which is all tested before a row goes up (the startup
	 * cost, lion_add_semijoin_path()): the rows work_mem holds
	 * (lion_join_batch_rows()) when the keys are looked up in key order or
	 * over a partitioned fact's leaves, and otherwise one row - never more
	 * than the child's - priced under the choices the whole run made.
	 */
	partitioned = (list_length(setup.targets) > 1 ||
				   ((LionCountTarget *) linitial(setup.targets))->rel != rel);
	perbatch = lion_join_batch_rows(rowbytes);
	firstrows = (walk || partitioned) ? Min(perbatch, dimrows) : 1.0;
	startrun = lion_cost_fkjoin_path(root, rel, setup.targets, fj->fkvar,
									 setup.joinclause, setup.whereclauses,
									 setup.wherekinds, ors, firstrows,
									 firstrows, true, rowbytes, 0, &collect,
									 &walk, true);
	run = lion_units_price(&units, run);
	startrun = lion_units_price(&units, startrun);
	foreach(lc, outerrel->pathlist)
	{
		Path	   *child = (Path *) lfirst(lc);

		if (child->param_info != NULL)
			continue;
		lion_add_semijoin_path(root, fj, &setup, child, 0, jointype, run,
							   startrun, firstrows, collect, walk, dimrows,
							   parallel && child->parallel_safe);
	}

	/*
	 * ---- parallel: over each partial path of the outer side ----
	 *
	 * The upper node's rules ("Parallel"): each participant tests the rows its
	 * share of the child returns, collecting the fact filters' copy with the
	 * others where the plan collects them ("One copy per query"), and the
	 * workers are what a parallel scan of the fk index pages the lookups read
	 * would get, or the child's, whichever is more - unless the key's table
	 * has a parallel_workers setting, which decides alone.  Core puts the
	 * Gather above it, as over any partial path of a join rel.
	 */
	if (parallel && outerrel->partial_pathlist != NIL)
	{
		Path	   *cheapest = (Path *) linitial(outerrel->partial_pathlist);
		int			workers;

		workers = compute_parallel_worker(fj->dimrel, -1,
										  setup.fkpages *
										  Min(dimrows /
											  lion_fkjoin_fk_ndistinct(root, rel,
																	   fj->fkvar),
											  1.0),
										  max_parallel_workers_per_gather);
		if (fj->dimrel->rel_parallel_workers == -1)
			workers = Max(workers, cheapest->parallel_workers);
		if (workers > 0)
		{
			double		childrows = clamp_row_est(dimrows /
												  lion_parallel_divisor(workers));

			run = lion_cost_fkjoin_path(root, rel, setup.targets, fj->fkvar,
										setup.joinclause, setup.whereclauses,
										setup.wherekinds, ors, childrows,
										childrows, true, rowbytes, workers,
										&collect, &walk, false);
			/* a participant's first batch, of its share of the child */
			firstrows = (walk || partitioned) ? Min(perbatch, childrows) : 1.0;
			startrun = lion_cost_fkjoin_path(root, rel, setup.targets,
											 fj->fkvar, setup.joinclause,
											 setup.whereclauses,
											 setup.wherekinds, ors, firstrows,
											 firstrows, true, rowbytes,
											 workers, &collect, &walk, true);
			run = lion_units_price(&units, run);
			startrun = lion_units_price(&units, startrun);
			foreach(lc, outerrel->partial_pathlist)
			{
				Path	   *child = (Path *) lfirst(lc);

				if (child->param_info != NULL)
					continue;
				lion_add_semijoin_path(root, fj, &setup, child, workers,
									   jointype, run, startrun, firstrows,
									   collect, walk, childrows, true);
			}
		}
	}
}
