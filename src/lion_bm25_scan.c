/*-------------------------------------------------------------------------
 *
 * lion_bm25_scan.c
 *		LionBm25: a CustomScan for
 *
 *			SELECT ... FROM t WHERE col @@ q2 [AND ...]
 *			ORDER BY lion_bm25_score(col, q, 'idx') DESC [LIMIT n]
 *
 *		that returns the rows best first straight from the walk lion_bm25()
 *		makes (lion_bm25.c, DESIGN.md §17 "Ranking"), instead of fetching
 *		every row the WHERE matches, scoring each and sorting them.
 *
 * The order is only claimed when the walk's candidates hold every row the
 * scan may return: rows the WHERE keeps all match q2, and a row that matches
 * q2 has one of q2's lexemes not under a NOT unless an empty document
 * matches q2 too - a tsquery is monotone in its operands once its NOTs are
 * pushed down to them.  So the scan is offered when q2 does not match an
 * empty document and every such lexeme of q2 is one of q's: a row with none
 * of q's lexemes scores 0 and would be missing from the walk, but such a row
 * fails the WHERE.  q2 is usually q itself.
 *
 * The scan asks the walk for the best L rows - the LIMIT and 16 more when
 * the planner knows the LIMIT, else 64 - fetches them best first under the
 * scan's snapshot, following a HOT chain to the visible version, and applies
 * the WHERE to each; when whatever is above it pulls past them, it asks for
 * four times as many ranked below the last row it had, until the walk has
 * no more: until one comes up short, since rows inserted meanwhile are in
 * the index too, and the snapshot does not see them (lion_bm25_next_L()).
 *
 * The scan returns the score it ranked a row by as well: the ORDER BY
 * expression is a column of its scan tuple (custom_scan_tlist), which
 * setrefs.c puts in place of every copy of the expression above it.  Called
 * on its own, lion_bm25_score() would read the index's statistics when the
 * first row comes, not when the walk did, and after rows are inserted in
 * between the scores shown would not be in the order they were ranked by.
 * The scan's tuple is the table's columns the plan needs, then the score.
 * A relation that is locked FOR UPDATE or the like is left to the ordinary
 * plan: EvalPlanQual hands a scan the table's row, not that tuple.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "lion.h"
#include "lion_bm25.h"
#include "lion_compat.h"
#include "lion_count.h"
#include "lion_customscan.h"
#include "lion_tid.h"

extern Datum lion_bm25_score(PG_FUNCTION_ARGS);

bool		lion_enable_bm25_scan = true;

static set_rel_pathlist_hook_type lion_bm25_prev_set_rel_pathlist_hook = NULL;

/*
 * custom_private: the index, the query, k1, b and the first L, as Consts, in
 * that order.  lion_bm25_priv_decode() is its one reader and
 * lion_bm25_priv_encode() its one writer.
 */
#define LION_BM25_PRIV_LEN	5

typedef struct LionBm25Priv
{
	Oid			indexoid;		/* a regclass */
	Datum		query;			/* a tsquery, pointing into the plan */
	double		k1;
	double		b;
	int64		firstL;
} LionBm25Priv;

typedef struct LionBm25ScanState
{
	CustomScanState css;
	Relation	index;
	TSQuery		query;
	double		k1;
	double		b;
	int64		firstL;
	bool		valid;			/* the query can score a row */
	LionBm25Query q;
	LionBm25Cand *best;			/* the last walk's rows, best first */
	int64		nbest;
	int64		next;			/* the next of them to return */
	int64		L;				/* what the last walk was asked for; 0: none */
	int64		seen;			/* the rows the walks have given, added up */
	bool		last;			/* that walk was the last one */
	TupleTableSlot *heapslot;	/* the table's row, fetched */
	int			natts;			/* the scan tuple's columns */
	AttrNumber *attnos;			/* each one's table column; the score: none */
	bool	   *isscore;
} LionBm25ScanState;

static Plan *lion_bm25_plan_path(PlannerInfo *root, RelOptInfo *rel,
								 CustomPath *best_path, List *tlist,
								 List *clauses, List *custom_plans);
static Node *lion_bm25_create_state(CustomScan *cscan);
static void lion_bm25_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *lion_bm25_exec(CustomScanState *node);
static void lion_bm25_end(CustomScanState *node);
static void lion_bm25_rescan(CustomScanState *node);
static void lion_bm25_explain(CustomScanState *node, List *ancestors,
							  ExplainState *es);

static const CustomPathMethods lion_bm25_path_methods = {
	.CustomName = "LionBm25",
	.PlanCustomPath = lion_bm25_plan_path,
};

static const CustomScanMethods lion_bm25_scan_methods = {
	.CustomName = "LionBm25",
	.CreateCustomScanState = lion_bm25_create_state,
};

static const CustomExecMethods lion_bm25_exec_methods = {
	.CustomName = "LionBm25",
	.BeginCustomScan = lion_bm25_begin,
	.ExecCustomScan = lion_bm25_exec,
	.EndCustomScan = lion_bm25_end,
	.ReScanCustomScan = lion_bm25_rescan,
	.ExplainCustomScan = lion_bm25_explain,
};

/* ---------------------------------------------------------------------
 * custom_private
 * --------------------------------------------------------------------- */

/* Member i of custom_private, which has to be a non-null Const of type */
static Datum
lion_bm25_priv_const(List *priv, int i, Oid type)
{
	Const	   *c = (Const *) list_nth(priv, i);

	if (c == NULL || !IsA(c, Const) || c->consttype != type ||
		c->constisnull)
		elog(ERROR, "LionBm25 plan's private item %d is not a %s", i,
			 format_type_be(type));
	return c->constvalue;
}

/*
 * Read a LionBm25 plan's custom_private into *out: a list this build did
 * not write is an ERROR, not a misread Oid.
 */
static void
lion_bm25_priv_decode(List *priv, LionBm25Priv *out)
{
	if (list_length(priv) != LION_BM25_PRIV_LEN)
		elog(ERROR, "LionBm25 plan has %d private items, not %d",
			 list_length(priv), LION_BM25_PRIV_LEN);
	out->indexoid = DatumGetObjectId(lion_bm25_priv_const(priv, 0,
														  REGCLASSOID));
	out->query = lion_bm25_priv_const(priv, 1, TSQUERYOID);
	out->k1 = DatumGetFloat8(lion_bm25_priv_const(priv, 2, FLOAT8OID));
	out->b = DatumGetFloat8(lion_bm25_priv_const(priv, 3, FLOAT8OID));
	out->firstL = DatumGetInt64(lion_bm25_priv_const(priv, 4, INT8OID));
}

/* lion_bm25_priv_encode() without its check */
static List *
lion_bm25_priv_build(const LionBm25Priv *p)
{
	return list_make5(makeConst(REGCLASSOID, -1, InvalidOid, sizeof(Oid),
								ObjectIdGetDatum(p->indexoid), false, true),
					  makeConst(TSQUERYOID, -1, InvalidOid, -1,
								p->query, false, false),
					  makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
								Float8GetDatum(p->k1), false,
								FLOAT8PASSBYVAL),
					  makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
								Float8GetDatum(p->b), false,
								FLOAT8PASSBYVAL),
					  makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
								Int64GetDatum(p->firstL), false,
								FLOAT8PASSBYVAL));
}

/*
 * Write *p as a LionBm25 path's custom_private, which the plan takes over.
 * The query is not copied: it points into a Const the caller has copied.  An
 * assert-enabled build decodes what it wrote, writes that again and checks
 * the two are equal(), as lion_count_priv_encode() does.
 */
static List *
lion_bm25_priv_encode(const LionBm25Priv *p)
{
	List	   *priv = lion_bm25_priv_build(p);

#ifdef USE_ASSERT_CHECKING
	{
		LionBm25Priv again;

		lion_bm25_priv_decode(priv, &again);
		Assert(equal(priv, lion_bm25_priv_build(&again)));
	}
#endif
	return priv;
}

/* ---------------------------------------------------------------------
 * Planning
 * --------------------------------------------------------------------- */

static Node *
lion_bm25_strip(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

/* A plain column of rel, as a Var. */
static Var *
lion_bm25_rel_var(Node *n, RelOptInfo *rel)
{
	n = lion_bm25_strip(n);
	if (n == NULL || !IsA(n, Var))
		return NULL;
	if (((Var *) n)->varno != (int) rel->relid ||
		((Var *) n)->varlevelsup != 0 || ((Var *) n)->varattno <= 0)
		return NULL;
	return (Var *) n;
}

static Const *
lion_bm25_const(Node *n, Oid type)
{
	n = lion_bm25_strip(n);
	if (n == NULL || !IsA(n, Const) || ((Const *) n)->constisnull ||
		((Const *) n)->consttype != type)
		return NULL;
	return (Const *) n;
}

/* Is e a call of lion_bm25_score()? */
static bool
lion_bm25_is_score(Expr *e)
{
	FmgrInfo	fi;
	FuncExpr   *f;

	if (e == NULL || !IsA(e, FuncExpr))
		return false;
	f = (FuncExpr *) e;
	if (f->funcresulttype != FLOAT8OID || list_length(f->args) != 5)
		return false;
	fmgr_info(f->funcid, &fi);
	return fi.fn_addr == lion_bm25_score;
}

/* The lion_bm25_score() call the query is ordered by first, or NULL. */
static FuncExpr *
lion_bm25_order_score(PlannerInfo *root)
{
	PathKey    *pk;
	ListCell   *lc;

	if (root->query_pathkeys == NIL)
		return NULL;
	pk = linitial_node(PathKey, root->query_pathkeys);
	if (pk->pk_eclass->ec_has_volatile)
		return NULL;
	foreach(lc, pk->pk_eclass->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *e = (Expr *) lion_bm25_strip((Node *) em->em_expr);

		if (lion_bm25_is_score(e))
			return (FuncExpr *) e;
	}
	return NULL;
}

static TSTernaryValue
lion_bm25_absent(void *arg, QueryOperand *val, ExecPhraseData *data)
{
	return TS_NO;
}

/*
 * Does every row matching where have one of q's lexemes (the header)?  Its
 * lexemes not under a NOT all q's, no prefix among its operands, and an
 * empty document not matching it.
 */
static bool
lion_bm25_where_covered(TSQuery where, List *qlexemes)
{
	bool		prefix;
	List	   *wl;
	ListCell   *lc;

	if (where->size == 0)
		return false;
	wl = lion_bm25_lexemes(where, &prefix);
	if (prefix || wl == NIL)
		return false;
	foreach(lc, wl)
	{
		text	   *w = (text *) lfirst(lc);
		ListCell   *lc2;
		bool		found = false;

		foreach(lc2, qlexemes)
		{
			text	   *t = (text *) lfirst(lc2);

			if (VARSIZE_ANY_EXHDR(t) == VARSIZE_ANY_EXHDR(w) &&
				memcmp(VARDATA_ANY(t), VARDATA_ANY(w),
					   VARSIZE_ANY_EXHDR(w)) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return !TS_execute(GETQUERY(where), NULL, TS_EXEC_EMPTY,
					   lion_bm25_absent);
}

/* The rows the walk would read for query: its lexemes' rows, added up. */
static double
lion_bm25_candidates(Oid indexoid, TSQuery query)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	LionBm25Query q;
	double		n = 0;

	if (lion_bm25_prepare(index, query, 1.2, 0.75, &q))
		n = (double) q.ncodes;
	index_close(index, AccessShareLock);
	return n;
}

static void
lion_bm25_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
						   RangeTblEntry *rte)
{
	PathKey    *pk;
	FuncExpr   *score;
	Var		   *var = NULL;
	Const	   *cquery;
	Const	   *cindex;
	Const	   *ck1;
	Const	   *cb;
	IndexOptInfo *idx = NULL;
	TSQuery		query;
	bool		prefix;
	List	   *qlexemes;
	bool		covered = false;
	double		ncand;
	double		rows;
	int64		firstL;
	Cost		startup;
	Cost		perrow;
	CustomPath *cp;
	LionBm25Priv priv;
	ListCell   *lc;
	bool		desc;

	if (lion_bm25_prev_set_rel_pathlist_hook != NULL)
		lion_bm25_prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!lion_enable_bm25_scan || root->query_pathkeys == NIL)
		return;
	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION ||
		rte->inh || rte->tablesample != NULL ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW) ||
		rte->securityQuals != NIL || IS_DUMMY_REL(rel) ||
		rel->indexlist == NIL)
		return;
	if (root->parse->resultRelation == (int) rel->relid ||
		bms_is_member((int) rel->relid, root->all_result_relids) ||
		get_plan_rowmark(root->rowMarks, rti) != NULL)
		return;
	if (lion_old_snapshot_threshold_active())
		return;

	/* ORDER BY lion_bm25_score(col, const, const, const, const) DESC */
	pk = linitial_node(PathKey, root->query_pathkeys);
#if PG_VERSION_NUM >= 180000
	desc = (pk->pk_cmptype == COMPARE_GT);
#else
	desc = (pk->pk_strategy == BTGreaterStrategyNumber);
#endif
	if (!desc)
		return;
	score = lion_bm25_order_score(root);
	if (score == NULL)
		return;
	var = lion_bm25_rel_var(linitial(score->args), rel);
	cquery = lion_bm25_const(lsecond(score->args), TSQUERYOID);
	cindex = lion_bm25_const(lthird(score->args), REGCLASSOID);
	ck1 = lion_bm25_const(lfourth(score->args), FLOAT8OID);
	cb = lion_bm25_const(list_nth(score->args, 4), FLOAT8OID);
	if (var == NULL || cquery == NULL || cindex == NULL || ck1 == NULL ||
		cb == NULL)
		return;

	/* the index: lion, on rel, storing positions for exactly that column */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *i = (IndexOptInfo *) lfirst(lc);

		if (i->indexoid == DatumGetObjectId(cindex->constvalue))
		{
			idx = i;
			break;
		}
	}
	if (idx == NULL || idx->relam != lion_get_am_oid() || idx->hypothetical ||
		idx->indpred != NIL)
		return;
	{
		int			c;
		bool		ok = false;

		for (c = 0; c < idx->nkeycolumns; c++)
			if (idx->indexkeys[c] == var->varattno &&
				lion_index_stores_positions(idx, (AttrNumber) (c + 1)))
				ok = true;
		if (!ok)
			return;
	}

	/* the score's lexemes, and a WHERE on the column they cover */
	query = DatumGetTSQuery(cquery->constvalue);
	qlexemes = lion_bm25_lexemes(query, &prefix);
	if (prefix || qlexemes == NIL)
		return;
	{
		double		k1 = DatumGetFloat8(ck1->constvalue);
		double		b = DatumGetFloat8(cb->constvalue);

		if (!(k1 >= 0) || !(b >= 0 && b <= 1))
			return;
	}
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *op = (OpExpr *) rinfo->clause;
		Var		   *wv;
		Const	   *wq;

		if (!IsA(op, OpExpr) || list_length(op->args) != 2 ||
			get_opcode(op->opno) != F_TS_MATCH_VQ)
			continue;
		wv = lion_bm25_rel_var(linitial(op->args), rel);
		wq = lion_bm25_const(lsecond(op->args), TSQUERYOID);
		if (wv == NULL || wq == NULL || wv->varattno != var->varattno)
			continue;
		if (lion_bm25_where_covered(DatumGetTSQuery(wq->constvalue),
									qlexemes))
		{
			covered = true;
			break;
		}
	}
	if (!covered)
		return;

	/*
	 * COST.  Before its first row the scan walks every member of the
	 * query's lexemes and each candidate's length - at most, since MaxScore
	 * skips some - for an index tuple and an operator's CPU each; then each
	 * row costs a heap fetch, a random page at worst, and the WHERE.
	 */
	ncand = lion_bm25_candidates(idx->indexoid, query);
	rows = Max(rel->rows, 1.0);
	firstL = (root->limit_tuples > 0 && root->limit_tuples < 1e9) ?
		(int64) root->limit_tuples + 16 : 64;
	startup = ncand * 2.0 * (cpu_index_tuple_cost + cpu_operator_cost);
	perrow = random_page_cost + cpu_tuple_cost +
		rel->baserestrictcost.per_tuple;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup + rel->baserestrictcost.startup;
	cp->path.total_cost = cp->path.startup_cost + rows * perrow;
	cp->path.pathkeys = list_make1(pk);
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	priv.indexoid = DatumGetObjectId(cindex->constvalue);
	priv.query = ((Const *) copyObject(cquery))->constvalue;
	priv.k1 = DatumGetFloat8(ck1->constvalue);
	priv.b = DatumGetFloat8(cb->constvalue);
	priv.firstL = firstL;
	cp->custom_private = lion_bm25_priv_encode(&priv);
	cp->methods = &lion_bm25_path_methods;
	add_path(rel, &cp->path);
}

static Plan *
lion_bm25_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *quals = extract_actual_clauses(clauses, false);
	List	   *vars;
	List	   *scantl = NIL;
	ListCell   *lc;

	/* the scan tuple: the table's columns the plan uses, then the score */
	vars = pull_var_clause((Node *) list_concat(list_concat(NIL, tlist), quals),
						   PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
						   PVC_RECURSE_PLACEHOLDERS);
	vars = list_concat(vars,
					   pull_var_clause((Node *) rel->reltarget->exprs,
									   PVC_RECURSE_PLACEHOLDERS));
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (v->varno != (int) rel->relid || v->varlevelsup != 0)
			elog(ERROR, "LionBm25 scan of relation %u given a column of another",
				 rel->relid);
		if (!tlist_member((Expr *) v, scantl))
			scantl = lappend(scantl,
							 makeTargetEntry((Expr *) copyObject(v),
											 list_length(scantl) + 1,
											 NULL, false));
	}
	scantl = lappend(scantl,
					 makeTargetEntry((Expr *) copyObject(lion_bm25_order_score(root)),
									 list_length(scantl) + 1, NULL, false));

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = quals;
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = best_path->custom_private;
	cscan->custom_scan_tlist = scantl;
	cscan->methods = &lion_bm25_scan_methods;
	return &cscan->scan.plan;
}

/* ---------------------------------------------------------------------
 * Execution
 * --------------------------------------------------------------------- */

static Node *
lion_bm25_create_state(CustomScan *cscan)
{
	LionBm25ScanState *st = (LionBm25ScanState *)
		newNode(sizeof(LionBm25ScanState), T_CustomScanState);

	st->css.methods = &lion_bm25_exec_methods;
	/* built from the table's row (heapslot) and the score */
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

static void
lion_bm25_begin(CustomScanState *node, EState *estate, int eflags)
{
	LionBm25ScanState *st = (LionBm25ScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	LionBm25Priv priv;
	ListCell   *lc;
	int			i = 0;

	lion_bm25_priv_decode(cscan->custom_private, &priv);
	st->query = DatumGetTSQuery(priv.query);
	st->k1 = priv.k1;
	st->b = priv.b;
	st->firstL = priv.firstL;

	st->natts = list_length(cscan->custom_scan_tlist);
	st->attnos = (AttrNumber *) palloc0(sizeof(AttrNumber) * st->natts);
	st->isscore = (bool *) palloc0(sizeof(bool) * st->natts);
	foreach(lc, cscan->custom_scan_tlist)
	{
		Expr	   *e = lfirst_node(TargetEntry, lc)->expr;

		if (IsA(e, Var))
			st->attnos[i] = ((Var *) e)->varattno;
		else if (lion_bm25_is_score(e))
			st->isscore[i] = true;
		else
			elog(ERROR, "LionBm25 scan tuple has an unexpected column");
		i++;
	}

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	st->index = index_open(priv.indexoid, AccessShareLock);
	lion_check_table_am(node->ss.ss_currentRelation);
	st->heapslot = table_slot_create(node->ss.ss_currentRelation,
									 &estate->es_tupleTable);

	/*
	 * The relation-level predicate lock index_beginscan() takes for an AM
	 * without ampredlocks, before the index is read, so that a SERIALIZABLE
	 * search that finds nothing is covered too.
	 */
	lion_reader_lock(st->index, estate->es_snapshot);
	st->valid = lion_bm25_prepare(st->index, st->query, st->k1, st->b, &st->q);
}

/* The scan tuple of the row in heapslot, whose score is score. */
static TupleTableSlot *
lion_bm25_store(LionBm25ScanState *st, double score)
{
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	TupleTableSlot *row = st->heapslot;
	int			i;

	ExecClearTuple(slot);
	for (i = 0; i < st->natts; i++)
	{
		AttrNumber	a = st->attnos[i];

		slot->tts_isnull[i] = false;
		if (st->isscore[i])
			slot->tts_values[i] = Float8GetDatum(score);
		else if (a > 0)
			slot->tts_values[i] = slot_getattr(row, a, &slot->tts_isnull[i]);
		else if (a == 0)
		{
			/* a whole-row Var: in the tuple's memory, reset per row */
			MemoryContext old = MemoryContextSwitchTo(
				st->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

			slot->tts_values[i] = ExecFetchSlotHeapTupleDatum(row);
			MemoryContextSwitchTo(old);
		}
		else if (a == SelfItemPointerAttributeNumber)
			slot->tts_values[i] = PointerGetDatum(&row->tts_tid);
		else if (a == TableOidAttributeNumber)
			slot->tts_values[i] = ObjectIdGetDatum(row->tts_tableOid);
		else
			slot->tts_values[i] = slot_getsysattr(row, a, &slot->tts_isnull[i]);
	}
	return ExecStoreVirtualTuple(slot);
}

/* The next row best first that the snapshot sees, or NULL. */
static TupleTableSlot *
lion_bm25_next(ScanState *ss)
{
	LionBm25ScanState *st = (LionBm25ScanState *) ss;
	Relation	heap = ss->ss_currentRelation;
	Snapshot	snapshot = ss->ps.state->es_snapshot;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (st->next < st->nbest)
		{
			const LionBm25Cand *c = &st->best[st->next++];
			ItemPointerData tid;

			lion_code_to_tid(c->code, &tid);
			if (!lion_table_fetch_tid(heap, &tid, snapshot, NULL))
				continue;
			if (!table_tuple_fetch_row_version(heap, &tid, snapshot,
											   st->heapslot))
				continue;
			return lion_bm25_store(st, c->score);
		}
		if (!st->valid || st->last)
			return ExecClearTuple(slot);

		/*
		 * The walk again for more: the rows ranked below the last one it
		 * gave, which is where the scan has got to (lion_bm25_topk()).
		 */
		st->L = lion_bm25_next_L(st->L, st->firstL, &st->q, st->seen);
		{
			LionBm25Cand after;
			bool		have_after = (st->nbest > 0);

			if (have_after)
				after = st->best[st->nbest - 1];
			if (st->best != NULL)
				pfree(st->best);
			st->best = (LionBm25Cand *)
				palloc_extended(sizeof(LionBm25Cand) * st->L, MCXT_ALLOC_HUGE);
			st->nbest = lion_bm25_topk(&st->q, st->L,
									   have_after ? &after : NULL, st->best);
		}
		lion_bm25_sort(st->best, st->nbest);
		st->next = 0;
		st->seen += st->nbest;
		if (st->nbest < st->L)
			st->last = true;
	}
}

static bool
lion_bm25_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
lion_bm25_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, (ExecScanAccessMtd) lion_bm25_next,
					(ExecScanRecheckMtd) lion_bm25_recheck);
}

static void
lion_bm25_end(CustomScanState *node)
{
	LionBm25ScanState *st = (LionBm25ScanState *) node;

	if (st->index != NULL)
		index_close(st->index, AccessShareLock);
	st->index = NULL;
}

static void
lion_bm25_rescan(CustomScanState *node)
{
	LionBm25ScanState *st = (LionBm25ScanState *) node;

	if (st->best != NULL)
		pfree(st->best);
	st->best = NULL;
	st->nbest = 0;
	st->next = 0;
	st->L = 0;
	st->seen = 0;
	st->last = false;
}

static void
lion_bm25_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	LionBm25ScanState *st = (LionBm25ScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	LionBm25Priv priv;

	lion_bm25_priv_decode(cscan->custom_private, &priv);
	ExplainPropertyText("Index", get_rel_name(priv.indexoid), es);
	if (es->analyze)
		ExplainPropertyInteger("Rows Ranked", NULL, st->seen, es);
}

/* ---------------------------------------------------------------------
 * Initialisation, from _PG_init (lion_am.c)
 * --------------------------------------------------------------------- */

void
lion_bm25_scan_init(void)
{
	DefineCustomBoolVariable("pg_lion.enable_bm25_scan",
							 "Answer ORDER BY lion_bm25_score(...) DESC from the index, best first.",
							 NULL,
							 &lion_enable_bm25_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	RegisterCustomScanMethods(&lion_bm25_scan_methods);

	lion_bm25_prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = lion_bm25_set_rel_pathlist;
}
