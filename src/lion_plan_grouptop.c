/*-------------------------------------------------------------------------
 *
 * lion_plan_grouptop.c
 *		The first rows of each group, planned again as a LATERAL probe of
 *		each group's key (DESIGN.md §57).
 *
 *	  SELECT DISTINCT ON (country) country, id, ts
 *	    FROM ev WHERE status = 'paid' ORDER BY country, ts DESC;
 *
 *	  SELECT * FROM (SELECT id, country, ts, row_number() OVER
 *	                   (PARTITION BY country ORDER BY ts DESC) AS rn
 *	                   FROM ev WHERE status = 'paid') s
 *	   WHERE rn <= 3;
 *
 *	  SELECT country, max(ts) FROM ev WHERE status = 'paid' GROUP BY country;
 *
 * PostgreSQL answers each by reading every paid row, sorting or hashing
 * them by country, and keeping the first one or three of each group (or the
 * greatest ts).  Each needs only the first k rows of each group in one
 * order, and those the same query finds one group at a time,
 *
 *	  SELECT ... FROM (SELECT DISTINCT country FROM ev
 *	                    WHERE status = 'paid') k,
 *	       LATERAL (SELECT ... FROM ev
 *	                 WHERE status = 'paid' AND country = k.country
 *	                 ORDER BY ts DESC LIMIT 1) l
 *
 * where lion lists the keys that have such rows (LionCount's first-visible-
 * row grouping) and an ordered index walks each key's first rows.  The
 * rewrite replaces the query's one relation with that join, and leaves the
 * rest of the query - the DISTINCT ON and its ORDER BY, the window and the
 * filter on it, the grouping and its aggregates - as it was, now over the
 * rows the probes return: the first k of each group, which give the same
 * answer.
 *
 * Which is faster depends on how many groups there are and how far each
 * probe walks, which only the planner can price: the query is planned as
 * written and as rewritten, by the planner hook of lion_plan_lateral.c
 * (§56), and the cheaper plan kept.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/multibitmapset.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "lion_customscan.h"

/* pg_lion.enable_lateral_tops */
bool		lion_enable_lateral_tops = true;

/*
 * What a query keeps of its relation's rows: the first k of each group of
 * the grouping column, in an order.
 */
typedef struct LionTopShape
{
	Index		ti;				/* the relation's range table index */
	Var		   *gvar;			/* the grouping column, a Var of ti */
	SortGroupClause *gsgc;		/* its grouping clause: eqop, sortop */
	List	   *order;			/* expressions of ti: the order in a group */
	List	   *ordsgc;			/* and their sort clauses */
	int64		k;				/* rows kept of each group */
} LionTopShape;

/*
 * Does the tree read anything of a query level above the one it starts in:
 * a Var, PlaceHolderVar, aggregate or GROUPING() of an outer query?
 */
static bool
lion_top_upper_walker(Node *node, int *depth)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		return ((Var *) node)->varlevelsup > (Index) *depth;
	if (IsA(node, PlaceHolderVar) &&
		((PlaceHolderVar *) node)->phlevelsup > (Index) *depth)
		return true;
	if (IsA(node, Aggref) && ((Aggref *) node)->agglevelsup > (Index) *depth)
		return true;
	if (IsA(node, GroupingFunc) &&
		((GroupingFunc *) node)->agglevelsup > (Index) *depth)
		return true;
	if (IsA(node, Query))
	{
		bool		found;

		(*depth)++;
		found = query_tree_walker((Query *) node, lion_top_upper_walker,
								  depth, 0);
		(*depth)--;
		return found;
	}
	return expression_tree_walker(node, lion_top_upper_walker, depth);
}

/* Does the tree read relation *ti's whole row? */
static bool
lion_top_wholerow_walker(Node *node, Index *ti)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		return ((Var *) node)->varlevelsup == 0 &&
			((Var *) node)->varno == (int) *ti &&
			((Var *) node)->varattno == 0;
	return expression_tree_walker(node, lion_top_wholerow_walker, ti);
}

/*
 * The range table index of q's one relation, or 0 where q is not a plain
 * SELECT over a single table whose rows the rewrite may replace: no set
 * operation, set-returning function in the target list, sublink, row lock,
 * CTE, row-level security, TABLESAMPLE, volatile function, reference to an
 * outer query or read of the relation's whole row (the probe's row is
 * another type), and nothing in the range table but the relation (and, from
 * PostgreSQL 18 on, the grouping's RTE).
 */
static Index
lion_top_relation(Query *q)
{
	RangeTblRef *rtr;
	RangeTblEntry *rte;
	ListCell   *lc;
	Index		i;
	int			depth = 0;

	if (q->commandType != CMD_SELECT || q->utilityStmt != NULL ||
		q->setOperations != NULL || q->hasTargetSRFs || q->hasSubLinks ||
		q->rowMarks != NIL || q->hasForUpdate || q->cteList != NIL ||
		q->hasRecursive || q->hasModifyingCTE || q->hasRowSecurity ||
		q->groupingSets != NIL || q->jointree == NULL ||
		list_length(q->jointree->fromlist) != 1 ||
		!IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return 0;
	rtr = (RangeTblRef *) linitial(q->jointree->fromlist);
	i = 0;
	foreach(lc, q->rtable)
	{
		i++;
		if (i == (Index) rtr->rtindex)
			continue;
#if PG_VERSION_NUM >= 180000
		if (((RangeTblEntry *) lfirst(lc))->rtekind == RTE_GROUP)
			continue;
#endif
		return 0;
	}
	rte = rt_fetch(rtr->rtindex, q->rtable);
	if (rte->rtekind != RTE_RELATION || rte->securityQuals != NIL ||
		rte->tablesample != NULL ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW &&
		 rte->relkind != RELKIND_PARTITIONED_TABLE))
		return 0;
	if (query_tree_walker(q, lion_top_upper_walker, &depth, 0))
		return 0;
	i = (Index) rtr->rtindex;
	if (query_tree_walker(q, lion_top_wholerow_walker, &i, 0))
		return 0;
	if (contain_volatile_functions((Node *) q))
		return 0;
	return (Index) rtr->rtindex;
}

/*
 * The column of relation ti that expression e of q's target list is, or
 * NULL: a plain Var of it (read through the grouping's RTE from PostgreSQL
 * 18 on).
 */
static Var *
lion_top_column(Query *q, Index ti, Node *e)
{
	Var		   *var;

	if (!IsA(e, Var))
		return NULL;
	var = (Var *) e;
	if (var->varlevelsup != 0)
		return NULL;
#if PG_VERSION_NUM >= 180000
	if (var->varno != (int) ti)
	{
		RangeTblEntry *rte = rt_fetch(var->varno, q->rtable);
		Node	   *g;

		if (rte->rtekind != RTE_GROUP ||
			var->varattno <= 0 || var->varattno > list_length(rte->groupexprs))
			return NULL;
		g = (Node *) list_nth(rte->groupexprs, var->varattno - 1);
		if (!IsA(g, Var))
			return NULL;
		var = (Var *) g;
		if (var->varlevelsup != 0)
			return NULL;
	}
#endif
	if (var->varno != (int) ti || var->varattno <= 0)
		return NULL;
	return var;
}

/*
 * Is column g of relation ti never NULL in a row q reads?  The probe of a
 * key is an equality, which no NULL meets, so a NULL group would be lost.
 * Either the column is declared NOT NULL (by a valid constraint, from 18 on;
 * not trusted on an inheritance parent with children, which may disagree),
 * or the WHERE rejects a NULL in it.
 */
static bool
lion_top_notnull(Query *q, Index ti, Var *g)
{
	RangeTblEntry *rte = rt_fetch(ti, q->rtable);
	Relation	rel;
	bool		notnull = false;

	rel = table_open(rte->relid, NoLock);
	if (!rte->inh || rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
		!has_subclass(rte->relid))
	{
		if (g->varattno <= RelationGetNumberOfAttributes(rel))
		{
#if PG_VERSION_NUM >= 180000
			CompactAttribute *att = TupleDescCompactAttr(RelationGetDescr(rel),
														 g->varattno - 1);

			notnull = att->attnullability == ATTNULLABLE_VALID &&
				!att->attisdropped;
#else
			Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel),
												  g->varattno - 1);

			notnull = att->attnotnull && !att->attisdropped;
#endif
		}
	}
	table_close(rel, NoLock);
	if (notnull)
		return true;
	return mbms_is_member((int) ti,
						  g->varattno - FirstLowInvalidHeapAttributeNumber,
						  find_nonnullable_vars(q->jointree->quals));
}

/*
 * The grouping column the sort or grouping clause sgc of q names, when the
 * rewrite may probe it: a plain column of relation ti, with a lion index
 * among whose key columns it is, compared by an equality, and never NULL in
 * the rows q reads.
 */
static Var *
lion_top_group_column(Query *q, Index ti, SortGroupClause *sgc)
{
	TargetEntry *tle;
	Var		   *g;

	if (sgc == NULL || !OidIsValid(sgc->eqop))
		return NULL;
	tle = get_sortgroupclause_tle(sgc, q->targetList);
	g = lion_top_column(q, ti, (Node *) tle->expr);
	if (g == NULL)
		return NULL;
	if (!lion_lateral_lion_key(rt_fetch(ti, q->rtable)->relid, g->varattno))
		return NULL;
	if (!lion_top_notnull(q, ti, g))
		return NULL;
	return g;
}

/*
 * The expressions and sort clauses of q's target list that the sort clauses
 * `clauses` name.
 */
static void
lion_top_order(Query *q, List *clauses, LionTopShape *s)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, q->targetList);

		s->order = lappend(s->order, copyObject(tle->expr));
		s->ordsgc = lappend(s->ordsgc, copyObject(sgc));
	}
}

/*
 * SELECT DISTINCT ON (g) ... ORDER BY g, <order>: the first row of each
 * group in <order>.
 */
static bool
lion_top_distinct_on(Query *q, LionTopShape *s)
{
	SortGroupClause *d;
	SortGroupClause *first;

	if (!q->hasDistinctOn || list_length(q->distinctClause) != 1 ||
		q->hasAggs || q->groupClause != NIL || q->hasWindowFuncs ||
		q->havingQual != NULL || q->sortClause == NIL)
		return false;
	d = (SortGroupClause *) linitial(q->distinctClause);
	first = (SortGroupClause *) linitial(q->sortClause);
	if (first->tleSortGroupRef != d->tleSortGroupRef)
		return false;
	s->ti = lion_top_relation(q);
	if (s->ti == 0)
		return false;
	s->gvar = lion_top_group_column(q, s->ti, d);
	if (s->gvar == NULL)
		return false;
	s->gsgc = d;
	lion_top_order(q, list_delete_first(list_copy(q->sortClause)), s);
	s->k = 1;
	return true;
}

/* The aggregates of a tree, not looking inside them. */
static bool
lion_top_aggs_walker(Node *node, List **aggs)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		*aggs = lappend(*aggs, node);
		return false;
	}
	return expression_tree_walker(node, lion_top_aggs_walker, aggs);
}

/*
 * SELECT g, max(x) ... GROUP BY g, every aggregate one whose result is the
 * first of its argument in its sort operator's order (min, max, bool_and,
 * bool_or, every: pg_aggregate.aggsortop), all of one argument and one
 * operator: the first row of each group in that order, NULLs last, since
 * the aggregates skip them.
 */
static bool
lion_top_minmax(Query *q, LionTopShape *s)
{
	List	   *aggs = NIL;
	ListCell   *lc;
	Oid			sortop = InvalidOid;
	Expr	   *arg = NULL;
	SortGroupClause *osgc;
	bool		reverse = false;
	Var		   *argvar;

	if (!q->hasAggs || list_length(q->groupClause) != 1 ||
		q->hasWindowFuncs || q->hasDistinctOn || q->constraintDeps != NIL)
		return false;
	(void) lion_top_aggs_walker((Node *) q->targetList, &aggs);
	(void) lion_top_aggs_walker(q->havingQual, &aggs);
	if (aggs == NIL)
		return false;
	foreach(lc, aggs)
	{
		Aggref	   *agg = (Aggref *) lfirst(lc);
		HeapTuple	tup;
		Oid			aop;
		Expr	   *a;

		if (agg->aggkind != AGGKIND_NORMAL || agg->agglevelsup != 0 ||
			agg->aggfilter != NULL || agg->aggdistinct != NIL ||
			agg->aggorder != NIL || agg->aggdirectargs != NIL ||
			list_length(agg->args) != 1)
			return false;
		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(agg->aggfnoid));
		if (!HeapTupleIsValid(tup))
			return false;
		aop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
		ReleaseSysCache(tup);
		if (!OidIsValid(aop))
			return false;
		a = ((TargetEntry *) linitial(agg->args))->expr;
		if (arg == NULL)
		{
			arg = a;
			sortop = aop;
		}
		else if (aop != sortop || !equal(a, arg))
			return false;
	}
	s->ti = lion_top_relation(q);
	if (s->ti == 0)
		return false;
	s->gsgc = (SortGroupClause *) linitial(q->groupClause);
	s->gvar = lion_top_group_column(q, s->ti, s->gsgc);
	if (s->gvar == NULL)
		return false;

	osgc = makeNode(SortGroupClause);
	osgc->sortop = sortop;
	osgc->eqop = get_equality_op_for_ordering_op(sortop, &reverse);
	if (!OidIsValid(osgc->eqop))
		return false;
#if PG_VERSION_NUM >= 180000
	osgc->reverse_sort = reverse;
#endif
	osgc->hashable = false;

	/*
	 * NULLs last, so that the first row has a value where the group has one.
	 * Where the argument is a column that has none, the order's own default
	 * (NULLs first for a descending one) is taken, which an index's default
	 * ordering provides.
	 */
	argvar = IsA(arg, Var) ? lion_top_column(q, s->ti, (Node *) arg) : NULL;
	osgc->nulls_first = argvar != NULL && reverse &&
		lion_top_notnull(q, s->ti, argvar);
	s->order = list_make1(copyObject(arg));
	s->ordsgc = list_make1(osgc);
	s->k = 1;
	return true;
}

/*
 * The number of rows a qual of o keeps of the column rncol of subquery si,
 * a row_number(): rn <= c, rn < c, rn = c, either way round, c a constant
 * integer; -1 for anything else.
 */
static int64
lion_top_rn_bound(Node *qual, Index si, AttrNumber rncol)
{
	OpExpr	   *op;
	Node	   *l;
	Node	   *r;
	Var		   *v;
	Const	   *c;
	bool		varleft;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;
	int64		val;

	if (!IsA(qual, OpExpr) || list_length(((OpExpr *) qual)->args) != 2)
		return -1;
	op = (OpExpr *) qual;
	l = (Node *) linitial(op->args);
	r = (Node *) lsecond(op->args);
	if (IsA(l, Var) && IsA(r, Const))
	{
		v = (Var *) l;
		c = (Const *) r;
		varleft = true;
	}
	else if (IsA(r, Var) && IsA(l, Const))
	{
		v = (Var *) r;
		c = (Const *) l;
		varleft = false;
	}
	else
		return -1;
	if (v->varno != (int) si || v->varattno != rncol || v->varlevelsup != 0 ||
		c->constisnull)
		return -1;
	if (!op_in_opfamily(op->opno, INTEGER_BTREE_FAM_OID))
		return -1;
	get_op_opfamily_properties(op->opno, INTEGER_BTREE_FAM_OID, false,
							   &strategy, &lefttype, &righttype);
	switch (c->consttype)
	{
		case INT2OID:
			val = DatumGetInt16(c->constvalue);
			break;
		case INT4OID:
			val = DatumGetInt32(c->constvalue);
			break;
		case INT8OID:
			val = DatumGetInt64(c->constvalue);
			break;
		default:
			return -1;
	}
	if (!varleft)
	{
		if (strategy == BTLessStrategyNumber)
			strategy = BTGreaterStrategyNumber;
		else if (strategy == BTLessEqualStrategyNumber)
			strategy = BTGreaterEqualStrategyNumber;
		else if (strategy == BTGreaterStrategyNumber)
			strategy = BTLessStrategyNumber;
		else if (strategy == BTGreaterEqualStrategyNumber)
			strategy = BTLessEqualStrategyNumber;
	}
	switch (strategy)
	{
		case BTLessStrategyNumber:
			return val <= 0 ? 0 : val - 1;
		case BTLessEqualStrategyNumber:
		case BTEqualStrategyNumber:
			return val <= 0 ? 0 : val;
		default:
			return -1;
	}
}

/* Every window function of the tree, not looking inside them. */
static bool
lion_top_winfuncs_walker(Node *node, List **funcs)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
	{
		*funcs = lappend(*funcs, node);
		return false;
	}
	return expression_tree_walker(node, lion_top_winfuncs_walker, funcs);
}

/*
 * Subquery si of o, SELECT ..., row_number() OVER (PARTITION BY g ORDER BY
 * <order>) AS rn FROM t WHERE ..., which o's WHERE filters by rn <= k: the
 * first k rows of each group in <order>.  The filter is one of the WHERE's
 * ANDed clauses, so that a row of the subquery it rejects reaches nothing
 * of o's result, whatever the joins.  Every window function of the
 * subquery is that row_number(), and nothing of it - a LIMIT, DISTINCT or
 * aggregate - sees all its rows.
 */
static bool
lion_top_window(Query *o, Index si, LionTopShape *s)
{
	Query	   *w = rt_fetch(si, o->rtable)->subquery;
	WindowClause *wc;
	List	   *funcs = NIL;
	ListCell   *lc;
	AttrNumber	col = 0;
	AttrNumber	rncol = 0;
	int64		k = -1;

	if (w == NULL || !w->hasWindowFuncs || list_length(w->windowClause) != 1 ||
		w->hasAggs || w->groupClause != NIL || w->havingQual != NULL ||
		w->distinctClause != NIL || w->hasDistinctOn ||
		w->limitCount != NULL || w->limitOffset != NULL)
		return false;
	wc = (WindowClause *) linitial(w->windowClause);
	if (list_length(wc->partitionClause) != 1)
		return false;
	(void) lion_top_winfuncs_walker((Node *) w->targetList, &funcs);
	foreach(lc, funcs)
	{
		WindowFunc *f = (WindowFunc *) lfirst(lc);

		if (f->winfnoid != F_ROW_NUMBER || f->winref != wc->winref ||
			f->args != NIL || f->aggfilter != NULL)
			return false;
	}

	/* the subquery's output column that is the row_number() */
	foreach(lc, w->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			continue;
		col++;
		if (IsA(tle->expr, WindowFunc))
		{
			rncol = col;
			break;
		}
	}
	/* a volatile WHERE would run as many times as there are rows */
	if (rncol == 0 || o->jointree == NULL ||
		contain_volatile_functions(o->jointree->quals))
		return false;
	foreach(lc, make_ands_implicit((Expr *) o->jointree->quals))
	{
		int64		b = lion_top_rn_bound((Node *) lfirst(lc), si, rncol);

		if (b >= 0 && (k < 0 || b < k))
			k = b;
	}
	if (k < 0)
		return false;

	s->ti = lion_top_relation(w);
	if (s->ti == 0)
		return false;
	s->gsgc = (SortGroupClause *) linitial(wc->partitionClause);
	s->gvar = lion_top_group_column(w, s->ti, s->gsgc);
	if (s->gvar == NULL)
		return false;
	lion_top_order(w, wc->orderClause, s);
	s->k = k;
	return true;
}

/*
 * q's Vars of relation ti, renumbered to the probe's columns, or the
 * grouping column's to the key's.
 */
typedef struct LionTopRemap
{
	Index		ti;
	List	   *attnos;			/* the probe's columns: ti's attnos */
	AttrNumber	gattno;			/* the grouping column read from the keys */
	Index		krti;			/* the keys' range table index */
} LionTopRemap;

static Node *
lion_top_remap_mutator(Node *node, LionTopRemap *cx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == (int) cx->ti && var->varlevelsup == 0 &&
			var->varattno == cx->gattno)
		{
			Var		   *nv = copyObject(var);

			nv->varno = cx->krti;
			nv->varattno = 1;
			nv->varnosyn = cx->krti;
			nv->varattnosyn = 1;
			return (Node *) nv;
		}
		if (var->varno == (int) cx->ti && var->varlevelsup == 0)
		{
			Var		   *nv = copyObject(var);
			int			pos = -1;
			ListCell   *lc;

			foreach(lc, cx->attnos)
			{
				if (lfirst_int(lc) == var->varattno)
				{
					pos = foreach_current_index(lc);
					break;
				}
			}
			if (pos < 0)
				elog(ERROR, "lion: column %d of a grouped probe not found",
					 var->varattno);
			nv->varattno = (AttrNumber) (pos + 1);
			nv->varnosyn = cx->ti;
			nv->varattnosyn = nv->varattno;
			return (Node *) nv;
		}
		return (Node *) copyObject(var);
	}
	return expression_tree_mutator(node, lion_top_remap_mutator, cx);
}

/* A query reading relation rte, with q's permission entry for it. */
static Query *
lion_top_query(Query *q, RangeTblEntry *rte, Node *quals)
{
	Query	   *n = makeNode(Query);
	RangeTblEntry *nrte = copyObject(rte);
	RangeTblRef *rtr = makeNode(RangeTblRef);

	n->commandType = CMD_SELECT;
	n->querySource = QSRC_ORIGINAL;
	n->canSetTag = true;
	if (rte->perminfoindex != 0)
	{
		n->rteperminfos =
			list_make1(copyObject(list_nth(q->rteperminfos,
										   rte->perminfoindex - 1)));
		nrte->perminfoindex = 1;
	}
	n->rtable = list_make1(nrte);
	rtr->rtindex = 1;
	n->jointree = makeFromExpr(list_make1(rtr), quals);
	return n;
}

/*
 * Replace relation s->ti of q with
 *
 *	  (SELECT DISTINCT g FROM t WHERE <q's WHERE>) k,
 *	  LATERAL (SELECT <t's columns q reads> FROM t
 *	           WHERE <q's WHERE> AND g = k.g ORDER BY <order> LIMIT k) l
 *
 * l taking the relation's place in the range table and its columns those of
 * the relation q reads, renumbered, so that the rest of q reads them from l.
 */
static void
lion_top_apply(Query *q, LionTopShape *s)
{
	RangeTblEntry *trte = rt_fetch(s->ti, q->rtable);
	Index		krti = (Index) list_length(q->rtable) + 1;
	LionTopRemap remap;
	List	   *outside = NIL;
	List	   *vars;
	ListCell   *lc;
	Query	   *ql;
	Query	   *qk;
	Node	   *quals;
	Var		   *g;
	Var		   *kv;
	Expr	   *eq;
	List	   *colnames = NIL;
	int			n = 0;
	RangeTblEntry *lrte;
	RangeTblEntry *krte;
	RangeTblRef *krtr;
	RangeTblRef *lrtr;
	TargetEntry *ktle;
	SortGroupClause *ksgc;

	/* the columns of the relation q reads outside its WHERE */
	outside = list_make2(q->targetList, q->havingQual);
#if PG_VERSION_NUM >= 180000
	foreach(lc, q->rtable)
	{
		RangeTblEntry *e = (RangeTblEntry *) lfirst(lc);

		if (e->rtekind == RTE_GROUP)
			outside = lappend(outside, e->groupexprs);
	}
#endif
	vars = pull_var_clause((Node *) outside,
						   PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
						   PVC_RECURSE_PLACEHOLDERS);
	remap.ti = s->ti;
	remap.attnos = NIL;
	remap.krti = krti;

	/*
	 * The grouping column is read from the keys where a key and the rows
	 * equal to it are spelled alike - so that the query prints the same - and
	 * compared by the type's own equality: its estimates then see one row per
	 * key, where the probe's column, whose rows are a probe's few, would make
	 * the groups a single one.
	 */
	{
		Oid			base = getBaseType(s->gvar->vartype);
		TypeCacheEntry *typentry = lookup_type_cache(base, TYPECACHE_EQ_OPR);

		remap.gattno = (s->gsgc->eqop == typentry->eq_opr &&
						lion_type_equalimage(base, s->gvar->varcollid)) ?
			s->gvar->varattno : InvalidAttrNumber;
	}
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (v->varno == (int) s->ti && v->varlevelsup == 0 &&
			v->varattno != remap.gattno)
			remap.attnos = list_append_unique_int(remap.attnos, v->varattno);
	}

	/* the probe: one key's first k rows */
	quals = copyObject(q->jointree->quals);
	ChangeVarNodes(quals, s->ti, 1, 0);
	g = copyObject(s->gvar);
	ChangeVarNodes((Node *) g, s->ti, 1, 0);
	kv = makeVar(krti, 1, g->vartype, g->vartypmod, g->varcollid, 1);
	eq = make_opclause(s->gsgc->eqop, BOOLOID, false, (Expr *) g, (Expr *) kv,
					   InvalidOid, g->varcollid);
	set_opfuncid((OpExpr *) eq);
	ql = lion_top_query(q, trte, make_and_qual(quals, (Node *) eq));
	foreach(lc, remap.attnos)
	{
		AttrNumber	attno = (AttrNumber) lfirst_int(lc);
		Var		   *v = makeVar(1, attno, InvalidOid, -1, InvalidOid, 0);
		char	   *name = get_attname(trte->relid, attno, false);
		Oid			typid;
		int32		typmod;
		Oid			collid;

		get_atttypetypmodcoll(trte->relid, attno, &typid, &typmod, &collid);
		v->vartype = typid;
		v->vartypmod = typmod;
		v->varcollid = collid;
		ql->targetList = lappend(ql->targetList,
								 makeTargetEntry((Expr *) v, (AttrNumber) ++n,
												 name, false));
		colnames = lappend(colnames, makeString(pstrdup(name)));
	}
	{
		ListCell   *le;
		ListCell   *ls;
		Index		ref = 0;

		forboth(le, s->order, ls, s->ordsgc)
		{
			Expr	   *e = (Expr *) copyObject(lfirst(le));
			SortGroupClause *sgc = (SortGroupClause *) copyObject(lfirst(ls));
			TargetEntry *tle;

			ChangeVarNodes((Node *) e, s->ti, 1, 0);
			tle = makeTargetEntry(e, (AttrNumber) ++n, NULL, true);
			tle->ressortgroupref = ++ref;
			sgc->tleSortGroupRef = ref;
			ql->targetList = lappend(ql->targetList, tle);
			ql->sortClause = lappend(ql->sortClause, sgc);
		}
	}
	ql->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
										sizeof(int64), Int64GetDatum(s->k),
										false, FLOAT8PASSBYVAL);
	ql->limitOption = LIMIT_OPTION_COUNT;

	/* the keys: those with a row the WHERE keeps */
	quals = copyObject(q->jointree->quals);
	ChangeVarNodes(quals, s->ti, 1, 0);
	qk = lion_top_query(q, trte, quals);
	g = copyObject(s->gvar);
	ChangeVarNodes((Node *) g, s->ti, 1, 0);
	ktle = makeTargetEntry((Expr *) g, 1,
						   get_attname(trte->relid, s->gvar->varattno, false),
						   false);
	ktle->ressortgroupref = 1;
	qk->targetList = list_make1(ktle);
	ksgc = copyObject(s->gsgc);
	ksgc->tleSortGroupRef = 1;
	qk->distinctClause = list_make1(ksgc);

	/* q reads them in the relation's place */
	lrte = makeNode(RangeTblEntry);
	lrte->rtekind = RTE_SUBQUERY;
	lrte->subquery = ql;
	lrte->lateral = true;
	lrte->inFromCl = true;
	lrte->eref = makeAlias(trte->eref->aliasname, colnames);
	krte = makeNode(RangeTblEntry);
	krte->rtekind = RTE_SUBQUERY;
	krte->subquery = qk;
	krte->inFromCl = true;
	krte->eref = makeAlias("lion_keys",
						   list_make1(makeString(pstrdup(ktle->resname))));
	list_nth_cell(q->rtable, s->ti - 1)->ptr_value = lrte;
	q->rtable = lappend(q->rtable, krte);
	q->rteperminfos = NIL;
	krtr = makeNode(RangeTblRef);
	krtr->rtindex = krti;
	lrtr = makeNode(RangeTblRef);
	lrtr->rtindex = s->ti;
	q->jointree = makeFromExpr(list_make2(krtr, lrtr), NULL);

	q->targetList = (List *) lion_top_remap_mutator((Node *) q->targetList,
													&remap);
	q->havingQual = lion_top_remap_mutator(q->havingQual, &remap);
#if PG_VERSION_NUM >= 180000
	foreach(lc, q->rtable)
	{
		RangeTblEntry *e = (RangeTblEntry *) lfirst(lc);

		if (e->rtekind == RTE_GROUP)
			e->groupexprs = (List *) lion_top_remap_mutator((Node *) e->groupexprs,
															&remap);
	}
#endif
}

/*
 * Rewrite every shape of q and of the subqueries in its FROM list, at any
 * depth; true if any was.
 */
static bool
lion_top_walk(Query *q)
{
	bool		any = false;
	Index		si;
	LionTopShape s;

	for (si = 1; si <= (Index) list_length(q->rtable); si++)
	{
		RangeTblEntry *rte = rt_fetch(si, q->rtable);

		if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
			continue;
		memset(&s, 0, sizeof(s));
		if (lion_top_window(q, si, &s))
		{
			lion_top_apply(rte->subquery, &s);
			any = true;
		}
		else if (lion_top_walk(rte->subquery))
			any = true;
	}
	memset(&s, 0, sizeof(s));
	if (!lion_top_distinct_on(q, &s))
	{
		memset(&s, 0, sizeof(s));
		if (!lion_top_minmax(q, &s))
			return any;
	}
	lion_top_apply(q, &s);
	return true;
}

/* Has q, or a subquery in its FROM list, a shape worth a look? */
static bool
lion_top_candidate(Query *q)
{
	ListCell   *lc;

	if (q->hasDistinctOn || q->hasWindowFuncs ||
		(q->hasAggs && q->groupClause != NIL))
		return true;
	foreach(lc, q->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL &&
			lion_top_candidate(rte->subquery))
			return true;
	}
	return false;
}

/*
 * A copy of parse with every query that keeps only the first rows of each
 * group rewritten to probe the groups one key at a time, or NULL where none
 * does.
 */
Query *
lion_top_rewrite(Query *parse)
{
	Query	   *q;

	if (parse->commandType != CMD_SELECT || parse->utilityStmt != NULL ||
		!lion_top_candidate(parse))
		return NULL;
	q = copyObject(parse);
	if (!lion_top_walk(q))
		return NULL;
	return q;
}

void
lion_top_init(void)
{
	DefineCustomBoolVariable("pg_lion.enable_lateral_tops",
							 "Lets the planner try the first rows of each group (DISTINCT ON, row_number() <= k, min or max per group) as a probe of each group's key.",
							 "The query is planned as written and as rewritten, and the cheaper plan kept. Off, it is planned as written.",
							 &lion_enable_lateral_tops,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
}
