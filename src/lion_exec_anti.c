/*-------------------------------------------------------------------------
 *
 * lion_exec_anti.c
 *		The anti join over a small inner side, run (DESIGN.md §27, "The anti
 *		join over a small inner side"): the count of the table's WHERE, less
 *		the rows of it some row of the inner side matches.
 *
 *		Before the count's first row the node reads its child whole, keeps
 *		each distinct inner row once under the values of its scan keys, looks
 *		each key up in a B-tree of the table under the query's snapshot, and
 *		tests every row found - the table's own restrictions over the row, and
 *		on a row that passes them the anti join's clauses over the row and an
 *		inner row of the key - counting the ones that pass: into one total for
 *		an ungrouped count, into each one's group for a GROUP BY.  Each row it
 *		emits then has its count less that (lion_anti_adjust(), from
 *		lion_emit_keys()), and a group that comes to nothing is no row.
 *		Beside a count by cells each row that passes is kept with its cell,
 *		the predicates evaluated on it, and the cells lose their rows as the
 *		tally gives them up (lion_anti_cells_hold(), from lion_next_cells()).
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
#include "utils/hsearch.h"

#include "lion_customscan.h"

#define LION_ANTI_BUCKETS	64

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
 * (lion_anti_cells_truths()).
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
 * grouping has them (LION_PRIV_ANTI), and its type's length and passing.
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
		Oid			lhash;
		Oid			rhash;
		Oid			typid;
		int			n = as->ngroup;

		if (attno[g] == 0)
			break;
		if (!get_op_hash_functions(eqop, &lhash, &rhash) || !OidIsValid(lhash))
			elog(ERROR, "LionCount: no hash function for operator %u", eqop);
		as->groupattno[n] = attno[g];
		fmgr_info(get_opcode(eqop), &as->groupeq[n]);
		fmgr_info(lhash, &as->grouphash[n]);
		as->groupcoll[n] = priv->anti.groupcoll[g];
		typid = get_atttype(priv->heapoid, attno[g]);
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

/*
 * The anti join's state, from the plan's ANTI member and the expressions
 * after the residual clauses in custom_exprs: its child, initialized for
 * EXPLAIN too, which prints it under the node; its keys' values and clauses
 * ready to evaluate; beside a count by cells, its predicates; and its run's
 * memory.  The B-tree and the table's slot are opened at the first run.
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
	as->tids = (a->flags & LION_ANTIFLAG_TIDS) != 0;
	as->nkey = a->nkey;
	as->keycol = (AttrNumber *) palloc(sizeof(AttrNumber) * a->nkey);
	as->keyop = (Oid *) palloc(sizeof(Oid) * a->nkey);
	as->keycoll = (Oid *) palloc(sizeof(Oid) * a->nkey);
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
		as->keystate[i] = ExecInitExpr(kexpr, &node->ss.ps);
		get_typlenbyval(exprType((Node *) kexpr), &as->keylen[i],
						&as->keybyval[i]);
	}
	as->joinexprs = list_copy_head(list_copy_tail(mine, a->nkey), a->njoin);
	as->restrictexprs = list_copy_tail(mine, a->nkey + a->njoin);
	as->restrictqual = ExecInitQual(as->restrictexprs, &node->ss.ps);
	as->joinqual = ExecInitQual(as->joinexprs, &node->ss.ps);

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
	as->cxt = AllocSetContextCreate(estate->es_query_cxt,
									"LionCount anti join",
									ALLOCSET_DEFAULT_SIZES);
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
 * The plan's B-tree is one of the table's, and each scan key's column one of
 * its key columns, or the plan is not this build's.
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

static uint32
lion_anti_key_hash(LionAntiState *as, const Datum *keys)
{
	uint32		h = 0;
	int			k;

	for (k = 0; k < as->nkey; k++)
		h = hash_combine(h, datum_image_hash(keys[k], as->keybyval[k],
											 as->keylen[k]));
	return h;
}

static void
lion_anti_key_grow(LionAntiState *as)
{
	int			n = (as->knbuckets == 0) ? LION_ANTI_BUCKETS :
		as->knbuckets * 2;
	LionAntiKeyEnt **b = (LionAntiKeyEnt **)
		MemoryContextAllocZero(as->cxt, sizeof(LionAntiKeyEnt *) * n);
	LionAntiKeyEnt *e;

	for (e = as->kfirst; e != NULL; e = e->link)
	{
		e->next = b[e->hash & (n - 1)];
		b[e->hash & (n - 1)] = e;
	}
	if (as->kbuckets != NULL)
		pfree(as->kbuckets);
	as->kbuckets = b;
	as->knbuckets = n;
}

/*
 * The entry of these scan-key values, made if there is none.  Values the
 * node keeps apart by their bytes: equal keys of a type whose equal values
 * may differ in them are two lookups, which find the same rows, and the TIDs
 * of the rows matched keep each counted once (LION_ANTIFLAG_TIDS).
 */
static LionAntiKeyEnt *
lion_anti_key_entry(LionAntiState *as, const Datum *keys)
{
	uint32		h = lion_anti_key_hash(as, keys);
	LionAntiKeyEnt *e;
	int			k;

	if (as->knbuckets == 0 || as->nkeys >= as->knbuckets)
		lion_anti_key_grow(as);
	for (e = as->kbuckets[h & (as->knbuckets - 1)]; e != NULL; e = e->next)
	{
		bool		same = (e->hash == h);

		for (k = 0; same && k < as->nkey; k++)
			same = datum_image_eq(e->keys[k], keys[k], as->keybyval[k],
								  as->keylen[k]);
		if (same)
			return e;
	}

	e = (LionAntiKeyEnt *) MemoryContextAllocZero(as->cxt,
												  sizeof(LionAntiKeyEnt));
	e->hash = h;
	e->keys = (Datum *) MemoryContextAlloc(as->cxt,
										   sizeof(Datum) * as->nkey);
	for (k = 0; k < as->nkey; k++)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(as->cxt);

		e->keys[k] = datumCopy(keys[k], as->keybyval[k], as->keylen[k]);
		MemoryContextSwitchTo(oldcxt);
	}
	e->next = as->kbuckets[h & (as->knbuckets - 1)];
	as->kbuckets[h & (as->knbuckets - 1)] = e;
	if (as->klast != NULL)
		as->klast->link = e;
	else
		as->kfirst = e;
	as->klast = e;
	as->nkeys++;
	return e;
}

/*
 * The inner row in evalslot, kept under its key's entry unless an inner row
 * of the same bytes is there already: it would match the same rows.
 */
static void
lion_anti_keep_inner(LionAntiState *as, LionAntiKeyEnt *e)
{
	TupleTableSlot *es = as->evalslot;
	LionAntiInner *in;
	MemoryContext oldcxt;
	int			j;

	for (in = e->inner; in != NULL; in = in->next)
	{
		bool		same = true;

		for (j = 0; same && j < as->ninner; j++)
		{
			int			pos = as->innerpos[j];

			if (in->isnull[j] || es->tts_isnull[pos])
				same = (in->isnull[j] && es->tts_isnull[pos]);
			else
				same = datum_image_eq(in->values[j], es->tts_values[pos],
									  as->innerbyval[j], as->innerlen[j]);
		}
		if (same)
			return;
	}

	oldcxt = MemoryContextSwitchTo(as->cxt);
	in = (LionAntiInner *) palloc(sizeof(LionAntiInner));
	in->values = (Datum *) palloc(sizeof(Datum) * Max(as->ninner, 1));
	in->isnull = (bool *) palloc(sizeof(bool) * Max(as->ninner, 1));
	for (j = 0; j < as->ninner; j++)
	{
		int			pos = as->innerpos[j];

		in->isnull[j] = es->tts_isnull[pos];
		in->values[j] = in->isnull[j] ? (Datum) 0 :
			datumCopy(es->tts_values[pos], as->innerbyval[j],
					  as->innerlen[j]);
	}
	in->next = e->inner;
	e->inner = in;
	MemoryContextSwitchTo(oldcxt);
}

/*
 * Every row of the child, read once: its scan keys' values, and the row kept
 * under them - a key with a NULL value matches nothing (each key is a strict
 * operator) and is skipped.  The child runs in the caller's memory, which is
 * the query's: a node may keep there what it makes on its first row.
 */
static void
lion_anti_collect(LionCountScanState *st)
{
	LionAntiState *as = st->anti;
	ExprContext *econtext = as->econtext;
	TupleTableSlot *es = as->evalslot;
	Datum		keys[LION_MAX_ANTI_KEYS];
	instr_time	start;
	int			j;

	if (as->timing)
		INSTR_TIME_SET_CURRENT(start);
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
		for (k = 0; k < as->nkey; k++)
		{
			bool		isnull;

			keys[k] = ExecEvalExprSwitchContext(as->keystate[k], econtext,
												&isnull);
			if (isnull)
			{
				anynull = true;
				break;
			}
		}
		if (anynull)
			continue;
		lion_anti_keep_inner(as, lion_anti_key_entry(as, keys));
	}
	if (as->timing)
	{
		instr_time	end;

		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(as->time[0], end, start);
	}
}

/* ---------------------------------------------------------------------
 * The rows the keys find
 * --------------------------------------------------------------------- */

/* The group of the row in heapslot, made if there is none. */
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
			MemoryContextAllocZero(as->cxt, sizeof(LionAntiGroup *) * n);
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
	grp = (LionAntiGroup *) MemoryContextAllocZero(as->cxt,
												   sizeof(LionAntiGroup));
	grp->hash = h;
	for (g = 0; g < as->ngroup; g++)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(as->cxt);

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
 * Does an inner row of the key match the row in heapslot?  The table's
 * columns go into evalslot once, and the table's restrictions are tested over
 * them: a row they reject is no row the count counts, and no join clause is
 * run on it - core's scan of the table rejects it before its join sees it.
 * On a row that passes them, each inner row's columns go in beside the
 * table's in turn, and the join clauses are tested over the two.  Each list
 * is in the order core would test it in (lion_anti_order_quals()).
 *
 * Beside a count by cells the predicates' truths go in first, where the
 * restrictions' copies of them read them: tests of a column against
 * constants, which a lion index answers (lion_plan_cells.c), and which fail
 * on no row.
 */
static bool
lion_anti_matches(LionAntiState *as, LionAntiKeyEnt *e)
{
	ExprContext *econtext = as->econtext;
	TupleTableSlot *es = as->evalslot;
	LionAntiInner *in;
	int			j;

	if (as->cells)
		lion_anti_cells_truths(as);
	for (j = 0; j < as->nouter; j++)
		es->tts_values[as->outerpos[j]] =
			slot_getattr(as->heapslot, as->outerattno[j],
						 &es->tts_isnull[as->outerpos[j]]);
	econtext->ecxt_scantuple = es;
	ResetExprContext(econtext);
	if (!ExecQual(as->restrictqual, econtext))
		return false;
	for (in = e->inner; in != NULL; in = in->next)
	{
		for (j = 0; j < as->ninner; j++)
		{
			es->tts_values[as->innerpos[j]] = in->values[j];
			es->tts_isnull[as->innerpos[j]] = in->isnull[j];
		}
		ResetExprContext(econtext);
		if (ExecQual(as->joinqual, econtext))
			return true;
	}
	return false;
}

/*
 * A row taken away from a count by cells: its cell, of the truths
 * lion_anti_matches() found, and the container key its TID is in - the
 * range of a parallel count it is counted in, the TID of a heap-only tuple
 * being in its chain's block, whose root the index has.  Of this
 * participant once its range is known (lion_anti_cells_range()).
 */
static void
lion_anti_cells_keep(LionCountScanState *st)
{
	LionAntiState *as = st->anti;
	LionAntiCellRow *row;

	if (as->ncellrows >= as->cellrowscap)
	{
		int			cap = (as->cellrowscap == 0) ? 64 : as->cellrowscap * 2;

		if ((Size) cap > MaxAllocSize / sizeof(LionAntiCellRow))
			elog(ERROR, "LionCount: too many rows for the anti join to take away");
		as->cellrows = (as->cellrows == NULL) ?
			(LionAntiCellRow *) MemoryContextAlloc(as->cxt,
												   sizeof(LionAntiCellRow) * cap) :
			(LionAntiCellRow *) repalloc(as->cellrows,
										 sizeof(LionAntiCellRow) * cap);
		as->cellrowscap = cap;
	}
	row = &as->cellrows[as->ncellrows++];
	row->code = lion_cells_code_of(st->cells, as->predtruth);
	row->ckey = lion_code_ckey((uint64)
							   ItemPointerGetBlockNumber(&as->heapslot->tts_tid) <<
							   LION_OFFSET_BITS);
	row->mine = false;
}

/*
 * Each key looked up in the B-tree under the query's snapshot - the one the
 * count is made under, so that the rows found are rows the count counts
 * when they pass the restrictions (DESIGN.md §27, "Why the subtraction is
 * exact") - and each row found that a key's inner row matches subtracted:
 * from the total, or from its group; or kept with its cell, beside a count
 * by cells.
 */
static void
lion_anti_lookup(LionCountScanState *st)
{
	LionAntiState *as = st->anti;
	EState	   *estate = st->css.ss.ps.state;
	IndexScanDesc scan;
	LionAntiKeyEnt *e;
	instr_time	start;
	int			k;

	if (as->kfirst == NULL)
		return;
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

	if (as->tids && as->tidset == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ItemPointerData);
		ctl.entrysize = sizeof(ItemPointerData);
		ctl.hcxt = as->cxt;
		as->tidset = hash_create("LionCount anti join rows", 1024, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	for (e = as->kfirst; e != NULL; e = e->link)
	{
		CHECK_FOR_INTERRUPTS();
		for (k = 0; k < as->nkey; k++)
			as->scankeys[k].sk_argument = e->keys[k];
		index_rescan(scan, as->scankeys, as->nkey, NULL, 0);
		as->lookups++;
		while (lion_index_getnext_slot(scan, ForwardScanDirection,
									   as->heapslot))
		{
			bool		found;

			as->fetched++;
			if (as->tids &&
				hash_search(as->tidset, &as->heapslot->tts_tid, HASH_FIND,
							NULL) != NULL)
				continue;
			if (!lion_anti_matches(as, e))
				continue;
			if (as->tids)
				(void) hash_search(as->tidset, &as->heapslot->tts_tid,
								   HASH_ENTER, &found);
			if (as->cells)
				lion_anti_cells_keep(st);
			else if (as->ngroup == 0)
			{
				as->subtracted++;
				as->total++;
			}
			else
			{
				Datum		key[2];
				bool		isnull[2];
				int			g;
				MemoryContext oldcxt;

				/* what hashing a key allocates goes with the row's tests */
				oldcxt = MemoryContextSwitchTo(as->econtext->ecxt_per_tuple_memory);
				for (g = 0; g < as->ngroup; g++)
					key[g] = slot_getattr(as->heapslot, as->groupattno[g],
										  &isnull[g]);
				lion_anti_group_of(as, key, isnull, true)->n++;
				MemoryContextSwitchTo(oldcxt);
				as->subtracted++;
			}
		}
	}
	index_endscan(scan);
	ExecClearTuple(as->heapslot);

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
 */
bool
lion_anti_adjust(LionCountScanState *st, int nkeys, const Datum *keys,
				 const bool *keyisnull, int64 *count)
{
	LionAntiState *as = st->anti;
	int64		n = 0;

	/* a cell loses its rows as the tally gives it up (lion_next_cells()) */
	if (as->cells)
		return false;
	if (as->owner)
	{
		if (as->ngroup == 0)
		{
			n = as->total;
			as->total = 0;
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

/* ---------------------------------------------------------------------
 * Beside a count by cells
 * --------------------------------------------------------------------- */

/*
 * The container keys lo up to, not including, hi are counted by this
 * participant - all of them, of a serial count: the rows taken away there
 * are its own to take away.  Before the tally gives its first cell.
 */
void
lion_anti_cells_range(LionCountScanState *st, uint32 lo, uint64 hi)
{
	LionAntiState *as = st->anti;
	int			i;

	if (!as->cells || as->heldready)
		elog(ERROR, "LionCount: an anti join's range out of its count by cells");
	for (i = 0; i < as->ncellrows; i++)
	{
		LionAntiCellRow *row = &as->cellrows[i];

		if (!row->mine && row->ckey >= lo && (uint64) row->ckey < hi)
		{
			row->mine = true;
			as->subtracted++;
		}
	}
}

static int
lion_anti_cell_cmp(const void *a, const void *b)
{
	uint64		x = ((const LionAntiCell *) a)->code;
	uint64		y = ((const LionAntiCell *) b)->code;

	return (x > y) - (x < y);
}

/*
 * The cells this participant's rows are of, each once with the rows it
 * loses, in the order of their codes: made when the tally gives its first
 * cell, every range counted.
 */
static void
lion_anti_cells_ready(LionAntiState *as)
{
	int			n = 0;
	int			i;

	if (as->heldready)
		return;
	as->heldready = true;
	for (i = 0; i < as->ncellrows; i++)
	{
		if (as->cellrows[i].mine)
			n++;
	}
	as->held = (LionAntiCell *)
		MemoryContextAllocZero(as->cxt, sizeof(LionAntiCell) * Max(n, 1));
	for (i = 0; i < as->ncellrows; i++)
	{
		if (as->cellrows[i].mine)
			as->held[as->nheld++].code = as->cellrows[i].code;
	}
	Assert(as->nheld == n);
	if (n > 1)
		qsort(as->held, n, sizeof(LionAntiCell), lion_anti_cell_cmp);

	/* ... each code once, with the rows of it */
	n = 0;
	for (i = 0; i < as->nheld; i++)
	{
		if (n > 0 && as->held[n - 1].code == as->held[i].code)
			as->held[n - 1].sub++;
		else
		{
			as->held[n].code = as->held[i].code;
			as->held[n].sub = 1;
			as->held[n].held = 0;
			n++;
		}
	}
	as->nheld = n;
	as->nextheld = 0;
}

/*
 * A count the tally gives of cell `code`: held back, true, when the cell
 * loses rows to the anti join - another count of it may follow, where the
 * tally spilled, and the rows come off the cell's whole count.
 */
bool
lion_anti_cells_hold(LionCountScanState *st, uint64 code, int64 count)
{
	LionAntiState *as = st->anti;
	LionAntiCell key;
	LionAntiCell *c;

	lion_anti_cells_ready(as);
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

	lion_anti_cells_ready(as);
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
	as->kbuckets = NULL;
	as->knbuckets = 0;
	as->nkeys = 0;
	as->kfirst = NULL;
	as->klast = NULL;
	as->gbuckets = NULL;
	as->gnbuckets = 0;
	as->ngroups = 0;
	as->tidset = NULL;
	as->cellrows = NULL;
	as->ncellrows = 0;
	as->cellrowscap = 0;
	as->held = NULL;
	as->nheld = 0;
	as->nextheld = 0;
	as->heldready = false;
	MemoryContextReset(as->cxt);
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
}

/* ---------------------------------------------------------------------
 * EXPLAIN
 * --------------------------------------------------------------------- */

/*
 * What the node does with its child (DESIGN.md §27): the kind of join, what
 * the rows it counts are, the B-tree the keys are looked up in, and the join
 * clauses every row found is tested on - as core prints a join's condition,
 * the inner side's columns and the table's both named.
 */
void
lion_explain_anti(LionCountScanState *st, List *ancestors, ExplainState *es)
{
	LionAntiState *as = st->anti;
	List	   *context;

	ExplainPropertyText("Join Type", "Anti", es);
	ExplainPropertyText("Join Rows", "the rows counted, less those with a match",
						es);
	ExplainPropertyText("Join Lookup Index", get_rel_name(as->idxoid), es);
	context = set_deparse_context_plan(es->deparse_cxt, st->css.ss.ps.plan,
									   ancestors);
	ExplainPropertyText("Join Cond",
						deparse_expression((Node *)
										   make_ands_explicit(as->joinexprs),
										   context, true, false),
						es);
}

/*
 * EXPLAIN ANALYZE's: the child's rows, the distinct keys looked up, the rows
 * the lookups found and those subtracted - every participant's, of which one
 * makes the run, or each beside a count by cells, which subtracts the rows
 * of its own ranges - and, with TIMING, the time reading the child and
 * looking the keys up took.
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
