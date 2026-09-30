/*-------------------------------------------------------------------------
 *
 * lion_range.c
 *		Range restrictions (DESIGN.md §28), and iterating every entry of an
 *		index.
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

/* ---------------------------------------------------------------------
 * Range restrictions (DESIGN.md §28)
 * --------------------------------------------------------------------- */


void
lion_range_init(LionRange *range, Relation index, AttrNumber attno)
{
	memset(range, 0, sizeof(LionRange));
	range->state = lion_index_column_state(index, attno);
	range->ordered = range->state->ordered;
	range->lower = -1;
	range->upper = -1;
	range->nupper = 0;
	range->nholes = 0;
	range->empty = false;
}

/*
 * Add one bound, `key <strategy> value`, to the range.  opfuncid is the
 * function of the operator the clause names, and valtype the type of value;
 * isnull says the value is NULL, which no strict comparison is satisfied by,
 * so the range is then empty.
 *
 * The comparison is the one §21's probe resolution finds for a value of this
 * type (lion_probe_init()), so the walk reads the directory in exactly the
 * order a lookup of the same value would: the column's own proc 4 for its own
 * type and for a binary coercion to it, the family's cross-type proc 4
 * otherwise - btint48cmp for an int8 bound on an int4 column, which compares
 * the two widths exactly, so a bound outside the column's domain needs no
 * special case.  Where that resolution finds none, the bound is tested with
 * the operator itself and the walk can no longer be bounded at all.
 */
void
lion_range_add(LionRange *range, Relation index, StrategyNumber strategy,
			   Oid opfuncid, Oid valtype, Datum value, bool isnull,
			   Oid collation)
{
	LionRangeBound *b;
	LionProbe	probe;

	if (!LION_STRAT_IS_WALK(strategy))
		elog(ERROR, "lion index: strategy %d is not a range comparison",
			 (int) strategy);

	if (isnull)
	{
		range->empty = true;
		return;
	}

	if (range->nbounds >= range->maxbounds)
	{
		range->maxbounds = Max(4, range->maxbounds * 2);
		range->bounds = (range->bounds == NULL) ?
			(LionRangeBound *) palloc0(sizeof(LionRangeBound) * range->maxbounds) :
			(LionRangeBound *) repalloc(range->bounds,
										sizeof(LionRangeBound) * range->maxbounds);
	}
	b = &range->bounds[range->nbounds];
	memset(b, 0, sizeof(LionRangeBound));
	b->strategy = strategy;
	b->value = value;
	b->collation = collation;

	/*
	 * A class declared on a polymorphic type (enum_ops, FOR TYPE anyenum)
	 * compares its keys with values of the column's own type, whatever that
	 * type is called: they are the class's own, not a cross-type search.
	 */
	if (IsPolymorphicType(index->rd_opcintype[range->state->attno - 1]))
		valtype = InvalidOid;

	lion_probe_init(index, range->state, valtype, &probe);
	if (probe.hascmp && !probe.needscan)
	{
		fmgr_info_copy(&b->cmpproc, &probe.cmpproc, CurrentMemoryContext);
		b->hascmp = true;
	}
	else
	{
		fmgr_info(opfuncid, &b->opproc);
		b->hascmp = false;
		range->ordered = false;
	}

	if (strategy == LION_STRAT_NE)
		range->nholes++;		/* neither end: the walk passes it by */
	else if (LION_STRAT_IS_LOWER(strategy))
	{
		if (range->lower < 0)
			range->lower = range->nbounds;
	}
	else
	{
		range->upper = (range->nupper == 0) ? range->nbounds : -1;
		range->nupper++;
	}
	range->nbounds++;
}

/* Does the stored key satisfy one bound? */
static bool
lion_range_bound_ok(LionRange *range, LionRangeBound *b, Datum key)
{
	int32		c;

	if (!b->hascmp)
		return DatumGetBool(FunctionCall2Coll(&b->opproc, b->collation,
											  key, b->value));

	c = DatumGetInt32(FunctionCall2Coll(&b->cmpproc, range->state->collation,
										key, b->value));
	switch (b->strategy)
	{
		case LION_STRAT_LT:
			return c < 0;
		case LION_STRAT_LE:
			return c <= 0;
		case LION_STRAT_GE:
			return c >= 0;
		case LION_STRAT_NE:
			return c != 0;
		default:
			return c > 0;
	}
}

/*
 * Does one entry of the range's column satisfy every bound?
 *
 * The reserved entries never do: a NULL key satisfies no comparison and the
 * EMPTY entry has no key at all.  In an ORDERED range the first entry that
 * fails an UPPER bound ends the walk - every later entry sorts at or above it
 * (the order leads with proc 4 within a column and a kind, §21) and so fails
 * that bound too - while one that fails only a LOWER bound is skipped, and the
 * entries that do are a prefix of what the walk visits: those of the landing
 * leaf below the bound it descended to, and those below any other lower bound.
 * Without an order nothing ends the walk early, and neither does a hole
 * (`<>`, DESIGN.md §35): it fails its own entry and no other.
 *
 * The caller has checked the entry's column and holds the page it is on.
 */
int
lion_range_test(LionRange *range, const LionEntryTuple *entry)
{
	LionState  *state = range->state;
	Datum		key;
	bool		skip = false;
	int			i;

	if (range->empty)
		return LION_RANGE_END;
	if (lion_entry_kind(entry) != LION_KIND_VALUE)
		return LION_RANGE_SKIP;

	key = lion_entry_key(state, entry);

	for (i = 0; i < range->nbounds; i++)
	{
		LionRangeBound *b = &range->bounds[i];

		if (lion_range_bound_ok(range, b, key))
			continue;
		if (range->ordered && !LION_STRAT_IS_LOWER(b->strategy) &&
			b->strategy != LION_STRAT_NE)
			return LION_RANGE_END;
		skip = true;
	}

	return skip ? LION_RANGE_SKIP : LION_RANGE_MATCH;
}

/*
 * ... and a LOWER bound?  In an ordered range those entries are the column's
 * first ones, and every entry BEFORE the last of them fails one too: which is
 * where a descending walk ends (DESIGN.md §30.11).  An entry the range skips
 * for anything else - an upper bound, a hole (§35) - is passed over.
 */
bool
lion_range_fails_lower(LionRange *range, const LionEntryTuple *entry)
{
	Datum		key;
	int			i;

	Assert(lion_entry_kind(entry) == LION_KIND_VALUE);
	key = lion_entry_key(range->state, entry);

	for (i = 0; i < range->nbounds; i++)
	{
		LionRangeBound *b = &range->bounds[i];

		if (LION_STRAT_IS_LOWER(b->strategy) &&
			!lion_range_bound_ok(range, b, key))
			return true;
	}
	return false;
}

/*
 * Does a VALUE entry of the range's column fail an UPPER bound?  In an ordered
 * range those entries are the column's last ones (the walk ABOVE the range,
 * DESIGN.md §28), and every entry after the first of them fails one too.
 */
bool
lion_range_fails_upper(LionRange *range, const LionEntryTuple *entry)
{
	Datum		key;
	int			i;

	Assert(lion_entry_kind(entry) == LION_KIND_VALUE);
	key = lion_entry_key(range->state, entry);

	for (i = 0; i < range->nbounds; i++)
	{
		LionRangeBound *b = &range->bounds[i];

		if (!LION_STRAT_IS_LOWER(b->strategy) &&
			b->strategy != LION_STRAT_NE &&
			!lion_range_bound_ok(range, b, key))
			return true;
	}
	return false;
}

/*
 * Where a descent for bound number `bound` lands among the column's entries of
 * `kind` - VALUE, or SUMMARY for the summaries of DESIGN.md §32 - and a copy of
 * the first item at or after that position, moving right past a leaf whose
 * last item is below it; *posp is NULL at the very end of the directory.  The
 * leaf returned is the one that item is on.
 *
 * The descent is an ordinary lookup's (lion_dir_search()) with a search key of
 * the kind whose key is the bound, whose hash is 0 and which has no stored
 * form, so it compares as the SMALLEST member of its own run (§21): the item
 * it lands on is the first one whose proc 4 is not below the bound.  The walk
 * that starts there re-reads the leaf with nothing held in between, which is
 * safe for the reason every resumed entry scan is: a split moves entries only
 * rightwards, onto a page the walk has yet to reach, and a directory leaf is
 * never unlinked (§21).
 *
 * The item is what puts the landings of SEVERAL bounds in order
 * (lion_cmp_entries(), the directory order itself), which is how a walk finds
 * the tightest of them (lion_range_side_leaf()).
 *
 * An item that sorts BEFORE the search key is stepped over, as
 * lion_dir_search_first() does: never there on a sound directory, it is what
 * a descent to a summary lands on in an index an earlier version damaged (a
 * SUMLAST pivot above the summary it routes to, DESIGN.md §32), and taking it
 * as E_j made the walk count whole buckets below the range's bound.
 */
static BlockNumber
lion_range_landing(Relation index, LionRange *range, int bound, int kind,
				   LionEntryTuple **posp)
{
	LionRangeBound *b = &range->bounds[bound];
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	BlockNumber blk;
	LionRightWalk walk;

	Assert(b->hascmp);
	lion_search_key_init(range->state, &sk, kind, b->value, 0);
	sk.cmpproc = &b->cmpproc;

	buf = lion_dir_search(index, NULL, range->state->ix, &sk,
						  BUFFER_LOCK_SHARE, false, &off);
	lion_rightwalk_init(&walk);
	for (;;)
	{
		Page		page = BufferGetPage(buf);

		if (off <= PageGetMaxOffsetNumber(page))
		{
			ItemId		iid = PageGetItemId(page, off);

			if (lion_cmp_entry(lion_page_entry(page, off), &sk) < 0)
			{
				off = OffsetNumberNext(off);
				continue;
			}
			*posp = (LionEntryTuple *) palloc(ItemIdGetLength(iid));
			memcpy(*posp, PageGetItem(page, iid), ItemIdGetLength(iid));
			break;
		}
		if (LionPageIsRightmost(page))
		{
			*posp = NULL;
			break;
		}
		buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE, &walk);
		off = lion_page_first_data(BufferGetPage(buf));
	}
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * The landing of the TIGHTEST bound of one side of an ordered range, among the
 * column's entries of `kind`: of its LOWER bounds the one that lands furthest
 * right, of its UPPER bounds the one that lands furthest left.  Every entry
 * before the first fails that lower bound, so a walk of what the range selects
 * may start there; every entry before the second passes every upper bound, so
 * a walk of what lies ABOVE the range must start there and may.  With one
 * bound of the side this is that bound's descent, as it always was; with two
 * (`k < 10 AND k <= $1`) it used to be the first one's, or - for the walk
 * above - where the range itself started, which made the complement no
 * cheaper than the range (DESIGN.md §28, fixed in §32).
 *
 * Returns InvalidBlockNumber when the side has no bound.  *posp is the copy
 * of the item the landing marks, NULL at the end of the directory.
 */
static BlockNumber
lion_range_side_leaf(Relation index, LionRange *range, bool lower, int kind,
					 LionEntryTuple **posp)
{
	BlockNumber best = InvalidBlockNumber;
	LionEntryTuple *bestpos = NULL;
	int			i;

	/*
	 * The column state is the index's relcache entry's, and a relcache
	 * invalidation since the range was built - any catalog read may process
	 * one - frees the LionIndexState it points back to, which the descent
	 * reads the cached root from.  The column states themselves live on in
	 * rd_indexcxt, so the stale one still says which column it is; look the
	 * current one up by that and re-point the range (the bounds' comparison
	 * functions are copies and need nothing).
	 */
	range->state = lion_index_column_state(index, range->state->attno);

	for (i = 0; i < range->nbounds; i++)
	{
		LionEntryTuple *pos;
		BlockNumber blk;
		bool		better;

		if (range->bounds[i].strategy == LION_STRAT_NE ||
			LION_STRAT_IS_LOWER(range->bounds[i].strategy) != lower)
			continue;			/* a hole is no end of either side */

		blk = lion_range_landing(index, range, i, kind, &pos);
		if (!BlockNumberIsValid(best))
			better = true;
		else if (pos == NULL || bestpos == NULL)
			better = lower ? (pos == NULL && bestpos != NULL) :
				(bestpos == NULL && pos != NULL);
		else
		{
			int			c = lion_cmp_entries(range->state->ix, pos, bestpos);

			better = lower ? (c > 0) : (c < 0);
		}

		if (better)
		{
			if (bestpos != NULL)
				pfree(bestpos);
			best = blk;
			bestpos = pos;
		}
		else if (pos != NULL)
			pfree(pos);
	}

	if (posp != NULL)
		*posp = bestpos;
	else if (bestpos != NULL)
		pfree(bestpos);
	return best;
}

/*
 * The directory leaf a walk of the range starts on: where the first entry at
 * or above its tightest lower bound lives, or - without a lower bound, or
 * without an order to descend by - where the column's entries begin.
 */
BlockNumber
lion_range_first_leaf(Relation index, LionRange *range)
{
	BlockNumber blk = InvalidBlockNumber;

	if (range->ordered)
		blk = lion_range_side_leaf(index, range, true, LION_KIND_VALUE, NULL);
	if (!BlockNumberIsValid(blk))
	{
		range->state = lion_index_column_state(index, range->state->attno);
		blk = lion_dir_column_first(index, range->state, NULL);
	}
	return blk;
}

/*
 * The directory leaf a DESCENDING walk of the range starts on (DESIGN.md
 * §30.11): where its tightest upper bound lands - the entry there is the
 * first that is not below the bound, so every entry the range selects is on
 * that leaf or on one to its left - or, without an upper bound or an order to
 * descend by, the leaf where the column's VALUE entries end.  A walk leftwards
 * from there passes over the entries of that leaf that lie above the range.
 */
BlockNumber
lion_range_last_leaf(Relation index, LionRange *range)
{
	BlockNumber blk = InvalidBlockNumber;

	if (range->ordered)
		blk = lion_range_side_leaf(index, range, false, LION_KIND_VALUE, NULL);
	if (!BlockNumberIsValid(blk))
	{
		Buffer		buf;
		OffsetNumber off;

		range->state = lion_index_column_state(index, range->state->attno);
		buf = lion_dir_value_end(index, range->state, &off);
		blk = BufferGetBlockNumber(buf);
		UnlockReleaseBuffer(buf);
	}
	return blk;
}

/*
 * Where a column's summaries begin (DESIGN.md §32): the leaf a descent to
 * (attno, SUMMARY) with no key lands on.  A search key of kind SUMMARY and no
 * comparison compares only on its hash, 0, and without a stored form it is the
 * smallest member of its run, so it sorts below every summary of the column
 * and above every value.
 */
static BlockNumber
lion_summary_first_leaf(Relation index, LionState *col)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	BlockNumber blk;

	lion_search_key_init(col, &sk, LION_KIND_SUMMARY, (Datum) 0, 0);
	sk.cmpproc = NULL;

	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * How many summaries a column has and the rows they hold, read off the
 * entries' counters from where the column's summaries begin - at most
 * LION_SUMMARY_SHAPE_LEAVES leaves of them, the first buckets, which is where
 * keys that arrive in descending order go (DESIGN.md §32, "Costs").  The cost
 * of a summed range needs it: buckets close at summary_tids rows only when
 * keys arrive in order, and a column whose keys arrive in descending order -
 * or in none - puts its rows into a few buckets far larger than that, whose
 * keys a range walks one by one.  complete says every summary of the column
 * was read.
 */
void
lion_summary_shape(Relation index, LionState *col, LionSumShape *shape)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	int			nleaves = 1;
	LionRightWalk walk;

	memset(shape, 0, sizeof(LionSumShape));
	lion_rightwalk_init(&walk);

	/* where the column's summaries begin, as lion_summary_first_leaf() */
	lion_search_key_init(col, &sk, LION_KIND_SUMMARY, (Datum) 0, 0);
	sk.cmpproc = NULL;
	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

		for (; off <= maxoff; off++)
		{
			LionEntryTuple *e = lion_page_entry(page, off);

			if (e->attno != col->attno || !LionEntryIsSummary(e))
			{
				shape->complete = true;
				break;
			}
			shape->nsummaries += 1.0;
			shape->rows += (double) e->ntids;
		}
		if (shape->complete || LionPageIsRightmost(page))
		{
			shape->complete = true;
			break;
		}
		if (nleaves >= LION_SUMMARY_SHAPE_LEAVES)
			break;
		buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE, &walk);
		off = lion_page_first_data(BufferGetPage(buf));
		nleaves++;
	}
	UnlockReleaseBuffer(buf);
}

/* The leaf a descent to (attno, VALUE, key) lands on, key a stored one. */
static BlockNumber
lion_value_leaf(Relation index, LionState *col, const char *raw)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	BlockNumber blk;

	lion_search_key_init(col, &sk, LION_KIND_VALUE, lion_fetch_key(col, raw),
						 0);
	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}


/* ---------------------------------------------------------------------
 * Iterating every entry of an index
 * --------------------------------------------------------------------- */

/*
 * The most entries one leaf can hold: every item at least an entry header and
 * a line pointer.  A batch is at most one leaf's worth (DESIGN.md §28).
 */
#define LION_LEAF_MAX_ENTRIES \
	((int) (BLCKSZ / (MAXALIGN(LION_ENTRY_HDRSZ) + sizeof(ItemIdData))) + 1)

/* Everything but where the walk starts. */
static void
lion_entry_scan_init(LionEntryScan *es, Relation index, AttrNumber attno)
{
	es->index = index;
	es->state = lion_index_column_state(index, attno);
	es->attno = es->state->attno;
	es->cxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion entry scan position",
									ALLOCSET_SMALL_SIZES);

	/*
	 * The walk starts at (attno, MINF), a position below every entry of this
	 * column and above every entry of the columns before it, and ends at the
	 * first entry whose attno is not this one (DESIGN.md §24).  MINF is not a
	 * kind any stored entry has, so "resume after the last key examined" -
	 * which is what every leaf read does - starts at the column's first entry
	 * without a special case.
	 */
	es->lastkind = LION_KIND_MINF;
	es->lasthash = 0;
	es->lastkeylen = 0;
	es->lastkey = (char *) MemoryContextAllocZero(es->cxt, 1);
	es->haslast = true;
	es->blkno = InvalidBlockNumber;
	es->stepfrom = InvalidBlockNumber;
	lion_rightwalk_init(&es->walk);
	es->range = NULL;
	es->part = LION_WALK_ALL;
	es->done = false;

	/* No summaries unless lion_entry_scan_begin_sum() plans them (§32). */
	es->usesum = false;
	es->phase = LION_PHASE_VALUES;
	es->nextphase = LION_PHASE_VALUES;
	es->resumekind = -1;
	es->hasclipmax = false;
	es->hasclipmin = false;
	es->hassumprev = false;
	es->clipmax = es->clipmin = es->sumprev = NULL;
	es->clipmaxlen = es->clipminlen = es->sumprevlen = 0;
	es->sumprevhash = 0;
	es->phasecxt = NULL;
	es->nsummaries = 0;

	/*
	 * The batch lives beside the position and is allocated once: one leaf's
	 * entries fit in a block's worth of bytes, because that is where they
	 * were copied from.
	 */
	es->batchcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "lion entry scan batch",
										 ALLOCSET_DEFAULT_SIZES);
	es->maxbatch = LION_LEAF_MAX_ENTRIES;
	es->bentry = (LionEntryTuple **)
		MemoryContextAlloc(es->batchcxt, sizeof(LionEntryTuple *) * es->maxbatch);
	es->bsize = (Size *)
		MemoryContextAlloc(es->batchcxt, sizeof(Size) * es->maxbatch);
	es->boff = (OffsetNumber *)
		MemoryContextAlloc(es->batchcxt, sizeof(OffsetNumber) * es->maxbatch);
	es->bpage = (char *) MemoryContextAlloc(es->batchcxt, BLCKSZ);
	es->nbatch = 0;
	es->nextbatch = 0;
	es->lastinline = -1;
	es->batchblk = InvalidBlockNumber;
	es->batchbuf = InvalidBuffer;
	es->nleaves = 0;
}

void
lion_entry_scan_begin_col(LionEntryScan *es, Relation index, AttrNumber attno)
{
	lion_entry_scan_init(es, index, attno);
	es->blkno = lion_dir_column_first(index, es->state, NULL);
}

/*
 * The walk of one column bounded by a range (DESIGN.md §28).  Only where it
 * STARTS differs from lion_entry_scan_begin_col(): the leaf the range's lower
 * bound lives on instead of the column's first.  The resume position is still
 * the column's (attno, MINF), so the entries of an earlier column that share
 * that leaf are passed by the ordinary resume comparison, and the entries of
 * this column below the bound by lion_range_test().
 */
void
lion_entry_scan_begin_range(LionEntryScan *es, Relation index,
							AttrNumber attno, LionRange *range)
{
	if (range == NULL)
		lion_entry_scan_begin_col(es, index, attno);
	else
		lion_entry_scan_begin_part(es, index, attno, range, LION_WALK_INSIDE);
}

/*
 * ... and of the parts of it the range does NOT select (DESIGN.md §28, "The
 * complement").  BELOW starts where the column does and stops at the first
 * entry the range selects or that fails an upper bound.  ABOVE starts where
 * the first entry that fails an upper bound lives - the landing of the
 * tightest upper bound (lion_range_side_leaf()) - and returns every entry that
 * fails one until the column ends.  INSIDE starts at the landing of the
 * tightest lower bound.
 */
void
lion_entry_scan_begin_part(LionEntryScan *es, Relation index,
						   AttrNumber attno, LionRange *range, int part)
{
	lion_entry_scan_init(es, index, attno);

	/* The scan's state is the current one (see lion_range_side_leaf()). */
	range->state = es->state;
	es->range = range;
	es->part = part;
	if (range->empty)
	{
		es->done = true;
		return;
	}

	switch (part)
	{
		case LION_WALK_INSIDE:
			es->blkno = lion_range_first_leaf(index, range);
			break;
		case LION_WALK_BELOW:
			Assert(range->ordered);
			es->blkno = lion_dir_column_first(index, es->state, NULL);
			break;
		case LION_WALK_ABOVE:
			Assert(range->ordered);
			if (range->nupper == 0)
			{
				es->done = true;
				return;
			}
			es->blkno = lion_range_side_leaf(index, range, false,
											 LION_KIND_VALUE, NULL);
			break;
		default:
			elog(ERROR, "lion index: unknown part %d of a range walk", part);
	}
	range->state = es->state = lion_index_column_state(index, attno);
}

/*
 * Keep a copy of an entry's stored key in *bufp, a buffer of the walk's phase
 * context that holds any key (LION_MAX_KEY_SIZE): a SUMS phase copies the key
 * of every bucket it takes into the same one.
 */
static void
lion_scan_keycopy(LionEntryScan *es, const LionEntryTuple *entry, char **bufp,
				  Size *lenp)
{
	if (unlikely(entry->keylen > LION_MAX_KEY_SIZE))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": a summary key of %u bytes",
						RelationGetRelationName(es->index), entry->keylen)));
	/* the copy goes to the opclass functions as it is (lion_check_key()) */
	lion_check_key(es->state, LionEntryGetKey(entry), entry->keylen);
	if (*bufp == NULL)
		*bufp = (char *) MemoryContextAlloc(es->phasecxt, LION_MAX_KEY_SIZE);
	memcpy(*bufp, LionEntryGetKey(entry), entry->keylen);
	*lenp = entry->keylen;
}

/*
 * Compare the key of an entry of the walk's column with a stored one the walk
 * kept (lion_scan_keycopy()), under the column's own comparison: how a key is
 * put on one side or the other of a bucket boundary (DESIGN.md §32).
 */
static int
lion_scan_keycmp(LionEntryScan *es, const LionEntryTuple *a, const char *b)
{
	LionState  *st = es->state;

	return DatumGetInt32(FunctionCall2Coll(&st->cmpproc, st->collation,
										   lion_entry_key(st, a),
										   lion_fetch_key(st, b)));
}

/*
 * Is the bucket whose summary this is - the keys above the previous summary's
 * key and at or below this one's - wholly in the walk's part?  The SUMS phase
 * starts at a bucket whose lower boundary is already on the right side of the
 * part's lower end (lion_entry_scan_begin_sum()), and every later one's is too,
 * so what is left to ask is whether the bucket's UPPER boundary - its key, the
 * largest key it may hold - is on the right side of the part's upper end:
 *
 *	INSIDE	it passes every upper bound;
 *	BELOW	it fails a lower bound and passes every upper bound;
 *	ABOVE, ALL	there is no upper end.
 *
 * The last summary of a column keeps the largest key its bucket holds, which
 * is the same question asked of its keys so far; a key an insert adds above
 * it later belongs to a row this walk's snapshot cannot see.
 */
static bool
lion_scan_bucket_inside(LionEntryScan *es, const LionEntryTuple *entry)
{
	LionRange  *range = es->range;
	bool		failslower = false;
	Datum		key;
	int			i;

	if (es->part == LION_WALK_ALL || es->part == LION_WALK_ABOVE)
		return true;

	key = lion_entry_key(es->state, entry);
	for (i = 0; i < range->nbounds; i++)
	{
		LionRangeBound *b = &range->bounds[i];
		bool		ok = lion_range_bound_ok(range, b, key);

		if (LION_STRAT_IS_LOWER(b->strategy))
		{
			if (!ok)
				failslower = true;
		}
		else if (!ok)
			return false;		/* an upper bound, or a hole (never here) */
	}
	return (es->part == LION_WALK_INSIDE) ? true : failslower;
}

/*
 * Plan a walk that uses the column's summaries (DESIGN.md §32): find the first
 * bucket the part covers whole, and set the walk up for the phase it starts
 * in.  The buckets of a column are (previous summary's key, this summary's
 * key], in key order, and a bucket is wholly in the part when both of its
 * boundaries are on the right side of the part's ends:
 *
 *	- the lower one.  INSIDE and ABOVE have a lower end: the first summary at
 *	  or above the TIGHTEST bound that makes it - the range's tightest lower
 *	  bound, or the tightest upper bound for ABOVE, whose rows are the ones
 *	  that fail one - is E_j, and every bucket after it has only keys above
 *	  E_j's key, which is at or above that bound and every other one of that
 *	  side.  So the first whole bucket is E_j's successor, and the part's
 *	  VALUE entries at or below E_j's key are walked first, one by one (LOWER).
 *	  BELOW and ALL start at the column's first bucket.
 *	- the upper one, the summary's own key: lion_scan_bucket_inside(), asked
 *	  of each summary in turn (SUMS).  The first that fails ends the phase, and
 *	  the part's VALUE entries above the last whole bucket are walked one by one
 *	  (UPPER).
 *
 * When E_j is the column's last summary, or not a summary of this column at
 * all, or its key is already above the part's upper end, no bucket is whole
 * and the walk is the plain one.  Returns whether summaries are used.
 */
static bool
lion_entry_scan_plan_sum(LionEntryScan *es, Relation index, LionRange *range,
						 int part)
{
	LionState  *col = es->state;
	LionEntryTuple *ej = NULL;
	BlockNumber jblk = InvalidBlockNumber;
	bool		haslowerend;

	if (!col->summarized)
		return false;
	if (range != NULL && (!range->ordered || range->empty))
		return false;
	/* a bucket may hold a hole's rows (DESIGN.md §35): key by key */
	if (range != NULL && range->nholes > 0)
		return false;
	if (part == LION_WALK_ABOVE && range->nupper == 0)
		return false;

	haslowerend = (part == LION_WALK_ABOVE) ||
		(part == LION_WALK_INSIDE && range->lower >= 0);

	if (haslowerend)
	{
		jblk = lion_range_side_leaf(index, range, part != LION_WALK_ABOVE,
									LION_KIND_SUMMARY, &ej);
		if (!BlockNumberIsValid(jblk) || ej == NULL ||
			LionEntryIsPivot(ej) || ej->attno != col->attno ||
			lion_entry_kind(ej) != LION_KIND_SUMMARY)
			return false;

		/*
		 * A range whose upper end is already below E_j's key ends inside E_j's
		 * bucket: there is nothing whole to read.
		 */
		if (part == LION_WALK_INSIDE && !lion_scan_bucket_inside(es, ej))
			return false;
	}

	es->usesum = true;

	/*
	 * Not a child of cxt: that one is reset at every leaf read, and a reset
	 * deletes a context's children.
	 */
	es->phasecxt = AllocSetContextCreate(CurrentMemoryContext,
										 "lion entry scan phases",
										 ALLOCSET_SMALL_SIZES);
	if (haslowerend)
	{
		/* LOWER: the part's values up to E_j's key, from the part's start. */
		es->phase = LION_PHASE_LOWER;
		es->hasclipmax = true;
		lion_scan_keycopy(es, ej, &es->clipmax, &es->clipmaxlen);
		es->hassumprev = true;
		lion_scan_keycopy(es, ej, &es->sumprev, &es->sumprevlen);
		es->sumprevhash = ej->hash;
		es->blkno = (part == LION_WALK_INSIDE) ?
			lion_range_first_leaf(index, range) :
			lion_range_side_leaf(index, range, false, LION_KIND_VALUE, NULL);
	}
	else
	{
		/* SUMS from the column's first summary. */
		es->phase = LION_PHASE_SUMS;
		es->resumekind = LION_KIND_VALUE;
		es->blkno = lion_summary_first_leaf(index, col);
	}
	es->nextphase = es->phase;
	if (ej != NULL)
		pfree(ej);
	es->state = lion_index_column_state(index, es->attno);
	if (range != NULL)
		range->state = es->state;
	return true;
}

bool
lion_entry_scan_begin_summed(LionEntryScan *es, Relation index,
							 AttrNumber attno, LionRange *range, int part)
{
	lion_entry_scan_init(es, index, attno);
	es->range = range;
	es->part = (range == NULL) ? LION_WALK_ALL : part;
	if (range != NULL)
		range->state = es->state;

	if ((range == NULL || !range->empty) &&
		lion_entry_scan_plan_sum(es, index, range, es->part))
		return true;

	lion_entry_scan_end(es);
	return false;
}

void
lion_entry_scan_begin_sum(LionEntryScan *es, Relation index, AttrNumber attno,
						  LionRange *range, int part)
{
	if (range != NULL && range->empty)
	{
		lion_entry_scan_init(es, index, attno);
		es->range = range;
		es->part = part;
		range->state = es->state;
		es->done = true;
		return;
	}

	if (lion_entry_scan_begin_summed(es, index, attno, range, part))
		return;

	/* No summaries to use: the plain walk. */
	if (range == NULL)
		lion_entry_scan_begin_col(es, index, attno);
	else
		lion_entry_scan_begin_part(es, index, attno, range, part);
}

/*
 * The phase that just ended hands over to the next one, which the next leaf
 * read sets up (lion_entry_scan_setup_phase()) - with nothing pinned, which a
 * descent in the middle of a leaf read could not promise.
 */
static void
lion_entry_scan_end_phase(LionEntryScan *es, bool partended)
{
	switch (es->phase)
	{
		case LION_PHASE_LOWER:
			/* A range that ended at or below E_j's key has nothing after it. */
			es->nextphase = partended ? LION_PHASE_DONE : LION_PHASE_SUMS;
			break;
		case LION_PHASE_SUMS:
			/* The column's summaries ran out: every bucket was whole. */
			es->nextphase = partended ? LION_PHASE_DONE : LION_PHASE_UPPER;
			break;
		default:
			es->nextphase = LION_PHASE_DONE;
			break;
	}
	if (es->nextphase == LION_PHASE_DONE)
		es->done = true;
	es->blkno = InvalidBlockNumber;
	es->stepfrom = InvalidBlockNumber;
}

static void
lion_entry_scan_setup_phase(LionEntryScan *es)
{
	/*
	 * Test hook: one phase of a summed walk is over and the next is about to
	 * descend to where it begins (DESIGN.md §32), holding nothing of the
	 * directory.  test/isolation/summary_race.spec parks here while inserts
	 * close and open buckets and split the leaves the next phase reads.
	 * Compiles to nothing without --enable-injection-points.
	 */
	LION_INJECTION_POINT("lion-entry-scan-phase");

	/* The phase starts where a descent lands: a walk right of its own. */
	es->stepfrom = InvalidBlockNumber;
	lion_rightwalk_init(&es->walk);

	switch (es->nextphase)
	{
		case LION_PHASE_SUMS:

			/*
			 * After E_j, whose leaf a descent to its key finds again: the
			 * summaries resume after (SUMMARY, E_j's key).
			 */
			Assert(es->hassumprev);
			MemoryContextReset(es->cxt);
			es->hasclipmax = false;
			es->lastkind = LION_KIND_SUMMARY;
			es->lasthash = es->sumprevhash;
			es->lastkeylen = es->sumprevlen;
			es->lastkey = (char *) MemoryContextAlloc(es->cxt,
													  Max(es->sumprevlen, 1));
			memcpy(es->lastkey, es->sumprev, es->sumprevlen);
			es->haslast = true;
			es->resumekind = -1;
			{
				LionSearchKey sk;
				Buffer		buf;
				OffsetNumber off;

				es->state = lion_index_column_state(es->index, es->attno);
				lion_search_key_init(es->state, &sk, LION_KIND_SUMMARY,
									 lion_fetch_key(es->state, es->sumprev), 0);
				buf = lion_dir_search(es->index, NULL, es->state->ix, &sk,
									  BUFFER_LOCK_SHARE, false, &off);
				es->blkno = BufferGetBlockNumber(buf);
				UnlockReleaseBuffer(buf);
			}
			break;

		case LION_PHASE_UPPER:

			/*
			 * The part's values above the last whole bucket: from where a
			 * descent to its key lands, every key at or below it passed over
			 * by the boundary test, which is what decides - the resume
			 * position only saves reading the entries before it.
			 */
			es->state = lion_index_column_state(es->index, es->attno);
			MemoryContextReset(es->cxt);
			es->lastkind = LION_KIND_MINF;
			es->lasthash = 0;
			es->lastkeylen = 0;
			es->lastkey = (char *) MemoryContextAllocZero(es->cxt, 1);
			es->haslast = true;
			es->resumekind = -1;
			if (es->hassumprev)
			{
				es->hasclipmin = true;
				es->clipmin = (char *) MemoryContextAlloc(es->phasecxt,
														  Max(es->sumprevlen, 1));
				memcpy(es->clipmin, es->sumprev, es->sumprevlen);
				es->clipminlen = es->sumprevlen;
				es->blkno = lion_value_leaf(es->index, es->state, es->sumprev);
			}
			else
			{
				/*
				 * The very first bucket was not whole: the part is walked from
				 * its own start, as if there were no summaries.
				 */
				es->hasclipmin = false;
				es->blkno = (es->part == LION_WALK_INSIDE) ?
					lion_range_first_leaf(es->index, es->range) :
					lion_dir_column_first(es->index, es->state, NULL);
			}
			break;

		default:
			Assert(false);
	}
	if (es->range != NULL)
		es->range->state = es->state;
	es->phase = es->nextphase;
}

/* Remember where to resume, as a KEY (see the comment on LionEntryScan). */
static void
lion_entry_scan_remember(LionEntryScan *es, const LionEntryTuple *entry)
{
	MemoryContextReset(es->cxt);
	es->lastkind = lion_entry_kind(entry);
	es->lasthash = entry->hash;
	es->lastkeylen = entry->keylen;
	es->resumekind = -1;

	/*
	 * Always a real pointer, even for the key-less reserved entries: a search
	 * key whose `raw` is NULL compares as the SMALLEST member of its own run
	 * (lion_cmp_entry()), and "resume after the last key" would then resume AT
	 * it and hand the same entry out for ever.
	 */
	es->lastkey = (char *) MemoryContextAlloc(es->cxt,
											  Max((Size) entry->keylen, 1));

	if (entry->keylen > 0)
	{
		/* the next read hands the copy to the opclass functions (§21) */
		lion_check_key(es->state, LionEntryGetKey(entry), entry->keylen);
		memcpy(es->lastkey, LionEntryGetKey(entry), entry->keylen);
	}
	es->haslast = true;
}

/*
 * Does the walk return this entry of its column?  *stop is set when no later
 * entry of the phase can be returned either: the first entry past an upper
 * bound ends a walk of what a range selects (DESIGN.md §28), and the first
 * entry the range selects - or that fails an upper bound - ends the walk below
 * it.  *partended says the PART ended there, not only the phase.
 */
static bool
lion_entry_scan_selects(LionEntryScan *es, const LionEntryTuple *entry,
						bool *stop, bool *partended)
{
	int			kind = lion_entry_kind(entry);
	int			r;

	*partended = false;

	/* The summaries of whole buckets (DESIGN.md §32). */
	if (es->phase == LION_PHASE_SUMS)
	{
		if (kind < LION_KIND_SUMMARY)
			return false;		/* the column's values, on the landing leaf */
		if (!lion_scan_bucket_inside(es, entry))
		{
			*stop = true;
			return false;
		}
		lion_scan_keycopy(es, entry, &es->sumprev, &es->sumprevlen);
		es->sumprevhash = entry->hash;
		es->hassumprev = true;
		return true;
	}

	/*
	 * A walk of values ends where the column's summaries begin: they sort
	 * after its last value (§32).
	 */
	if (kind >= LION_KIND_SUMMARY)
	{
		*stop = true;
		*partended = true;
		return false;
	}

	if (es->usesum)
	{
		/* The NULL entry is in no bucket, and a sum of values never wants it. */
		if (kind != LION_KIND_VALUE)
			return false;
		if (es->hasclipmin &&
			lion_scan_keycmp(es, entry, es->clipmin) <= 0)
			return false;
		if (es->hasclipmax &&
			lion_scan_keycmp(es, entry, es->clipmax) > 0)
		{
			*stop = true;
			return false;
		}
	}

	switch (es->part)
	{
		case LION_WALK_ALL:
			return true;
		case LION_WALK_INSIDE:
			r = lion_range_test(es->range, entry);
			if (r == LION_RANGE_END)
				*stop = *partended = true;
			return r == LION_RANGE_MATCH;
		case LION_WALK_BELOW:
			if (kind != LION_KIND_VALUE)
				return false;
			r = lion_range_test(es->range, entry);
			if (r != LION_RANGE_SKIP)
				*stop = *partended = true;
			return r == LION_RANGE_SKIP;
		case LION_WALK_ABOVE:
			if (kind != LION_KIND_VALUE)
				return false;
			return lion_range_fails_upper(es->range, entry);
	}
	return false;
}

/*
 * Read the leaf the walk stands at, ONCE, and take what the walk selects from
 * it: copies of the entries into the batch (copy), or only how many there are.
 * Returns how many were taken.
 *
 * Everything is decided under one share lock: where to resume on this leaf
 * (the first key above the last one examined, however the leaf has changed
 * since the walk left it), which entries the walk returns, where the next
 * read resumes (after the last entry examined here, which may be one the walk
 * passed over) and which leaf it reads next - the right link as it stands,
 * unless the walk ended here.  The lock is then given up; the pin is kept
 * while the batch holds an INLINE copy (see lion_entry_scan_next()).
 *
 * A walk that uses summaries (DESIGN.md §32) moves from one phase to the next
 * here, and sets the next phase up at the start of the following read.
 */
static int64
lion_entry_scan_fill(LionEntryScan *es, bool copy)
{
	LionState  *state;
	Buffer		buf;
	Page		page;
	OffsetNumber off;
	OffsetNumber maxoff;
	LionEntryTuple *last = NULL;
	char	   *dst = es->bpage;
	int64		ntaken = 0;
	bool		stop = false;
	bool		partended = false;

	Assert(es->nextbatch >= es->nbatch);
	Assert(!BufferIsValid(es->batchbuf));
	es->nbatch = 0;
	es->nextbatch = 0;
	es->lastinline = -1;

	if (es->nextphase != es->phase)
		lion_entry_scan_setup_phase(es);
	state = es->state;

	/*
	 * Test hook: the walk is between two leaves and holds nothing of the
	 * directory at all - no lock, and no pin of its own - so a concurrent
	 * insert may split the leaf it has just left and the one it is about to
	 * read.  It fires before every leaf but the first, in a counting walk
	 * (lion_entry_scan_skip_leaf()) as in a fetching one;
	 * test/isolation/count_range_split_race.spec parks the race of
	 * lion_range_choose() here.  Compiles to nothing without
	 * --enable-injection-points.
	 */
	if (es->nleaves > 0)
		LION_INJECTION_POINT("lion-entry-scan-leaf");

	/*
	 * A leaf reached through a right link is a step of the walk, which ends a
	 * cycle of damaged links in an ERROR (lion.h); nothing is held here, so a
	 * cancel is answered too.
	 */
	if (BlockNumberIsValid(es->stepfrom))
	{
		lion_rightwalk_step(es->index, &es->walk, es->stepfrom, es->blkno);
		es->stepfrom = InvalidBlockNumber;
	}
	CHECK_FOR_INTERRUPTS();

	buf = ReadBuffer(es->index, es->blkno);
	lion_dir_pages_read++;
	es->nleaves++;
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!LionPageIsLeaf(page))
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index: block %u is not a directory leaf",
			 es->blkno);
	}

	if (es->resumekind >= 0)
	{
		/*
		 * After every entry of the column up to a kind (§32: where a column's
		 * summaries begin), which a key cannot say.  The landing leaf holds at
		 * most the tail of the run before it, passed over here.
		 */
		off = lion_page_first_data(page);
	}
	else if (es->haslast)
	{
		LionSearchKey sk;

		sk.attno = es->attno;
		sk.col = state;
		sk.kind = es->lastkind;
		sk.key = LION_KIND_HAS_KEY(es->lastkind) ?
			lion_fetch_key(state, es->lastkey) : (Datum) 0;
		sk.hash = es->lasthash;
		sk.cmpproc = state->ordered ? &state->cmpproc : NULL;
		sk.eqproc = &state->eqproc;
		sk.collation = state->collation;
		sk.raw = es->lastkey;
		sk.rawlen = es->lastkeylen;

		/*
		 * Everything on this page may already be behind us, which is what a
		 * split of the page we were on looks like from here.
		 */
		if (!LionPageIsRightmost(page) &&
			lion_cmp_entry(lion_dir_highkey(page), &sk) <= 0)
		{
			es->stepfrom = es->blkno;
			es->blkno = LionPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buf);
			return 0;
		}

		off = lion_dir_binsrch(page, &sk);
		while (off <= PageGetMaxOffsetNumber(page) &&
			   lion_cmp_entry(lion_page_entry(page, off), &sk) <= 0)
			off = OffsetNumberNext(off);
	}
	else
		off = lion_page_first_data(page);

	maxoff = PageGetMaxOffsetNumber(page);

	for (; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		LionEntryTuple *entry;

		if (!ItemIdIsUsed(iid))
			continue;
		entry = (LionEntryTuple *) PageGetItem(page, iid);

		if (es->resumekind >= 0)
		{
			if (entry->attno < es->attno ||
				(entry->attno == es->attno &&
				 lion_entry_kind(entry) <= es->resumekind))
				continue;
		}

		/*
		 * The walk is bounded to one key column (DESIGN.md §24): the entries
		 * are sorted by attno, so the first entry of the next column ends it.
		 */
		if (entry->attno != es->attno)
		{
			stop = true;
			partended = true;
			break;
		}

		/*
		 * A bounded walk (DESIGN.md §28) returns only the entries of its part
		 * of the range, and may end here.
		 */
		if (!lion_entry_scan_selects(es, entry, &stop, &partended))
		{
			if (stop)
				break;
			last = entry;
			continue;
		}
		last = entry;

		/*
		 * An entry whose posting set is empty can never produce a group.
		 * VACUUM deletes those (DESIGN.md §18), but one can be seen here
		 * between the moment its last TID was filtered out and the moment the
		 * leaf's final step removes it.
		 */
		if (entry->ntids == 0)
			continue;

		ntaken++;
		if (copy)
		{
			Size		sz = ItemIdGetLength(iid);
			int			n = es->nbatch;

			if (unlikely(n >= es->maxbatch ||
						 dst + sz > es->bpage + BLCKSZ))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": directory leaf %u holds more entries than fit on a page",
								RelationGetRelationName(es->index),
								BufferGetBlockNumber(buf))));
			memcpy(dst, entry, sz);
			es->bentry[n] = (LionEntryTuple *) dst;
			es->bsize[n] = sz;
			es->boff[n] = off;
			if ((entry->flags & LION_ENTRY_INLINE) != 0)
				es->lastinline = n;
			es->nbatch = n + 1;
			dst += MAXALIGN(sz);
			if (LionEntryIsSummary(entry))
				es->nsummaries++;
		}
	}

	/* The next read of any leaf resumes after the last entry examined here. */
	if (last != NULL)
		lion_entry_scan_remember(es, last);

	es->batchblk = BufferGetBlockNumber(buf);
	if (stop)
	{
		if (es->usesum)
			lion_entry_scan_end_phase(es, partended);
		else
			es->done = true;
	}
	else
	{
		/*
		 * Everything this leaf holds after the resume position has been
		 * examined, so the walk goes on at the right sibling this leaf had
		 * when it was read, without reading this leaf again.  What a split of
		 * it moves to a new page in between after that are entries the batch
		 * has already, or entries inserted since, which hold no row this
		 * walk's snapshot can see (DESIGN.md §28, "One read per leaf").
		 */
		es->stepfrom = es->blkno;
		es->blkno = LionPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(es->blkno))
		{
			if (es->usesum)
				lion_entry_scan_end_phase(es, true);
			else
				es->done = true;
		}
	}

	/*
	 * DESIGN.md section 9: an INLINE copy is counted against the visibility
	 * map, so the leaf it was copied from stays pinned until that set has been
	 * handed out.  A batch without one needs no pin at all.
	 */
	if (es->lastinline >= 0)
	{
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		es->batchbuf = buf;
	}
	else
		UnlockReleaseBuffer(buf);

	return ntaken;
}

/*
 * Fetch the next entry of the walk, in directory order.
 *
 * The entries come out of the batch the last leaf read copied (DESIGN.md §28,
 * "One read per leaf"), so a leaf is read once however many of its entries
 * the walk returns - it used to be read again for every entry, to find "the
 * first key above the last one returned" on it, which made a range over n
 * keys read n leaves and more.  Handing out a copy after the lock is gone is
 * safe for the reasons a resumed scan always was:
 *
 *	- a split moves entries only rightwards, onto a page the walk has not
 *	  passed; the ones that were on this leaf when it was read are in the
 *	  batch, and the walk reads next the right sibling the leaf had then, so
 *	  nothing is skipped or returned twice;
 *	- a deleted entry had an empty posting set, which nothing this walk's
 *	  snapshot can see was in (§18), and a CHAIN copy whose set was freed
 *	  meanwhile reads as empty (the owner check of §18);
 *	- an entry inserted after the leaf was read holds only rows no older
 *	  snapshot can see (§29.5 case 3), so missing it is right - the same
 *	  freedom the walk has always had behind its position;
 *	- and an INLINE payload is counted under the pin of the leaf it was copied
 *	  from, which the scan holds from the copy until the set is handed out and
 *	  the set holds after that: §9's rule, only with the pin taken once for the
 *	  leaf instead of once per entry.
 */
bool
lion_entry_scan_next(LionEntryScan *es, Datum *key, LionPostingSet *ps)
{
	int			i;
	bool		keeppin;

	while (es->nextbatch >= es->nbatch)
	{
		/* a phase that has ended hands over at the next read (§32) */
		if (es->done ||
			(!BlockNumberIsValid(es->blkno) && es->nextphase == es->phase))
		{
			es->done = true;
			return false;
		}
		CHECK_FOR_INTERRUPTS();
		(void) lion_entry_scan_fill(es, true);
	}

	i = es->nextbatch++;

	/*
	 * Test hook: the walk is between two entries of one leaf and holds no lock
	 * on it at all (a pin only while an INLINE copy is still to come), so a
	 * concurrent VACUUM is free to delete entries and a concurrent insert to
	 * split the page.  It fires once per leaf, which is what lets an isolation
	 * test park a walk here exactly once; test/isolation/vacuum_entry_delete,
	 * dir_split_scan and count_range_split_race are such cases.  Compiles to
	 * nothing without --enable-injection-points.
	 */
	if (i == 1)
		LION_INJECTION_POINT("lion-entry-scan-resumed");

	lion_fill_posting_set_entry(es->index, es->state, es->bentry[i],
								es->bsize[i], es->batchblk, es->boff[i], ps,
								&keeppin);
	*key = ps->storedkey;
	if (keeppin)
	{
		/*
		 * DESIGN.md section 9: the INLINE payload needs a pin of its own on the
		 * leaf it was copied from.  The last INLINE copy of the batch takes the
		 * scan's pin over.
		 */
		Assert(BufferIsValid(es->batchbuf));
		if (i == es->lastinline)
		{
			ps->pinbuf = es->batchbuf;
			es->batchbuf = InvalidBuffer;
		}
		else
		{
			IncrBufferRefCount(es->batchbuf);
			ps->pinbuf = es->batchbuf;
		}
	}

	return true;
}

/*
 * The same walk for a caller that needs neither a located set nor a pin: a
 * scan that hands every TID it reads to the executor, which visits each one
 * in the heap (see lion_count.h).  The copies are the batch's own, so an
 * entry stays valid until the next call - which may read the next leaf into
 * the same buffer - and the pin the batch keeps for its INLINE copies, which
 * only a count needs (DESIGN.md §9), is let go of as soon as it is taken.
 */
LionEntryTuple *
lion_entry_scan_next_copy(LionEntryScan *es, Size *itemlen)
{
	int			i;

	while (es->nextbatch >= es->nbatch)
	{
		/* a phase that has ended hands over at the next read (§32) */
		if (es->done ||
			(!BlockNumberIsValid(es->blkno) && es->nextphase == es->phase))
		{
			es->done = true;
			return NULL;
		}
		CHECK_FOR_INTERRUPTS();
		(void) lion_entry_scan_fill(es, true);
	}

	if (BufferIsValid(es->batchbuf))
	{
		ReleaseBuffer(es->batchbuf);
		es->batchbuf = InvalidBuffer;
	}
	es->lastinline = -1;

	i = es->nextbatch++;
	*itemlen = es->bsize[i];
	return es->bentry[i];
}

int64
lion_entry_scan_skip_leaf(LionEntryScan *es)
{
	Assert(es->nextbatch >= es->nbatch);

	if (es->done ||
		(!BlockNumberIsValid(es->blkno) && es->nextphase == es->phase))
	{
		es->done = true;
		return 0;
	}
	return lion_entry_scan_fill(es, false);
}

/*
 * No pin across a row (see lion_count.h).  Called with at least one entry of
 * the batch handed out, which is where the leaf is read again from: the walk
 * resumes after it, so the INLINE copies dropped here come back in the next
 * batch - or, if VACUUM or a split has moved them meanwhile, from wherever
 * the key-based resume finds them.  A batch with nothing handed out yet (no
 * caller pauses there) keeps its pin rather than lose its position.
 */
void
lion_entry_scan_pause(LionEntryScan *es)
{
	if (!BufferIsValid(es->batchbuf) || es->nextbatch == 0)
		return;

	ReleaseBuffer(es->batchbuf);
	es->batchbuf = InvalidBuffer;
	lion_entry_scan_remember(es, es->bentry[es->nextbatch - 1]);
	es->nbatch = es->nextbatch;
	es->lastinline = -1;
	es->blkno = es->batchblk;
	es->stepfrom = InvalidBlockNumber;	/* read again, not a step */
	es->done = false;
}

void
lion_entry_scan_end(LionEntryScan *es)
{
	es->done = true;
	if (BufferIsValid(es->batchbuf))
	{
		ReleaseBuffer(es->batchbuf);
		es->batchbuf = InvalidBuffer;
	}
	es->nbatch = 0;
	es->nextbatch = 0;
	if (es->cxt != NULL)
	{
		MemoryContextDelete(es->cxt);
		es->cxt = NULL;
	}
	if (es->batchcxt != NULL)
	{
		MemoryContextDelete(es->batchcxt);
		es->batchcxt = NULL;
	}
	if (es->phasecxt != NULL)
	{
		MemoryContextDelete(es->phasecxt);
		es->phasecxt = NULL;
	}
}
