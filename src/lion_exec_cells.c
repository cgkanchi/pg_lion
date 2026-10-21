/*-------------------------------------------------------------------------
 *
 * lion_exec_cells.c
 *		Counts by cells (DESIGN.md §10, "Counts by cells"), as the node runs
 *		them: the labels of each dimension located, the driver walked a pass
 *		at a time and every row it keeps counted under its cell
 *		(lion_count_cells()), and each cell with rows sent up with the truth
 *		of every predicate in it - serially, or by ranges of container keys
 *		in parallel.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/* The most keys of a multi-key dimension whose outcomes are tabled */
#define LION_CELLS_MAP_KEYS		16

/*
 * The run's state, for a plan that counts by cells: each dimension's weight
 * in a code, the last dimension's 1, and the memory the labels take.  The
 * driver's walk is the decoded walk's (lion_begin_decoded_walk()), of one
 * column.
 */
void
lion_begin_cells(LionCountScanState *st, const LionCountPriv *priv,
				 EState *estate)
{
	LionCellsRun *cr;
	MemoryContext oldcxt;
	uint64		weight = 1;
	int			d;

	st->cells = NULL;
	if (priv->ncelldim == 0)
		return;
	if (st->decode == NULL || st->decode->ncol != 1)
		elog(ERROR, "LionCount: a count by cells without its driver's walk");

	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	cr = (LionCellsRun *) palloc0(sizeof(LionCellsRun));
	cr->ndim = priv->ncelldim;
	cr->npred = priv->ncellpred;
	cr->drv = priv->celldrv;
	cr->dimpriv = priv->celldim;
	cr->pred = priv->cellpred;
	cr->idx = (Relation *) palloc0(sizeof(Relation) * cr->ndim);
	cr->idxcol = (AttrNumber *) palloc0(sizeof(AttrNumber) * cr->ndim);
	cr->radix = (int *) palloc(sizeof(int) * cr->ndim);
	cr->dims = (LionCellDim *) palloc0(sizeof(LionCellDim) * cr->ndim);
	for (d = cr->ndim - 1; d >= 0; d--)
	{
		const LionCellsDimPriv *pv = &cr->dimpriv[d];

		cr->radix[d] = pv->radix;
		cr->dims[d].multi = (pv->kind == LION_CELLDIM_MULTI);
		cr->dims[d].weight = weight;
		cr->dims[d].nullout = pv->nullout;
		cr->dims[d].nbit = pv->nbit;
		if ((uint64) pv->radix > (UINT64CONST(1) << 62) / weight)
			elog(ERROR, "LionCount: the cells of a count by cells do not fit a code");
		weight *= (uint64) pv->radix;
	}
	cr->ncells = weight;
	cr->outs = (int *) palloc0(sizeof(int) * cr->ndim);
	cr->truth = (uint8 *) palloc0(sizeof(uint8) * cr->npred);
	cr->labelcxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount cell labels",
										 ALLOCSET_DEFAULT_SIZES);
	if (cr->drv >= 0 && cr->dimpriv[cr->drv].nval > 0)
	{
		fmgr_info_cxt(get_opcode(cr->dimpriv[cr->drv].eqop), &cr->drveq,
					  estate->es_query_cxt);
		cr->hasdrveq = true;
	}
	MemoryContextSwitchTo(oldcxt);
	st->cells = cr;
}

/*
 * The relation is opened (lion_open_relation()): each dimension's index but
 * the driver's, which the walk has opened, its key column found in the index
 * as it is (DESIGN.md §24).  Under SERIALIZABLE each label's lookup locks the
 * page that stands for its key, found or not (DESIGN.md §9).
 */
void
lion_cells_open(LionCountScanState *st)
{
	LionCellsRun *cr = st->cells;
	int			d;

	if (cr == NULL)
		return;
	for (d = 0; d < cr->ndim; d++)
	{
		const LionCellsDimPriv *pv = &cr->dimpriv[d];

		if (d == cr->drv)
			continue;
		cr->idx[d] = index_open(pv->idxoid, AccessShareLock);
		cr->idxcol[d] =
			lion_index_col_for(cr->idx[d],
							   lion_heap_attno_in(st->heap, st->plan.heapoid,
												  pv->attno),
							   pv->kind == LION_CELLDIM_MULTI,
							   st->plan.vcols);
	}
}

/* ... and closed, its labels gone first */
void
lion_cells_close(LionCountScanState *st)
{
	LionCellsRun *cr = st->cells;
	int			d;

	if (cr == NULL)
		return;
	lion_cells_reset(st);
	for (d = 0; d < cr->ndim; d++)
	{
		if (cr->idx[d] != NULL)
			index_close(cr->idx[d], AccessShareLock);
		cr->idx[d] = NULL;
		cr->idxcol[d] = 0;
	}
}

/*
 * The run ends, or begins again for a rescan: the labels go, to be located
 * again by the next run.  The driver's walk and the tally are the decoded
 * walk's, which lion_decode_reset() resets.
 */
void
lion_cells_reset(LionCountScanState *st)
{
	LionCellsRun *cr = st->cells;
	int			d;

	if (cr == NULL)
		return;
	MemoryContextReset(cr->labelcxt);
	cr->located = false;
	cr->nlabel = 0;
	cr->labels = NULL;
	cr->labelsets = NULL;
	cr->images = NULL;
	cr->drvadd = NULL;
	for (d = 0; d < cr->ndim; d++)
	{
		cr->dims[d].nkeys = 0;
		cr->dims[d].trees = NULL;
		cr->dims[d].map = NULL;
	}
}

/* A copy of key tree n, every key numbered `offset` further on */
static LionKeyNode *
lion_cells_tree_copy(const LionKeyNode *n, int offset)
{
	LionKeyNode *c = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	int			i;

	if (n->kind != LION_KN_KEY && n->kind != LION_KN_AND &&
		n->kind != LION_KN_OR)
		elog(ERROR, "LionCount: a key tree of kind %d in a count by cells",
			 (int) n->kind);
	c->kind = n->kind;
	c->keyno = (n->kind == LION_KN_KEY) ? n->keyno + offset : 0;
	c->nargs = n->nargs;
	if (n->nargs > 0)
	{
		c->args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * n->nargs);
		for (i = 0; i < n->nargs; i++)
			c->args[i] = lion_cells_tree_copy(n->args[i], offset);
	}
	return c;
}

/* One more label: the set lookup located, unpinned */
static void
lion_cells_add_label(LionCellsRun *cr, int *cap, int d, uint64 bits,
					 const LionPostingSet *ps)
{
	if (cr->nlabel >= *cap)
	{
		*cap = Max(*cap * 2, 16);
		if (cr->labelsets == NULL)
		{
			cr->labelsets = (LionPostingSet *)
				palloc(sizeof(LionPostingSet) * *cap);
			cr->labels = (LionCellLabel *) palloc(sizeof(LionCellLabel) * *cap);
		}
		else
		{
			cr->labelsets = (LionPostingSet *)
				repalloc(cr->labelsets, sizeof(LionPostingSet) * *cap);
			cr->labels = (LionCellLabel *)
				repalloc(cr->labels, sizeof(LionCellLabel) * *cap);
		}
	}
	cr->labelsets[cr->nlabel] = *ps;
	lion_posting_set_unpin(&cr->labelsets[cr->nlabel]);
	cr->labels[cr->nlabel].dim = d;
	cr->labels[cr->nlabel].bits = bits;
	cr->nlabel++;
}

/*
 * Every dimension's labels but the driver's, a dimension's together: a
 * scalar one's the entries of its values whose outcome is not 0 and its NULL
 * entry when NULL's is not, each set's bits its outcome; a multi-key one's
 * the keys each leaf's query extracts - the same query, by the same
 * function, as the plan was made for, so exact - each key a bit of the
 * row's mask, and its NULL entry.  Located after the snapshot was taken, and
 * unpinned: they only label the driver's rows (lion_count_cells()).
 */
static void
lion_cells_locate(LionCountScanState *st)
{
	LionCellsRun *cr = st->cells;
	MemoryContext oldcxt;
	int			cap = 0;
	int			d;
	int			l;

	if (cr->located)
		return;
	oldcxt = MemoryContextSwitchTo(cr->labelcxt);
	cr->nlabel = 0;
	for (d = 0; d < cr->ndim; d++)
	{
		const LionCellsDimPriv *pv = &cr->dimpriv[d];
		LionCellDim *dim = &cr->dims[d];
		Relation	idx = cr->idx[d];
		AttrNumber	col = cr->idxcol[d];
		LionPostingSet ps;

		if (d == cr->drv)
			continue;
		Assert(idx != NULL);
		if (pv->kind == LION_CELLDIM_SCALAR)
		{
			ListCell   *lc;
			int			i = 0;

			foreach(lc, pv->vals)
			{
				Const	   *c = (Const *) lfirst(lc);

				(void) lion_posting_set_lookup_col(idx, col, c->constvalue,
												   c->consttype, &ps);
				lion_cells_add_label(cr, &cap, d, (uint64) pv->valout[i], &ps);
				i++;
				CHECK_FOR_INTERRUPTS();
			}
			if (pv->nullout != 0)
			{
				(void) lion_posting_set_lookup_null_col(idx, col, &ps);
				lion_cells_add_label(cr, &cap, d, (uint64) pv->nullout, &ps);
			}
		}
		else
		{
			LionState  *istate = lion_index_column_state(idx, col);
			Datum	   *keys = (Datum *) palloc(sizeof(Datum) * 64);
			int			nkeys = 0;
			int			b;
			int			k;

			dim->trees = (LionKeyNode **)
				palloc0(sizeof(LionKeyNode *) * Max(pv->nbit, 1));
			for (b = 0; b < pv->nbit; b++)
			{
				StrategyNumber strategy =
					get_op_opfamily_strategy(pv->bitop[b],
											 idx->rd_opfamily[col - 1]);
				LionQuery	q;
				int		   *map;
				LionKeyNode *tree;

				lion_extract_query(istate, pv->bitquery[b]->constvalue,
								   strategy, &q);
				if (q.mode != LION_QMODE_KEYS || q.nkeys <= 0)
					elog(ERROR, "roaring count: query for index \"%s\" is no longer exact",
						 RelationGetRelationName(idx));

				/* each key once in the dimension, numbered among its keys */
				map = (int *) palloc(sizeof(int) * q.nkeys);
				for (k = 0; k < q.nkeys; k++)
				{
					int			j;

					for (j = 0; j < nkeys; j++)
					{
						if (datumIsEqual(keys[j], q.keys[k], istate->typbyval,
										 istate->typlen))
							break;
					}
					if (j == nkeys)
					{
						if (nkeys >= LION_CELLS_MAX_KEYS)
							elog(ERROR, "LionCount: more than %d keys in one column of a count by cells",
								 LION_CELLS_MAX_KEYS);
						keys[nkeys++] = q.keys[k];
					}
					map[k] = j;
				}
				tree = lion_cells_tree_copy(q.tree, 0);

				/* the tree's keys renumbered as the dimension's */
				{
					LionKeyNode **stack = (LionKeyNode **)
						palloc(sizeof(LionKeyNode *) * 1024);
					int			nstack = 0;

					stack[nstack++] = tree;
					while (nstack > 0)
					{
						LionKeyNode *n = stack[--nstack];
						int			a;

						if (n->kind == LION_KN_KEY)
						{
							if (n->keyno < 0 || n->keyno >= q.nkeys)
								elog(ERROR, "LionCount: a key tree names key %d of %d",
									 n->keyno, q.nkeys);
							n->keyno = map[n->keyno];
							continue;
						}
						for (a = 0; a < n->nargs; a++)
						{
							if (nstack >= 1024)
								elog(ERROR, "LionCount: a key tree too deep for a count by cells");
							stack[nstack++] = n->args[a];
						}
					}
				}
				dim->trees[b] = tree;
			}
			dim->nkeys = nkeys;
			for (k = 0; k < nkeys; k++)
			{
				(void) lion_posting_set_lookup_col(idx, col, keys[k], InvalidOid,
												   &ps);
				lion_cells_add_label(cr, &cap, d, UINT64CONST(1) << k, &ps);
				CHECK_FOR_INTERRUPTS();
			}
			(void) lion_posting_set_lookup_null_col(idx, col, &ps);
			lion_cells_add_label(cr, &cap, d, LION_CELL_NULLBIT, &ps);

			/* the outcome of every mask, when there are few keys */
			dim->map = NULL;
			if (nkeys <= LION_CELLS_MAP_KEYS)
			{
				uint16	   *m = (uint16 *) palloc(sizeof(uint16) *
												  ((Size) 1 << nkeys));
				uint64		mask;

				for (mask = 0; mask < (UINT64CONST(1) << nkeys); mask++)
					m[mask] = (uint16) lion_cells_outcome(dim, mask);
				dim->map = m;
			}
		}
	}

	/* each label's set where the engine finds it, and a page image each */
	for (l = 0; l < cr->nlabel; l++)
		cr->labels[l].set = &cr->labelsets[l];
	cr->images = (PGAlignedBlock *)
		MemoryContextAllocHuge(cr->labelcxt,
							   sizeof(PGAlignedBlock) * (Size) Max(cr->nlabel, 1));
	for (l = 0; l < cr->nlabel; l++)
		cr->labels[l].image = &cr->images[l];
	cr->drvadd = (uint64 *) palloc(sizeof(uint64) *
								   Max(st->decode->cap[0], 1));
	cr->located = true;
	MemoryContextSwitchTo(oldcxt);
}

/*
 * One pass of the driver's walk: the chunk of its values it stands at, each
 * value's part of a code - its outcome in the driver's dimension, told by the
 * dimension's equality from the entry's key, NULL's for its NULL entry, 0 for
 * a column no predicate reads - and the sets located again under their pins,
 * as column 0's of a decoded pass are (lion_decode_pass()); then every row of
 * them from lokey up to hikey counted under its cell.
 */
static void
lion_cells_pass(LionCountScanState *st, uint32 lokey, uint64 hikey)
{
	LionDecodeRun *dr = st->decode;
	LionCellsRun *cr = st->cells;
	EState	   *estate = st->css.ss.ps.state;
	LionDecodeCol drv;
	bool	   *relocated;
	MemoryContext oldcxt;
	int			v;

	MemoryContextReset(dr->passcxt);
	oldcxt = MemoryContextSwitchTo(dr->passcxt);

	for (v = 0; v < dr->nvals[0]; v++)
	{
		int			out = 0;

		if (cr->drv >= 0)
		{
			const LionCellsDimPriv *pv = &cr->dimpriv[cr->drv];

			if (dr->isnull[0][v])
				out = pv->nullout;
			else if (cr->hasdrveq)
			{
				ListCell   *lc;
				int			i = 0;

				foreach(lc, pv->vals)
				{
					if (DatumGetBool(FunctionCall2Coll(&cr->drveq,
													   pv->collation,
													   dr->keys[0][v],
													   ((Const *) lfirst(lc))->constvalue)))
					{
						out = pv->valout[i];
						break;
					}
					i++;
				}
			}
		}
		cr->drvadd[v] = (uint64) out * cr->dims[Max(cr->drv, 0)].weight;
	}

	relocated = (bool *) palloc0(sizeof(bool) * Max(dr->nvals[0], 1));
	drv.nsets = dr->nvals[0];
	drv.images = dr->images[0];
	drv.sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) *
										 Max(dr->nvals[0], 1));
	for (v = 0; v < dr->nvals[0]; v++)
	{
		LionPostingSet *ps = &dr->sets[0][v];

		if (ps->found && ps->nopin)
		{
			if (ps->keyisnull)
				(void) lion_posting_set_lookup_null_col(dr->idx[0],
														dr->idxcol[0],
														&drv.sets[v]);
			else
				(void) lion_posting_set_lookup_col(dr->idx[0], dr->idxcol[0],
												  ps->storedkey, InvalidOid,
												  &drv.sets[v]);
			relocated[v] = true;
		}
		else
			drv.sets[v] = *ps;
	}

	lion_count_cells(st->heap, estate->es_snapshot, &drv, cr->drvadd,
					 cr->ndim, cr->dims, cr->nlabel, cr->labels, cr->ncells,
					 lokey, hikey, st->wcollected ? &st->wherecoll : NULL,
					 dr->tally, &st->stats, &dr->stats, st->viscache,
					 st->rel_read_only);

	for (v = 0; v < dr->nvals[0]; v++)
	{
		if (relocated[v])
			lion_posting_set_release(&drv.sets[v]);
	}
	MemoryContextSwitchTo(oldcxt);
	dr->passes++;
}

/*
 * The count, serial: the WHERE read as the decoded walk reads it
 * (lion_decode_where()), the labels located, and every pass of the driver's
 * walk counted into the one tally.  False when the WHERE selects nothing.
 */
static bool
lion_cells_run(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	int			k;

	for (k = 1; k <= st->nitem; k++)
	{
		if (st->sources[k].rangewalk != NULL)
			elog(ERROR, "LionCount: a count by cells beside a range it walks");
	}
	if (!lion_decode_where(st))
		return false;
	if (dr->cap[0] == 0)
		lion_decode_caps(st);
	lion_cells_locate(st);
	if (!lion_decode_fill(st, 0, true))
		return true;
	do
	{
		lion_cells_pass(st, 0, (uint64) PG_UINT32_MAX + 1);
	} while (lion_decode_fill(st, 0, false));
	return true;
}

/*
 * The count in parallel (DESIGN.md §10, "A GROUP BY in parallel"): ranges of
 * container keys claimed one after the other, as a parallel GROUP BY claims
 * them (lion_group_range_next()), the WHERE collected for each range alone
 * and every pass of the driver's walk counted there, into the participant's
 * one tally; its cells go up as partial rows, which the Finalize Agg adds to
 * the other participants'.  Every row is in one range, counted by one
 * participant, under its own pins and the leader's snapshot.
 */
static void
lion_cells_run_ranged(LionCountScanState *st)
{
	LionJoinShared *shared = st->shared;
	LionRangedState *rs = lion_st_ranged(st);
	LionDecodeRun *dr = st->decode;
	EState	   *estate = st->css.ss.ps.state;
	int			k;

	if (RecoveryInProgress())
		elog(ERROR, "LionCount: a parallel count by cells during recovery");
	if ((st->nitem > 0 && st->nsource != st->nitem + 1) ||
		st->ingroupitem >= 0)
		elog(ERROR, "LionCount: a parallel count by cells of another shape");
	for (k = 1; k <= st->nitem; k++)
	{
		if (st->sources[k].rangewalk != NULL)
			elog(ERROR, "LionCount: a parallel count by cells of another shape");
	}
	if (dr->cap[0] == 0)
		lion_decode_caps(st);
	lion_cells_locate(st);
	if (rs->cxt == NULL)
		rs->cxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionCount key range",
										ALLOCSET_DEFAULT_SIZES);

	for (;;)
	{
		uint32		r = pg_atomic_fetch_add_u32(&shared->nextchunk, 1);
		uint32		lo;
		uint64		hi;

		if (r >= (uint32) shared->nranges)
			break;
		CHECK_FOR_INTERRUPTS();

		rs->ranges++;
		rs->range = (int) r;
		lion_key_range(shared->nranges, shared->ckeys, (int) r, &lo, &hi);
		/* ... and the anti join's rows of it, which this one takes away */
		if (st->anti != NULL)
			lion_anti_cells_range(st, lo, hi);
		lion_posting_set_release(&st->wherecoll);
		st->wcollected = false;
		MemoryContextReset(rs->cxt);
		/* ... the range's WHERE, or every row of it without one */
		if (st->nitem > 0 && !lion_where_collect_range(st, lo, hi))
			continue;
		if (!lion_decode_fill(st, 0, true))
			continue;
		do
		{
			lion_cells_pass(st, lo, hi);
		} while (lion_decode_fill(st, 0, false));
	}
	rs->range = -1;
	lion_posting_set_release(&st->wherecoll);
	st->wcollected = false;
}

/* The truth of predicate p in outcome `out` of its dimension */
static uint8
lion_cells_truth_in(const LionCellsRun *cr, int p, int out)
{
	const LionCellsPredPriv *pp = &cr->pred[p];
	const LionCellsDimPriv *pv = &cr->dimpriv[pp->dim];

	switch (pp->kind)
	{
		case LION_CELLPRED_TABLE:
			return pp->truth[out];
		case LION_CELLPRED_BIT:
			if (out == pv->nullout)
				return LION_TRUTH_NULL;
			return ((out >> pp->bit) & 1) ? LION_TRUTH_TRUE : LION_TRUTH_FALSE;
		case LION_CELLPRED_NONE:
			return (out == pv->nullout) ? LION_TRUTH_NULL : LION_TRUTH_FALSE;
		case LION_CELLPRED_NULLS:
			return pp->truth[(out == pv->nullout) ? 1 : 0];
	}
	elog(ERROR, "LionCount: a cells predicate of kind %d", pp->kind);
	return LION_TRUTH_NULL;		/* keep the compiler quiet */
}

/* ... and in the cell whose outcomes are cr->outs */
static uint8
lion_cells_truth(const LionCellsRun *cr, int p)
{
	return lion_cells_truth_in(cr, p, cr->outs[cr->pred[p].dim]);
}

/*
 * The code of the cell a row is in whose predicates have the truths
 * truth[] (LION_TRUTH_*) - of a row the anti join over a small inner side
 * takes away (DESIGN.md §27, "Counts by cells beside the anti join"), its
 * predicates evaluated on the heap's tuple.  Each dimension's outcome is the
 * one in which every predicate of the dimension has the row's truth: one at
 * most, since the outcomes are the classes of the rows the predicates tell
 * apart - a scalar dimension's values by their truths, a multi-key one's by
 * the leaves true of them, NULL by its own - and one at least, the row's
 * own, which a row in no cell would contradict.
 */
uint64
lion_cells_code_of(const LionCellsRun *cr, const uint8 *truth)
{
	int			preds[LION_CELLS_MAX_PREDS];
	uint64		code = 0;
	int			d;

	if (cr->npred > LION_CELLS_MAX_PREDS)
		elog(ERROR, "LionCount: %d predicates in a count by cells", cr->npred);
	for (d = 0; d < cr->ndim; d++)
	{
		int			npd = 0;
		int			o;
		int			p;

		for (p = 0; p < cr->npred; p++)
		{
			if (cr->pred[p].dim == d)
				preds[npd++] = p;
		}
		for (o = 0; o < cr->radix[d]; o++)
		{
			int			i;

			for (i = 0; i < npd; i++)
			{
				if (lion_cells_truth_in(cr, preds[i], o) != truth[preds[i]])
					break;
			}
			if (i == npd)
				break;
		}
		if (o == cr->radix[d])
			elog(ERROR, "LionCount: a row of no outcome of cell column %d", d);
		code += (uint64) o * cr->dims[d].weight;
	}
	return code;
}

/*
 * The next partial row of a count by cells: a cell with rows, its
 * predicates' truths and its count.  The whole count is made by the first
 * call, which holds no pin past it; the cells then go up one a call - more
 * than once a cell, where the tally spilled, which the Finalize Agg adds up
 * as it adds up the cells of one group.  A cell the plan's qual (the WHERE's
 * clauses over the predicates) rejects is consumed like any other.  Beside
 * the anti join over a small inner side, a cell that loses rows to it is
 * held back until the tally has given all of its counts, and goes up last
 * with what is left of it (lion_anti_cells_hold()).
 */
TupleTableSlot *
lion_next_cells(LionCountScanState *st, bool *exhausted)
{
	LionDecodeRun *dr = st->decode;
	LionCellsRun *cr = st->cells;

	*exhausted = false;
	for (;;)
	{
		uint64		code;
		int64		count;
		TupleTableSlot *slot;
		int			p;

		CHECK_FOR_INTERRUPTS();
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);

		if (dr->finished)
		{
			*exhausted = true;
			return NULL;
		}
		if (!dr->started)
		{
			dr->started = true;
			if (!lion_decode_tally_begin(dr->tally, cr->ndim, cr->radix))
				elog(ERROR, "LionCount: the cells of a count by cells do not fit a code");
			if (st->shared != NULL)
				lion_cells_run_ranged(st);
			else
			{
				/* every row the anti join takes away is this one's */
				if (st->anti != NULL)
					lion_anti_cells_range(st, 0, LION_KEYS_END);
				if (!lion_cells_run(st) && st->anti == NULL)
				{
					dr->finished = true;
					continue;
				}
			}
			dr->emitting = true;
		}

		if (lion_decode_tally_next(dr->tally, &code, &count))
		{
			/* a cell that loses rows to the anti join waits for the rest */
			if (st->anti != NULL && lion_anti_cells_hold(st, code, count))
				continue;
		}
		else if (st->anti == NULL || !lion_anti_cells_next(st, &code, &count))
		{
			dr->emitting = false;
			lion_decode_tally_end(dr->tally);
			dr->finished = true;
			continue;
		}
		lion_decode_code_split(code, cr->ndim, cr->radix, cr->outs);
		for (p = 0; p < cr->npred; p++)
			cr->truth[p] = lion_cells_truth(cr, p);
		dr->rowsup++;
		slot = lion_emit_keys(st, 0, NULL, NULL, count);
		if (slot == NULL)
			continue;
		return slot;
	}
}

/*
 * EXPLAIN: the strategy, the columns the predicates are of and the
 * predicates themselves - the expressions the plan's custom_scan_tlist
 * holds for its LION_TL_CELLPRED columns, by number - and the cells they
 * make.
 */
void
lion_explain_cells(LionCountScanState *st, List *ancestors, ExplainState *es)
{
	LionCellsRun *cr = st->cells;
	CustomScan *cscan = (CustomScan *) st->css.ss.ps.plan;
	List	   *context;
	List	   *cols = NIL;
	List	   *preds = NIL;
	char	  **text;
	ListCell   *lc;
	int			d;
	int			p;
	int			i = 0;

	ExplainPropertyText("Group Strategy", "Cells", es);
	/* each column with the index it is read from: `ix.col (col)` */
	for (d = 0; d < cr->ndim; d++)
	{
		const LionCellsDimPriv *dp = &cr->dimpriv[d];
		Oid			idxoid = OidIsValid(dp->idxoid) ? dp->idxoid :
			st->plan.groupidxoid;

		cols = lappend(cols,
					   psprintf("%s%s (%s)", get_rel_name(idxoid),
								lion_explain_col(idxoid, dp->attno,
												 dp->kind == LION_CELLDIM_MULTI,
												 NIL),
								get_attname(st->plan.heapoid, dp->attno,
											false)));
	}
	ExplainPropertyList("Cell Columns", cols, es);

	context = set_deparse_context_plan(es->deparse_cxt, &cscan->scan.plan,
									   ancestors);
	text = (char **) palloc0(sizeof(char *) * cr->npred);
	foreach(lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		int			kind = (i < st->ntlist) ? st->tlkind[i] : 0;

		if (LION_TL_IS_CELLPRED(kind) &&
			LION_TL_CELLPRED_NO(kind) < cr->npred)
			text[LION_TL_CELLPRED_NO(kind)] =
				deparse_expression((Node *) tle->expr, context, false, false);
		i++;
	}
	for (p = 0; p < cr->npred; p++)
		preds = lappend(preds, text[p] != NULL ? text[p] : pstrdup("?"));
	ExplainPropertyList("Cell Predicates", preds, es);
	ExplainPropertyInteger("Cells", NULL, (int64) cr->ncells, es);
}
