/*-------------------------------------------------------------------------
 *
 * lion_exec_locate.c
 *		Finding the posting sets of the WHERE clauses for the relation being
 *		counted, and emitting the node's tuples.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * Evaluate the clause values that are not literals (DESIGN.md §10).
 *
 * Called once per scan, before anything is looked up, and again after every
 * ReScan: a nested loop or a LATERAL reference sets a new exec Param between
 * the two, and the node has to see the new value.  A generic prepared plan's
 * PARAM_EXTERN is constant for the statement but is still only available
 * here, and so is a stable expression's value: a cached plan carries the
 * expression, never the value it had when it was planned.
 *
 * ExecEvalExprSwitchContext() leaves its result in the per-tuple memory of
 * the node's ExprContext, which nothing here owns, so the value is copied
 * into a context of the node's own that lives exactly as long as the scan.
 */
void
lion_eval_clause_values(LionCountScanState *st)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	MemoryContext oldcxt;
	int			i;

	st->valsdone = true;

	MemoryContextReset(st->valcxt);
	oldcxt = MemoryContextSwitchTo(st->valcxt);

	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];
		int16		typlen;
		bool		typbyval;
		Datum		val;
		bool		isnull;

		if (cl->valstate == NULL)
			continue;			/* a literal: cl->val is already right */

		val = ExecEvalExprSwitchContext(cl->valstate, econtext, &isnull);
		cl->valisnull = isnull;
		if (isnull)
		{
			cl->val = (Datum) 0;
			continue;
		}

		get_typlenbyval(cl->valtype, &typlen, &typbyval);
		cl->val = datumCopy(val, typbyval, typlen);
	}

	MemoryContextSwitchTo(oldcxt);
}

/* ---------------------------------------------------------------------
 * Locating the posting sets of one relation's WHERE clauses
 *
 * Each clause produces some located posting sets and a LionKeyNode tree over
 * them, and the two go into a LionCountSource that the merge in lion_count.c
 * evaluates.  A plain clause is one source; an OR restriction is one source
 * over the sets of all its leaves (DESIGN.md §19).
 * --------------------------------------------------------------------- */

LionKeyNode *
lion_key_node(int keyno)
{
	LionKeyNode *n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));

	n->kind = LION_KN_KEY;
	n->keyno = keyno;
	return n;
}

/*
 * An AND or OR over nargs subtrees, or the subtree itself when there is only
 * one of them.  args is consumed.
 */
LionKeyNode *
lion_bool_node(LionKeyNodeKind kind, LionKeyNode **args, int nargs)
{
	LionKeyNode *n;

	Assert(nargs >= 1);
	if (nargs == 1)
		return args[0];

	n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	n->kind = kind;
	n->nargs = nargs;
	n->args = args;
	return n;
}

/*
 * A leaf of an OR whose sets only bound its query - a phrase, a weight - and
 * which no recheck can see to, since the row passes when any arm holds: the
 * planner took it only where the column stores positions and the query can
 * be followed through them (lion_query_posexact()), and its superset is put
 * through the position filter as the count walks it (LION_KN_POSFILTER).
 */
static LionKeyNode *
lion_posfilter_node(LionClauseState *cl, LionKeyNode *child)
{
	LionState  *istate = lion_index_column_state(cl->idx, cl->idxcol);
	StrategyNumber strategy = (StrategyNumber)
		get_op_opfamily_strategy(cl->opno,
								 cl->idx->rd_opfamily[cl->idxcol - 1]);
	Datum	   *itemkeys;

	if (!istate->positions ||
		!lion_tsquery_item_keys(istate, cl->val, strategy, &itemkeys))
		elog(ERROR, "roaring count: query for index \"%s\" can no longer be decided from positions",
			 RelationGetRelationName(cl->idx));
	return lion_posfilter_keynode(cl->idx, istate, cl->val, strategy, child);
}

/*
 * Renumber a tree's leaves, which name posting sets by position: the leaves
 * of an OR's arms are concatenated into one array, so each clause's tree has
 * to be moved to where its own sets ended up.
 */
static void
lion_shift_keynos(LionKeyNode *node, int delta)
{
	int			i;

	if (node == NULL || delta == 0)
		return;
	if (node->kind == LION_KN_KEY)
	{
		node->keyno += delta;
		return;
	}
	for (i = 0; i < node->nargs; i++)
		lion_shift_keynos(node->args[i], delta);
}

/*
 * Locate the posting sets of one `col = ANY (array)` clause (DESIGN.md §15):
 * one set per distinct non-NULL element, which the count then unions.
 */
static int
lion_locate_array(LionClauseState *cl, LionPostingSet **sets)
{
	ArrayType  *arr = DatumGetArrayTypeP(cl->val);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			nsets;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	/*
	 * A parameter's array has no length cap (DESIGN.md §15), and past some
	 * nine million values the sets pass the 1GB a plain allocation may have.
	 * A plain count locates such a list a batch at a time instead
	 * (lion_count_batched()); every other shape holds it whole.
	 */
	*sets = (LionPostingSet *)
		palloc_extended(sizeof(LionPostingSet) * Max(nelems, 1),
						MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

	/*
	 * One call rather than a lookup per element: the values are hashed first
	 * and their entries located in (bucket, hash) order, so the bucket pages
	 * are read in block order and duplicates are dropped in one pass over
	 * that order instead of by comparing every value with every earlier one -
	 * which at LION_MAX_ARRAY_ELEMS values is half a million datumIsEqual()
	 * calls (DESIGN.md §15).  Looking a value up twice could not change the
	 * answer either way, because a union of a set with itself is that set; it
	 * would only cost the merge another sub-cursor.
	 */
	nsets = lion_posting_set_lookup_many_col(cl->idx, cl->idxcol, elemtype,
											nelems, elems, nulls, *sets, NULL);

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(cl->val))
		pfree(arr);

	return nsets;
}

/*
 * Is WHERE item k, the IN list cl, to be located and counted a batch at a
 * time?  A parameter's array has no length cap (DESIGN.md §15), and the
 * located sets of a long one were all held at once, work_mem or not - about
 * 200 bytes a value, and past nine million values an array of them larger
 * than an allocation may be (2026-09-28 review).
 *
 * The count of a list is a SUM over pieces of it in one shape: a count of
 * the relation as one row (lion_count_relation()).  The entries of one
 * scalar index are disjoint (§15), so with the list cut into batches of
 * whole entries B_1 .. B_m, R the other positive sources and N the negated
 * ones,
 *
 *		|((B_1 ∪ ... ∪ B_m) ∩ R) \ N| = Σ_j |(B_j ∩ R) \ N|,
 *
 * the terms being disjoint - lion_run_batches()'s argument, one level up.  A
 * GROUP BY, a count(DISTINCT) and an FK-side join count the list many times
 * over and hold it whole, as a list under an OR does, whose union is not a
 * disjoint one.  One list per relation is batched.
 *
 * The values are sorted as a lookup sorts them (lion_probe_sort()) and a
 * batch ends only where the hash changes: two values of one equality class
 * hash alike and sort together, so no class is split and no entry is located
 * in two batches - the argument that lets a plain scan locate a long list
 * piece by piece (§29.4).  What is held whole is the values themselves: the
 * array, and a Datum and a hash per value.
 */
static bool
lion_array_batch_prepare(LionCountScanState *st, int k, LionClauseState *cl)
{
	ArrayType  *arr;
	Oid			elemtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;

	if (st->hasgroupidx || st->joinclause >= 0 || st->batchitem >= 0 ||
		cl->valisnull)
		return false;

	arr = DatumGetArrayTypeP(cl->val);
	if (ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) <= lion_array_batch_size())
	{
		if ((Pointer) arr != DatumGetPointer(cl->val))
			pfree(arr);
		return false;
	}

	elemtype = ARR_ELEMTYPE(arr);
	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	st->batchval = (Datum *)
		palloc_extended(sizeof(Datum) * Max(nelems, 1), MCXT_ALLOC_HUGE);
	st->batchhash = (uint32 *)
		palloc_extended(sizeof(uint32) * Max(nelems, 1), MCXT_ALLOC_HUGE);
	st->nbatchval = lion_probe_sort(cl->idx, cl->idxcol, elemtype, nelems,
									elems, nulls, st->batchval,
									st->batchhash);
	st->nbatchval = lion_probe_sort_unique(st->batchval, st->batchhash,
										   st->nbatchval, elmbyval, elmlen);
	st->batchtype = elemtype;
	st->batchitem = k;

	/* a by-reference value points into arr, which stays in wherecxt */
	pfree(elems);
	pfree(nulls);
	return true;
}

/*
 * Locate the posting sets of one multi-key clause (DESIGN.md §17).
 *
 * The query is extracted again here, with the index's OWN extractQuery
 * function - which lion_match_index() has already insisted is the one the
 * planner used - so the keys and the boolean tree are the same the plan was
 * costed with.  The tree becomes the source's combining expression; the
 * merge in lion_count.c evaluates it over the sets with the same cursors it
 * uses for an IN list, so the DESIGN.md §9 pin discipline is unchanged.
 *
 * A query the node only has at run time - a Param, a stable expression - is
 * extracted as a SUPERSET instead (DESIGN.md §17, "A query known only at run
 * time"), and cl->qmode says how it came out: KEYS is what a literal gives;
 * LOSSY locates the keys of a wider tree; NONE and ALL locate nothing, NONE
 * because no row can match and ALL because every row may.  The caller makes
 * the last two no source at all and a source that selects nothing, and
 * rechecks LOSSY and ALL in the heap (lion_build_filter()).
 */
static int
lion_locate_multikey(LionClauseState *cl, LionPostingSet **sets,
					LionKeyNode **tree)
{
	LionState   *istate = lion_index_column_state(cl->idx, cl->idxcol);
	StrategyNumber strategy;
	LionQuery	q;
	Buffer		lastpinned = InvalidBuffer;
	int			i;

	/*
	 * Only a multi-key column has an extractQuery to call; a scalar one's
	 * state leaves it unset (lion_fill_state()).  lion_open_relation() asked
	 * for a multi-key column, so this is drift - but it is an error, not a
	 * call through an empty FmgrInfo (the 2026-09-27 review).
	 */
	if (!istate->multikey)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("key column %d of lion index \"%s\" is not a multi-key column",
						(int) cl->idxcol, RelationGetRelationName(cl->idx))));

	strategy = (StrategyNumber)
		get_op_opfamily_strategy(cl->opno,
								 cl->idx->rd_opfamily[cl->idxcol - 1]);

	if (cl->con == NULL)
	{
		lion_extract_query_superset(istate, cl->val, strategy, &q);
		cl->qmode = q.mode;
		if (q.mode != LION_QMODE_KEYS && q.mode != LION_QMODE_LOSSY)
		{
			*sets = NULL;
			*tree = NULL;
			return 0;
		}
	}
	else
	{
		lion_extract_query(istate, cl->val, strategy, &q);
		cl->qmode = LION_QMODE_KEYS;

		/*
		 * The plan was only made because this extraction came out exact
		 * (lion_multikey_query_is_exact()) or a superset the row filter
		 * rechecks (a phrase, a weight: lion_analyze_leaf()), against this
		 * very function and this very constant.  Anything else now would
		 * mean the count could silently miss rows, so say so instead.
		 */
		if (q.mode != LION_QMODE_KEYS)
		{
			lion_extract_query_superset(istate, cl->val, strategy, &q);
			if (q.mode != LION_QMODE_LOSSY)
				elog(ERROR, "roaring count: query for index \"%s\" is no longer exact",
					 RelationGetRelationName(cl->idx));
			cl->qmode = LION_QMODE_LOSSY;
		}
	}

	*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(q.nkeys, 1));
	*tree = q.tree;

	/*
	 * Up to LION_MAX_QUERY_KEYS lookups, and every one of them may keep an
	 * INLINE leaf pinned for as long as the node runs: they draw on the list
	 * pin budget, as an IN list's do (DESIGN.md §15, "The pin budget").
	 */
	for (i = 0; i < q.nkeys; i++)
	{
		(void) lion_posting_set_lookup_budgeted_col(cl->idx, cl->idxcol,
													q.keys[i], InvalidOid,
													&(*sets)[i], &lastpinned);
		CHECK_FOR_INTERRUPTS();
	}

	return q.nkeys;
}

/*
 * Locate one clause's posting sets into *sets and return the tree that
 * combines them, its leaves numbered from 0.  NULL means the clause selects
 * no rows at all - a NULL parameter, an empty IN list - and then *nsets is 0
 * and nothing was located.
 *
 * This is the whole of a clause's run-time meaning, and it is the same
 * whether the clause is a source of its own or a leaf of an OR (DESIGN.md
 * §19).
 */
static LionKeyNode *
lion_locate_leaf(LionClauseState *cl, LionPostingSet **sets, int *nsets)
{
	LionKeyNode *tree = NULL;
	int			n = 0;
	int			i;

	*sets = NULL;
	*nsets = 0;
	cl->qmode = LION_QMODE_NONE;	/* until a multi-key query says otherwise */

	/*
	 * A parameter that came out NULL selects no rows at all, whatever the
	 * clause: `k = NULL`, `k = ANY (NULL)` and a NULL multi-key query are all
	 * never true (every one of those operators is strict).  The clause is
	 * then not looked up.
	 */
	if (cl->valisnull && LION_CLAUSE_IS_POSITIVE(cl->kind) &&
		cl->kind != LION_CLAUSE_NULL)
		return NULL;

	switch (cl->kind)
	{
		case LION_CLAUSE_EQ:
			*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet));
			n = 1;
			(void) lion_posting_set_lookup_col(cl->idx, cl->idxcol, cl->val,
											  cl->valtype, &(*sets)[0]);
			tree = lion_key_node(0);
			break;

		case LION_CLAUSE_ARRAY:
			n = lion_locate_array(cl, sets);
			if (n > 0)
			{
				LionKeyNode **args = (LionKeyNode **)
					palloc(sizeof(LionKeyNode *) * n);

				for (i = 0; i < n; i++)
					args[i] = lion_key_node(i);
				tree = lion_bool_node(LION_KN_OR, args, n);
			}
			break;

		case LION_CLAUSE_MULTI:
			n = lion_locate_multikey(cl, sets, &tree);
			if (n == 0)
				tree = NULL;
			break;

		case LION_CLAUSE_NE:

			/*
			 * `col <> c` (DESIGN.md §35) rejects the rows of c's entry and of
			 * the NULL one: a negated source of whichever of the two exist.
			 * The caller has seen to a NULL c, which rejects every row.
			 */
			Assert(!cl->valisnull);
			*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * 2);
			if (lion_posting_set_lookup_col(cl->idx, cl->idxcol, cl->val,
											cl->valtype, &(*sets)[n]))
				n++;
			else
				lion_posting_set_release(&(*sets)[n]);
			if (lion_posting_set_lookup_null_col(cl->idx, cl->idxcol,
												 &(*sets)[n]))
				n++;
			else
				lion_posting_set_release(&(*sets)[n]);
			if (n == 1)
				tree = lion_key_node(0);
			else if (n == 2)
			{
				/* the node keeps the array: not the stack's */
				LionKeyNode **args = (LionKeyNode **)
					palloc(sizeof(LionKeyNode *) * 2);

				args[0] = lion_key_node(0);
				args[1] = lion_key_node(1);
				tree = lion_bool_node(LION_KN_OR, args, 2);
			}
			break;

		case LION_CLAUSE_NULL:
		case LION_CLAUSE_NOTNULL:
			*sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet));
			n = 1;
			if (lion_posting_set_lookup_null_col(cl->idx, cl->idxcol,
												&(*sets)[0]))
				tree = lion_key_node(0);
			else
			{
				/*
				 * No NULL entry at all: `IS NULL` selects nothing, and
				 * `IS NOT NULL` has nothing to subtract.
				 */
				n = 0;
				tree = NULL;
			}
			break;

		default:
			elog(ERROR, "LionCount: unknown clause kind %d", cl->kind);
	}

	*nsets = n;
	return tree;
}

/*
 * The range the RANGESRC clauses on heap column attno among clauses
 * [first, first + n) make - one OR arm's, or every clause outside an OR when
 * toplevel - ANDed into one, as a driving walk's bounds are (DESIGN.md §28),
 * and resolved against the index the planner matched them to.  That is one
 * index for all of them: a column's clauses are matched to the one index
 * that holds it (lion_match_index()), so two would be planner drift.
 */
static LionRangeSource *
lion_rangesrc_range(LionCountScanState *st, int first, int n, bool toplevel,
					AttrNumber attno)
{
	LionRangeSource *rs = (LionRangeSource *) palloc0(sizeof(LionRangeSource));
	int			i;

	for (i = first; i < first + n; i++)
	{
		LionClauseState *cl = &st->clause[i];
		StrategyNumber strategy;

		if (cl->kind != LION_CLAUSE_RANGESRC || cl->attno != attno ||
			(toplevel && st->inor[i]) || cl->idx == NULL)
			continue;
		if (rs->index == NULL)
		{
			rs->index = cl->idx;
			rs->col = cl->idxcol;
			lion_range_init(&rs->range, cl->idx, cl->idxcol);
		}
		else if (RelationGetRelid(cl->idx) != RelationGetRelid(rs->index) ||
				 cl->idxcol != rs->col)
			elog(ERROR, "LionCount: the bounds of one range on two indexes");

		strategy = get_op_opfamily_strategy(cl->opno,
											cl->idx->rd_opfamily[cl->idxcol - 1]);
		lion_range_add(&rs->range, cl->idx, strategy, get_opcode(cl->opno),
					   cl->valtype, cl->val, cl->valisnull,
					   cl->idx->rd_indcollation[cl->idxcol - 1]);
	}
	if (rs->index == NULL)
		elog(ERROR, "LionCount: a range source without bounds");
	return rs;
}

/*
 * Locate a range taken as a source (DESIGN.md §32, "A range as a source"):
 * the rows whose key lies in it, which is the union of the disjoint sets a
 * walk of it hands out - entries, and the summaries of the buckets it covers
 * whole.  They are COLLECTED into one private set when that fits what is
 * left of a hash table's memory (get_hash_memory_limit(), shared by every
 * range of the relation), which the counts then read like any other set.
 * One that does not fit is left to be walked at every count instead
 * (src->rangewalk, lion_node_count()) - unless it is an OR's leaf, which
 * cannot be taken apart that way (`walkable` false; the planner declines one
 * it expects to be large).  That one used to be collected whatever it took,
 * in memory; it gets the same memory as the others now, and past it is
 * collected a window of container keys at a time into a temporary file
 * (lion_range_collect(), 2026-09-28 review).
 *
 * Returns the source's tree the way lion_locate_leaf() does: NULL when the
 * range selects nothing - an empty range, a NULL bound, or no row in it - and
 * also when it is to be walked, which `*walked` then says.
 */
static LionKeyNode *
lion_locate_range(LionCountScanState *st, LionRangeSource *rs, bool walkable,
				  LionPostingSet **sets, int *nsets, bool *walked)
{
	Size		limit = get_hash_memory_limit();
	Size		budget;
	LionPostingSet ps;
	Size		held;
	bool		spilled;
	int64		nread;
	int64		nsums;

	*sets = NULL;
	*nsets = 0;
	*walked = false;

	if (rs->range.empty)
		return NULL;

	budget = (st->rangesrc_held < limit) ? limit - st->rangesrc_held : 0;
	if (!lion_range_collect(rs->index, rs->col, &rs->range, budget, !walkable,
							&ps, &held, &spilled, &nread, &nsums))
	{
		st->rangesrc_walked++;
		*walked = true;
		return NULL;
	}

	st->rangesrc_collected++;
	if (spilled)
		st->rangesrc_spilled++;
	st->summaries += nsums;
	if (!ps.found)
		return NULL;
	st->rangesrc_held += held;

	*sets = (LionPostingSet *) palloc(sizeof(LionPostingSet));
	(*sets)[0] = ps;
	*nsets = 1;
	return lion_key_node(0);
}

/*
 * Locate one OR restriction as a single source: the union of its arms, each
 * arm the AND of its leaves (DESIGN.md §19).
 *
 * The leaves' sets are concatenated into one array, because a LionKeyNode
 * names a set by its position in the source's array; each leaf's own tree is
 * renumbered onto its slice of it.  An arm with a leaf that selects nothing
 * selects nothing itself and is dropped; an OR with no arm left selects
 * nothing at all.
 *
 * A partition leaves out what its bounds make of the OR (DESIGN.md §16, "OR
 * arms the partition bounds refute"): the leaves it has no index open for.
 * An arm left with none of its leaves is one the bounds refute, and adds
 * nothing to the union; a leaf left out of an arm that keeps others is one
 * they imply, TRUE of every row, and the AND goes on without it.  The planner
 * never leaves an arm all of whose leaves are implied: that OR is left out
 * whole (lion_leaf_drops()).
 *
 * THE PIN RULE (DESIGN.md §9 and §19).  A source is only allowed to serve its
 * containers from pinless private copies while some OTHER positive source is
 * still read the pinned way, and `lion_source_pinned()` decides that per
 * source: for an OR it needs EVERY child to hold a pin, because which of them
 * contributed a given container key is not known in advance and a dead TID
 * may have come from a single one of them.  That is what makes this source
 * carry the interlock at all, and it is why the source is marked
 * nomaterialize: the leaves of a union are never copied out.
 */
static void
lion_locate_or(LionCountScanState *st, LionOrState *orst, LionCountSource *src)
{
	LionPostingSet **leafsets;
	LionKeyNode **leaftree;
	int		   *leafn;
	bool	   *absorbed;
	bool	   *left;
	LionKeyNode **arms;
	int			narms = 0;
	int			total = 0;
	int			off = 0;
	int			leaf = 0;
	int			armfirst = 0;
	int			arm = 0;
	int			i;
	int			j;

	leafsets = (LionPostingSet **)
		palloc0(sizeof(LionPostingSet *) * orst->nleaves);
	leaftree = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * orst->nleaves);
	leafn = (int *) palloc0(sizeof(int) * orst->nleaves);
	absorbed = (bool *) palloc0(sizeof(bool) * orst->nleaves);
	left = (bool *) palloc0(sizeof(bool) * orst->nleaves);

	for (i = 0; i < orst->nleaves; i++)
	{
		LionClauseState *cl = &st->clause[orst->first + i];

		while (i >= armfirst + orst->armlen[arm])
			armfirst += orst->armlen[arm++];

		/* left out of this partition: no index was opened for it */
		if (cl->idx == NULL)
		{
			left[i] = true;
			continue;
		}

		/*
		 * A range in an arm (DESIGN.md §32) is one leaf however many bounds
		 * it has there: the first bound on its column stands for all of them
		 * and the others are absorbed into it.
		 */
		if (cl->kind == LION_CLAUSE_RANGESRC)
		{
			bool		walked;

			for (j = armfirst; j < i; j++)
			{
				LionClauseState *prev = &st->clause[orst->first + j];

				if (prev->kind == LION_CLAUSE_RANGESRC &&
					prev->attno == cl->attno)
					break;
			}
			if (j < i)
			{
				absorbed[i] = true;
				continue;
			}
			leaftree[i] = lion_locate_range(st,
											lion_rangesrc_range(st,
																orst->first + armfirst,
																orst->armlen[arm],
																false, cl->attno),
											false, &leafsets[i], &leafn[i],
											&walked);
			Assert(!walked);
		}
		else
		{
			leaftree[i] = lion_locate_leaf(cl, &leafsets[i], &leafn[i]);
			if (leaftree[i] != NULL && cl->kind == LION_CLAUSE_MULTI &&
				cl->qmode == LION_QMODE_LOSSY)
				leaftree[i] = lion_posfilter_node(cl, leaftree[i]);
		}
		total += leafn[i];
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * One array for the whole source, with every leaf's tree moved onto it,
	 * and each leaf's own array let go as it is copied: an IN list of a
	 * parameter can be millions of sets long (DESIGN.md §15).
	 */
	src->sets = (LionPostingSet *)
		palloc_extended(sizeof(LionPostingSet) * Max(total, 1),
						MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	src->nsets = total;
	src->nomaterialize = true;
	for (i = 0; i < orst->nleaves; i++)
	{
		if (leafn[i] > 0)
		{
			memcpy(&src->sets[off], leafsets[i],
				   sizeof(LionPostingSet) * leafn[i]);
			pfree(leafsets[i]);
			leafsets[i] = NULL;
		}
		lion_shift_keynos(leaftree[i], off);
		off += leafn[i];
	}
	Assert(off == total);

	arms = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * orst->narms);
	for (i = 0; i < orst->narms; i++)
	{
		LionKeyNode **conj = (LionKeyNode **)
			palloc0(sizeof(LionKeyNode *) * orst->armlen[i]);
		int			nconj = 0;
		bool		empty = false;

		for (j = 0; j < orst->armlen[i]; j++, leaf++)
		{
			if (absorbed[leaf])
				continue;		/* a bound of a range an earlier leaf is */
			if (left[leaf])
				continue;		/* refuted with its arm, or implied */
			if (leaftree[leaf] == NULL)
				empty = true;	/* an AND with a leaf that selects nothing */
			else
				conj[nconj++] = leaftree[leaf];
		}

		if (empty || nconj == 0)
			continue;			/* this arm contributes nothing to the union */

		arms[narms++] = lion_bool_node(LION_KN_AND, conj, nconj);
	}

	if (narms == 0)
	{
		src->tree = NULL;
		st->wheremissing = true;
		return;
	}

	src->tree = lion_bool_node(LION_KN_OR, arms, narms);

	/* Every arm wants a key no entry holds: the union selects nothing. */
	if (!lion_sets_satisfiable(src->nsets, src->sets, src->tree))
		st->wheremissing = true;
}

/*
 * Remember the key a clause's entry holds, so that a target list which prints
 * the pinned column can report it after the posting set is gone.  The first
 * relation that has the key wins; with partitions the others hold a key that
 * compares equal to it by the index's own equality, which is exactly the
 * guarantee the printed value rests on in the single-table case as well.
 */
static void
lion_save_clause_key(LionCountScanState *st, LionClauseState *cl,
					const LionPostingSet *ps)
{
	MemoryContext oldcxt;
	LionState   *istate;

	if (cl->hasstoredkey || !ps->found || !ps->hasstoredkey)
		return;

	if (ps->keyisnull)
	{
		cl->storedkey = (Datum) 0;
		cl->keyisnull = true;
		cl->hasstoredkey = true;
		return;
	}

	istate = lion_index_column_state(cl->idx, cl->idxcol);
	oldcxt = MemoryContextSwitchTo(st->keycxt);
	cl->storedkey = datumCopy(ps->storedkey, istate->typbyval, istate->typlen);
	MemoryContextSwitchTo(oldcxt);
	cl->keyisnull = false;
	cl->hasstoredkey = true;
}

/*
 * The type an argument of type `type` has when the parser hands it to a
 * function that declares `declared` (coerce_type(), parse_coerce.c), for the
 * row filter below, which rebuilds the call the executor would have made.  The
 * clause analysis peeled the relabels off both operands (lion_strip()), so the
 * column is a bare Var of its own type and the value an expression of its own
 * - a domain, say - where the parser had passed:
 *
 *	- the actual type, domains included, to an argument of that very type and
 *	  to one declared any, anyelement, anynonarray, anycompatible or
 *	  anycompatiblenonarray;
 *	- the BASE type to the other polymorphic types (anyarray, anyrange, ...),
 *	  which relabel a domain over an array, a range or an enum to what it is
 *	  over: a function declared on anyarray never sees the domain;
 *	- and the declared type itself to any other argument, which can only have
 *	  got here by a binary coercion - of a domain over that type, or of a
 *	  type binary-coercible to it.
 */
static Oid
lion_arg_type_passed(Oid type, Oid declared)
{
	if (!OidIsValid(declared) || type == declared)
		return type;

	switch (declared)
	{
		case ANYOID:
		case ANYELEMENTOID:
		case ANYNONARRAYOID:
		case ANYCOMPATIBLEOID:
		case ANYCOMPATIBLENONARRAYOID:
			return type;
		case ANYARRAYOID:
		case ANYENUMOID:
		case ANYRANGEOID:
		case ANYMULTIRANGEOID:
		case ANYCOMPATIBLEARRAYOID:
		case ANYCOMPATIBLERANGEOID:
		case ANYCOMPATIBLEMULTIRANGEOID:
			return getBaseType(type);
		default:
			return declared;
	}
}

/*
 * The heap recheck this scan's multi-key queries need (DESIGN.md §17, "A
 * query known only at run time"), as a row filter over the relation being
 * counted, set on the visibility cache every count of this execution is
 * handed (lion_vis_cache_set_filter()) - or none, which is what every scan
 * whose queries all came out exact gets.
 *
 * Each clause is `col op value`: the clause's own operator and its value as
 * this scan evaluated it, and the column as THIS relation numbers it, which
 * the index says (a partition may number it differently from the parent,
 * DESIGN.md §16).  The collation is the index column's, which
 * lion_match_index() found equal to the clause's input collation whenever
 * the clause has one; a clause over a type that has none ignores it.
 */
void
lion_build_filter(LionCountScanState *st)
{
	MemoryContext oldcxt;
	LionRowFilter *filter;
	int			n = 0;
	int			i;

	st->filter = NULL;
	lion_vis_cache_set_filter(st->viscache, NULL);

	/* not a clause the relation's partition bounds imply (DESIGN.md §16) */
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];

		if (cl->kind == LION_CLAUSE_MULTI && !st->inor[i] && cl->idx != NULL &&
			(cl->qmode == LION_QMODE_LOSSY || cl->qmode == LION_QMODE_ALL))
			n++;
	}
	if (n == 0)
		return;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);
	filter = (LionRowFilter *) palloc0(sizeof(LionRowFilter));
	filter->heap = st->heap;
	filter->clauses = (LionRowFilterClause *)
		palloc0(sizeof(LionRowFilterClause) * n);
	filter->tmpcxt = AllocSetContextCreate(st->wherecxt,
										   "LionCount row filter",
										   ALLOCSET_SMALL_SIZES);
	filter->pos = (LionPosFilter **) palloc0(sizeof(LionPosFilter *) * n);
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];
		LionRowFilterClause *c;
		Form_pg_attribute att;
		Oid			lefttype;
		Oid			righttype;
		Oid			coltype;
		Oid			valtype;
		Expr	   *col;
		int16		typlen;
		bool		typbyval;

		if (!(cl->kind == LION_CLAUSE_MULTI && !st->inor[i] &&
			  cl->idx != NULL &&
			  (cl->qmode == LION_QMODE_LOSSY || cl->qmode == LION_QMODE_ALL)))
			continue;

		c = &filter->clauses[filter->nclauses++];

		/*
		 * A superset over a column that stores positions is decided from
		 * them, before the visibility map is asked (lion_posfilter.c), and
		 * the heap need not see its rows; anything else - a query no key
		 * narrows, one with a prefix lexeme - only the heap can decide.
		 */
		{
			LionState  *istate = lion_index_column_state(cl->idx, cl->idxcol);
			LionPosFilter *pf = NULL;

			if (cl->qmode == LION_QMODE_LOSSY && istate->positions)
				pf = lion_posfilter_begin(cl->idx, istate, cl->val,
										  (StrategyNumber)
										  get_op_opfamily_strategy(cl->opno,
																   cl->idx->rd_opfamily[cl->idxcol - 1]));
			if (pf != NULL)
				filter->pos[filter->npos++] = pf;
			else
				filter->nheap++;
		}

		c->attno = cl->idx->rd_index->indkey.values[cl->idxcol - 1];
		if (c->attno <= 0 || c->attno > RelationGetDescr(st->heap)->natts)
			elog(ERROR, "LionCount: a multi-key clause on an index expression");
		c->notnull = false;
		c->collation = cl->idx->rd_indcollation[cl->idxcol - 1];
		c->value = cl->val;
		fmgr_info_cxt(get_opcode(cl->opno), &c->flinfo, st->wherecxt);

		/*
		 * The call the executor would have made for the clause, expression
		 * included, which is what a polymorphic operator's function asks
		 * its argument types of (get_fn_expr_argtype()).  That includes the
		 * parser's relabels (lion_arg_type_passed()): a column of a domain
		 * over int[] reaches `@>(anyarray, anyarray)` as int[], and the Var
		 * alone would have shown the function the domain (the 2026-09-27
		 * review).  The value is a Const of the type the parser would have
		 * given it, as constant folding leaves a relabelled literal.
		 */
		att = TupleDescAttr(RelationGetDescr(st->heap), c->attno - 1);
		op_input_types(cl->opno, &lefttype, &righttype);
		col = (Expr *) makeVar(1, c->attno, att->atttypid, att->atttypmod,
							   att->attcollation, 0);
		coltype = lion_arg_type_passed(att->atttypid, lefttype);
		if (coltype != att->atttypid)
			col = (Expr *) makeRelabelType(col, coltype, -1,
										   type_is_collatable(coltype) ?
										   att->attcollation : InvalidOid,
										   COERCE_IMPLICIT_CAST);
		valtype = lion_arg_type_passed(cl->valtype, righttype);
		get_typlenbyval(valtype, &typlen, &typbyval);
		fmgr_info_set_expr((Node *)
						   make_opclause(cl->opno, BOOLOID, false, col,
										 (Expr *) makeConst(valtype, -1,
															InvalidOid,
															typlen, cl->val,
															false, typbyval),
										 InvalidOid, c->collation),
						   &c->flinfo);
	}
	MemoryContextSwitchTo(oldcxt);

	st->filter = filter;
	lion_vis_cache_set_filter(st->viscache, filter);
}

/*
 * Locate the posting sets of every WHERE clause of the relation the node is
 * counting.  They stay located until lion_release_where() - with partitions
 * for one partition's turn, with a plain table until the node is done - and
 * keep their pins (for INLINE entries) until the node first hands a row to
 * the executor, when they become NOPIN copies (lion_pause_run(); DESIGN.md
 * §15, "Paused and finished counts").  Until then that is the DESIGN.md
 * section 9 discipline applied for the length of the counts rather than for
 * one container; after it, each count takes its interlock from the set that
 * drives it.
 */
void
lion_locate_where(LionCountScanState *st)
{
	MemoryContext oldcxt;
	int			k;

	/* what this locates may be pinned until the next pause unpins it */
	st->wherepinned = true;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);

	for (k = 0; k < st->nitem; k++)
	{
		LionClauseState *cl = &st->clause[st->item[k].clauseno];
		LionCountSource *src = &st->sources[k + 1];

		src->nsets = 0;
		src->sets = NULL;
		src->tree = NULL;
		src->negated = false;
		src->nomaterialize = false;
		src->disjoint = false;
		src->rangewalk = NULL;

		if (st->item[k].orno >= 0)
		{
			lion_locate_or(st, &st->ors[st->item[k].orno], src);
			continue;
		}

		/* A range as a source (DESIGN.md §32): collected, or walked. */
		if (st->item[k].rangesrc)
		{
			LionRangeSource *rs = lion_rangesrc_range(st, 0, st->nclause, true,
													  cl->attno);
			bool		walked;

			src->tree = lion_locate_range(st, rs, true, &src->sets,
										  &src->nsets, &walked);
			if (walked)
				src->rangewalk = rs;
			else if (src->tree == NULL)
				st->wheremissing = true;
			continue;
		}

		/*
		 * An IN list too long to locate at once, in a count that can take it
		 * a batch at a time (lion_array_batch_prepare()): the count locates
		 * it, and until then it is a positive source of no sets.
		 */
		if (cl->kind == LION_CLAUSE_ARRAY &&
			lion_array_batch_prepare(st, k, cl))
		{
			src->disjoint = true;
			continue;
		}

		/* `col <> NULL` is true of no row (DESIGN.md §35) */
		if (cl->kind == LION_CLAUSE_NE && cl->valisnull)
		{
			st->wheremissing = true;
			src->negated = true;
			continue;
		}

		src->negated = LION_CLAUSE_IS_NEGATED(cl->kind);
		src->tree = lion_locate_leaf(cl, &src->sets, &src->nsets);

		/*
		 * A multi-key query no key narrows (DESIGN.md §17, "A query known
		 * only at run time"): as far as this clause goes every row is a
		 * candidate, so it is no source of the intersection - a negated
		 * source with nothing to subtract, which is exactly how `IS NOT NULL`
		 * over a column without NULLs reads - and the row filter below tests
		 * it on each candidate the other sources leave.
		 */
		if (cl->kind == LION_CLAUSE_MULTI && cl->qmode == LION_QMODE_ALL)
		{
			src->negated = true;
			continue;
		}

		/*
		 * An IN list is one scalar index's entries, one per distinct listed
		 * value: disjoint by construction, so a count of their union is the
		 * SUM of their counts and lion_count_sources() may skip the k-way
		 * merge entirely (DESIGN.md §15).  Only a clause that is a source of
		 * its own may say so; under an OR the source's sets are several
		 * clauses' and overlap freely.
		 */
		src->disjoint = (cl->kind == LION_CLAUSE_ARRAY);

		/*
		 * A positive clause that can select nothing makes the whole count 0,
		 * and saying so here saves the merge - and, in the GROUP BY path,
		 * every group of it.  A negated one simply has nothing to subtract.
		 */
		if (LION_CLAUSE_IS_POSITIVE(cl->kind) &&
			!lion_sets_satisfiable(src->nsets, src->sets, src->tree))
			st->wheremissing = true;

		/*
		 * Only a clause that pins the column to ONE value can have its key
		 * printed, and those are the ones with a single set.
		 */
		if (LION_CLAUSE_PINS_VALUE(cl->kind) && src->nsets == 1)
			lion_save_clause_key(st, cl, &src->sets[0]);
	}

	/*
	 * The RANGE clauses, as one bound on the driving walk (DESIGN.md §28),
	 * resolved against THIS relation's driving index: a partition's may be
	 * another index, put its column elsewhere and compare the bounds through
	 * a family of its own (§16).  The planner found every one of them on the
	 * driving index, and the executor opened both by the plan's Oids, so a
	 * clause on another index or column is drift and not a query.  A NULL
	 * bound - a Param that came out NULL - selects nothing.
	 */
	if (st->hasrange)
	{
		lion_range_init(&st->range, st->groupidx, st->groupidxcol);
		for (k = 0; k < st->nclause; k++)
		{
			LionClauseState *cl = &st->clause[k];
			StrategyNumber strategy;

			if (cl->kind != LION_CLAUSE_RANGE)
				continue;
			if (RelationGetRelid(cl->idx) != RelationGetRelid(st->groupidx) ||
				cl->idxcol != st->groupidxcol)
				elog(ERROR, "LionCount: range clause on another index than the driving one");
			strategy = get_op_opfamily_strategy(cl->opno,
												st->groupidx->rd_opfamily[st->groupidxcol - 1]);
			lion_range_add(&st->range, st->groupidx, strategy,
						   get_opcode(cl->opno), cl->valtype, cl->val,
						   cl->valisnull,
						   st->groupidx->rd_indcollation[st->groupidxcol - 1]);
		}
		if (st->range.empty)
			st->wheremissing = true;
	}

	MemoryContextSwitchTo(oldcxt);

	/*
	 * Can an IN list drive the groups instead of the index's entry scan
	 * (DESIGN.md §15)?  Only when the clause's index IS the index that would
	 * drive them - same relation, same column, so the entries are the same
	 * entries - and the grouping is the plain one-column form.  A two-column
	 * GROUP BY (§20) and a sum-over-all (§14) both walk the entries for
	 * reasons of their own and are left alone, and so does the (g, k) loop of
	 * a count(DISTINCT k) per group (§26), whose groups are emitted in g's
	 * directory order, which the planner may have claimed as pathkeys.
	 *
	 * The count(DISTINCT k) walk WITHOUT a GROUP BY is driven like a GROUP BY
	 * k whose groups are summed (§26), and takes a list on k the same way -
	 * and an equality on k as a list of one, which a GROUP BY never sees
	 * (the planner folds the grouping column it pins).
	 */
	st->ingroupitem = -1;
	st->ingroupset = 0;
	if (st->hasgroupidx && st->driveattno != 0 && st->innerattno == 0 &&
		!st->sumall)
	{
		for (k = 0; k < st->nitem; k++)
		{
			LionClauseState *cl;

			if (st->item[k].orno >= 0)
				continue;
			cl = &st->clause[st->item[k].clauseno];
			if (!(cl->kind == LION_CLAUSE_ARRAY ||
				  (cl->kind == LION_CLAUSE_EQ && st->distattno != 0)) ||
				cl->attno != st->driveattno ||
				cl->idxoid != st->groupidxoid ||
				cl->idxcol != st->groupidxcol)
				continue;
			st->ingroupitem = k;
			break;
		}
	}
	st->ingroupleft = 0;
	if (st->ingroupitem >= 0)
	{
		LionCountSource *src = &st->sources[st->ingroupitem + 1];

		for (k = 0; k < src->nsets; k++)
		{
			if (src->sets[k].found)
				st->ingroupleft++;
		}
	}

	/*
	 * And the sum-over-all's own `IS NOT NULL` (DESIGN.md §14, the sumallitem
	 * half of the comment on the field).  The clause has to be the one on the
	 * DRIVING index AND on its driving KEY COLUMN: another column's NULL entry
	 * says nothing about this column's entries, and since DESIGN.md §24 the
	 * two may live in one relation, so the Oid alone no longer tells them
	 * apart (`a IS NOT NULL AND b IS NOT NULL` over one index on (a, b) would
	 * otherwise drop b's NULL set and subtract a's from a's own entries,
	 * which removes nothing: every b NULL would be counted).
	 *
	 * The count(DISTINCT k) walk without a GROUP BY (§26) drops a
	 * `k IS NOT NULL` the same way, for the same reason: the NULL entry is
	 * the only one it removes anything from, and that entry is never a
	 * distinct value.
	 */
	st->sumallitem = -1;
	if ((st->sumall || (st->distattno != 0 && st->groupattno == 0)) &&
		st->hasgroupidx)
	{
		for (k = 0; k < st->nitem; k++)
		{
			LionClauseState *cl;

			if (st->item[k].orno >= 0)
				continue;
			cl = &st->clause[st->item[k].clauseno];
			if (cl->kind != LION_CLAUSE_NOTNULL ||
				cl->attno != st->driveattno ||
				cl->idxoid != st->groupidxoid ||
				cl->idxcol != st->groupidxcol)
				continue;
			st->sumallitem = k;
			break;
		}
	}

	if (st->ingroupitem >= 0 || st->sumallitem >= 0)
	{
		int			drop = (st->ingroupitem >= 0) ? st->ingroupitem
			: st->sumallitem;
		int			n = 1;		/* slot 0 is the driver's own set */

		st->dsources[0] = st->sources[0];
		for (k = 0; k < st->nitem; k++)
		{
			if (k == drop)
				continue;
			st->dsources[n++] = st->sources[k + 1];
		}
		st->ndsource = n;
	}

	lion_build_filter(st);
	st->located = true;
}

void
lion_release_where(LionCountScanState *st)
{
	int			i;
	int			j;

	if (st->sources == NULL)
		return;

	for (i = 0; i < st->nitem; i++)
	{
		LionCountSource *src = &st->sources[i + 1];

		for (j = 0; j < src->nsets; j++)
			lion_posting_set_release(&src->sets[j]);
		src->nsets = 0;
		src->sets = NULL;
		src->tree = NULL;		/* it lived in wherecxt, reset below */
		src->nomaterialize = false;
		src->disjoint = false;
		src->rangewalk = NULL;
	}
	st->rangesrc_held = 0;

	/*
	 * The collected WHERE (lion_group_count()): its file, if it spilled, is
	 * closed here, and its memory goes with wherecxt below.
	 */
	lion_posting_set_release(&st->wherecoll);
	st->wtried = false;
	st->wcollected = false;
	st->wknown = false;
	st->wcompound = false;
	st->wsingle = NULL;
	st->wreach = 0;
	st->wspent = 0;
	st->wcounts = 0;
	st->wckeys = 0;

	/* ... and a batch of groups counted against it, whose sets are gone */
	st->gbn = 0;
	st->gbpos = 0;
	if (st->gbatchcxt != NULL)
		MemoryContextReset(st->gbatchcxt);

	/* ... and a parallel GROUP BY's range, whose copy it was (grangecxt) */
	st->grange = -1;
	if (st->grangecxt != NULL)
		MemoryContextReset(st->grangecxt);

	/* ... and so do the values of a list counted in batches */
	st->batchitem = -1;
	st->batchval = NULL;
	st->batchhash = NULL;
	st->nbatchval = 0;

	/* the row filter lives in wherecxt too, and names this relation */
	st->filter = NULL;
	lion_vis_cache_set_filter(st->viscache, NULL);

	if (st->wherecxt != NULL)
		MemoryContextReset(st->wherecxt);
	st->located = false;
	st->wheremissing = false;
	st->ingroupitem = -1;
	st->ingroupset = 0;
	st->ingroupleft = 0;
	st->sumallitem = -1;
}

/*
 * A row of one or two GROUP BY columns - the entry walk's group, a pair of
 * the nested loop (DESIGN.md §20) - or of none: lion_emit_keys() of them.
 */
TupleTableSlot *
lion_emit_tuple(LionCountScanState *st, Datum key, bool keyisnull,
			   Datum key2, bool key2isnull, int64 count)
{
	Datum		keys[2];
	bool		isnull[2];

	keys[0] = key;
	isnull[0] = keyisnull;
	keys[1] = key2;
	isnull[1] = key2isnull;
	return lion_emit_keys(st, 2, keys, isnull, count);
}

/*
 * The row of a group: the key of each of its nkeys GROUP BY columns, in the
 * plan's order, and its count - the target list's columns made of them, the
 * HAVING applied and the projection done.  NULL when the HAVING rejects it.
 */
TupleTableSlot *
lion_emit_keys(LionCountScanState *st, int nkeys, const Datum *keys,
			   const bool *keyisnull, int64 count)
{
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	int			i;

	/*
	 * What the previous group's projection and HAVING allocated is dead once
	 * the executor asks for the next tuple (ExecScan() resets at the same
	 * point).  Nothing of the node's own lives in this memory: the clause
	 * values were copied out of it (lion_eval_clause_values()).
	 */
	ResetExprContext(econtext);

	ExecClearTuple(slot);
	for (i = 0; i < st->ntlist; i++)
	{
		int			kind = st->tlkind[i];

		/*
		 * A dimension column of the FK-side join, from the child's row - of
		 * which a summed join's one row has none: nothing reads its dimension
		 * columns (LION_JOINFLAG_SUM).
		 */
		if (LION_TL_IS_CHILDCOL(kind))
		{
			if (st->childslot == NULL)
			{
				Assert(st->joinsum);
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
				continue;
			}
			slot->tts_values[i] = slot_getattr(st->childslot,
											   LION_TL_CHILDRESNO(kind),
											   &slot->tts_isnull[i]);
			continue;
		}

		slot->tts_isnull[i] = false;
		switch (kind)
		{
			case LION_TL_GROUPKEY:
				Assert(nkeys >= 1);
				slot->tts_values[i] = keys[0];
				slot->tts_isnull[i] = keyisnull[0];
				break;

			case LION_TL_GROUPKEY2:
				Assert(nkeys >= 2);
				slot->tts_values[i] = keys[1];
				slot->tts_isnull[i] = keyisnull[1];
				break;

			case LION_TL_COUNT:
				slot->tts_values[i] = Int64GetDatum(count);
				break;

			case LION_TL_COUNT_GROUPCOL:
				/* count(group column): 0 in the NULL group (DESIGN.md §14) */
				slot->tts_values[i] = Int64GetDatum(keyisnull[0] ? 0 : count);
				break;

			case LION_TL_COUNT_GROUPCOL2:
				slot->tts_values[i] = Int64GetDatum(keyisnull[1] ? 0 : count);
				break;

			case LION_TL_COUNT_ZERO:
				/* count(col) where a clause pins col to NULL */
				slot->tts_values[i] = Int64GetDatum(0);
				break;

			case LION_TL_COUNT_DISTINCT:
				/* count(DISTINCT k) of the finished group (DESIGN.md §26) */
				slot->tts_values[i] = Int64GetDatum(st->distcount);
				break;

			case LION_TL_COUNT_DISTCOL:
				/* count(k): the rows of k's non-NULL entries (§26) */
				slot->tts_values[i] = Int64GetDatum(st->distcolcount);
				break;

			default:
				if (LION_TL_IS_GROUPKEYN(kind))
				{
					/* the third or a later column of the decoded walk (§34) */
					int			g = LION_TL_GROUPN_COL(kind);

					Assert(g < nkeys);
					slot->tts_values[i] = keys[g];
					slot->tts_isnull[i] = keyisnull[g];
					break;
				}
				if (LION_TL_IS_WAGG(kind))
				{
					/* an aggregate over a column's entries (DESIGN.md §37) */
					LionWAgg   *a = &st->wagg[LION_TL_WAGG_NO(kind)];

					slot->tts_values[i] = a->result;
					slot->tts_isnull[i] = a->resnull;
					break;
				}
				if (LION_TL_IS_WKEY(kind))
				{
					/* ... and the key its argument read, which nothing prints */
					slot->tts_values[i] = (Datum) 0;
					slot->tts_isnull[i] = true;
					break;
				}
				if (LION_TL_IS_COUNT_GROUPCOLN(kind))
				{
					int			g = LION_TL_GROUPN_COL(kind);

					Assert(g < nkeys);
					slot->tts_values[i] = Int64GetDatum(keyisnull[g] ? 0 : count);
					break;
				}
				{
					/*
					 * A column a clause pins to one value: report the key the
					 * index stored for it, which is the value the heap holds
					 * (or NULL, for an `IS NULL` clause).
					 */
					LionClauseState *cl = &st->clause[kind - LION_TL_WHEREKEY];

					/*
					 * A clause with no entry anywhere counts zero rows, and a
					 * group of zero rows is never emitted, so the key is
					 * always there by the time we get here.
					 */
					Assert(cl->hasstoredkey);
					if (cl->hasstoredkey)
					{
						slot->tts_values[i] = cl->storedkey;
						slot->tts_isnull[i] = cl->keyisnull;
					}
					else
					{
						slot->tts_values[i] = (Datum) 0;
						slot->tts_isnull[i] = true;
					}
					break;
				}
		}
	}
	ExecStoreVirtualTuple(slot);

	econtext->ecxt_scantuple = slot;

	/*
	 * HAVING (DESIGN.md §10): the plan's qual, rewritten by setrefs.c to read
	 * the counts and keys of this very tuple.  A group that fails it is
	 * consumed like any other; the caller fetches the next one.
	 */
	if (st->css.ss.ps.qual != NULL && !ExecQual(st->css.ss.ps.qual, econtext))
	{
		InstrCountFiltered1(st, 1);
		st->filtered = true;
		return NULL;
	}

	if (st->css.ss.ps.ps_ProjInfo != NULL)
		return ExecProject(st->css.ss.ps.ps_ProjInfo);
	return slot;
}

/* ---------------------------------------------------------------------
 * Counting one relation
 *
 * These three are what a plain table and one partition of a partitioned one
 * have in common (DESIGN.md §16): the caller has opened the relation with
 * lion_open_relation() and located its WHERE clauses, and every posting set
 * they take is released before they return, so no pin of this relation
 * outlives its turn.
 * --------------------------------------------------------------------- */
