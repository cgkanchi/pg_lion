/*-------------------------------------------------------------------------
 *
 * lion_exec_anti.c
 *		The anti join over a small inner side, run (DESIGN.md §27, "The anti
 *		join over a small inner side"): the count of the table's WHERE, less
 *		the rows of it some row of the inner side matches.
 *
 *		Before the count's first row the node reads its child whole into a
 *		sort by the values of its scan keys, in the B-tree's order, looks
 *		each distinct key up once in a B-tree of the table under the query's
 *		snapshot, and tests every row found - the table's own restrictions
 *		over the row, and on a row that passes them the anti join's other
 *		clauses over the row and an inner row of the key - counting the ones
 *		that pass: into one total for an ungrouped count, into each one's
 *		group for a GROUP BY.  Each row it emits then has its count less that
 *		(lion_anti_adjust(), from lion_emit_keys()), and a group that comes to
 *		nothing is no row.  Beside a count by cells each row that passes is
 *		counted under its cell, the predicates evaluated on it, and the cells
 *		lose their rows as the tally gives them up (lion_anti_cells_hold(),
 *		from lion_next_cells()).
 *
 *		Nothing it keeps grows without bound in memory: the child's rows and
 *		a key's inner rows are a sort's and a tuplestore's, within work_mem
 *		and on disk past it, and the groups' or the cells' losses go, past
 *		hash_mem, to a sort that the count's own rows are then held back
 *		into and merged with (lion_anti_groups_next(), lion_anti_cells_next()).
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "common/hashfn.h"
#include "executor/nodeIndexscan.h"
#include "utils/sortsupport.h"

#include "lion_customscan.h"

#define LION_ANTI_BUCKETS	64

static void lion_anti_cells_truths(LionAntiState *as);

/* How many of the node's expressions are the anti join's (LION_PRIV_ANTI). */
int
lion_anti_nexprs(const LionCountPriv *priv)
{
	if (!priv->hasanti)
		return 0;
	return priv->anti.nkey + priv->anti.njoin + priv->anti.nrestrict;
}

/*
 * Where each column the anti join's expressions read comes from: a column of
 * the inner side, read from the child's row at its LION_TL_ANTI position, or
 * one of the table's, read from each row a key finds at the attnum its place
 * in custom_scan_tlist names - a LION_TL_RESID column or one the target list
 * prints.  Positions in the scan tuple, which evalslot has the descriptor of.
 * Beside a count by cells setrefs.c points a clause's copy of a predicate at
 * the predicate's LION_TL_CELLPRED place, which holds its truth in the row
 * (lion_anti_cells_truths()): predsread says whether any clause does.
 */
static void
lion_anti_columns(LionAntiState *as, CustomScanState *node,
				  const LionCountPriv *priv, List *exprs)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	TupleDesc	desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	List	   *vars = pull_var_clause((Node *) exprs, 0);
	int			nvar = Max(list_length(vars), 1);
	ListCell   *lc;

	as->innerpos = (int *) palloc(sizeof(int) * nvar);
	as->innerresno = (AttrNumber *) palloc(sizeof(AttrNumber) * nvar);
	as->innerlen = (int16 *) palloc(sizeof(int16) * nvar);
	as->innerbyval = (bool *) palloc(sizeof(bool) * nvar);
	as->outerpos = (int *) palloc(sizeof(int) * nvar);
	as->outerattno = (AttrNumber *) palloc(sizeof(AttrNumber) * nvar);
	as->ninner = 0;
	as->nouter = 0;
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);
		int			pos;
		int			kind;
		int			i;
		bool		dup = false;

		if (!IsA(var, Var) || var->varno != INDEX_VAR || var->varattno < 1 ||
			var->varattno > list_length(cscan->custom_scan_tlist) ||
			var->varattno > priv->ntl)
			elog(ERROR, "LionCount: an anti join clause reads no column of the scan");
		pos = var->varattno - 1;
		for (i = 0; i < as->ninner; i++)
			dup |= (as->innerpos[i] == pos);
		for (i = 0; i < as->nouter; i++)
			dup |= (as->outerpos[i] == pos);
		if (dup)
			continue;

		kind = priv->tlkind[pos];
		if (LION_TL_IS_CELLPRED(kind))
		{
			if (priv->ncelldim == 0)
				elog(ERROR, "LionCount: an anti join clause reads a cells predicate without cells");
			as->predsread = true;
			continue;
		}
		if (LION_TL_IS_ANTI(kind))
		{
			Form_pg_attribute att = TupleDescAttr(desc, pos);

			as->innerpos[as->ninner] = pos;
			as->innerresno[as->ninner] = LION_TL_ANTI_RESNO(kind);
			as->innerlen[as->ninner] = att->attlen;
			as->innerbyval[as->ninner] = att->attbyval;
			as->ninner++;
		}
		else
		{
			TargetEntry *tle = (TargetEntry *) list_nth(cscan->custom_scan_tlist,
														pos);

			if (!IsA(tle->expr, Var) || ((Var *) tle->expr)->varattno <= 0)
				elog(ERROR, "LionCount: an anti join clause reads no column of the table");
			as->outerpos[as->nouter] = pos;
			as->outerattno[as->nouter] = ((Var *) tle->expr)->varattno;
			as->nouter++;
		}
	}
	list_free(vars);
}

/*
 * The GROUP BY columns a row found is subtracted from the group of: each
 * one's equality and hash function under its collation, as the plan's
 * grouping has them (LION_PRIV_ANTI), the `<` of the equality's family the
 * groups are sorted by past hash_mem, and its type, length and passing.
 * None of a count by cells, whose walked column groups nothing.
 */
static void
lion_anti_groups(LionAntiState *as, const LionCountPriv *priv)
{
	AttrNumber	attno[2];
	int			g;

	attno[0] = priv->groupattno;
	attno[1] = priv->groupattno2;
	as->ngroup = 0;
	if (priv->ncelldim > 0)
		return;
	for (g = 0; g < 2; g++)
	{
		Oid			eqop = priv->anti.groupeq[g];
		Oid			sortop = priv->anti.groupsort[g];
		Oid			lhash;
		Oid			rhash;
		Oid			typid;
		int			n = as->ngroup;

		if (attno[g] == 0)
			break;
		if (!get_op_hash_functions(eqop, &lhash, &rhash) || !OidIsValid(lhash))
			elog(ERROR, "LionCount: no hash function for operator %u", eqop);
		if (get_equality_op_for_ordering_op(sortop, NULL) != eqop)
			elog(ERROR, "LionCount: operator %u is no order of operator %u",
				 sortop, eqop);
		typid = get_atttype(priv->heapoid, attno[g]);
		if (!OidIsValid(typid))
			elog(ERROR, "LionCount: the anti join groups by no column %d",
				 (int) attno[g]);
		as->groupattno[n] = attno[g];
		fmgr_info(get_opcode(eqop), &as->groupeq[n]);
		fmgr_info(lhash, &as->grouphash[n]);
		as->groupcoll[n] = priv->anti.groupcoll[g];
		as->groupsort[n] = sortop;
		as->grouptype[n] = typid;
		get_typlenbyval(typid, &as->grouplen[n], &as->groupbyval[n]);
		as->ngroup++;
	}
}

/*
 * Beside a count by cells, each predicate ready to evaluate on a row a key
 * finds: the expression of its LION_TL_CELLPRED place in custom_scan_tlist,
 * over the table's columns, which reads them from the heap's own slot - no
 * parent node, whose scan slot's form would let the expression skip
 * deforming the tuple.
 */
static void
lion_anti_cells_begin(LionAntiState *as, CustomScan *cscan,
					  const LionCountPriv *priv)
{
	ListCell   *lc;
	int			i = 0;
	int			p;

	as->cells = true;
	as->npred = priv->ncellpred;
	as->predstate = (ExprState **)
		palloc0(sizeof(ExprState *) * Max(as->npred, 1));
	as->predpos = (int *) palloc0(sizeof(int) * Max(as->npred, 1));
	as->predtruth = (uint8 *) palloc0(sizeof(uint8) * Max(as->npred, 1));
	foreach(lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		int			kind = (i < priv->ntl) ? priv->tlkind[i] : 0;

		if (LION_TL_IS_CELLPRED(kind))
		{
			p = LION_TL_CELLPRED_NO(kind);
			if (p >= as->npred || as->predstate[p] != NULL)
				elog(ERROR, "LionCount: cells predicate %d in the scan twice", p);
			as->predstate[p] = ExecInitExpr(tle->expr, NULL);
			as->predpos[p] = i;
		}
		i++;
	}
	for (p = 0; p < as->npred; p++)
	{
		if (as->predstate[p] == NULL)
			elog(ERROR, "LionCount: cells predicate %d is no column of the scan", p);
	}
}

/* A comparator of `<` operator sortop under collation coll, NULLs last. */
static void
lion_anti_ssup(SortSupport ssup, Oid sortop, Oid coll)
{
	memset(ssup, 0, sizeof(SortSupportData));
	ssup->ssup_cxt = CurrentMemoryContext;
	ssup->ssup_collation = coll;
	ssup->ssup_nulls_first = false;
	ssup->abbreviate = false;
	PrepareSortSupportFromOrderingOp(sortop, ssup);
}

/*
 * The sort of the child's rows (lion_anti_collect()): a column for each scan
 * key's value, of its expression's type, and - where a join clause other
 * than the scan keys' reads the inner row (joinqual) - one for each inner
 * column the clauses read, after them.  Sorted by the keys, each by the `<`
 * of its B-tree column's operator family for two of its values, under the
 * key's collation: in the index's order, and equal keys - equal under the
 * family, so that a lookup of either finds the same rows - together.  Then by
 * each inner column whose type has an order (and a collation, if it takes
 * one), so that inner rows alike in every column come one after another and
 * are kept once.
 */
static void
lion_anti_sort_begin(LionAntiState *as, EState *estate, List *keyexprs)
{
	TupleDesc	evaldesc = as->evalslot->tts_tupleDescriptor;
	int			ninner = (as->joinqual != NULL) ? as->ninner : 0;
	TupleDesc	desc;
	int			n = 0;
	int			k;
	int			j;

	as->nsortcol = as->nkey + ninner;
	desc = CreateTemplateTupleDesc(as->nsortcol);
	as->sortattno = (AttrNumber *) palloc(sizeof(AttrNumber) * as->nsortcol);
	as->sortops = (Oid *) palloc(sizeof(Oid) * as->nsortcol);
	as->sortcolls = (Oid *) palloc(sizeof(Oid) * as->nsortcol);
	as->sortnulls = (bool *) palloc0(sizeof(bool) * as->nsortcol);
	as->keyssup = (SortSupport) palloc0(sizeof(SortSupportData) * as->nkey);
	for (k = 0; k < as->nkey; k++)
	{
		Node	   *kexpr = (Node *) list_nth(keyexprs, k);

		TupleDescInitEntry(desc, k + 1, NULL, exprType(kexpr),
						   exprTypmod(kexpr), 0);
		TupleDescInitEntryCollation(desc, k + 1, as->keycoll[k]);
		as->sortattno[n] = k + 1;
		as->sortops[n] = as->keysort[k];
		as->sortcolls[n] = as->keycoll[k];
		n++;
		lion_anti_ssup(&as->keyssup[k], as->keysort[k], as->keycoll[k]);
	}
	for (j = 0; j < ninner; j++)
	{
		Form_pg_attribute att = TupleDescAttr(evaldesc, as->innerpos[j]);
		TypeCacheEntry *tc = lookup_type_cache(att->atttypid,
											   TYPECACHE_LT_OPR);

		TupleDescInitEntry(desc, as->nkey + j + 1, NULL, att->atttypid,
						   att->atttypmod, 0);
		TupleDescInitEntryCollation(desc, as->nkey + j + 1,
									att->attcollation);
		if (!OidIsValid(tc->lt_opr) ||
			(type_is_collatable(att->atttypid) &&
			 !OidIsValid(att->attcollation)))
			continue;
		as->sortattno[n] = as->nkey + j + 1;
		as->sortops[n] = tc->lt_opr;
		as->sortcolls[n] = att->attcollation;
		n++;
	}
	TupleDescFinalize(desc);
	as->sortdesc = desc;
	as->nsortkey = n;
	as->sortin = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);
	as->sortcur = ExecInitExtraTupleSlot(estate, desc, &TTSOpsMinimalTuple);
	as->sortprev = ExecInitExtraTupleSlot(estate, desc, &TTSOpsMinimalTuple);
	as->innerslot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsMinimalTuple);
	as->keyvals = (Datum *) palloc0(sizeof(Datum) * as->nkey);
}

/*
 * The sort the losses go to past hash_mem, and the count's rows with them
 * (lion_anti_held_put()): the GROUP BY's columns, each by the `<` of its
 * equality's family, NULLs last - of one column NOT NULL, the order the
 * plan's pathkeys say its groups come in (lion_count_path_anti()) - or a
 * cell's code; then the count and the loss.  And, beside a parallel count by
 * cells, the sort of the rows taken away by the container key of their TIDs.
 */
static void
lion_anti_held_desc(LionAntiState *as, EState *estate)
{
	TupleDesc	desc;
	int			g;

	if (as->cells)
		as->nheldkey = 1;
	else if (as->ngroup > 0)
		as->nheldkey = as->ngroup;
	else
		return;
	desc = CreateTemplateTupleDesc(as->nheldkey + 2);
	as->heldssup = (SortSupport)
		palloc0(sizeof(SortSupportData) * as->nheldkey);
	if (as->cells)
	{
		TupleDescInitEntry(desc, 1, NULL, INT8OID, -1, 0);
		lion_anti_ssup(&as->heldssup[0], Int8LessOperator, InvalidOid);
	}
	else
	{
		for (g = 0; g < as->ngroup; g++)
		{
			TupleDescInitEntry(desc, g + 1, NULL, as->grouptype[g], -1, 0);
			TupleDescInitEntryCollation(desc, g + 1, as->groupcoll[g]);
			lion_anti_ssup(&as->heldssup[g], as->groupsort[g],
						   as->groupcoll[g]);
		}
	}
	TupleDescInitEntry(desc, as->nheldkey + 1, NULL, INT8OID, -1, 0);
	TupleDescInitEntry(desc, as->nheldkey + 2, NULL, INT8OID, -1, 0);
	TupleDescFinalize(desc);
	as->helddesc = desc;
	as->heldin = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);
	as->heldcur = ExecInitExtraTupleSlot(estate, desc, &TTSOpsMinimalTuple);
	as->heldnext = ExecInitExtraTupleSlot(estate, desc, &TTSOpsMinimalTuple);

	if (!as->cells)
		return;
	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, NULL, INT8OID, -1, 0);
	TupleDescInitEntry(desc, 2, NULL, INT8OID, -1, 0);
	TupleDescFinalize(desc);
	as->rangedesc = desc;
	as->rangein = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);
	as->rangeslot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsMinimalTuple);
}

/*
 * The anti join's state, from the plan's ANTI member and the expressions
 * after the residual clauses in custom_exprs: its child, initialized for
 * EXPLAIN too, which prints it under the node; its keys' values and clauses
 * ready to evaluate; beside a count by cells, its predicates; the forms of
 * its sorts; and its run's memory.  The B-tree and the table's slot are
 * opened at the first run.
 */
void
lion_begin_anti(LionCountScanState *st, CustomScanState *node,
				CustomScan *cscan, const LionCountPriv *priv, List *exprs,
				EState *estate, int eflags)
{
	LionAntiState *as;
	const LionCountPrivAnti *a = &priv->anti;
	int			nexpr = lion_anti_nexprs(priv);
	List	   *mine;
	TupleDesc	desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	MemoryContext oldcxt;
	int			i;

	st->anti = NULL;
	if (!priv->hasanti)
		return;
	if (list_length(exprs) < nexpr)
		elog(ERROR, "LionCount: an anti join without its expressions");

	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	as = (LionAntiState *) palloc0(sizeof(LionAntiState));
	as->idxoid = a->idxoid;
	as->nkey = a->nkey;
	as->keycol = (AttrNumber *) palloc(sizeof(AttrNumber) * a->nkey);
	as->keyop = (Oid *) palloc(sizeof(Oid) * a->nkey);
	as->keycoll = (Oid *) palloc(sizeof(Oid) * a->nkey);
	as->keysort = (Oid *) palloc(sizeof(Oid) * a->nkey);
	as->keystate = (ExprState **) palloc(sizeof(ExprState *) * a->nkey);
	as->keylen = (int16 *) palloc(sizeof(int16) * a->nkey);
	as->keybyval = (bool *) palloc(sizeof(bool) * a->nkey);

	mine = list_copy_tail(exprs, list_length(exprs) - nexpr);
	for (i = 0; i < a->nkey; i++)
	{
		Expr	   *kexpr = (Expr *) list_nth(mine, i);

		as->keycol[i] = a->keycol[i];
		as->keyop[i] = a->keyop[i];
		as->keycoll[i] = a->keycoll[i];
		as->keysort[i] = a->keysort[i];
		as->keystate[i] = ExecInitExpr(kexpr, &node->ss.ps);
		get_typlenbyval(exprType((Node *) kexpr), &as->keylen[i],
						&as->keybyval[i]);
	}
	as->joinexprs = list_copy_head(list_copy_tail(mine, a->nkey), a->njoin);
	as->restrictexprs = list_copy_tail(mine, a->nkey + a->njoin);
	as->restrictqual = ExecInitQual(as->restrictexprs, &node->ss.ps);

	/*
	 * The join clauses but the scan keys' own, which come first, one to a
	 * scan key (lion_anti_join_clauses()): those are true of every row their
	 * lookup finds, a B-tree's equality being exact.  With none the inner
	 * rows are not kept, only their keys.
	 */
	if (a->njoin > a->nkey)
		as->joinqual = ExecInitQual(list_copy_tail(as->joinexprs, a->nkey),
									&node->ss.ps);

	/* the columns they read, in a slot of the scan tuple's own shape */
	lion_anti_columns(as, node, priv, mine);
	for (i = 0; i < as->ninner; i++)
	{
		if (as->innerresno[i] >
			list_length(((Plan *) linitial(cscan->custom_plans))->targetlist))
			elog(ERROR, "LionCount: an anti join clause reads no column of its child");
	}
	as->evalslot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);
	ExecClearTuple(as->evalslot);
	for (i = 0; i < desc->natts; i++)
	{
		as->evalslot->tts_values[i] = (Datum) 0;
		as->evalslot->tts_isnull[i] = true;
	}
	ExecStoreVirtualTuple(as->evalslot);
	as->econtext = CreateExprContext(estate);

	if (priv->ncelldim > 0)
	{
		if (st->cells == NULL)
			elog(ERROR, "LionCount: an anti join beside cells that do not run");
		lion_anti_cells_begin(as, cscan, priv);
	}
	lion_anti_groups(as, priv);
	lion_anti_sort_begin(as, estate, list_copy_head(mine, a->nkey));
	lion_anti_held_desc(as, estate);
	as->cxt = AllocSetContextCreate(estate->es_query_cxt,
									"LionCount anti join",
									ALLOCSET_DEFAULT_SIZES);
	as->gcxt = AllocSetContextCreate(estate->es_query_cxt,
									 "LionCount anti join losses",
									 ALLOCSET_DEFAULT_SIZES);
	as->keycxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionCount anti join key",
									   ALLOCSET_SMALL_SIZES);
	as->memlimit = Max(get_hash_memory_limit(), (Size) 64 * 1024);
	as->timing = (estate->es_instrument & INSTRUMENT_TIMER) != 0;
	MemoryContextSwitchTo(oldcxt);

	/* the inner side, the node's one child */
	as->child = ExecInitNode((Plan *) linitial(cscan->custom_plans), estate,
							 eflags);
	node->custom_ps = list_make1(as->child);
	st->anti = as;
}

/*
 * The B-tree, its scan keys, and the table's slot: once, at the first run.
 * The plan's B-tree is one of the table's, each scan key's column one of its
 * key columns, its operator an equality of the column's family and its order
 * that family's `<` - or the plan is not this build's.
 */
static void
lion_anti_open(LionCountScanState *st)
{
	LionAntiState *as = st->anti;
	MemoryContext oldcxt;
	int			k;

	if (as->index != NULL)
		return;
	oldcxt = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);
	as->index = index_open(as->idxoid, AccessShareLock);
	if (as->index->rd_rel->relam != BTREE_AM_OID ||
		as->index->rd_index->indrelid != RelationGetRelid(st->heap))
		elog(ERROR, "LionCount: index %u is no B-tree of the table counted",
			 as->idxoid);
	as->scankeys = (ScanKeyData *) palloc0(sizeof(ScanKeyData) * as->nkey);
	for (k = 0; k < as->nkey; k++)
	{
		Oid			opfamily;
		int			strategy;
		Oid			lefttype;
		Oid			righttype;

		if (as->keycol[k] < 1 ||
			as->keycol[k] > IndexRelationGetNumberOfKeyAttributes(as->index))
			elog(ERROR, "LionCount: column %d is no key column of index %u",
				 as->keycol[k], as->idxoid);
		opfamily = as->index->rd_opfamily[as->keycol[k] - 1];
		get_op_opfamily_properties(as->keyop[k], opfamily, false, &strategy,
								   &lefttype, &righttype);
		if (strategy != BTEqualStrategyNumber)
			elog(ERROR, "LionCount: operator %u is no equality of the B-tree",
				 as->keyop[k]);
		if (get_opfamily_member(opfamily, righttype, righttype,
								BTLessStrategyNumber) != as->keysort[k])
			elog(ERROR, "LionCount: operator %u is no order of the B-tree",
				 as->keysort[k]);
		ScanKeyEntryInitialize(&as->scankeys[k], 0, as->keycol[k],
							   BTEqualStrategyNumber, righttype,
							   as->keycoll[k], get_opcode(as->keyop[k]),
							   (Datum) 0);
	}
	as->heapslot = table_slot_create(st->heap, NULL);
	MemoryContextSwitchTo(oldcxt);
}

/* ---------------------------------------------------------------------
 * The child's keys
 * --------------------------------------------------------------------- */

/*
 * Every row of the child, read once into the sort by its scan keys' values -
 * and its inner columns, with a joinqual - then sorted.  A key with a NULL
 * value matches nothing (each key is a strict operator), and its row is left
 * out.  The child runs in the caller's memory, which is the query's: a node
 * may keep there what it makes on its first row.  The sort is the run's,
 * within work_mem and on disk past it.
 */
static void
lion_anti_collect(LionCountScanState *st)
{
	LionAntiState *as = st->anti;
	ExprContext *econtext = as->econtext;
	TupleTableSlot *es = as->evalslot;
	TupleTableSlot *in = as->sortin;
	TuplesortInstrumentation stats;
	MemoryContext oldcxt;
	instr_time	start;
	int			j;

	if (as->timing)
		INSTR_TIME_SET_CURRENT(start);
	oldcxt = MemoryContextSwitchTo(as->cxt);
	as->keysorted = tuplesort_begin_heap(as->sortdesc, as->nsortkey,
										 as->sortattno, as->sortops,
										 as->sortcolls, as->sortnulls,
										 work_mem, NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(oldcxt);
	for (;;)
	{
		TupleTableSlot *slot;
		bool		anynull = false;
		int			k;

		CHECK_FOR_INTERRUPTS();
		slot = ExecProcNode(as->child);
		if (TupIsNull(slot))
			break;
		as->childrows++;

		ResetExprContext(econtext);
		for (j = 0; j < as->ninner; j++)
			es->tts_values[as->innerpos[j]] =
				slot_getattr(slot, as->innerresno[j],
							 &es->tts_isnull[as->innerpos[j]]);
		econtext->ecxt_scantuple = es;
		ExecClearTuple(in);
		for (k = 0; k < as->nkey; k++)
		{
			in->tts_values[k] =
				ExecEvalExprSwitchContext(as->keystate[k], econtext,
										  &in->tts_isnull[k]);
			if (in->tts_isnull[k])
			{
				anynull = true;
				break;
			}
		}
		if (anynull)
			continue;
		for (j = as->nkey; j < as->nsortcol; j++)
		{
			in->tts_values[j] = es->tts_values[as->innerpos[j - as->nkey]];
			in->tts_isnull[j] = es->tts_isnull[as->innerpos[j - as->nkey]];
		}
		ExecStoreVirtualTuple(in);
		tuplesort_puttupleslot(as->keysorted, in);
	}
	ExecClearTuple(in);
	tuplesort_performsort(as->keysorted);
	tuplesort_get_stats(as->keysorted, &stats);
	if (stats.spaceType == SORT_SPACE_TYPE_DISK)
		as->spilled |= LION_ANTI_SPILL_KEYS;
	if (as->timing)
	{
		instr_time	end;

		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(as->time[0], end, start);
	}
}

/*
 * Is the row in sortcur of the key being looked up?  Equal under the order of
 * each key's B-tree column, which is the family's equality.
 */
static bool
lion_anti_same_key(LionAntiState *as)
{
	int			k;

	for (k = 0; k < as->nkey; k++)
	{
		bool		isnull;
		Datum		d = slot_getattr(as->sortcur, k + 1, &isnull);

		if (isnull ||
			ApplySortComparator(d, false, as->keyvals[k], false,
								&as->keyssup[k]) != 0)
			return false;
	}
	return true;
}

/* ... and is its inner row the one before it, by their bytes? */
static bool
lion_anti_same_inner(LionAntiState *as)
{
	int			j;

	for (j = 0; j < as->ninner; j++)
	{
		bool		n1;
		bool		n2;
		Datum		d1 = slot_getattr(as->sortcur, as->nkey + j + 1, &n1);
		Datum		d2 = slot_getattr(as->sortprev, as->nkey + j + 1, &n2);

		if (n1 || n2)
		{
			if (n1 != n2)
				return false;
		}
		else if (!datum_image_eq(d1, d2, as->innerbyval[j], as->innerlen[j]))
			return false;
	}
	return true;
}

/*
 * The row in sortcur begins a key: its values, copied for the lookup, and -
 * with a joinqual - its inner row the first of the key's, in a tuplestore
 * within work_mem and on disk past it.
 */
static void
lion_anti_key_begin(LionAntiState *as)
{
	MemoryContext oldcxt;
	int			k;

	MemoryContextReset(as->keycxt);
	oldcxt = MemoryContextSwitchTo(as->keycxt);
	for (k = 0; k < as->nkey; k++)
	{
		bool		isnull;
		Datum		d = slot_getattr(as->sortcur, k + 1, &isnull);

		if (isnull)
			elog(ERROR, "LionCount: an anti join key of no value");
		as->keyvals[k] = datumCopy(d, as->keybyval[k], as->keylen[k]);
	}
	MemoryContextSwitchTo(oldcxt);
	if (as->joinqual == NULL)
		return;
	if (as->innerrows == NULL)
	{
		oldcxt = MemoryContextSwitchTo(as->cxt);
		as->innerrows = tuplestore_begin_heap(false, false, work_mem);
		MemoryContextSwitchTo(oldcxt);
	}
	else
		tuplestore_clear(as->innerrows);
	tuplestore_puttupleslot(as->innerrows, as->sortcur);
}

/* ---------------------------------------------------------------------
 * What the rows found subtract
 * --------------------------------------------------------------------- */

/*
 * The sort the losses go to past hash_mem (lion_anti_held_desc()), begun:
 * from here on a loss and every row the count emits go into it.
 */
static void
lion_anti_held_begin(LionAntiState *as)
{
	AttrNumber	attno[2];
	Oid			ops[2];
	Oid			colls[2];
	bool		nulls[2] = {false, false};
	MemoryContext oldcxt;
	int			g;

	Assert(as->nheldkey >= 1 && as->nheldkey <= 2);
	for (g = 0; g < as->nheldkey; g++)
	{
		attno[g] = (AttrNumber) (g + 1);
		ops[g] = as->cells ? Int8LessOperator : as->groupsort[g];
		colls[g] = as->cells ? InvalidOid : as->groupcoll[g];
	}
	oldcxt = MemoryContextSwitchTo(as->cxt);
	as->heldsort = tuplesort_begin_heap(as->helddesc, as->nheldkey, attno,
										ops, colls, nulls, work_mem, NULL,
										TUPLESORT_NONE);
	MemoryContextSwitchTo(oldcxt);
	as->merging = true;
	as->spilled |= LION_ANTI_SPILL_SUB;
}

/* A row of the held sort: a group's columns or a cell's code, a count, a loss */
static void
lion_anti_held_put(LionAntiState *as, const Datum *key, const bool *isnull,
				   int64 count, int64 sub)
{
	TupleTableSlot *in = as->heldin;
	int			g;

	ExecClearTuple(in);
	for (g = 0; g < as->nheldkey; g++)
	{
		in->tts_values[g] = key[g];
		in->tts_isnull[g] = isnull[g];
	}
	in->tts_values[as->nheldkey] = Int64GetDatum(count);
	in->tts_isnull[as->nheldkey] = false;
	in->tts_values[as->nheldkey + 1] = Int64GetDatum(sub);
	in->tts_isnull[as->nheldkey + 1] = false;
	ExecStoreVirtualTuple(in);
	tuplesort_puttupleslot(as->heldsort, in);
	ExecClearTuple(in);
}

static void
lion_anti_held_put_code(LionAntiState *as, uint64 code, int64 count,
						int64 sub)
{
	Datum		key = Int64GetDatum((int64) code);
	bool		isnull = false;

	lion_anti_held_put(as, &key, &isnull, count, sub);
}

/* Do the held sort's rows in heldcur and heldnext have one group or cell? */
static bool
lion_anti_held_same(LionAntiState *as)
{
	int			g;

	for (g = 0; g < as->nheldkey; g++)
	{
		bool		n1;
		bool		n2;
		Datum		d1 = slot_getattr(as->heldcur, g + 1, &n1);
		Datum		d2 = slot_getattr(as->heldnext, g + 1, &n2);

		if (ApplySortComparator(d1, n1, d2, n2, &as->heldssup[g]) != 0)
			return false;
	}
	return true;
}

/*
 * The held sort's next group or cell, once the count has given everything:
 * its first row in heldcur, and the counts and the losses of all its rows
 * added up.  False once there is none.
 */
static bool
lion_anti_held_next(LionAntiState *as, int64 *count, int64 *sub)
{
	TupleTableSlot *t;
	bool		isnull;

	if (!as->draining)
	{
		TuplesortInstrumentation stats;

		as->draining = true;
		tuplesort_performsort(as->heldsort);
		tuplesort_get_stats(as->heldsort, &stats);
		if (stats.spaceType == SORT_SPACE_TYPE_DISK)
			as->spilled |= LION_ANTI_SPILL_HELD;
		as->heldmore = tuplesort_gettupleslot(as->heldsort, true, true,
											  as->heldnext, NULL);
	}
	if (!as->heldmore)
		return false;
	t = as->heldcur;
	as->heldcur = as->heldnext;
	as->heldnext = t;
	*count = DatumGetInt64(slot_getattr(as->heldcur, as->nheldkey + 1,
										&isnull));
	*sub = DatumGetInt64(slot_getattr(as->heldcur, as->nheldkey + 2,
									  &isnull));
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		as->heldmore = tuplesort_gettupleslot(as->heldsort, true, true,
											  as->heldnext, NULL);
		if (!as->heldmore || !lion_anti_held_same(as))
			break;
		*count += DatumGetInt64(slot_getattr(as->heldnext, as->nheldkey + 1,
											 &isnull));
		*sub += DatumGetInt64(slot_getattr(as->heldnext, as->nheldkey + 2,
										   &isnull));
	}
	return true;
}

/*
 * The group a row found is subtracted from, made if there is none: in the
 * hash table of the groups' losses, whose memory is gcxt's.
 */
static LionAntiGroup *
lion_anti_group_of(LionAntiState *as, const Datum *key, const bool *isnull,
				   bool make)
{
	uint32		h = 0;
	LionAntiGroup *grp;
	int			g;

	for (g = 0; g < as->ngroup; g++)
	{
		uint32		gh = isnull[g] ? 0 :
			DatumGetUInt32(FunctionCall1Coll(&as->grouphash[g],
											 as->groupcoll[g], key[g]));

		h = hash_combine(h, gh);
	}
	if (as->gnbuckets > 0)
	{
		for (grp = as->gbuckets[h & (as->gnbuckets - 1)]; grp != NULL;
			 grp = grp->next)
		{
			bool		same = (grp->hash == h);

			for (g = 0; same && g < as->ngroup; g++)
			{
				if (grp->isnull[g] || isnull[g])
					same = (grp->isnull[g] && isnull[g]);
				else
					same = DatumGetBool(FunctionCall2Coll(&as->groupeq[g],
														  as->groupcoll[g],
														  grp->key[g],
														  key[g]));
			}
			if (same)
				return grp;
		}
	}
	if (!make)
		return NULL;

	/* a new group: the table grows at one entry per bucket */
	if (as->gnbuckets == 0 || as->ngroups >= as->gnbuckets)
	{
		int			n = (as->gnbuckets == 0) ? LION_ANTI_BUCKETS :
			as->gnbuckets * 2;
		LionAntiGroup **b = (LionAntiGroup **)
			MemoryContextAllocZero(as->gcxt, sizeof(LionAntiGroup *) * n);
		int			i;

		for (i = 0; i < as->gnbuckets; i++)
		{
			LionAntiGroup *next;

			for (grp = as->gbuckets[i]; grp != NULL; grp = next)
			{
				next = grp->next;
				grp->next = b[grp->hash & (n - 1)];
				b[grp->hash & (n - 1)] = grp;
			}
		}
		if (as->gbuckets != NULL)
			pfree(as->gbuckets);
		as->gbuckets = b;
		as->gnbuckets = n;
	}
	grp = (LionAntiGroup *) MemoryContextAllocZero(as->gcxt,
												   sizeof(LionAntiGroup));
	grp->hash = h;
	for (g = 0; g < as->ngroup; g++)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(as->gcxt);

		grp->isnull[g] = isnull[g];
		grp->key[g] = isnull[g] ? (Datum) 0 :
			datumCopy(key[g], as->groupbyval[g], as->grouplen[g]);
		MemoryContextSwitchTo(oldcxt);
	}
	grp->next = as->gbuckets[h & (as->gnbuckets - 1)];
	as->gbuckets[h & (as->gnbuckets - 1)] = grp;
	as->ngroups++;
	return grp;
}

/*
 * A row found taken from its group: in the hash table while its memory is
 * within hash_mem; past it, every group's loss so far goes to the held sort,
 * the table is let go of, and the losses after go there too.
 */
static void
lion_anti_group_lose(LionAntiState *as, const Datum *key, const bool *isnull)
{
	int			i;

	if (as->merging)
	{
		lion_anti_held_put(as, key, isnull, 0, 1);
		return;
	}
	lion_anti_group_of(as, key, isnull, true)->n++;
	if (MemoryContextMemAllocated(as->gcxt, true) <= as->memlimit &&
		as->ngroups < (int64) (MaxAllocSize / sizeof(LionAntiGroup *) / 2))
		return;

	lion_anti_held_begin(as);
	for (i = 0; i < as->gnbuckets; i++)
	{
		LionAntiGroup *grp;

		for (grp = as->gbuckets[i]; grp != NULL; grp = grp->next)
			lion_anti_held_put(as, grp->key, grp->isnull, 0, grp->n);
	}
	MemoryContextReset(as->gcxt);
	as->gbuckets = NULL;
	as->gnbuckets = 0;
	as->ngroups = 0;
}

static int
lion_anti_cell_cmp(const void *a, const void *b)
{
	uint64		x = ((const LionAntiCell *) a)->code;
	uint64		y = ((const LionAntiCell *) b)->code;

	return (x > y) - (x < y);
}

/* The cells' losses sorted by code, each code once with its rows. */
static void
lion_anti_cells_compact(LionAntiState *as)
{
	int			n = 0;
	int			i;

	if (as->nheld > 1)
		qsort(as->held, as->nheld, sizeof(LionAntiCell), lion_anti_cell_cmp);
	for (i = 0; i < as->nheld; i++)
	{
		if (n > 0 && as->held[n - 1].code == as->held[i].code)
			as->held[n - 1].sub += as->held[i].sub;
		else
			as->held[n++] = as->held[i];
	}
	as->nheld = n;
}

/*
 * A row taken from cell `code`: into the array of the cells' losses, which
 * grows to hash_mem and is then compacted, each code once; if that leaves it
 * more than half full, its losses go to the held sort, and those after them.
 */
static void
lion_anti_cell_lose(LionAntiState *as, uint64 code)
{
	LionAntiCell *c;

	if (!as->merging && as->nheld >= as->heldcap)
	{
		Size		most = Min(Max(as->memlimit / sizeof(LionAntiCell), 64),
							   MaxAllocSize / sizeof(LionAntiCell));

		if (as->heldcap == 0 || (Size) as->heldcap * 2 <= most)
		{
			int			cap = (as->heldcap == 0) ? 64 : as->heldcap * 2;

			as->held = (as->held == NULL) ?
				(LionAntiCell *) MemoryContextAlloc(as->gcxt,
													sizeof(LionAntiCell) * cap) :
				(LionAntiCell *) repalloc(as->held,
										  sizeof(LionAntiCell) * cap);
			as->heldcap = cap;
		}
		else
		{
			lion_anti_cells_compact(as);
			if (as->nheld > as->heldcap / 2)
			{
				int			i;

				lion_anti_held_begin(as);
				for (i = 0; i < as->nheld; i++)
					lion_anti_held_put_code(as, as->held[i].code, 0,
											as->held[i].sub);
				MemoryContextReset(as->gcxt);
				as->held = NULL;
				as->nheld = 0;
				as->heldcap = 0;
			}
		}
	}
	if (as->merging)
	{
		lion_anti_held_put_code(as, code, 0, 1);
		return;
	}
	c = &as->held[as->nheld++];
	c->code = code;
	c->sub = 1;
	c->held = 0;
}

/*
 * A row taken away beside a parallel count by cells, kept by the container
 * key its TID is in - the range it is counted in, the TID of a heap-only
 * tuple being in its chain's block, whose root the index has - until this
 * participant claims that range (lion_anti_cells_range()).
 */
static void
lion_anti_ranged_put(LionAntiState *as, uint32 ckey, uint64 code)
{
	TupleTableSlot *in = as->rangein;

	if (as->ranged == NULL)
	{
		AttrNumber	attno = 1;
		Oid			op = Int8LessOperator;
		Oid			coll = InvalidOid;
		bool		nulls = false;
		MemoryContext oldcxt = MemoryContextSwitchTo(as->cxt);

		as->ranged = tuplesort_begin_heap(as->rangedesc, 1, &attno, &op,
										  &coll, &nulls, work_mem, NULL,
										  TUPLESORT_NONE);
		MemoryContextSwitchTo(oldcxt);
	}
	ExecClearTuple(in);
	in->tts_values[0] = Int64GetDatum((int64) ckey);
	in->tts_isnull[0] = false;
	in->tts_values[1] = Int64GetDatum((int64) code);
	in->tts_isnull[1] = false;
	ExecStoreVirtualTuple(in);
	tuplesort_puttupleslot(as->ranged, in);
	ExecClearTuple(in);
}

/*
 * A row found that an inner row of its key matches, subtracted: from the
 * total; from its group; or from its cell, of the predicates' truths in it -
 * evaluated here unless a clause read them (lion_anti_matches()) - beside a
 * parallel count by cells once this participant claims its range.
 */
static void
lion_anti_subtract(LionCountScanState *st)
{
	LionAntiState *as = st->anti;
	Datum		key[2];
	bool		isnull[2];
	MemoryContext oldcxt;
	int			g;

	if (as->cells)
	{
		uint64		code;

		if (!as->predsread)
			lion_anti_cells_truths(as);
		code = lion_cells_code_of(st->cells, as->predtruth);

		if (st->shared != NULL)
			lion_anti_ranged_put(as,
								 lion_code_ckey((uint64)
												ItemPointerGetBlockNumber(&as->heapslot->tts_tid) <<
												LION_OFFSET_BITS),
								 code);
		else
		{
			as->subtracted++;
			lion_anti_cell_lose(as, code);
		}
		return;
	}
	as->subtracted++;
	if (as->ngroup == 0)
	{
		as->total++;
		return;
	}

	/* what hashing a key allocates goes with the row's tests */
	oldcxt = MemoryContextSwitchTo(as->econtext->ecxt_per_tuple_memory);
	for (g = 0; g < as->ngroup; g++)
		key[g] = slot_getattr(as->heapslot, as->groupattno[g], &isnull[g]);
	lion_anti_group_lose(as, key, isnull);
	MemoryContextSwitchTo(oldcxt);
}

/* ---------------------------------------------------------------------
 * The rows the keys find
 * --------------------------------------------------------------------- */

/*
 * Beside a count by cells, the truth of each predicate in the row in
 * heapslot (LION_TRUTH_*), into predtruth - what tells the row's cell - and,
 * as a boolean, into evalslot at the predicate's place, where a clause's
 * copy of it reads it.
 */
static void
lion_anti_cells_truths(LionAntiState *as)
{
	ExprContext *econtext = as->econtext;
	TupleTableSlot *es = as->evalslot;
	int			p;

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = as->heapslot;
	for (p = 0; p < as->npred; p++)
	{
		bool		isnull;
		Datum		d = ExecEvalExprSwitchContext(as->predstate[p], econtext,
												  &isnull);
		bool		b = !isnull && DatumGetBool(d);

		as->predtruth[p] = isnull ? LION_TRUTH_NULL :
			(b ? LION_TRUTH_TRUE : LION_TRUTH_FALSE);
		es->tts_values[as->predpos[p]] = BoolGetDatum(b);
		es->tts_isnull[as->predpos[p]] = isnull;
	}
}

/*
 * Does an inner row of the key looked up match the row in heapslot?  The
 * table's columns go into evalslot once, and the table's restrictions are
 * tested over them: a row they reject is no row the count counts, and no
 * join clause is run on it - core's scan of the table rejects it before its
 * join sees it.  On a row that passes them, the scan keys' clauses hold - the
 * lookup found it by them - and the other join clauses are tested over it
 * and each of the key's inner rows in turn, read back from the tuplestore.
 * Each list is in the order core would test it in (lion_anti_order_quals()).
 *
 * Beside a count by cells the predicates' truths go in first when a clause
 * reads them, through its copy of one (predsread): tests of a column against
 * constants, which a lion index answers (lion_plan_cells.c), and which fail
 * on no row.  When no clause does, only a row that matches needs them, to
 * tell its cell, and they are evaluated on it alone (lion_anti_subtract()):
 * the restrictions reject most of the rows a lookup finds where the WHERE is
 * narrower than the key.
 */
static bool
lion_anti_matches(LionAntiState *as)
{
	ExprContext *econtext = as->econtext;
	TupleTableSlot *es = as->evalslot;
	TupleTableSlot *in = as->innerslot;
	int			j;

	if (as->cells && as->predsread)
		lion_anti_cells_truths(as);
	for (j = 0; j < as->nouter; j++)
		es->tts_values[as->outerpos[j]] =
			slot_getattr(as->heapslot, as->outerattno[j],
						 &es->tts_isnull[as->outerpos[j]]);
	econtext->ecxt_scantuple = es;
	ResetExprContext(econtext);
	if (!ExecQual(as->restrictqual, econtext))
		return false;
	if (as->joinqual == NULL)
		return true;
	tuplestore_rescan(as->innerrows);
	while (tuplestore_gettupleslot(as->innerrows, true, false, in))
	{
		slot_getallattrs(in);
		for (j = 0; j < as->ninner; j++)
		{
			es->tts_values[as->innerpos[j]] = in->tts_values[as->nkey + j];
			es->tts_isnull[as->innerpos[j]] = in->tts_isnull[as->nkey + j];
		}
		ResetExprContext(econtext);
		if (ExecQual(as->joinqual, econtext))
		{
			ExecClearTuple(in);
			return true;
		}
	}
	ExecClearTuple(in);
	return false;
}

/* The key in keyvals looked up, and each row it finds tested. */
static void
lion_anti_lookup_key(LionCountScanState *st, IndexScanDesc scan)
{
	LionAntiState *as = st->anti;
	int			k;

	if (as->joinqual != NULL && !tuplestore_in_memory(as->innerrows))
		as->spilled |= LION_ANTI_SPILL_INNER;
	for (k = 0; k < as->nkey; k++)
		as->scankeys[k].sk_argument = as->keyvals[k];
	index_rescan(scan, as->scankeys, as->nkey, NULL, 0);
	as->lookups++;
	while (lion_index_getnext_slot(scan, ForwardScanDirection, as->heapslot))
	{
		CHECK_FOR_INTERRUPTS();
		as->fetched++;
		if (lion_anti_matches(as))
			lion_anti_subtract(st);
	}
}

/*
 * Each key looked up in the B-tree under the query's snapshot - the one the
 * count is made under, so that the rows found are rows the count counts
 * when they pass the restrictions (DESIGN.md §27, "Why the subtraction is
 * exact") - once, in the sort's order, which is the index's; and each row
 * found that a key's inner row matches subtracted.  Keys the sort keeps
 * apart are unequal under the B-tree's family, so no row is found by two.
 */
static void
lion_anti_lookup(LionCountScanState *st)
{
	LionAntiState *as = st->anti;
	EState	   *estate = st->css.ss.ps.state;
	IndexScanDesc scan;
	instr_time	start;
	bool		open = false;

	if (as->timing)
		INSTR_TIME_SET_CURRENT(start);

#if PG_VERSION_NUM >= 200000
	scan = index_beginscan(st->heap, as->index, false, estate->es_snapshot,
						   NULL, as->nkey, 0, SO_NONE);
#elif PG_VERSION_NUM >= 190000
	scan = index_beginscan(st->heap, as->index, estate->es_snapshot, NULL,
						   as->nkey, 0, SO_NONE);
#elif PG_VERSION_NUM >= 180000
	scan = index_beginscan(st->heap, as->index, estate->es_snapshot, NULL,
						   as->nkey, 0);
#else
	scan = index_beginscan(st->heap, as->index, estate->es_snapshot,
						   as->nkey, 0);
#endif

	for (;;)
	{
		TupleTableSlot *t;

		CHECK_FOR_INTERRUPTS();
		if (!tuplesort_gettupleslot(as->keysorted, true, true, as->sortcur,
									NULL))
			break;
		if (open && lion_anti_same_key(as))
		{
			/* another inner row of the key, kept unless it is the last's */
			if (as->joinqual != NULL && !lion_anti_same_inner(as))
				tuplestore_puttupleslot(as->innerrows, as->sortcur);
		}
		else
		{
			if (open)
				lion_anti_lookup_key(st, scan);
			lion_anti_key_begin(as);
			open = true;
		}
		t = as->sortprev;
		as->sortprev = as->sortcur;
		as->sortcur = t;
	}
	if (open)
		lion_anti_lookup_key(st, scan);
	index_endscan(scan);
	ExecClearTuple(as->heapslot);
	ExecClearTuple(as->sortcur);
	ExecClearTuple(as->sortprev);
	tuplesort_end(as->keysorted);
	as->keysorted = NULL;
	if (as->innerrows != NULL)
	{
		tuplestore_end(as->innerrows);
		as->innerrows = NULL;
	}

	if (as->timing)
	{
		instr_time	end;

		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(as->time[1], end, start);
	}
}

/*
 * The run, once before the count's first row.  Of a parallel count the
 * participant that comes here first makes it, and the others subtract
 * nothing: their partial counts and its own add up to the count, less the
 * rows it found once.  Of a parallel count by cells every participant makes
 * it, and takes away the rows of the ranges it counts.
 */
void
lion_anti_run(LionCountScanState *st)
{
	LionAntiState *as = st->anti;

	if (as == NULL || as->ran)
		return;
	as->ran = true;
	as->total = 0;
	if (st->shared != NULL && !as->cells &&
		pg_atomic_exchange_u32(&st->shared->antiowner, 1) != 0)
	{
		as->owner = false;
		return;
	}
	as->owner = true;
	lion_anti_open(st);
	lion_anti_collect(st);
	lion_anti_lookup(st);
}

/*
 * The row of one group about to be emitted, of `nkeys` GROUP BY keys - the
 * plan's columns in order - and its count, which loses the rows found of its
 * group (or, ungrouped, every row found) once: true when it has none left
 * and is no row - a group, or a GROUP BY the planner folded to one group -
 * where a plain count's one row stays a row of 0.  A count that would go
 * below zero is a row subtracted that the count did not count, which the
 * serial node says rather than emit; a participant's partial count may.
 *
 * Once the groups' losses have gone to the held sort, the group is held
 * back into it - true, no row now - and goes up when the count is done
 * (lion_anti_groups_next()), whose rows come here again, their counts what
 * is left.
 */
bool
lion_anti_adjust(LionCountScanState *st, int nkeys, const Datum *keys,
				 const bool *keyisnull, int64 *count)
{
	LionAntiState *as = st->anti;
	int64		n = 0;

	/* a cell loses its rows as the tally gives it up (lion_next_cells()) */
	if (as->cells || as->draining)
		return false;
	if (as->owner)
	{
		if (as->ngroup == 0)
		{
			n = as->total;
			as->total = 0;
		}
		else if (as->merging)
		{
			Assert(nkeys >= as->ngroup);
			lion_anti_held_put(as, keys, keyisnull, *count, 0);
			return true;
		}
		else
		{
			LionAntiGroup *grp;

			Assert(nkeys >= as->ngroup);
			grp = lion_anti_group_of(as, keys, keyisnull, false);
			if (grp != NULL)
			{
				n = grp->n;
				grp->n = 0;
			}
		}
	}
	*count -= n;
	if (!st->css.ss.ps.plan->parallel_aware)
	{
		if (*count < 0)
			elog(ERROR, "LionCount: the anti join subtracted %lld rows from a count of %lld",
				 (long long) n, (long long) (*count + n));
		return *count == 0 &&
			(as->ngroup > 0 || lion_plan_flag(st, LION_FLAG_SINGLEGROUP));
	}
	return false;
}

/* Are the GROUP BY's held groups going up (lion_anti_groups_next())? */
bool
lion_anti_draining(LionCountScanState *st)
{
	return st->anti != NULL && st->anti->draining && !st->anti->cells;
}

/*
 * The count's groups run out: the next group held back, with the count of
 * its rows less those it loses - each group of the held sort once, in its
 * order - or *exhausted when there is none, or nothing was held.  A group
 * that comes to nothing is no row; one that loses more rows than it has, or
 * rows the count did not count at all, is an error.  NULL with *exhausted
 * false is a group the HAVING rejected (lion_emit_keys()).
 */
TupleTableSlot *
lion_anti_groups_next(LionCountScanState *st, bool *exhausted)
{
	LionAntiState *as = st->anti;
	int64		count;
	int64		sub;

	*exhausted = true;
	if (!as->merging || as->cells)
		return NULL;
	while (lion_anti_held_next(as, &count, &sub))
	{
		Datum		keys[2];
		bool		isnull[2];
		int			g;

		if (count < sub)
			elog(ERROR, "LionCount: the anti join subtracted %lld rows from a count of %lld",
				 (long long) sub, (long long) count);
		if (count == sub)
			continue;
		for (g = 0; g < 2; g++)
		{
			keys[g] = (Datum) 0;
			isnull[g] = true;
			if (g < as->ngroup)
				keys[g] = slot_getattr(as->heldcur, g + 1, &isnull[g]);
		}
		*exhausted = false;
		return lion_emit_keys(st, 2, keys, isnull, count - sub);
	}
	return NULL;
}

/* ---------------------------------------------------------------------
 * Beside a count by cells
 * --------------------------------------------------------------------- */

/*
 * The container keys lo up to, not including, hi are counted by this
 * participant: the rows taken away there are its own to take away, read off
 * the sort of them by container key, which the ranges a participant claims
 * go through in order.  A serial count took every row away as it found it.
 * Before the tally gives its first cell.
 */
void
lion_anti_cells_range(LionCountScanState *st, uint32 lo, uint64 hi)
{
	LionAntiState *as = st->anti;

	if (!as->cells || as->heldready)
		elog(ERROR, "LionCount: an anti join's range out of its count by cells");
	if (st->shared == NULL)
		return;
	if ((uint64) lo < as->rangehi)
		elog(ERROR, "LionCount: an anti join's range before the last one");
	as->rangehi = hi;
	if (as->ranged == NULL)
		return;
	if (!as->rangesorted)
	{
		TuplesortInstrumentation stats;

		as->rangesorted = true;
		tuplesort_performsort(as->ranged);
		tuplesort_get_stats(as->ranged, &stats);
		if (stats.spaceType == SORT_SPACE_TYPE_DISK)
			as->spilled |= LION_ANTI_SPILL_RANGES;
		as->rangemore = tuplesort_gettupleslot(as->ranged, true, false,
											   as->rangeslot, NULL);
	}
	while (as->rangemore)
	{
		bool		isnull;
		uint64		ckey = (uint64) DatumGetInt64(slot_getattr(as->rangeslot, 1,
															   &isnull));

		CHECK_FOR_INTERRUPTS();
		if (ckey >= hi)
			break;
		if (ckey >= lo)
		{
			as->subtracted++;
			lion_anti_cell_lose(as,
								(uint64) DatumGetInt64(slot_getattr(as->rangeslot,
																	2, &isnull)));
		}
		as->rangemore = tuplesort_gettupleslot(as->ranged, true, false,
											   as->rangeslot, NULL);
	}
}

/*
 * The sort of the rows taken away by range let go of.  A participant that
 * claimed no range - the others took them all - never sorted it, and so
 * never asked it whether it went past work_mem (lion_anti_cells_range()):
 * it is asked here, since the rows that did not fit went to disk as they
 * went in.
 */
static void
lion_anti_ranged_end(LionAntiState *as)
{
	if (as->ranged == NULL)
		return;
	if (!as->rangesorted)
	{
		TuplesortInstrumentation stats;

		tuplesort_get_stats(as->ranged, &stats);
		if (stats.spaceType == SORT_SPACE_TYPE_DISK)
			as->spilled |= LION_ANTI_SPILL_RANGES;
	}
	tuplesort_end(as->ranged);
	as->ranged = NULL;
	as->rangemore = false;
}

/*
 * The cells this participant's rows are of, each once with the rows it
 * loses, in the order of their codes: made when the tally gives its first
 * cell, every range counted.  Of the held sort, nothing to make.
 */
static void
lion_anti_cells_ready(LionAntiState *as)
{
	if (as->heldready)
		return;
	as->heldready = true;
	if (as->ranged != NULL)
	{
		lion_anti_ranged_end(as);
		ExecClearTuple(as->rangeslot);
	}
	if (!as->merging)
		lion_anti_cells_compact(as);
	as->nextheld = 0;
}

/*
 * A count the tally gives of cell `code`: held back, true, when the cell
 * loses rows to the anti join - another count of it may follow, where the
 * tally spilled, and the rows come off the cell's whole count.  Once the
 * losses have gone to the held sort, every count is held back into it.
 */
bool
lion_anti_cells_hold(LionCountScanState *st, uint64 code, int64 count)
{
	LionAntiState *as = st->anti;
	LionAntiCell key;
	LionAntiCell *c;

	lion_anti_cells_ready(as);
	if (as->merging)
	{
		lion_anti_held_put_code(as, code, count, 0);
		return true;
	}
	if (as->nheld == 0)
		return false;
	key.code = code;
	c = (LionAntiCell *) bsearch(&key, as->held, as->nheld,
								 sizeof(LionAntiCell), lion_anti_cell_cmp);
	if (c == NULL)
		return false;
	c->held += count;
	return true;
}

/*
 * The tally given up, the next cell held back with rows left: its code and
 * their number, false once there is none.  A cell that comes to nothing is
 * no row.  A participant's every row taken away is a row of a range it
 * counted, so a cell with fewer rows than it loses is a row taken away that
 * the count did not count, which the node says rather than emit - of a
 * parallel count too.
 */
bool
lion_anti_cells_next(LionCountScanState *st, uint64 *code, int64 *count)
{
	LionAntiState *as = st->anti;
	int64		held;
	int64		sub;

	lion_anti_cells_ready(as);
	if (as->merging)
	{
		while (lion_anti_held_next(as, &held, &sub))
		{
			bool		isnull;

			if (held < sub)
				elog(ERROR, "LionCount: the anti join took %lld rows from a cell of %lld",
					 (long long) sub, (long long) held);
			if (held == sub)
				continue;
			*code = (uint64) DatumGetInt64(slot_getattr(as->heldcur, 1,
														&isnull));
			*count = held - sub;
			return true;
		}
		return false;
	}
	while (as->nextheld < as->nheld)
	{
		LionAntiCell *c = &as->held[as->nextheld++];

		if (c->held < c->sub)
			elog(ERROR, "LionCount: the anti join took %lld rows from a cell of %lld",
				 (long long) c->sub, (long long) c->held);
		if (c->held == c->sub)
			continue;
		*code = c->code;
		*count = c->held - c->sub;
		return true;
	}
	return false;
}

/*
 * The sorts and the tuplestore of a run let go of - their memory, and any
 * files they wrote - and the run's state cleared.
 */
static void
lion_anti_release(LionAntiState *as)
{
	if (as->keysorted != NULL)
		tuplesort_end(as->keysorted);
	if (as->innerrows != NULL)
		tuplestore_end(as->innerrows);
	if (as->heldsort != NULL)
		tuplesort_end(as->heldsort);
	lion_anti_ranged_end(as);
	as->keysorted = NULL;
	as->innerrows = NULL;
	as->heldsort = NULL;
	if (as->sortcur != NULL)
	{
		ExecClearTuple(as->sortcur);
		ExecClearTuple(as->sortprev);
		ExecClearTuple(as->innerslot);
	}
	if (as->heldcur != NULL)
	{
		ExecClearTuple(as->heldcur);
		ExecClearTuple(as->heldnext);
	}
	if (as->rangeslot != NULL)
		ExecClearTuple(as->rangeslot);
	as->merging = false;
	as->draining = false;
	as->heldmore = false;
	as->gbuckets = NULL;
	as->gnbuckets = 0;
	as->ngroups = 0;
	as->held = NULL;
	as->nheld = 0;
	as->heldcap = 0;
	as->nextheld = 0;
	as->heldready = false;
	as->rangemore = false;
	as->rangesorted = false;
	as->rangehi = 0;
	if (as->cxt != NULL)
		MemoryContextReset(as->cxt);
	if (as->gcxt != NULL)
		MemoryContextReset(as->gcxt);
	if (as->keycxt != NULL)
		MemoryContextReset(as->keycxt);
}

/*
 * A rescan: the run is made again - the table's restrictions and the inner
 * side's rows may have other parameters - and the child starts over, by
 * itself where a parameter it reads changed (core hands a changed parameter
 * to outer and inner plans, not to custom_ps).
 */
void
lion_anti_rescan(LionCountScanState *st, CustomScanState *node)
{
	LionAntiState *as = st->anti;

	if (as == NULL)
		return;
	as->ran = false;
	as->owner = false;
	as->total = 0;
	lion_anti_release(as);
	ResetExprContext(as->econtext);
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(as->child, node->ss.ps.chgParam);
	if (as->child->chgParam == NULL)
		ExecReScan(as->child);
}

/* The end of the node: the child, the B-tree and the run's memory. */
void
lion_anti_end(LionCountScanState *st)
{
	LionAntiState *as = st->anti;

	if (as == NULL)
		return;
	lion_anti_release(as);
	if (as->child != NULL)
	{
		ExecEndNode(as->child);
		as->child = NULL;
	}
	if (as->heapslot != NULL)
	{
		ExecDropSingleTupleTableSlot(as->heapslot);
		as->heapslot = NULL;
	}
	if (as->index != NULL)
	{
		index_close(as->index, NoLock);
		as->index = NULL;
	}
	if (as->cxt != NULL)
	{
		MemoryContextDelete(as->cxt);
		as->cxt = NULL;
	}
	if (as->gcxt != NULL)
	{
		MemoryContextDelete(as->gcxt);
		as->gcxt = NULL;
	}
	if (as->keycxt != NULL)
	{
		MemoryContextDelete(as->keycxt);
		as->keycxt = NULL;
	}
}

/* ---------------------------------------------------------------------
 * EXPLAIN
 * --------------------------------------------------------------------- */

/*
 * A join clause with each reference to a cells predicate's LION_TL_CELLPRED
 * place (lion_anti_columns()) replaced by the predicate itself, for EXPLAIN:
 * ruleutils.c puts a reference to an expression of custom_scan_tlist in
 * parentheses of its own, round the expression's, so `t.x = 1` in a join
 * clause beside a count by cells would print as `((x = 1))`.
 */
static Node *
lion_anti_cellpred_inline(Node *node, void *context)
{
	LionCountScanState *st = (LionCountScanState *) context;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		CustomScan *cscan = (CustomScan *) st->css.ss.ps.plan;
		List	   *tlist = cscan->custom_scan_tlist;

		if (var->varno == INDEX_VAR && var->varattno >= 1 &&
			var->varattno <= st->ntlist &&
			var->varattno <= list_length(tlist) &&
			LION_TL_IS_CELLPRED(st->tlkind[var->varattno - 1]))
			return (Node *) copyObject(((TargetEntry *)
										list_nth(tlist,
												 var->varattno - 1))->expr);
		return node;
	}
	return expression_tree_mutator(node, lion_anti_cellpred_inline, context);
}

/*
 * What the node does with its child (DESIGN.md §27): the kind of join, what
 * the rows it counts are, the B-tree the keys are looked up in, and the join
 * clauses every row found is tested on - as core prints a join's condition,
 * the inner side's columns and the table's both named, and a clause that is
 * a cells predicate printed as the predicate.
 */
void
lion_explain_anti(LionCountScanState *st, List *ancestors, ExplainState *es)
{
	LionAntiState *as = st->anti;
	List	   *context;
	Node	   *cond;

	ExplainPropertyText("Join Type", "Anti", es);
	ExplainPropertyText("Join Rows", "the rows counted, less those with a match",
						es);
	ExplainPropertyText("Join Lookup Index", get_rel_name(as->idxoid), es);
	context = set_deparse_context_plan(es->deparse_cxt, st->css.ss.ps.plan,
									   ancestors);
	cond = (Node *) make_ands_explicit(as->joinexprs);
	if (as->cells)
		cond = lion_anti_cellpred_inline(cond, st);
	ExplainPropertyText("Join Cond",
						deparse_expression(cond, context, true, false), es);
}

/*
 * What went past its memory (LION_ANTI_SPILL_*), if anything did: on to disk
 * past work_mem, the child's rows sorted by key, a key's inner rows, the sort
 * that holds the losses ("held rows") and the rows taken away sorted by
 * range; and past hash_mem, the groups' or the cells' losses, which then go
 * to that sort, the count's rows held back into it with them ("losses").
 */
static void
lion_explain_anti_spilled(uint32 spilled, ExplainState *es)
{
	static const struct
	{
		uint32		bit;
		const char *name;
	}			what[] = {
		{LION_ANTI_SPILL_KEYS, "keys"},
		{LION_ANTI_SPILL_INNER, "inner rows"},
		{LION_ANTI_SPILL_SUB, "losses"},
		{LION_ANTI_SPILL_HELD, "held rows"},
		{LION_ANTI_SPILL_RANGES, "ranges"},
	};
	StringInfoData buf;
	size_t		i;

	initStringInfo(&buf);
	for (i = 0; i < lengthof(what); i++)
	{
		if ((spilled & what[i].bit) == 0)
			continue;
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, what[i].name);
	}
	if (buf.len > 0)
		ExplainPropertyText("Join Spilled", buf.data, es);
	pfree(buf.data);
}

/*
 * EXPLAIN ANALYZE's: the child's rows, the distinct keys looked up, the rows
 * the lookups found and those subtracted - every participant's, of which one
 * makes the run, or each beside a count by cells, which subtracts the rows
 * of its own ranges - what of it went past its memory in any participant,
 * and, with TIMING, the time reading and sorting the child and looking the
 * keys up took.
 */
void
lion_explain_anti_counters(LionCountScanState *st, ExplainState *es)
{
	LionAntiState *as = st->anti;

	ExplainPropertyInteger("Join Child Rows", NULL,
						   as->childrows + as->wchildrows, es);
	ExplainPropertyInteger("Join Keys Looked Up", NULL,
						   as->lookups + as->wlookups, es);
	ExplainPropertyInteger("Join Rows Fetched", NULL,
						   as->fetched + as->wfetched, es);
	ExplainPropertyInteger("Join Rows Subtracted", NULL,
						   as->subtracted + as->wsubtracted, es);
	lion_explain_anti_spilled(as->spilled | as->wspilled, es);
	if (es->timing)
	{
		instr_time	t;

		t = as->time[0];
		INSTR_TIME_ADD(t, as->wtime[0]);
		ExplainPropertyFloat("Join Child Time", "ms",
							 INSTR_TIME_GET_MILLISEC(t), 3, es);
		t = as->time[1];
		INSTR_TIME_ADD(t, as->wtime[1]);
		ExplainPropertyFloat("Join Lookup Time", "ms",
							 INSTR_TIME_GET_MILLISEC(t), 3, es);
	}
}
