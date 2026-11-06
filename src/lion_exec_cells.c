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

#include "utils/numeric.h"

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
	cr->baseweight = (uint64 *) palloc(sizeof(uint64) * cr->ndim);
	cr->gradix = (int *) palloc(sizeof(int) * (cr->ndim + 1));
	cr->grouped = priv->cellgroup;
	for (d = cr->ndim - 1; d >= 0; d--)
	{
		const LionCellsDimPriv *pv = &cr->dimpriv[d];

		cr->radix[d] = pv->radix;
		cr->dims[d].multi = (pv->kind == LION_CELLDIM_MULTI);
		cr->dims[d].weight = weight;
		cr->baseweight[d] = weight;
		cr->gradix[d] = pv->radix;
		cr->dims[d].nullout = pv->nullout;
		cr->dims[d].nbit = pv->nbit;
		if (pv->kind == LION_CELLDIM_SUM)
		{
			LionCellDim *dim = &cr->dims[d];
			int			t;

			dim->sum = true;
			dim->numeric = (priv->cellpred[pv->sumpred].sumtype == NUMERICOID);
			dim->nterm = pv->nterm;
			dim->zscale = pv->zscale;
			dim->sumzero = pv->sumzero;
			dim->nsum = pv->radix - 1;
			dim->sumval = pv->sumval;
			dim->sumscale = pv->sumscale;
			if (cr->termfn == NULL)
				cr->termfn = (FmgrInfo **) palloc0(sizeof(FmgrInfo *) *
												   cr->ndim);
			cr->termfn[d] = (FmgrInfo *) palloc(sizeof(FmgrInfo) * pv->nterm);
			for (t = 0; t < pv->nterm; t++)
				fmgr_info_cxt(get_opcode(pv->termop[t]), &cr->termfn[d][t],
							  estate->es_query_cxt);
		}
		if ((uint64) pv->radix > (UINT64CONST(1) << 62) / weight)
			elog(ERROR, "LionCount: the cells of a count by cells do not fit a code");
		weight *= (uint64) pv->radix;
	}
	cr->ncells = weight;
	cr->basecells = weight;
	cr->outs = (int *) palloc0(sizeof(int) * (cr->ndim + 1));
	cr->truth = (uint8 *) palloc0(sizeof(uint8) * cr->npred);
	cr->sumvalue = (Datum *) palloc0(sizeof(Datum) * cr->npred);
	cr->sumisnull = (bool *) palloc0(sizeof(bool) * cr->npred);
	cr->heapattno = (AttrNumber *) palloc0(sizeof(AttrNumber) * cr->ndim);
	cr->valcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionCount cell values",
									   ALLOCSET_SMALL_SIZES);
	cr->rowcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionCount weighted sum of a row",
									   ALLOCSET_SMALL_SIZES);
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

		cr->heapattno[d] = lion_heap_attno_in(st->heap, st->plan.heapoid,
											  pv->attno);
		if (d == cr->drv)
			continue;
		cr->idx[d] = index_open(pv->idxoid, AccessShareLock);
		cr->idxcol[d] =
			lion_index_col_for(cr->idx[d], cr->heapattno[d],
							   pv->kind != LION_CELLDIM_SCALAR,
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
 * again by the next run, and the outcomes by truths with them, made again by
 * the next run that needs them.  The driver's walk and the tally are the
 * decoded walk's, which lion_decode_reset() resets.
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
	cr->bytruth = NULL;
	cr->drvadd = NULL;
	for (d = 0; d < cr->ndim; d++)
	{
		cr->dims[d].nkeys = 0;
		cr->dims[d].trees = NULL;
		cr->dims[d].map = NULL;
		cr->dims[d].keyinc = NULL;
		cr->dims[d].keycnt = NULL;
		cr->dims[d].keys1 = NULL;
		cr->dims[d].keymask = NULL;
		cr->dims[d].ntree = 0;
		cr->dims[d].treeterm = NULL;
		cr->dims[d].treeinc = NULL;
		cr->dims[d].trees1 = NULL;
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

/* Every key of tree n renumbered by map, of nmap keys */
static void
lion_cells_tree_renumber(LionKeyNode *tree, const int *map, int nmap)
{
	LionKeyNode **stack = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * 1024);
	int			nstack = 0;

	stack[nstack++] = tree;
	while (nstack > 0)
	{
		LionKeyNode *n = stack[--nstack];
		int			a;

		if (n->kind == LION_KN_KEY)
		{
			if (n->keyno < 0 || n->keyno >= nmap)
				elog(ERROR, "LionCount: a key tree names key %d of %d",
					 n->keyno, nmap);
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
	pfree(stack);
}

/*
 * A SUM dimension's labels (DESIGN.md §10, "Weighted sums"): the keys each
 * term's query extracts - the same query, by the same function, as the plan
 * was made for, so exact - each once, a label whose bits are its number, and
 * the NULL entry.  A term whose tree is one key adds its increment at that
 * key (keyinc, keycnt, keys1); any other's tree is over bits of a mask, one
 * a key of such trees, at most LION_CELLS_MAX_KEYS of them.  A term whose
 * query selects no row has no key and is never true.
 */
static void
lion_cells_locate_sum(LionCellsRun *cr, int *cap, int d)
{
	const LionCellsDimPriv *pv = &cr->dimpriv[d];
	LionCellDim *dim = &cr->dims[d];
	Relation	idx = cr->idx[d];
	AttrNumber	col = cr->idxcol[d];
	LionState  *istate = lion_index_column_state(idx, col);
	Datum	   *keys = (Datum *) palloc(sizeof(Datum) * LION_CELLS_MAX_SUMKEYS);
	int			nkeys = 0;
	int			nmask = 0;
	LionPostingSet ps;
	int			t;
	int			k;

	dim->keyinc = (int64 *) palloc0(sizeof(int64) * LION_CELLS_MAX_SUMKEYS);
	dim->keycnt = (int32 *) palloc0(sizeof(int32) * LION_CELLS_MAX_SUMKEYS);
	dim->keys1 = (int16 *) palloc0(sizeof(int16) * LION_CELLS_MAX_SUMKEYS);
	dim->keymask = (uint64 *) palloc0(sizeof(uint64) * LION_CELLS_MAX_SUMKEYS);
	dim->treeterm = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * pv->nterm);
	dim->treeinc = (int64 *) palloc(sizeof(int64) * pv->nterm);
	dim->trees1 = (int16 *) palloc(sizeof(int16) * pv->nterm);
	dim->ntree = 0;
	for (t = 0; t < pv->nterm; t++)
	{
		StrategyNumber strategy =
			get_op_opfamily_strategy(pv->termop[t], idx->rd_opfamily[col - 1]);
		LionQuery	q;
		int		   *map;
		LionKeyNode *tree;

		lion_extract_query(istate, pv->termquery[t]->constvalue, strategy, &q);
		if (q.mode == LION_QMODE_NONE)
			continue;			/* a query that selects no row: never true */
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
				if (nkeys >= LION_CELLS_MAX_SUMKEYS)
					elog(ERROR, "LionCount: more than %d keys in one weighted sum's column",
						 LION_CELLS_MAX_SUMKEYS);
				keys[nkeys++] = q.keys[k];
			}
			map[k] = j;
		}
		tree = lion_cells_tree_copy(q.tree, 0);
		lion_cells_tree_renumber(tree, map, q.nkeys);

		if (tree->kind == LION_KN_KEY)
		{
			/* one key: its increment where the key's set has the row */
			k = tree->keyno;
			dim->keyinc[k] += pv->terminc[t];
			dim->keycnt[k]++;
			dim->keys1[k] = Max(dim->keys1[k], pv->terms1[t]);
			continue;
		}

		/* ... any other: a tree over the bits of its keys */
		{
			int		   *bit = (int *) palloc(sizeof(int) * nkeys);

			for (k = 0; k < nkeys; k++)
			{
				bit[k] = -1;
				if (dim->keymask[k] != 0)
					bit[k] = pg_rightmost_one_pos64(dim->keymask[k]);
			}
			for (k = 0; k < q.nkeys; k++)
			{
				int			j = map[k];

				if (bit[j] < 0)
				{
					if (nmask >= LION_CELLS_MAX_KEYS)
						elog(ERROR, "LionCount: more than %d keys in the terms of one weighted sum's column that are not one key",
							 LION_CELLS_MAX_KEYS);
					bit[j] = nmask++;
					dim->keymask[j] = UINT64CONST(1) << bit[j];
				}
			}
			lion_cells_tree_renumber(tree, bit, nkeys);
			dim->treeterm[dim->ntree] = tree;
			dim->treeinc[dim->ntree] = pv->terminc[t];
			dim->trees1[dim->ntree] = pv->terms1[t];
			dim->ntree++;
		}
	}
	dim->nkeys = nkeys;
	for (k = 0; k < nkeys; k++)
	{
		(void) lion_posting_set_lookup_col(idx, col, keys[k], InvalidOid, &ps);
		lion_cells_add_label(cr, cap, d, (uint64) k, &ps);
		CHECK_FOR_INTERRUPTS();
	}
	(void) lion_posting_set_lookup_null_col(idx, col, &ps);
	lion_cells_add_label(cr, cap, d, LION_CELL_NULLBIT, &ps);
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
		if (pv->kind == LION_CELLDIM_SUM)
		{
			lion_cells_locate_sum(cr, &cap, d);
			continue;
		}
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
				/* the tree's keys renumbered as the dimension's */
				tree = lion_cells_tree_copy(q.tree, 0);
				lion_cells_tree_renumber(tree, map, q.nkeys);
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

		/* ... and, of a GROUP BY column beside the cells, the entry's own */
		if (cr->grouped)
			cr->drvadd[v] += (uint64) v;
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
					 lion_anti_exclude(st), dr->tally, &st->stats, &dr->stats,
					 st->viscache, st->rel_read_only);

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

/*
 * The truth of predicate pp in outcome `out` of its dimension pv: the
 * planner's table, a multi-key leaf's bit, or a null test's - what
 * LionProbeJoin computes a row's truths with too (DESIGN.md §46).
 */
uint8
lion_cells_pred_truth(const LionCellsDimPriv *pv, const LionCellsPredPriv *pp,
					  int out)
{
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
	elog(ERROR, "lion: a cells predicate of kind %d", pp->kind);
	return LION_TRUTH_NULL;		/* keep the compiler quiet */
}

/* The truth of predicate p in outcome `out` of its dimension */
static uint8
lion_cells_truth_in(const LionCellsRun *cr, int p, int out)
{
	const LionCellsPredPriv *pp = &cr->pred[p];

	return lion_cells_pred_truth(&cr->dimpriv[pp->dim], pp, out);
}

/* ... and in the cell whose outcomes are cr->outs */
static uint8
lion_cells_truth(const LionCellsRun *cr, int p)
{
	return lion_cells_truth_in(cr, p, cr->outs[cr->pred[p].dim]);
}

/*
 * A weighted sum's value (DESIGN.md §10, "Weighted sums"), its constants'
 * part plus each of its SUM dimensions' outcome's, `outs` - NULL where a
 * dimension's outcome is NULL's and its NULL is the sum's - as a Datum of
 * its type in the current memory context: a numeric of the display scale
 * core's `+` would give it, the largest of the constants' it adds.
 */
static Datum
lion_cells_sum_value(const LionCellsDimPriv *dimpriv, int ndim,
					 const LionCellsPredPriv *pp, int p, const int *outs,
					 bool *isnull)
{
	int64		v = pp->sumbase;
	int			scale = pp->sumbasescale;
	int			d;

	*isnull = false;
	for (d = pp->dim; d < ndim; d++)
	{
		const LionCellsDimPriv *pv = &dimpriv[d];
		int			o = outs[d];

		if (pv->kind != LION_CELLDIM_SUM || pv->sumpred != p)
			continue;
		if (o < 0 || o >= pv->radix)
			elog(ERROR, "LionCount: outcome %d of a weighted sum's column of %d",
				 o, pv->radix);
		if (o == pv->nullout && pv->sumnull)
		{
			*isnull = true;
			return (Datum) 0;
		}
		v += pv->sumval[o];
		scale = Max(scale, pv->sumscale[o]);
	}
	switch (pp->sumtype)
	{
		case INT2OID:
			return Int16GetDatum((int16) v);
		case INT4OID:
			return Int32GetDatum((int32) v);
		case INT8OID:
			return Int64GetDatum(v);
		case NUMERICOID:
			{
				int			i;

				/* every digit past the scale is a zero of the unit's */
				scale = Max(scale, 0);
				for (i = scale; i < pp->sumunit; i++)
					v /= 10;
				return NumericGetDatum(int64_div_fast_to_numeric(v, scale));
			}
	}
	elog(ERROR, "LionCount: a weighted sum of type %u", pp->sumtype);
	return (Datum) 0;			/* keep the compiler quiet */
}

/*
 * The cell that goes up, whose outcomes are cr->outs: each predicate's truth
 * in it, and each weighted sum's value, made in valcxt, which the next cell
 * resets.
 */
static void
lion_cells_fill_cell(LionCellsRun *cr)
{
	MemoryContext oldcxt;
	int			p;

	MemoryContextReset(cr->valcxt);
	oldcxt = MemoryContextSwitchTo(cr->valcxt);
	for (p = 0; p < cr->npred; p++)
	{
		if (cr->pred[p].kind == LION_CELLPRED_SUM)
			cr->sumvalue[p] = lion_cells_sum_value(cr->dimpriv, cr->ndim,
												   &cr->pred[p], p, cr->outs,
												   &cr->sumisnull[p]);
		else
			cr->truth[p] = lion_cells_truth(cr, p);
	}
	MemoryContextSwitchTo(oldcxt);
}

/* The order of a dimension's outcomes by their truths: npd bytes each */
typedef struct LionCellsTupSort
{
	const uint8 *tups;			/* every outcome's, in the outcomes' order */
	int			npd;
} LionCellsTupSort;

static int
lion_cells_tup_cmp(const void *a, const void *b, void *arg)
{
	const LionCellsTupSort *ts = (const LionCellsTupSort *) arg;
	int			x = *(const int *) a;
	int			y = *(const int *) b;
	int			c = memcmp(ts->tups + (Size) x * ts->npd,
						   ts->tups + (Size) y * ts->npd, ts->npd);

	if (c != 0)
		return c;
	return (x > y) - (x < y);
}

/*
 * Each dimension's outcomes by the truths its predicates have in them
 * (LionCellsByTruth): every outcome's truths, lion_cells_truth_in()'s,
 * sorted, and of the outcomes that share theirs only the lowest - the one a
 * search of the outcomes in their order finds first.  A dimension no
 * predicate reads has one tuple, the empty one, of outcome 0.  The table is
 * the plan's alone, the same in every run; it is made once a run in the
 * labels' memory, which lion_cells_reset() resets with the pointer.  It is a
 * byte a predicate for each outcome - by the plan at most
 * 2^LION_CELLS_MAX_BITS + 1 outcomes of a multi-key dimension and
 * LION_CELLS_MAX_VALUES + 2 of a scalar one - and the radix is checked
 * against the largest allocation all the same.
 */
static void
lion_cells_bytruth(LionCellsRun *cr)
{
	MemoryContext oldcxt;
	LionCellsByTruth *bt;
	int			d;

	if (cr->bytruth != NULL)
		return;
	oldcxt = MemoryContextSwitchTo(cr->labelcxt);
	bt = (LionCellsByTruth *) palloc0(sizeof(LionCellsByTruth) *
									  Max(cr->ndim, 1));
	for (d = 0; d < cr->ndim; d++)
	{
		LionCellsByTruth *t = &bt[d];
		int			radix = cr->radix[d];
		Size		width;
		uint8	   *all;
		int		   *ord;
		LionCellsTupSort ts;
		int			o;
		int			i;
		int			p;

		/* the dimension's predicates */
		t->preds = (int *) palloc(sizeof(int) * Max(cr->npred, 1));
		for (p = 0; p < cr->npred; p++)
		{
			if (cr->pred[p].dim == d && cr->pred[p].kind != LION_CELLPRED_SUM)
				t->preds[t->npd++] = p;
		}
		width = (Size) Max(t->npd, 1);
		if (radix < 0 ||
			(Size) radix > MaxAllocSize / Max(width, sizeof(int)))
			elog(ERROR, "LionCount: %d outcomes of cell column %d", radix, d);

		/* every outcome's truths, in the outcomes' order */
		all = (uint8 *) palloc(width * Max(radix, 1));
		for (o = 0; o < radix; o++)
		{
			for (i = 0; i < t->npd; i++)
				all[(Size) o * t->npd + i] =
					lion_cells_truth_in(cr, t->preds[i], o);
		}

		/* ... the outcomes in the order of their truths, the lowest first */
		ord = (int *) palloc(sizeof(int) * Max(radix, 1));
		for (o = 0; o < radix; o++)
			ord[o] = o;
		ts.tups = all;
		ts.npd = t->npd;
		qsort_arg(ord, radix, sizeof(int), lion_cells_tup_cmp, &ts);

		/* ... and each tuple of truths once, with its lowest outcome */
		t->tups = (uint8 *) palloc(width * Max(radix, 1));
		t->outs = (int *) palloc(sizeof(int) * Max(radix, 1));
		for (i = 0; i < radix; i++)
		{
			const uint8 *tup = all + (Size) ord[i] * t->npd;

			if (t->ntup > 0 &&
				memcmp(tup, t->tups + (Size) (t->ntup - 1) * t->npd,
					   t->npd) == 0)
				continue;
			memcpy(t->tups + (Size) t->ntup * t->npd, tup, t->npd);
			t->outs[t->ntup++] = ord[i];
		}
		pfree(all);
		pfree(ord);
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextSwitchTo(oldcxt);
	cr->bytruth = bt;
}

/* The outcome whose truths are tup[], by a binary search; -1 if none is */
static int
lion_cells_bytruth_find(const LionCellsByTruth *t, const uint8 *tup)
{
	int			lo = 0;
	int			hi = t->ntup;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;
		int			c = memcmp(tup, t->tups + (Size) mid * t->npd, t->npd);

		if (c == 0)
			return t->outs[mid];
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return -1;
}

/*
 * The outcome of SUM dimension d of the row in heapslot (DESIGN.md §10,
 * "Weighted sums"): its column's value, and each term's leaf - the
 * operator, on the column and the query - evaluated on it, as core would;
 * NULL's for a NULL.  In rowcxt, reset once it is known.
 */
static int
lion_cells_sum_of_row(LionCellsRun *cr, int d, TupleTableSlot *heapslot)
{
	const LionCellsDimPriv *pv = &cr->dimpriv[d];
	const LionCellDim *dim = &cr->dims[d];
	MemoryContext oldcxt;
	Datum		val;
	bool		isnull;
	int64		sum = 0;
	int32		cnt = 0;
	int			s1 = 0;
	int			scale = 0;
	int			o;
	int			t;

	if (heapslot == NULL || cr->heapattno[d] <= 0)
		elog(ERROR, "LionCount: a weighted sum's column without its row");
	val = slot_getattr(heapslot, cr->heapattno[d], &isnull);
	if (isnull)
		return dim->nullout;
	oldcxt = MemoryContextSwitchTo(cr->rowcxt);
	for (t = 0; t < pv->nterm; t++)
	{
		if (DatumGetBool(FunctionCall2Coll(&cr->termfn[d][t],
										   pv->termcoll[t], val,
										   pv->termquery[t]->constvalue)))
		{
			sum += pv->terminc[t];
			cnt++;
			s1 = Max(s1, pv->terms1[t]);
		}
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(cr->rowcxt);
	if (dim->numeric)
	{
		scale = s1;
		if (cnt < pv->nterm)
			scale = Max(scale, pv->zscale);
	}
	o = lion_cells_sum_find(dim, pv->sumzero + sum, scale);
	if (o < 0)
		elog(ERROR, "LionCount: a row of no outcome of weighted sum column %d", d);
	return o;
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
 * own, which a row in no cell would contradict.  The outcome is found by a
 * binary search of the dimension's outcomes by their truths
 * (lion_cells_bytruth()), of no more tuples than the radix or 3^npd: at most
 * two comparisons of npd bytes a predicate, which the lookups' price has as a
 * cpu_operator_cost a predicate (lion_anti_price()).
 */
uint64
lion_cells_code_of(LionCellsRun *cr, const uint8 *truth,
				   TupleTableSlot *heapslot)
{
	uint8		tup[LION_CELLS_MAX_PREDS];
	uint64		code = 0;
	int			d;

	if (cr->npred > LION_CELLS_MAX_PREDS)
		elog(ERROR, "LionCount: %d predicates in a count by cells", cr->npred);
	lion_cells_bytruth(cr);
	for (d = 0; d < cr->ndim; d++)
	{
		const LionCellsByTruth *t = &cr->bytruth[d];
		int			o;
		int			i;

		if (cr->dims[d].sum)
		{
			code += (uint64) lion_cells_sum_of_row(cr, d, heapslot) *
				cr->dims[d].weight;
			continue;
		}
		for (i = 0; i < t->npd; i++)
			tup[i] = truth[t->preds[i]];
		o = lion_cells_bytruth_find(t, tup);
		if (o < 0)
			elog(ERROR, "LionCount: a row of no outcome of cell column %d", d);
		code += (uint64) o * cr->dims[d].weight;
	}
	return code;
}

/* ---------------------------------------------------------------------
 * A GROUP BY column beside the cells (DESIGN.md §10, "A GROUP BY column
 * beside the cells")
 * --------------------------------------------------------------------- */

/*
 * The driver's column is a GROUP BY column, each of its entries a group: the
 * code of a row is its cell's times the pass's capacity, plus its entry's
 * place in the pass, which the tally's last place is (gradix).  Once the
 * walk's capacity is known, at the run's first pass.
 */
static void
lion_cells_grouped_scale(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	LionCellsRun *cr = st->cells;
	uint64		cap = (uint64) Max(dr->cap[0], 1);
	int			d;

	if (cr->basecells > (UINT64CONST(1) << 62) / cap)
		elog(ERROR, "LionCount: the cells of a count by cells do not fit a code");
	for (d = 0; d < cr->ndim; d++)
		cr->dims[d].weight = cr->baseweight[d] * cap;
	cr->ncells = cr->basecells * cap;
	cr->gradix[cr->ndim] = (int) cap;
}

/*
 * The first pass of the next range a participant claims (as
 * lion_cells_run_ranged() claims them) that has one: its WHERE collected,
 * its first chunk of the driver's entries read.  False once no range is
 * left.
 */
static bool
lion_cells_grouped_range(LionCountScanState *st)
{
	LionJoinShared *shared = st->shared;
	LionRangedState *rs = lion_st_ranged(st);
	LionCellsRun *cr = st->cells;

	for (;;)
	{
		uint32		r = pg_atomic_fetch_add_u32(&shared->nextchunk, 1);

		if (r >= (uint32) shared->nranges)
		{
			rs->range = -1;
			cr->inrange = false;
			return false;
		}
		CHECK_FOR_INTERRUPTS();
		rs->ranges++;
		rs->range = (int) r;
		lion_key_range(shared->nranges, shared->ckeys, (int) r,
					   &cr->rangelo, &cr->rangehi);
		cr->inrange = true;
		lion_posting_set_release(&st->wherecoll);
		st->wcollected = false;
		MemoryContextReset(rs->cxt);
		if (st->nitem > 0 &&
			!lion_where_collect_range(st, cr->rangelo, cr->rangehi))
			continue;
		if (lion_decode_fill(st, 0, true))
			return true;
	}
}

/*
 * The run's first pass: the WHERE read and the labels located, as
 * lion_cells_run() and lion_cells_run_ranged() have them, and the driver's
 * first chunk.  False when there is no pass at all.
 */
static bool
lion_cells_grouped_start(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	LionCellsRun *cr = st->cells;
	EState	   *estate = st->css.ss.ps.state;
	int			k;

	for (k = 1; k <= st->nitem; k++)
	{
		if (st->sources[k].rangewalk != NULL)
			elog(ERROR, "LionCount: a count by cells beside a range it walks");
	}
	if (st->shared != NULL)
	{
		LionRangedState *rs = lion_st_ranged(st);

		if (RecoveryInProgress())
			elog(ERROR, "LionCount: a parallel count by cells during recovery");
		if ((st->nitem > 0 && st->nsource != st->nitem + 1) ||
			st->ingroupitem >= 0)
			elog(ERROR, "LionCount: a parallel count by cells of another shape");
		if (dr->cap[0] == 0)
			lion_decode_caps(st);
		lion_cells_locate(st);
		lion_cells_grouped_scale(st);
		if (rs->cxt == NULL)
			rs->cxt = AllocSetContextCreate(estate->es_query_cxt,
											"LionCount key range",
											ALLOCSET_DEFAULT_SIZES);
		return lion_cells_grouped_range(st);
	}
	if (!lion_decode_where(st))
		return false;
	if (dr->cap[0] == 0)
		lion_decode_caps(st);
	lion_cells_locate(st);
	lion_cells_grouped_scale(st);
	cr->rangelo = 0;
	cr->rangehi = (uint64) PG_UINT32_MAX + 1;
	return lion_decode_fill(st, 0, true);
}

/* ... and the next pass: the next chunk, or the next range's first */
static bool
lion_cells_grouped_advance(LionCountScanState *st)
{
	if (lion_decode_fill(st, 0, false))
		return true;
	if (st->shared == NULL)
		return false;
	return lion_cells_grouped_range(st);
}

/*
 * The next partial row of a count by cells beside a GROUP BY column: a cell
 * with rows of one group, the group's key, its predicates' truths and its
 * count.  Each pass of the driver's entries is counted into a tally of its
 * own and goes up before the next is counted: a pass's rows are of its
 * entries alone, so no row of a group is in two passes - but a range's are,
 * in parallel, whose partial rows the Finalize Agg adds up as it does the
 * cells' of one group.  The anti join over a small inner side leaves its
 * rows found out of the walk (lion_anti_exclude()), and takes nothing from a
 * row that goes up.
 */
static TupleTableSlot *
lion_next_cells_grouped(LionCountScanState *st, bool *exhausted)
{
	LionDecodeRun *dr = st->decode;
	LionCellsRun *cr = st->cells;

	*exhausted = false;
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);

		if (dr->finished)
		{
			*exhausted = true;
			return NULL;
		}
		if (dr->emitting)
		{
			uint64		code;
			int64		count;
			TupleTableSlot *slot;
			Datum		key;
			bool		isnull;
			int			v;

			if (lion_decode_tally_next(dr->tally, &code, &count))
			{
				lion_decode_code_split(code, cr->ndim + 1, cr->gradix,
									   cr->outs);
				v = cr->outs[cr->ndim];
				if (v < 0 || v >= dr->nvals[0])
					elog(ERROR, "LionCount: a cell of entry %d of a pass of %d",
						 v, dr->nvals[0]);
				lion_cells_fill_cell(cr);
				key = dr->keys[0][v];
				isnull = dr->isnull[0][v];
				dr->rowsup++;
				slot = lion_emit_keys(st, 1, &key, &isnull, count);
				if (slot == NULL)
					continue;
				return slot;
			}
			dr->emitting = false;
			lion_decode_tally_end(dr->tally);
			if (!lion_cells_grouped_advance(st))
			{
				dr->finished = true;
				continue;
			}
		}
		else if (!dr->started)
		{
			dr->started = true;
			if (!lion_cells_grouped_start(st))
			{
				dr->finished = true;
				continue;
			}
		}

		/* the pass the walk stands at, counted */
		if (!lion_decode_tally_begin(dr->tally, cr->ndim + 1, cr->gradix))
			elog(ERROR, "LionCount: the cells of a count by cells do not fit a code");
		lion_anti_exclude_rewind(st);
		lion_cells_pass(st, cr->rangelo, cr->rangehi);
		dr->emitting = true;
	}
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

	if (cr->grouped)
		return lion_next_cells_grouped(st, exhausted);
	*exhausted = false;
	for (;;)
	{
		uint64		code;
		int64		count;
		TupleTableSlot *slot;

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
		lion_cells_fill_cell(cr);
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

	ExplainPropertyText("Group Strategy",
						cr->grouped ? "Cells, by the grouping column" : "Cells",
						es);
	/* each column with the index it is read from: `ix.col (col)` */
	for (d = 0; d < cr->ndim; d++)
	{
		const LionCellsDimPriv *dp = &cr->dimpriv[d];
		Oid			idxoid = OidIsValid(dp->idxoid) ? dp->idxoid :
			st->plan.groupidxoid;

		cols = lappend(cols,
					   psprintf("%s%s (%s)", get_rel_name(idxoid),
								lion_explain_col(idxoid, dp->attno,
												 dp->kind != LION_CELLDIM_SCALAR,
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
	/* ... of the predicates: beside a GROUP BY column, each group's */
	ExplainPropertyInteger("Cells", NULL,
						   (int64) (cr->grouped ? cr->basecells : cr->ncells),
						   es);
}
