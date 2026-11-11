/*-------------------------------------------------------------------------
 *
 * lion_plan_lateral.c
 *		A grouped subquery joined in, planned again as a LATERAL subquery
 *		(DESIGN.md §56).
 *
 *	  SELECT u.id, s.n
 *	    FROM users u
 *	    JOIN (SELECT user_id, count(*) AS n FROM events
 *	           WHERE status = 'paid' GROUP BY user_id) s ON s.user_id = u.id
 *	   WHERE u.plan = 'ent';
 *
 * PostgreSQL plans the subquery on its own: every user's count, then the
 * join keeps the few the outer side has.  The join clause cannot reach the
 * subquery as a restriction, since it names another relation.  Written with
 * the clause inside the subquery, as a LATERAL reference,
 *
 *	    JOIN LATERAL (SELECT user_id, count(*) AS n FROM events
 *	                  WHERE status = 'paid' AND user_id = u.id
 *	                  GROUP BY user_id) s ON s.user_id = u.id
 *
 * the subquery counts one key for each outer row, which a lion index on the
 * key answers from its sets (the count pushdown's equality on a nested
 * loop's parameter).  The two are the same query wherever the clause is a
 * strict equality of the grouping column in the grouping's own operator
 * family and collation, there is no outer join, and nothing in the
 * subquery's result depends on rows outside the group: filtering the rows by
 * their group's key before grouping keeps exactly the groups the join keeps.
 *
 * Which is faster depends on how many outer rows there are, which only the
 * planner knows: the planner hook plans the query as written and as
 * rewritten and keeps the cheaper plan.  The rewrite is tried only where the
 * subquery's grouping column is a key column of a lion index, so a query that
 * cannot gain is planned once.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_index.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/syscache.h"

#include "lion_customscan.h"

/* pg_lion.enable_lateral_counts */
bool		lion_enable_lateral_counts = true;

static planner_hook_type lion_prev_planner_hook = NULL;

/*
 * A join clause found for a subquery: `s.key OP expr` or `expr OP s.key`,
 * expr of relations before s and of nothing else.
 */
typedef struct LionLateralClause
{
	OpExpr	   *op;
	int			keyarg;			/* which of op's arguments is s.key */
	Var		   *keyvar;			/* the table column the subquery groups by */
} LionLateralClause;

/*
 * Is every join of the tree an inner join?  A LATERAL reference into the
 * nullable side of an outer join, or out of it, is not the same query.
 */
static bool
lion_lateral_all_inner(Node *jt)
{
	if (jt == NULL || IsA(jt, RangeTblRef))
		return true;
	if (IsA(jt, FromExpr))
	{
		ListCell   *lc;

		foreach(lc, ((FromExpr *) jt)->fromlist)
		{
			if (!lion_lateral_all_inner((Node *) lfirst(lc)))
				return false;
		}
		return true;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		return j->jointype == JOIN_INNER &&
			lion_lateral_all_inner(j->larg) &&
			lion_lateral_all_inner(j->rarg);
	}
	return false;
}

/*
 * The tree's relations in the order the query names them, and its clauses -
 * the WHERE's and every ON's - as one implicit AND.
 */
static void
lion_lateral_walk(Node *jt, List **order, List **quals)
{
	if (jt == NULL)
		return;
	if (IsA(jt, RangeTblRef))
	{
		*order = lappend_int(*order, ((RangeTblRef *) jt)->rtindex);
		return;
	}
	if (IsA(jt, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jt;
		ListCell   *lc;

		foreach(lc, f->fromlist)
			lion_lateral_walk((Node *) lfirst(lc), order, quals);
		if (f->quals != NULL)
			*quals = list_concat(*quals,
								 make_ands_implicit((Expr *) f->quals));
		return;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		lion_lateral_walk(j->larg, order, quals);
		lion_lateral_walk(j->rarg, order, quals);
		if (j->quals != NULL)
			*quals = list_concat(*quals,
								 make_ands_implicit((Expr *) j->quals));
	}
}

/*
 * The qual `quals` of a FromExpr or JoinExpr less `clause`, one of its
 * implicit AND's members: NULL where it was the only one.
 */
static Node *
lion_lateral_drop_from(Node *quals, Node *clause, bool *found)
{
	if (quals == NULL)
		return NULL;
	if (quals == clause)
	{
		*found = true;
		return NULL;
	}
	if (is_andclause(quals) &&
		list_member_ptr(((BoolExpr *) quals)->args, clause))
	{
		BoolExpr   *b = (BoolExpr *) quals;

		b->args = list_delete_ptr(b->args, clause);
		*found = true;
		if (list_length(b->args) == 1)
			return (Node *) linitial(b->args);
	}
	return quals;
}

/*
 * Take `clause`, which lion_lateral_walk() found, out of the join tree.
 */
static bool
lion_lateral_drop(Node *jt, Node *clause)
{
	bool		found = false;
	ListCell   *lc;

	if (jt == NULL || IsA(jt, RangeTblRef))
		return false;
	if (IsA(jt, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jt;

		f->quals = lion_lateral_drop_from(f->quals, clause, &found);
		foreach(lc, f->fromlist)
		{
			if (!found)
				found = lion_lateral_drop((Node *) lfirst(lc), clause);
		}
		return found;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		j->quals = lion_lateral_drop_from(j->quals, clause, &found);
		if (!found)
			found = lion_lateral_drop(j->larg, clause);
		if (!found)
			found = lion_lateral_drop(j->rarg, clause);
	}
	return found;
}

/*
 * Has relation `relid` a lion index with column `attno` among its keys?  A
 * partitioned table's own (partitioned) index counts: its partitions' are
 * the same method's.
 */
static bool
lion_lateral_lion_key(Oid relid, AttrNumber attno)
{
	Oid			lionam = lion_get_am_oid();
	Relation	rel;
	List	   *indexes;
	ListCell   *lc;
	bool		found = false;

	if (!OidIsValid(lionam))
		return false;
	rel = table_open(relid, AccessShareLock);
	indexes = RelationGetIndexList(rel);
	table_close(rel, AccessShareLock);
	foreach(lc, indexes)
	{
		Oid			indexoid = lfirst_oid(lc);
		HeapTuple	ctup;
		HeapTuple	itup;
		bool		islion;

		ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(ctup))
			continue;
		islion = ((Form_pg_class) GETSTRUCT(ctup))->relam == lionam;
		ReleaseSysCache(ctup);
		if (!islion)
			continue;
		itup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(itup))
			continue;
		{
			Form_pg_index index = (Form_pg_index) GETSTRUCT(itup);
			int			k;

			for (k = 0; k < index->indnkeyatts; k++)
			{
				if (index->indkey.values[k] == attno)
					found = true;
			}
		}
		ReleaseSysCache(itup);
		if (found)
			break;
	}
	list_free(indexes);
	return found;
}

/*
 * The grouping column of subquery sq that its output column `attno` is, when
 * a clause on it may be moved into the subquery's WHERE: a plain SELECT ...
 * GROUP BY (no grouping sets), with nothing whose result depends on rows
 * outside a group or on how often it runs - no window function, set-returning
 * function in the target list, DISTINCT, LIMIT, OFFSET, row lock, set
 * operation, CTE, sublink, volatile function or row-level security - and the
 * column a plain column of a table with a lion index on it.
 */
static Var *
lion_lateral_group_key(Query *sq, AttrNumber attno, Oid *eqop)
{
	TargetEntry *tle;
	SortGroupClause *sgc;
	Var		   *var;
	RangeTblEntry *rte;

	if (sq->commandType != CMD_SELECT || sq->utilityStmt != NULL ||
		sq->setOperations != NULL || !sq->hasAggs ||
		sq->groupClause == NIL || sq->groupingSets != NIL ||
		sq->hasWindowFuncs || sq->hasTargetSRFs || sq->hasSubLinks ||
		sq->hasDistinctOn || sq->distinctClause != NIL ||
		sq->limitCount != NULL || sq->limitOffset != NULL ||
		sq->rowMarks != NIL || sq->hasForUpdate || sq->cteList != NIL ||
		sq->hasRecursive || sq->hasModifyingCTE || sq->hasRowSecurity ||
		sq->jointree == NULL)
		return NULL;
	tle = get_tle_by_resno(sq->targetList, attno);
	if (tle == NULL || tle->resjunk || tle->ressortgroupref == 0)
		return NULL;
	sgc = get_sortgroupref_clause_noerr(tle->ressortgroupref, sq->groupClause);
	if (sgc == NULL || !OidIsValid(sgc->eqop))
		return NULL;
	if (!IsA(tle->expr, Var))
		return NULL;
	var = (Var *) tle->expr;
	if (var->varlevelsup != 0 || var->varattno <= 0)
		return NULL;
	rte = rt_fetch(var->varno, sq->rtable);
#if PG_VERSION_NUM >= 180000
	/* from PG 18 the target list reads grouped columns from the group RTE */
	if (rte->rtekind == RTE_GROUP)
	{
		Node	   *g;

		if (var->varattno > list_length(rte->groupexprs))
			return NULL;
		g = (Node *) list_nth(rte->groupexprs, var->varattno - 1);
		if (!IsA(g, Var))
			return NULL;
		var = (Var *) g;
		if (var->varlevelsup != 0 || var->varattno <= 0)
			return NULL;
		rte = rt_fetch(var->varno, sq->rtable);
	}
#endif
	if (rte->rtekind != RTE_RELATION || rte->securityQuals != NIL)
		return NULL;
	if (contain_volatile_functions((Node *) sq))
		return NULL;
	if (!lion_lateral_lion_key(rte->relid, var->varattno))
		return NULL;
	*eqop = sgc->eqop;
	return var;
}

/* The relations of this query level an expression reads (varlevelsup 0). */
static bool
lion_lateral_varnos_walker(Node *node, Bitmapset **varnos)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) node;

		if (v->varlevelsup == 0)
			*varnos = bms_add_member(*varnos, v->varno);
		return false;
	}
	if (IsA(node, Aggref) || IsA(node, WindowFunc) ||
		IsA(node, SubLink) || IsA(node, PlaceHolderVar) ||
		IsA(node, GroupingFunc))
		return true;
	return expression_tree_walker(node, lion_lateral_varnos_walker,
								  (void *) varnos);
}

/*
 * A clause of the query that may go into subquery `si` as a LATERAL
 * reference: s.key OP expr, OP a strict equality compatible with the
 * grouping's (equality_ops_are_compatible()) under the column's collation,
 * expr reading relations named before s - plain relations, not joins - and
 * nothing volatile.
 */
static bool
lion_lateral_clause(Query *parse, Index si, Node *qual, List *order,
					LionLateralClause *out)
{
	RangeTblEntry *srte = rt_fetch(si, parse->rtable);
	OpExpr	   *op;
	int			k;

	if (!IsA(qual, OpExpr))
		return false;
	op = (OpExpr *) qual;
	if (list_length(op->args) != 2 || op->opretset ||
		op->opresulttype != BOOLOID || !op_strict(op->opno))
		return false;
	for (k = 0; k < 2; k++)
	{
		Node	   *keyarg = (Node *) list_nth(op->args, k);
		Node	   *other = (Node *) list_nth(op->args, 1 - k);
		Var		   *kv;
		Var		   *keyvar;
		Bitmapset  *varnos = NULL;
		Oid			eqop;
		int			pos;
		int			x;

		if (!IsA(keyarg, Var))
			continue;
		kv = (Var *) keyarg;
		if (kv->varno != (int) si || kv->varlevelsup != 0)
			continue;
		if (lion_lateral_varnos_walker(other, &varnos) ||
			bms_is_empty(varnos) || bms_is_member(si, varnos) ||
			contain_volatile_functions(other))
			continue;

		/* expr's relations: plain ones, every one named before s */
		pos = -1;
		for (x = 0; x < list_length(order); x++)
		{
			if (list_nth_int(order, x) == (int) si)
				pos = x;
		}
		if (pos < 0)
			continue;
		x = -1;
		while ((x = bms_next_member(varnos, x)) >= 0)
		{
			int			y;
			bool		before = false;

			if (rt_fetch(x, parse->rtable)->rtekind == RTE_JOIN)
				break;
			for (y = 0; y < pos; y++)
			{
				if (list_nth_int(order, y) == x)
					before = true;
			}
			if (!before)
				break;
		}
		if (x >= 0)
			continue;

		keyvar = lion_lateral_group_key(srte->subquery, kv->varattno, &eqop);
		if (keyvar == NULL)
			continue;
		if (!equality_ops_are_compatible(eqop, op->opno) ||
			!(op_mergejoinable(op->opno, exprType(linitial(op->args))) ||
			  op_hashjoinable(op->opno, exprType(linitial(op->args)))))
			continue;
		if (op->inputcollid != exprCollation((Node *) keyvar))
			continue;
		out->op = op;
		out->keyarg = k;
		out->keyvar = keyvar;
		return true;
	}
	return false;
}

/*
 * The query with every grouped subquery that a join clause of it may enter
 * made LATERAL and given that clause in its WHERE - a copy - or NULL where
 * there is none.
 */
static Query *
lion_lateral_rewrite(Query *parse)
{
	Query	   *q;
	List	   *order = NIL;
	List	   *quals = NIL;
	ListCell   *lc;
	bool		any = false;
	Index		si;

	if (parse->commandType != CMD_SELECT || parse->utilityStmt != NULL ||
		parse->jointree == NULL)
		return NULL;

	/* the quick test: a grouped subquery in the FROM list at all */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && !rte->lateral &&
			rte->subquery != NULL && rte->subquery->hasAggs &&
			rte->subquery->groupClause != NIL)
			any = true;
	}
	if (!any || !lion_lateral_all_inner((Node *) parse->jointree))
		return NULL;

	q = copyObject(parse);
	lion_lateral_walk((Node *) q->jointree, &order, &quals);
	any = false;
	for (si = 1; si <= (Index) list_length(q->rtable); si++)
	{
		RangeTblEntry *rte = rt_fetch(si, q->rtable);
		LionLateralClause c;

		/* a security barrier view keeps a leaky qual out of its WHERE */
		if (rte->rtekind != RTE_SUBQUERY || rte->lateral ||
			rte->security_barrier ||
			rte->subquery == NULL || !rte->subquery->hasAggs ||
			rte->subquery->groupClause == NIL ||
			!list_member_int(order, (int) si))
			continue;
		foreach(lc, quals)
		{
			if (lion_lateral_clause(q, si, (Node *) lfirst(lc), order, &c))
				break;
		}
		if (lc == NULL)
			continue;

		/*
		 * The clause moves into the subquery, its key read there and the
		 * rest outside it.  It is not kept outside as well: every group the
		 * subquery returns now meets it - the group's rows all did, and its
		 * key is one of theirs - and kept, it would be counted a second time
		 * in the join's row estimate, as if it filtered there.
		 */
		{
			OpExpr	   *inner = (OpExpr *) copyObject(c.op);
			Node	   *other = (Node *) list_nth(inner->args, 1 - c.keyarg);
			Query	   *sq = rte->subquery;

			IncrementVarSublevelsUp(other, 1, 0);
			list_nth_cell(inner->args, c.keyarg)->ptr_value =
				copyObject(c.keyvar);
			sq->jointree->quals = make_and_qual(sq->jointree->quals,
												(Node *) inner);
			if (!lion_lateral_drop((Node *) q->jointree, (Node *) c.op))
				elog(ERROR, "lion: join clause of a grouped subquery not found");
			quals = list_delete_ptr(quals, c.op);
			rte->lateral = true;
			any = true;
		}
	}
	return any ? q : NULL;
}

static PlannedStmt *
lion_lateral_plan(Query *parse, const char *query_string, int cursorOptions,
				  ParamListInfo boundParams
#if PG_VERSION_NUM >= 190000
				  ,ExplainState *es
#endif
)
{
	if (lion_prev_planner_hook != NULL)
		return lion_prev_planner_hook(parse, query_string, cursorOptions,
									  boundParams
#if PG_VERSION_NUM >= 190000
									  ,es
#endif
			);
	return standard_planner(parse, query_string, cursorOptions, boundParams
#if PG_VERSION_NUM >= 190000
							,es
#endif
		);
}

/*
 * The rewritten query's plan, or NULL where planning it fails.  The query as
 * written has a plan already, so an error here - from another extension's
 * planner hook that does not take a LATERAL subquery, say - must not fail
 * the statement: the planning runs in a subtransaction, as a PL/pgSQL
 * exception block does, rolled back on an error, which is then dropped.  A
 * cancel is not an error of the rewrite and goes on up.
 */
static PlannedStmt *
lion_lateral_plan_alt(Query *alt, const char *query_string, int cursorOptions,
					  ParamListInfo boundParams
#if PG_VERSION_NUM >= 190000
					  ,ExplainState *es
#endif
)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	PlannedStmt *volatile plan = NULL;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		plan = lion_lateral_plan(alt, query_string, cursorOptions, boundParams
#if PG_VERSION_NUM >= 190000
								 ,es
#endif
			);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
			edata->elevel > ERROR)
			ReThrowError(edata);
		FreeErrorData(edata);
		plan = NULL;
	}
	PG_END_TRY();
	return plan;
}

/*
 * The planner hook: the query as written, and - where lion_lateral_rewrite()
 * makes another of it - as rewritten, the cheaper of the two plans kept.  A
 * cursor that asks for its first rows fast is planned as written alone,
 * since the two plans' total costs are not what it is decided on, and so is
 * a query planned in a parallel worker, which cannot start a subtransaction,
 * or on a hot standby, where a recovery conflict met in one ends the session
 * instead of the statement (ProcessRecoveryConflictInterrupt()).
 */
static PlannedStmt *
lion_lateral_planner(Query *parse, const char *query_string,
					 int cursorOptions, ParamListInfo boundParams
#if PG_VERSION_NUM >= 190000
					 ,ExplainState *es
#endif
)
{
	Query	   *alt = NULL;
	PlannedStmt *plan;
	PlannedStmt *altplan;

	if (lion_enable_lateral_counts &&
		(cursorOptions & CURSOR_OPT_FAST_PLAN) == 0 && !IsInParallelMode() &&
		!RecoveryInProgress())
		alt = lion_lateral_rewrite(parse);
	plan = lion_lateral_plan(parse, query_string, cursorOptions, boundParams
#if PG_VERSION_NUM >= 190000
							 ,es
#endif
		);
	if (alt == NULL)
		return plan;
	altplan = lion_lateral_plan_alt(alt, query_string, cursorOptions,
									boundParams
#if PG_VERSION_NUM >= 190000
									,es
#endif
		);
	if (altplan != NULL &&
		altplan->planTree->total_cost < plan->planTree->total_cost)
		return altplan;
	return plan;
}

void
lion_lateral_init(void)
{
	DefineCustomBoolVariable("pg_lion.enable_lateral_counts",
							 "Lets the planner try a grouped subquery joined on its grouping column as a LATERAL subquery.",
							 "The query is planned as written and as rewritten, and the cheaper plan kept. Off, it is planned as written.",
							 &lion_enable_lateral_counts,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	lion_prev_planner_hook = planner_hook;
	planner_hook = lion_lateral_planner;
}
