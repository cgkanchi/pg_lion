/*-------------------------------------------------------------------------
 *
 * lion_exec_count.c
 *		Counting: single counts, summed ranges, GROUP BY walks and
 *		count(DISTINCT).
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

static int64 lion_walk_count(LionCountScanState *st, LionCountSource *sources,
							 int nsource, int w, bool exists);

/*
 * Is any WHERE item of the relation being counted a source that SELECTS
 * rows?  Always, except when a multi-key query no key narrows was the only
 * one (DESIGN.md §17, "A query known only at run time"): it is no source then
 * (lion_locate_where()), and `IS NOT NULL` only subtracts.
 */
static bool
lion_where_has_positive(LionCountScanState *st)
{
	int			k;

	for (k = 0; k < st->nitem; k++)
	{
		if (!st->sources[k + 1].negated)
			return true;
	}
	return false;
}

/*
 * A count with nothing to take its candidates from: every WHERE clause that
 * selects rows was a multi-key query no key narrows, so the candidates are
 * every row of the relation, and the heap is read once, sequentially, with
 * the row filter - those queries - and the `IS NOT NULL` clauses tested on
 * each row the snapshot sees.  It is the ordinary plan's work, which the
 * cost model charged for a value it could not estimate (lion_cost_recheck()).
 */
static int64
lion_count_scan_filtered(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	LionRowFilter scan;
	int			k;

	scan = *st->filter;
	scan.clauses = (LionRowFilterClause *)
		palloc0(sizeof(LionRowFilterClause) * (st->filter->nclauses + st->nitem));
	memcpy(scan.clauses, st->filter->clauses,
		   sizeof(LionRowFilterClause) * st->filter->nclauses);
	for (k = 0; k < st->nitem; k++)
	{
		LionClauseState *cl = &st->clause[st->item[k].clauseno];
		LionRowFilterClause *c;

		if (st->item[k].orno >= 0 || cl->kind != LION_CLAUSE_NOTNULL)
			continue;
		c = &scan.clauses[scan.nclauses++];
		c->attno = cl->idx->rd_index->indkey.values[cl->idxcol - 1];
		c->notnull = true;
	}

	return lion_count_heap_filtered(st->heap, estate->es_snapshot, &scan,
									&st->stats);
}

/*
 * The intersection of the WHERE clauses, with no index driving the count.
 */
/*
 * Is src a positive source that can drive a count of F minus a NULL entry
 * (the complement of DESIGN.md §28)?  A COLLECTED set - a range taken as a
 * source (§32) - cannot: it holds no pin, and a count it drove alone would
 * have nothing to carry the §9 interlock and would recheck every row in the
 * heap.  A range still to be walked can: its pieces are located sets.  The
 * planner prices the complement only beside a source this says yes to
 * (lion_cost_range_sum()), taking every range as a collected one.
 */
static bool
lion_source_drives(const LionCountSource *src)
{
	if (src->negated)
		return false;
	if (src->rangewalk != NULL)
		return true;
	if (src->nsets == 1 && src->sets[0].found && src->sets[0].mat != NULL &&
		!BlockNumberIsValid(src->sets[0].head))
		return false;
	return true;
}


/*
 * Every count the node makes, and every existence test (`exists`: 1 or 0),
 * goes through here: the AND of sources, as lion_count_sources_cached()
 * counts it - once any range taken as a source that was too large to collect
 * (LionCountSource.rangewalk; DESIGN.md §32) has been expanded.  Such a range
 * is the union of the disjoint sets a walk of it hands out, so the count is
 * the SUM of the counts of those sets, each ANDed with the other sources in
 * its place (lion_walk_count()); the counts of the pieces come back through
 * here and expand the next such range, if there is one.
 */
int64
lion_node_count(LionCountScanState *st, int nsource, LionCountSource *sources,
				bool exists)
{
	EState	   *estate = st->css.ss.ps.state;
	int			w;

	for (w = 0; w < nsource; w++)
	{
		if (sources[w].rangewalk != NULL)
			return lion_walk_count(st, sources, nsource, w, exists);
	}

	if (exists)
		return lion_exists_sources_cached(st->heap, estate->es_snapshot,
										  nsource, sources, &st->stats,
										  st->viscache, st->rel_read_only) ?
			1 : 0;
	return lion_count_sources_cached(st->heap, estate->es_snapshot, nsource,
									 sources, &st->stats, st->viscache,
									 st->rel_read_only);
}

/*
 * The count of the relation with its IN list located a batch at a time
 * (lion_array_batch_prepare()): the sum of the counts of the batches, each
 * ANDed with the other sources exactly as the whole list would have been.  A
 * batch is lion_array_batch_size() values, moved on to where the hash
 * changes, and its sets are released - pins and all - before the next one is
 * located, so a list of any length holds one batch of sets at a time.  A
 * batch none of whose values has an entry adds nothing, as a list with no
 * entry makes the whole count 0.
 */
static int64
lion_count_batched(LionCountScanState *st)
{
	LionClauseState *cl = &st->clause[st->item[st->batchitem].clauseno];
	LionCountSource *src = &st->sources[st->batchitem + 1];
	int			batch = lion_array_batch_size();
	MemoryContext batchcxt;
	MemoryContext oldcxt;
	int64		count = 0;
	int			start = 0;

	batchcxt = AllocSetContextCreate(st->wherecxt, "LionCount list batch",
									 ALLOCSET_DEFAULT_SIZES);
	while (start < st->nbatchval)
	{
		int			end = start + Min(batch, st->nbatchval - start);
		LionPostingSet *sets;
		int			nsets;
		int			nfound;
		int			i;

		while (end < st->nbatchval &&
			   st->batchhash[end] == st->batchhash[end - 1])
			end++;

		st->listbatches++;
		oldcxt = MemoryContextSwitchTo(batchcxt);
		sets = (LionPostingSet *)
			palloc_extended(sizeof(LionPostingSet) * (end - start),
							MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		nsets = lion_posting_set_lookup_many_col(cl->idx, cl->idxcol,
												 st->batchtype, end - start,
												 &st->batchval[start], NULL,
												 sets, &nfound);
		src->sets = sets;
		src->nsets = nsets;
		src->tree = NULL;
		if (nfound > 0)
		{
			LionKeyNode **args = (LionKeyNode **)
				palloc(sizeof(LionKeyNode *) * nsets);

			for (i = 0; i < nsets; i++)
				args[i] = lion_key_node(i);
			src->tree = lion_bool_node(LION_KN_OR, args, nsets);
		}
		MemoryContextSwitchTo(oldcxt);

		if (src->tree != NULL)
		{
			MemoryContextReset(st->pergroup);
			oldcxt = MemoryContextSwitchTo(st->pergroup);
			count += lion_node_count(st, st->nitem, &st->sources[1], false);
			MemoryContextSwitchTo(oldcxt);
		}

		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		src->sets = NULL;
		src->nsets = 0;
		src->tree = NULL;
		MemoryContextReset(batchcxt);
		start = end;
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextDelete(batchcxt);

	return count;
}

int64
lion_count_relation(LionCountScanState *st)
{
	MemoryContext oldcxt;
	int64		count;

	Assert(st->nitem > 0);
	if (st->wheremissing)
		return 0;

	/* the positive list is there, so this is never the filtered scan below */
	if (st->batchitem >= 0)
		return lion_count_batched(st);

	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);
	if (st->filter != NULL && !lion_where_has_positive(st))
		count = lion_count_scan_filtered(st);
	else
		count = lion_node_count(st, st->nitem, &st->sources[1], false);
	MemoryContextSwitchTo(oldcxt);

	return count;
}

/*
 * A SUMMED WALK PROBED AT THE OTHER SOURCES' ROWS (DESIGN.md §32, "Summed
 * ranges: dense and probed").
 *
 * A walk over a summarized column counts each set it hands out - a key of a
 * partial bucket, a whole bucket's summary - ANDed with the other sources F.
 * On a column whose rows lie all over the heap a summary has a few rows at
 * nearly every container key, and each of those counts is a merge of
 * thousands of containers with F, however few rows F has.  Once the walk has
 * handed out LION_PROBE_SWITCH times the rows of F's smallest positive source
 * - it has shown itself to be the larger side - the rest of it is PROBED
 * instead (lion_range_probe_begin()): F is collected once, each further set
 * is read only at F's container keys, and the rows it holds among F's are
 * counted ONCE at the end of the walk.  The sets counted before the switch
 * and the ones probed after it are disjoint, as every set of the walk is, so
 * their counts add up.
 *
 * The switch is bounded regret: the probe costs a count of F to collect and
 * one to finish, and it is only taken after the walk has already read more
 * than F holds.  It is not taken when F has no positive source, holds a
 * range still to be walked, would not carry the §9 interlock, or does not fit
 * work_mem - the walk then counts every set as it always has - nor for a walk
 * that uses no summaries: a column without them keeps what §28 measured.
 */
typedef struct LionSumProbe
{
	double		frows;			/* F's smallest positive source's rows, or
								 * -1: never probe */
	double		walked;			/* rows of the sets handed out so far */
	bool		tried;
	LionRangeProbe *rp;
	MemoryContext cxt;			/* where the probe is begun: outlives the
								 * walk's batches */
} LionSumProbe;

static void
lion_sum_probe_init(LionSumProbe *sp, const LionEntryScan *walk,
					const LionCountSource *sources, int nsource, int slot)
{
	Size		maxbytes = (Size) work_mem * 1024;
	int			i;
	int			j;

	sp->frows = -1.0;
	sp->walked = 0.0;
	sp->tried = false;
	sp->rp = NULL;
	sp->cxt = CurrentMemoryContext;

	if (!walk->usesum)
		return;
	for (i = 0; i < nsource; i++)
	{
		double		rows = 0.0;

		if (i == slot)
			continue;
		if (sources[i].rangewalk != NULL)
		{
			sp->frows = -1.0;
			return;
		}
		if (sources[i].negated)
			continue;
		for (j = 0; j < sources[i].nsets; j++)
			if (sources[i].sets[j].found)
				rows += (double) sources[i].sets[j].ntids;
		if (sp->frows < 0.0 || rows < sp->frows)
			sp->frows = rows;
	}

	/* a copy of F takes some two bytes a row: not worth trying past memory */
	if (sp->frows * 2.0 * sizeof(uint16) > (double) maxbytes)
		sp->frows = -1.0;
}

/*
 * The walk has handed out `rows` more: the probe, if the rest of it is to be
 * probed - begun now, when this is where the walk has shown itself to be the
 * larger side - or NULL.
 */
static LionRangeProbe *
lion_sum_probe_due(LionCountScanState *st, LionSumProbe *sp, double rows,
				   LionCountSource *sources, int nsource, int slot)
{
	EState	   *estate = st->css.ss.ps.state;
	LionCountSource *others;
	MemoryContext oldcxt;
	int			n = 0;
	int			i;

	sp->walked += rows;
	if (sp->rp != NULL || sp->tried || sp->frows < 0.0 ||
		sp->walked < LION_PROBE_SWITCH * sp->frows)
		return sp->rp;
	sp->tried = true;

	oldcxt = MemoryContextSwitchTo(sp->cxt);
	others = (LionCountSource *) palloc(sizeof(LionCountSource) * nsource);
	for (i = 0; i < nsource; i++)
		if (i != slot)
			others[n++] = sources[i];
	sp->rp = lion_range_probe_begin(st->heap, estate->es_snapshot, n, others,
									(Size) work_mem * 1024, &st->stats);
	pfree(others);
	MemoryContextSwitchTo(oldcxt);

	if (sp->rp != NULL)
		st->rangeprobed++;
	return sp->rp;
}

/* The count of what the probe gathered: the rest of the walk's sum. */
static int64
lion_sum_probe_finish(LionCountScanState *st, LionSumProbe *sp)
{
	EState	   *estate = st->css.ss.ps.state;
	int64		count;

	if (sp->rp == NULL)
		return 0;
	count = lion_range_probe_count(sp->rp, st->heap, estate->es_snapshot,
								   &st->stats, st->viscache,
								   st->rel_read_only);
	lion_range_probe_end(sp->rp);
	sp->rp = NULL;
	return count;
}

/*
 * Sum the counts of the entries one walk of the driving column returns - all
 * of them (LION_WALK_ALL), or one part of a range's (LION_WALK_*) - each ANDed
 * with sources[1 .. nsource - 1]; sources[0] is the driver's slot, which is
 * borrowed for the walk's entries and handed back as it was.
 *
 * The entries of one directory leaf are taken together (DESIGN.md §28,
 * "Counting a walk").  When they are small they are counted as ONE source
 * whose sets are DISJOINT - distinct entries of one scalar column - so the
 * count of the batch is the count of their union ANDed with the rest, and the
 * sum over the leaves is the sum over the entries: §15's argument, the one the
 * sum-over-all has always rested on, taken a leaf at a time instead of an
 * entry at a time.  lion_count_sources_cached() then counts the batch as §15
 * counts an IN list - summed set by set when it is the only positive source
 * and summing is cheaper, merged as a union otherwise.  Large entries are
 * counted one by one, as they always were (LION_SUM_UNION_MAX_ITEMS).
 *
 * Pins (DESIGN.md §9): every INLINE set of the batch holds the pin of the one
 * leaf it was copied from until it has been counted, so a walk of any width
 * holds one directory leaf pinned.  The WHERE sources are the relation's,
 * located once, and are materialized on their second use as for any GROUP BY;
 * the batch is then the positive source that carries the interlock.
 */
static int64
lion_sum_walk(LionCountScanState *st, LionCountSource *sources, int nsource,
			  int part)
{
	LionCountSource saved = sources[0];
	LionPostingSet *sets;
	LionSumProbe probe;
	int64		total = 0;
	int			i;

	/*
	 * A sum only adds up what the walk hands out, so it may be handed the
	 * column's SUMMARY entries in place of the keys they cover (DESIGN.md
	 * §32): still disjoint sets, still exactly the rows of the part.
	 */
	lion_entry_scan_begin_sum(&st->escan, st->groupidx, st->groupidxcol,
							  st->hasrange ? &st->range : NULL, part);
	st->scanning = true;

	/* One leaf's entries, which is at most what lion_range.c copies of one. */
	sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) *
									 st->escan.maxbatch);
	lion_sum_probe_init(&probe, &st->escan, sources, nsource, 0);

	for (;;)
	{
		MemoryContext oldcxt;
		LionRangeProbe *rp;
		int			nsets = 0;
		int			per;
		double		items = 0;
		double		rows = 0;
		Datum		key;
		bool		more = true;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		/*
		 * The first entry may read a new leaf; the rest are what that leaf
		 * holds of the walk.
		 */
		do
		{
			if (!lion_entry_scan_next(&st->escan, &key, &sets[nsets]))
			{
				more = false;
				break;
			}

			/* The NULL entry's rows are the ones `IS NOT NULL` excludes. */
			if (st->sumallitem >= 0 && sets[nsets].keyisnull)
			{
				lion_posting_set_release(&sets[nsets]);
				continue;
			}
			items += sets[nsets].ncontainers;
			rows += (double) sets[nsets].ntids;
			nsets++;
		} while (lion_entry_scan_batch_left(&st->escan) > 0);

		/*
		 * Past the rows of the other sources, the rest of the walk is probed
		 * at theirs and counted once at the end (lion_sum_probe_due()).
		 */
		rp = lion_sum_probe_due(st, &probe, rows, sources, nsource, 0);
		if (rp != NULL)
		{
			for (i = 0; i < nsets; i++)
				lion_range_probe_add(rp, &sets[i]);
			st->stats.sets_summed += nsets;
		}

		/* Small entries are one count, large ones one count each. */
		per = (items <= (double) nsets * LION_SUM_UNION_MAX_ITEMS) ?
			Max(nsets, 1) : 1;

		for (i = 0; rp == NULL && i < nsets; i += per)
		{
			int64		summed = st->stats.sets_summed;
			int			n = Min(per, nsets - i);

			sources[0].nsets = n;
			sources[0].sets = &sets[i];
			sources[0].tree = NULL;
			sources[0].negated = false;
			sources[0].nomaterialize = false;
			sources[0].disjoint = (n > 1);

			total += lion_node_count(st, nsource, sources, false);

			/*
			 * "Posting Sets Summed" is the entries whose counts the walk added
			 * up, however they were counted.
			 */
			st->stats.sets_summed = summed + n;
		}

		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		MemoryContextSwitchTo(oldcxt);

		if (!more)
			break;
	}

	sources[0] = saved;
	pfree(sets);
	st->summaries += st->escan.nsummaries;
	lion_entry_scan_end(&st->escan);
	st->scanning = false;

	/* the probed rest of the walk, counted once (§9 as for any count) */
	total += lion_sum_probe_finish(st, &probe);
	return total;
}

/*
 * count(*) WHERE k IS NOT NULL AND <sources[1 .. nsource - 1]>, k being the
 * driving column: the WHERE sources minus k's reserved NULL entry, which is
 * what `k IS NOT NULL` is as a source (DESIGN.md §14) - one merge, however
 * many entries k has.  The caller has checked that a positive source is among
 * them (lion_range_choose()).
 */
static int64
lion_count_nonnull(LionCountScanState *st, LionCountSource *sources,
				   int nsource)
{
	LionCountSource *srcs;
	LionPostingSet nullset;
	MemoryContext oldcxt;
	int64		count;
	int			n = 0;
	int			i;

	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);

	srcs = (LionCountSource *) palloc0(sizeof(LionCountSource) * nsource);
	for (i = 1; i < nsource; i++)
		srcs[n++] = sources[i];

	if (lion_posting_set_lookup_null_col(st->groupidx, st->groupidxcol,
										 &nullset))
	{
		srcs[n].nsets = 1;
		srcs[n].sets = &nullset;
		srcs[n].tree = NULL;
		srcs[n].negated = true;
		n++;
	}

	count = lion_node_count(st, n, srcs, false);
	lion_posting_set_release(&nullset);
	MemoryContextSwitchTo(oldcxt);

	return count;
}

/*
 * How to evaluate the range that bounds a sum (DESIGN.md §28, "The
 * complement").  The VALUE entries of the range's column are three runs in
 * directory order - BELOW the range, INSIDE it, ABOVE it - and the rows the
 * sum wants are those of INSIDE ANDed with the WHERE sources F.  The entries
 * being disjoint and every row with a value being under exactly one of them,
 *
 *		|INSIDE ∩ F| = |F − NULL(k)| − |BELOW ∩ F| − |ABOVE ∩ F|
 *
 * and |F − NULL(k)| is one merge.  So the sum can walk whichever side has
 * fewer entries, and when BELOW and ABOVE are both empty - the range covers
 * every key the column has - it walks nothing at all.
 *
 * Which side is smaller is decided here exactly, not from statistics: the
 * INSIDE walk and the BELOW-then-ABOVE walk are stepped a leaf at a time in
 * turn, counting what each would return without locating anything
 * (lion_entry_scan_skip_leaf()), until one of them runs out.  That costs at
 * most twice the leaves of the side that is then walked, which is small next
 * to counting it, and it is what makes "every key is inside" an exact answer:
 * the walks of BELOW and ABOVE ran to their ends and found no entry.  (An
 * entry with a visible row exists from before the snapshot until the count is
 * over - VACUUM deletes only empty ones - so a walk that saw none saw that
 * there is none.  The same holds for the walks that count BELOW and ABOVE
 * afterwards, which are ordinary walks.)
 *
 * The complement is not taken when
 *
 *	- the range is unordered, or empty: there is no run to take apart, or no
 *	  row to count;
 *	- no WHERE source is positive: |F − NULL(k)| has nothing to drive it, and
 *	  a count of every row of the table is not something an index can give;
 *	- the driving index is PARTIAL: its entries hold only the rows its
 *	  predicate admits, and F − NULL(k) counts the others too.
 */
static int
lion_range_choose_on(Relation index, AttrNumber col, LionRange *range,
					 LionCountSource *sources, int nsource, int skip)
{
	LionEntryScan in;
	LionEntryScan below;
	LionEntryScan above;
	int64		nin = 0;
	int64		nout = 0;
	bool		positive = false;
	int			eval;
	int			i;

	if (!range->ordered || range->empty)
		return LION_RANGE_EVAL_INSIDE;

	/*
	 * |F − NULL(k)| needs a source of F that can drive it, which a range
	 * collected as a source (DESIGN.md §32) cannot (lion_source_drives()).
	 */
	for (i = 0; i < nsource; i++)
		if (i != skip && lion_source_drives(&sources[i]))
			positive = true;
	if (!positive)
		return LION_RANGE_EVAL_INSIDE;
	if (RelationGetIndexPredicate(index) != NIL)
		return LION_RANGE_EVAL_INSIDE;	/* the planner never picks one */

	/*
	 * The walks that will be counted, summaries and all (DESIGN.md §32): a
	 * side is as long as the sets it would count, whatever they stand for.
	 */
	lion_entry_scan_begin_sum(&in, index, col, range, LION_WALK_INSIDE);
	lion_entry_scan_begin_sum(&below, index, col, range, LION_WALK_BELOW);
	lion_entry_scan_begin_sum(&above, index, col, range, LION_WALK_ABOVE);

	for (;;)
	{
		if (!in.done)
			nin += lion_entry_scan_skip_leaf(&in);
		if (!below.done)
			nout += lion_entry_scan_skip_leaf(&below);
		else if (!above.done)
			nout += lion_entry_scan_skip_leaf(&above);

		if (in.done || (below.done && above.done))
			break;
		CHECK_FOR_INTERRUPTS();
	}

	if (!below.done || !above.done)
		eval = LION_RANGE_EVAL_INSIDE;
	else if (nout == 0)
		eval = LION_RANGE_EVAL_FULL;
	else if (!in.done || nout < nin)
		eval = LION_RANGE_EVAL_COMPLEMENT;
	else
		eval = LION_RANGE_EVAL_INSIDE;

	lion_entry_scan_end(&in);
	lion_entry_scan_end(&below);
	lion_entry_scan_end(&above);

	return eval;
}

/* ... for the range that bounds the node's own sum, driven from slot 0. */
static int
lion_range_choose(LionCountScanState *st, LionCountSource *sources,
				  int nsource)
{
	return lion_range_choose_on(st->groupidx, st->groupidxcol, &st->range,
								sources, nsource, 0);
}

/*
 * The SUM over the sets one part (LION_WALK_*) of a walk of rs's range hands
 * out, each ANDed with the other sources in slot's place: the counting half of
 * lion_sum_walk(), for a range taken as a source that was too large to
 * collect (DESIGN.md §32).  It has an entry scan and a memory context of its
 * own, because it runs inside the counts of another walk - the driver's - and
 * must leave that walk's batch alone.  The entries of one leaf are one count
 * when they are small and one each when they are not, as lion_sum_walk()
 * takes them, and an existence test stops at the first piece with a row.
 */
static int64
lion_walk_range_part(LionCountScanState *st, LionRangeSource *rs, int part,
					 LionCountSource *sources, int nsource, int slot,
					 bool exists)
{
	LionEntryScan es;
	LionPostingSet *sets;
	LionCountSource saved = sources[slot];
	LionSumProbe probe;
	MemoryContext cxt;
	int64		total = 0;
	bool		found = false;

	lion_entry_scan_begin_sum(&es, rs->index, rs->col, &rs->range, part);
	sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) * es.maxbatch);

	/* A count may be probed (lion_sum_walk()); an existence test stops early. */
	lion_sum_probe_init(&probe, &es, sources, nsource, slot);
	if (exists)
		probe.frows = -1.0;
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"LionCount range source walk",
								ALLOCSET_DEFAULT_SIZES);

	for (;;)
	{
		MemoryContext oldcxt;
		LionRangeProbe *rp;
		int			nsets = 0;
		int			per;
		int			i;
		double		items = 0;
		double		rows = 0;
		Datum		key;
		bool		more = true;

		CHECK_FOR_INTERRUPTS();
		oldcxt = MemoryContextSwitchTo(cxt);

		do
		{
			if (!lion_entry_scan_next(&es, &key, &sets[nsets]))
			{
				more = false;
				break;
			}
			items += sets[nsets].ncontainers;
			rows += (double) sets[nsets].ntids;
			nsets++;
		} while (lion_entry_scan_batch_left(&es) > 0);

		rp = lion_sum_probe_due(st, &probe, rows, sources, nsource, slot);
		if (rp != NULL)
		{
			for (i = 0; i < nsets; i++)
				lion_range_probe_add(rp, &sets[i]);
		}

		per = (items <= (double) nsets * LION_SUM_UNION_MAX_ITEMS) ?
			Max(nsets, 1) : 1;
		for (i = 0; rp == NULL && i < nsets && !found; i += per)
		{
			int			n = Min(per, nsets - i);
			int64		c;

			sources[slot].nsets = n;
			sources[slot].sets = &sets[i];
			sources[slot].tree = NULL;
			sources[slot].negated = false;
			sources[slot].nomaterialize = false;
			sources[slot].disjoint = (n > 1);
			sources[slot].rangewalk = NULL;

			c = lion_node_count(st, nsource, sources, exists);
			total += c;
			if (exists && c > 0)
				found = true;
		}

		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(cxt);

		if (!more || found)
			break;
	}

	sources[slot] = saved;
	st->summaries += es.nsummaries;
	lion_entry_scan_end(&es);
	MemoryContextDelete(cxt);
	pfree(sets);
	total += lion_sum_probe_finish(st, &probe);
	return exists ? (found ? 1 : 0) : total;
}

/*
 * The AND of sources with a range taken as a source in slot w that was too
 * large to collect (DESIGN.md §32, "A range as a source"): the range's column
 * drives the count as it drives a summed range (§28) - the sum over the sets
 * of its inside, or the count of the other sources minus the column's NULL
 * entry less the sums over the entries below and above it, whichever side
 * the leaf-by-leaf race finds shorter (lion_range_choose_on()).  An existence
 * test walks the inside, and stops at the first set with a row.
 */
static int64
lion_walk_count(LionCountScanState *st, LionCountSource *sources, int nsource,
				int w, bool exists)
{
	LionRangeSource *rs = sources[w].rangewalk;
	LionCountSource *srcs;
	int64		total = 0;
	int			eval = LION_RANGE_EVAL_INSIDE;

	srcs = (LionCountSource *) palloc(sizeof(LionCountSource) * nsource);
	memcpy(srcs, sources, sizeof(LionCountSource) * nsource);
	srcs[w].rangewalk = NULL;

	if (!exists)
		eval = lion_range_choose_on(rs->index, rs->col, &rs->range, srcs,
									nsource, w);

	if (eval == LION_RANGE_EVAL_INSIDE)
		total = lion_walk_range_part(st, rs, LION_WALK_INSIDE, srcs, nsource,
									 w, exists);
	else
	{
		LionCountSource *rest;
		LionPostingSet nullset;
		bool		hasnull;
		int			n = 0;
		int			i;

		/* the other sources minus the column's NULL entry, in w's place */
		rest = (LionCountSource *) palloc0(sizeof(LionCountSource) * nsource);
		for (i = 0; i < nsource; i++)
			if (i != w)
				rest[n++] = srcs[i];
		hasnull = lion_posting_set_lookup_null_col(rs->index, rs->col,
												   &nullset);
		if (hasnull)
		{
			rest[n].nsets = 1;
			rest[n].sets = &nullset;
			rest[n].negated = true;
			n++;
		}
		total = lion_node_count(st, n, rest, false);
		if (hasnull)
			lion_posting_set_release(&nullset);
		pfree(rest);

		if (eval == LION_RANGE_EVAL_COMPLEMENT)
		{
			total -= lion_walk_range_part(st, rs, LION_WALK_BELOW, srcs,
										  nsource, w, false);
			total -= lion_walk_range_part(st, rs, LION_WALK_ABOVE, srcs,
										  nsource, w, false);
		}
		Assert(total >= 0);
	}

	pfree(srcs);
	return total;
}

/*
 * The sum over every entry of the driving index (DESIGN.md §14,
 * `col IS NOT NULL` with nothing else to drive the merge).
 *
 * The entries of one SCALAR index are DISJOINT - a row has one value in the
 * column, so its TID is under exactly one of them - which is what makes a sum
 * over them the count of their union at all (DESIGN.md §15 states the argument
 * in full; §14 has always rested on it).  The same disjointness is what lets
 * the driver's own `IS NOT NULL` be dropped instead of subtracted, which
 * st->sumallitem says and lion_locate_where() decides: that clause is the
 * column's NULL entry as a negated source, and
 *
 *	- subtracting it from ANOTHER entry of the same index removes nothing, the
 *	  two being disjoint, and
 *	- subtracting it from ITSELF leaves nothing, so that entry contributes 0
 *	  and skipping it outright is the same answer.
 *
 * What that saves is not bookkeeping: the NULL entry of a column with many
 * NULLs has a container at every container key, so the merge was running a
 * lion_container_andnot() against a dense bitset at each of the entry's
 * container keys, for every entry of the index.  A `IS NOT NULL` on another
 * column is a different index's set and keeps its source.
 *
 * The entries are counted a leaf at a time (lion_sum_walk()), and a range on
 * the driving column may be answered from the entries it does NOT select
 * (lion_range_choose()); DESIGN.md §28 has both.
 */
int64
lion_sumall_relation(LionCountScanState *st)
{
	LionCountSource *sources;
	int64		total;
	int			nsource;
	int			eval = LION_RANGE_EVAL_INSIDE;

	if (st->wheremissing)
		return 0;

	if (st->sumallitem >= 0)
	{
		sources = st->dsources;
		nsource = st->ndsource;
	}
	else
	{
		sources = st->sources;
		nsource = st->nsource;
	}

	if (st->hasrange)
		eval = lion_range_choose(st, sources, nsource);

	switch (eval)
	{
		case LION_RANGE_EVAL_FULL:
			total = lion_count_nonnull(st, sources, nsource);
			break;
		case LION_RANGE_EVAL_COMPLEMENT:
			total = lion_count_nonnull(st, sources, nsource);
			total -= lion_sum_walk(st, sources, nsource, LION_WALK_BELOW);
			total -= lion_sum_walk(st, sources, nsource, LION_WALK_ABOVE);
			Assert(total >= 0);
			break;
		default:
			total = lion_sum_walk(st, sources, nsource,
								  st->hasrange ? LION_WALK_INSIDE :
								  LION_WALK_ALL);
			break;
	}

	if (st->hasrange)
		st->rangeeval[eval]++;
	return total;
}

/* ---------------------------------------------------------------------
 * The WHERE sets of a GROUP BY, collected once (DESIGN.md §10)
 * --------------------------------------------------------------------- */

/*
 * What the WHERE items sources[1 .. nwhere] of the relation being counted
 * are, as lion_group_count() weighs them: worked out once per relation, from
 * the hints of the sets lion_locate_where() located once per relation.
 *
 * A range still to be walked - one too large to collect (DESIGN.md §32) - is
 * no item here: every count walks it beside the collected set, as the planner
 * priced it.  A NEGATED item is one to combine when it has a set to subtract,
 * and nothing when it has none (`IS NOT NULL` over a column without NULLs, a
 * multi-key query no key narrows); the collection needs a POSITIVE item to
 * start from, and a WHERE without one is never collected.
 *
 * The REACH of a set is the container keys its rows can lie at: its rows, up
 * to the relation's container keys.  A merge reads about that many containers
 * of its driver and probes each other source at about as many keys, so the
 * collection - driven by the positive item with the fewest rows - and the
 * counts are weighed in it.
 */
static void
lion_where_describe(LionCountScanState *st, LionCountSource *sources,
					int nwhere)
{
	LionCountSource *lone = NULL;
	int			nitems = 0;
	int			npositive = 0;
	int			k;
	int			j;

	st->wknown = true;
	st->wcompound = false;
	st->wsingle = NULL;
	st->wreach = -1;
	if (st->wckeys <= 0)
		st->wckeys = (double) (RelationGetNumberOfBlocks(st->heap) /
							   LION_BLOCKS_PER_CONTAINER + 1);

	for (k = 1; k <= nwhere; k++)
	{
		LionCountSource *src = &sources[k];
		double		rows = 0;
		bool		any = false;

		if (src->rangewalk != NULL)
			continue;
		for (j = 0; j < src->nsets; j++)
		{
			if (src->sets[j].found)
			{
				any = true;
				rows += (double) src->sets[j].ntids;
			}
		}
		if (src->negated)
		{
			if (any)
				nitems++;
			continue;
		}
		nitems++;
		npositive++;
		lone = src;
		rows = Min(rows, st->wckeys);
		if (st->wreach < 0 || rows < st->wreach)
			st->wreach = rows;
	}

	if (npositive == 0)
	{
		st->wtried = true;		/* nothing to intersect */
		return;
	}

	/*
	 * Two items or more, or one that is a union - an IN list, an OR (§19), a
	 * multi-key query of several keys (§17) - are merged again by every
	 * count, whatever copies the count keeps of their sets.  One item of one
	 * set is worth collecting only where no count keeps a copy of it.
	 */
	if (nitems > 1 || lone->nsets > 1 || lone->nomaterialize ||
		(lone->tree != NULL && lone->tree->kind != LION_KN_KEY))
		st->wcompound = true;
	else if (lone->nsets == 1)
		st->wsingle = &lone->sets[0];
}

/*
 * Collect the WHERE items sources[1 .. nwhere] into st->wherecoll: the
 * intersection of the positive ones less what the negated ones subtract, as
 * one private, pinless posting set (lion_sources_collect()).  Made at most
 * once per relation, whatever comes of it.
 *
 * It is budgeted like every set the node collects: what the ranges taken as
 * sources have left of a hash table's memory (get_hash_memory_limit(),
 * lion_locate_range()), and past that it SPILLS to a temporary file, as the
 * FK-side join's copy of its fact filters does (DESIGN.md §27).  A spilled
 * set keeps sixteen bytes a container key in memory, and a count reads it a
 * container at a time.
 *
 * Not on a standby.  Whether a count there may trust the visibility map
 * depends on the WAL mode of every index it reads (lion_sources_all_rmgr()),
 * and a collected set carries the TIDs of every item under the name of one
 * index; the counts read the items there, as they always did.
 */
static void
lion_where_collect(LionCountScanState *st, LionCountSource *sources,
				   int nwhere)
{
	EState	   *estate = st->css.ss.ps.state;
	LionCountSource *items;
	MemoryContext oldcxt;
	Size		limit = get_hash_memory_limit();
	bool		spilled;
	int			n = 0;
	int			k;

	st->wtried = true;
	if (RecoveryInProgress())
		return;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);
	items = (LionCountSource *) palloc(sizeof(LionCountSource) * nwhere);
	for (k = 1; k <= nwhere; k++)
	{
		if (sources[k].rangewalk == NULL)
			items[n++] = sources[k];
	}
	if (lion_sources_collect(st->heap, estate->es_snapshot, n, items,
							 (st->rangesrc_held < limit) ?
							 limit - st->rangesrc_held : 0,
							 true, &st->wherecoll, &spilled, &st->stats))
	{
		st->wcollected = true;
		st->wherecollected++;
		if (spilled)
			st->wherespilled++;
	}
	pfree(items);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * One count of a GROUP BY: the group's set in sources[0] - and, beside a
 * second GROUP BY column (§20), the inner group's in the slots after the WHERE
 * items - ANDed with the WHERE items sources[1 .. nwhere].  It is
 * lion_node_count(), except that once the WHERE items are collected into
 * st->wherecoll the count reads that one set in their place, and that before
 * each count it decides whether to collect them now (DESIGN.md §10, "The
 * WHERE sets, collected once").
 *
 * WHEN.  Collecting reads the items once, a merge over all their containers.
 * A count reads them again at the container keys of its own set: probes into
 * each of them, and for a set the count keeps no copy of, its pages - which is
 * how a GROUP BY of many groups used to read the WHERE about once per group.
 * Neither is known in advance: the walk finds out how many groups there
 * are, and how many rows each has, as it goes.  So the counts go on reading
 * the items until what they have read of them, with what THIS count would
 * read, reaches what the collection reads, and the items are collected then.
 * Collecting at that point costs at most what the counts so far and this one
 * would have read: never more than twice the better of never collecting and
 * collecting before the first group.  `ownrows` is the rows of the count's own
 * smallest set - the count reads the items at no more keys than that set
 * spans - and `more` says that another count of this relation is sure to
 * follow.  Without
 * one, the first count is never preceded by a collection - which is what keeps
 * a GROUP BY that has one group the count it always was.
 *
 * WHY THE COUNT IS STILL EXACT (DESIGN.md §9).  The collected set is a copy,
 * pinless and possibly stale, and it is safe on the terms of
 * lion_posting_set_materialize(): it is only ever counted ANDed with the
 * group's own set, which each count locates afresh under a pin of its own -
 * the walk's entry, the listed value located again (§15), the inner set of a
 * pair (§20).  A dead TID the copy still lists is then either gone from the
 * group's container, and out of the intersection, or in it - which means the
 * group's page was read before VACUUM's ambulkdelete got past it, so VACUUM
 * has not finished ambulkdelete on that index, so it has not set the TID's
 * heap page all-visible, and the TID goes to the heap recheck where the
 * snapshot decides.  The copy cannot lack a row the snapshot sees: it was
 * made after the snapshot was taken, and a visible row was in every index
 * before its transaction committed.  With no pinned positive source in a
 * count the merge trusts no map (cx.novm), as for any count; none of the
 * shapes that come here lacks one.
 */
static int64
lion_group_count(LionCountScanState *st, int nsource, LionCountSource *sources,
				 int nwhere, uint64 ownrows, bool more)
{
	int			n = 0;
	int			k;

	if (!st->wtried && !st->wknown)
		lion_where_describe(st, sources, nwhere);
	if (!st->wtried)
	{
		double		own = Min(Min((double) ownrows, st->wckeys), st->wreach);

		if ((st->wcounts > 0 || more) &&
			st->wspent + own >= st->wreach &&
			(st->wcompound ||
			 (st->wsingle != NULL && lion_posting_set_rewalked(st->wsingle))))
			lion_where_collect(st, sources, nwhere);
		st->wspent += own;
	}
	st->wcounts++;

	if (!st->wcollected)
		return lion_node_count(st, nsource, sources, false);

	/* the group, the collected set, what it does not hold, a second group */
	st->wsources[n++] = sources[0];
	memset(&st->wsources[n], 0, sizeof(LionCountSource));
	st->wsources[n].nsets = 1;
	st->wsources[n].sets = &st->wherecoll;
	n++;
	for (k = 1; k <= nwhere; k++)
	{
		if (sources[k].rangewalk != NULL)
			st->wsources[n++] = sources[k];
	}
	for (k = nwhere + 1; k < nsource; k++)
		st->wsources[n++] = sources[k];
	Assert(n <= st->nsource + 1);

	return lion_node_count(st, n, st->wsources, false);
}

/*
 * May the rest of the entry walk be counted a batch of groups at a time
 * (lion_count_groups_copy(); DESIGN.md §10, "The groups of a walk, counted
 * together")?  Once the WHERE is collected, when every count is that one set
 * ANDed with the group's own: no range still to walk beside it
 * (LionCountSource.rangewalk), no second group column, and every entry its
 * own row - GROUP BY coalesce(g, c) adds two entries into one, and keeps the
 * walk one entry at a time.
 */
static bool
lion_group_batch_ok(LionCountScanState *st)
{
	int			k;

	if (!st->wcollected || st->hascoal || st->nsource != st->nitem + 1)
		return false;
	for (k = 1; k <= st->nitem; k++)
	{
		if (st->sources[k].rangewalk != NULL)
			return false;
	}
	return true;
}

/*
 * Take the next batch of entries from the walk, count them together against
 * the collected WHERE, and keep their keys and counts for the rows that
 * follow; the sets are released before this returns.  False when the walk
 * has no entry left.
 *
 * The sets come from the walk as its one-at-a-time counts took them: an
 * INLINE one keeps the pin of the leaf it was copied from, a CHAIN one is
 * pinned by the cursor that reads it, and each group's count rests on those
 * pins as its own count did (lion_group_count()'s argument).  Nothing is
 * pinned between the rows of a batch.
 */
static bool
lion_group_batch_fill(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;
	int			n = 0;
	int			i;

	if (st->gbatchcxt == NULL)
		st->gbatchcxt = AllocSetContextCreate(estate->es_query_cxt,
											  "LionCount group batch",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(st->gbatchcxt);
	st->gbn = 0;
	st->gbpos = 0;
	if (st->gbmax <= 0)
	{
		st->gbmax = lion_count_groups_batch(st->groupidx);
		st->gbimages = (PGAlignedBlock *)
			MemoryContextAlloc(estate->es_query_cxt,
							   sizeof(PGAlignedBlock) * st->gbmax);
	}

	oldcxt = MemoryContextSwitchTo(st->gbatchcxt);
	st->gbsets = (LionPostingSet *) palloc(sizeof(LionPostingSet) * st->gbmax);
	st->gbkey = (Datum *) palloc(sizeof(Datum) * st->gbmax);
	st->gbnull = (bool *) palloc(sizeof(bool) * st->gbmax);
	st->gbcount = (int64 *) palloc(sizeof(int64) * st->gbmax);
	while (n < st->gbmax &&
		   lion_entry_scan_next(&st->escan, &st->gbkey[n], &st->gbsets[n]))
	{
		st->gbnull[n] = st->gbsets[n].keyisnull;
		n++;
	}
	MemoryContextSwitchTo(oldcxt);
	if (n == 0)
		return false;

	lion_count_groups_copy(st->heap, estate->es_snapshot, n, st->gbsets,
						   &st->wherecoll, st->gbcount, &st->stats,
						   st->viscache, st->rel_read_only, st->gbimages);
	for (i = 0; i < n; i++)
		lion_posting_set_release(&st->gbsets[i]);

	st->gbn = n;
	st->wcounts += n;
	st->groupbatches++;
	st->groupsbatched += n;
	return true;
}

/*
 * The WHERE items of a parallel GROUP BY collected for one range of container
 * keys, lo up to hi (lion_sources_collect_range()), into st->wherecoll in
 * grangecxt: lion_where_collect() for the keys of the range alone, under the
 * same budget and spilling the same way.  False when the range needs no
 * counting at all: the intersection has no container there, or no source has
 * a found set - which leaves a positive source with none, and nothing in the
 * intersection anywhere.  A WHERE of negated sources alone has nothing to
 * collect, and the planner never makes a parallel GROUP BY of one.
 */
static bool
lion_where_collect_range(LionCountScanState *st, uint32 lo, uint64 hi)
{
	EState	   *estate = st->css.ss.ps.state;
	LionCountSource *items;
	MemoryContext oldcxt;
	bool		positive = false;
	bool		spilled;
	bool		ok;
	int			k;

	if (RecoveryInProgress())
		elog(ERROR, "LionCount: a parallel GROUP BY during recovery");

	oldcxt = MemoryContextSwitchTo(st->grangecxt);
	items = (LionCountSource *) palloc(sizeof(LionCountSource) *
									   Max(st->nitem, 1));
	for (k = 1; k <= st->nitem; k++)
	{
		items[k - 1] = st->sources[k];
		if (!st->sources[k].negated)
			positive = true;
	}
	ok = lion_sources_collect_range(st->heap, estate->es_snapshot, st->nitem,
									items, get_hash_memory_limit(), true,
									lo, hi, &st->wherecoll, &spilled,
									&st->stats);
	MemoryContextSwitchTo(oldcxt);

	if (!ok)
	{
		if (!positive)
			elog(ERROR, "LionCount: a parallel GROUP BY without a positive WHERE source");
		return false;
	}
	st->wcollected = true;
	st->wherecollected++;
	if (spilled)
		st->wherespilled++;
	return st->wherecoll.found;
}

/*
 * A PARALLEL GROUP BY (DESIGN.md §10, "A GROUP BY in parallel"): the next
 * range of container keys for this participant to count, its WHERE collected
 * and the entry walk begun again from the first entry.  False once every range
 * has been claimed.
 *
 * The heap's container keys are cut into ranges (lion_key_ranges(), when the
 * Gather sets up its shared memory: a few for each participant), and each
 * participant claims the next range nobody has from the shared counter,
 * collects the WHERE of that range alone and counts every group of the entry
 * walk against it a batch at a time (lion_group_batch_fill()), a group's rows
 * in the range being one partial row that the Finalize Agg above adds to the
 * group's others.  The ranges cover every key once, so every row a group
 * counts is counted in exactly one range, by one participant.  Each range
 * walks the entries from the first, and each group's cursor is sought to the
 * first key of the range the copy has (§22): that is what a range costs over
 * its share of the serial walk, and why a range is never narrower than a few
 * keys.
 *
 * WHY IT IS EXACT (§9): lion_count_groups_copy()'s argument, range by range.
 * Every group's set is located afresh by this participant's own walk, under
 * its own pins, and is counted against a pinless copy of the WHERE made after
 * the snapshot was taken, whose containers are the whole copy's at the keys
 * of the range.  Workers run under the leader's snapshot.
 */
static bool
lion_group_range_next(LionCountScanState *st)
{
	LionJoinShared *shared = st->joinshared;
	EState	   *estate = st->css.ss.ps.state;
	int			k;

	/* the range the batches came to the end of: its walk and its copy */
	if (st->scanning)
	{
		lion_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	lion_posting_set_release(&st->wherecoll);
	st->wcollected = false;
	st->grange = -1;
	if (st->grangecxt == NULL)
		st->grangecxt = AllocSetContextCreate(estate->es_query_cxt,
											  "LionCount key range",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(st->grangecxt);

	/*
	 * Every count is the group's set ANDed with the one copy: nothing else
	 * may be a source (lion_group_batch_ok()), which the planner has seen to.
	 */
	if (st->nsource != st->nitem + 1 || st->ingroupitem >= 0)
		elog(ERROR, "LionCount: a parallel GROUP BY of another shape");
	for (k = 1; k <= st->nitem; k++)
	{
		if (st->sources[k].rangewalk != NULL)
			elog(ERROR, "LionCount: a parallel GROUP BY of another shape");
	}

	for (;;)
	{
		uint32		r = pg_atomic_fetch_add_u32(&shared->nextchunk, 1);
		uint32		lo;
		uint64		hi;

		if (r >= (uint32) shared->nranges)
			return false;
		CHECK_FOR_INTERRUPTS();

		st->granges++;
		lion_key_range(shared->nranges, shared->ckeys, (int) r, &lo, &hi);
		if (!lion_where_collect_range(st, lo, hi))
		{
			/* nothing of the WHERE in the range: no group has a row there */
			lion_posting_set_release(&st->wherecoll);
			st->wcollected = false;
			MemoryContextReset(st->grangecxt);
			continue;
		}

		lion_entry_scan_begin_range(&st->escan, st->groupidx, st->groupidxcol,
									NULL);
		st->scanning = true;
		st->grange = (int) r;
		return true;
	}
}

/*
 * The next partial row of a parallel GROUP BY (lion_group_range_next()): a
 * group's count in the range this participant is counting, the ranges taken
 * one after the other until none is left.  A group with no row in the range
 * has no row of it.
 */
TupleTableSlot *
lion_next_group_ranged(LionCountScanState *st, bool *exhausted)
{
	*exhausted = false;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* the key of the last row lives in the batch, and pergroup is spare */
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		MemoryContextReset(st->pergroup);

		if (st->gbpos < st->gbn)
		{
			int			i = st->gbpos++;

			if (st->gbcount[i] == 0)
				continue;
			return lion_emit_tuple(st, st->gbkey[i], st->gbnull[i],
								   (Datum) 0, true, st->gbcount[i]);
		}
		if (st->grange >= 0 && lion_group_batch_fill(st))
			continue;
		if (!lion_group_range_next(st))
		{
			*exhausted = true;
			return NULL;
		}
	}
}

/*
 * The next group of the relation the node has open, as one row.
 *
 * Returns NULL and sets *exhausted once the relation's entry scan has run
 * out; every other return is a row.  This is the streaming group loop of
 * DESIGN.md §10, shared by the single-table path and by each partition of a
 * partitioned one (§16), which is why nothing here knows about partitions:
 * the caller has opened one relation and located its WHERE clauses.
 *
 * GROUP BY coalesce(g, c) (st->hascoal, DESIGN.md §10) takes two entries out
 * of the stream: the NULL entry, whose rows are c's group, and the entry whose
 * key the grouping equality finds equal to c, if the walk meets one.  Their
 * counts are added up in coalcount and the one group they make is emitted,
 * with the value c, after the last entry - wherever the two came in the walk,
 * and whichever of them exists.  An entry of c that VACUUM removes before the
 * walk reaches it held no row this snapshot sees, so the group is the same
 * without it; a key equal to c inserted meanwhile holds none either.  The
 * classes of one index are disjoint, so no other entry can equal c.
 */
static TupleTableSlot *
lion_next_group(LionCountScanState *st, bool *exhausted)
{
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		Datum		key;
		bool		keyisnull;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		/*
		 * The key of the group we returned last time lives in pergroup, and
		 * the caller is done with it by now: a scan node's tuple is only
		 * guaranteed until its next call.
		 */
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		MemoryContextReset(st->pergroup);

		/*
		 * Once the WHERE is collected, the rest of the walk is counted a
		 * batch at a time (lion_group_batch_fill()), and a batch's rows go up
		 * one a call, in the walk's order.  A group exists only if at least
		 * one of its rows is visible.
		 */
		if (st->gbpos < st->gbn)
		{
			int			i = st->gbpos++;

			if (st->gbcount[i] == 0)
				continue;
			return lion_emit_tuple(st, st->gbkey[i], st->gbnull[i],
								   (Datum) 0, true, st->gbcount[i]);
		}
		if (lion_group_batch_ok(st))
		{
			if (lion_group_batch_fill(st))
				continue;
			*exhausted = true;
			return NULL;
		}

		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (st->coalwalked ||
			!lion_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);

			/*
			 * The group of c, once: a HAVING that rejects it sends the caller
			 * back here, and the walk is not asked for another entry after
			 * it has run out.
			 */
			if (st->hascoal && !st->coalwalked)
			{
				st->coalwalked = true;
				if (st->coalcount > 0)
					return lion_emit_tuple(st, st->coalconst->constvalue,
										   false, (Datum) 0, true,
										   st->coalcount);
			}
			*exhausted = true;
			return NULL;
		}

		/* another entry already in hand is another count to come */
		count = lion_group_count(st, st->nsource, st->sources, st->nitem,
								 st->groupset.ntids,
								 lion_entry_scan_batch_left(&st->escan) > 0);
		keyisnull = st->groupset.keyisnull;
		lion_posting_set_release(&st->groupset);

		if (st->hascoal &&
			(keyisnull ||
			 DatumGetBool(FunctionCall2Coll(&st->coaleqfn, st->coalcoll, key,
											st->coalconst->constvalue))))
		{
			st->coalcount += count;
			MemoryContextSwitchTo(oldcxt);
			continue;
		}
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		return lion_emit_tuple(st, key, keyisnull, (Datum) 0, true, count);
	}
}

/*
 * The same when an IN list on the grouping column drives the groups
 * (DESIGN.md §15): the groups are the listed values, so the node steps through
 * that clause's located posting sets instead of the index's entries.
 *
 * Each group's rows are its own entry's, intersected with whatever else the
 * WHERE says: the IN clause itself is not intersected, because the entries of
 * a scalar index are disjoint and `entry ∩ (entry ∪ the rest of the list)` is
 * the entry.  With no other clause that leaves ONE source, which is the plain
 * single-key count, and the answer for the whole query is then the same sum
 * the ungrouped form short-circuits to - split into its terms.
 *
 * Pins and memory (DESIGN.md §9).  The sets belong to the clause and were
 * located once for this relation, with their pins, by lion_locate_where(), and
 * the key the loop emits is the copy the set already holds, which outlives the
 * row.  A set that holds no pin - every INLINE one once a row has gone up
 * (lion_pause_run()), and any located past the §15 pin budget - is located
 * again for its group, into groupset, and released after the count: it is the
 * source that carries the interlock for the group, the other WHERE sets being
 * NOPIN copies by then too - or, once they are worth it, one collected set
 * (lion_group_count()), which is a pinless copy as well.
 */
static TupleTableSlot *
lion_next_group_inlist(LionCountScanState *st, bool *exhausted)
{
	LionCountSource *src = &st->sources[st->ingroupitem + 1];
	LionClauseState *cl = &st->clause[st->item[st->ingroupitem].clauseno];
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		LionPostingSet *ps;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);

		if (st->ingroupset >= src->nsets)
		{
			*exhausted = true;
			return NULL;
		}
		ps = &src->sets[st->ingroupset++];
		if (!ps->found)
			continue;			/* a listed value with no entry: no group */
		st->ingroupleft--;

		Assert(ps->hasstoredkey && !ps->keyisnull);

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		st->dsources[0].nsets = 1;
		st->dsources[0].sets = ps;
		st->dsources[0].tree = NULL;
		st->dsources[0].negated = false;
		st->dsources[0].nomaterialize = false;
		st->dsources[0].disjoint = false;

		/*
		 * The entry as it is now, under a pin of its own.  One that has gone
		 * meanwhile held no row anyone can see (VACUUM deletes only an empty
		 * entry, §18): no group.
		 */
		if (ps->nopin)
		{
			if (!lion_posting_set_lookup_col(cl->idx, cl->idxcol,
											 ps->storedkey, InvalidOid,
											 &st->groupset))
			{
				lion_posting_set_release(&st->groupset);
				MemoryContextSwitchTo(oldcxt);
				continue;
			}
			st->dsources[0].sets = &st->groupset;
		}

		/* the WHERE is every other item: collected once, when that pays */
		count = lion_group_count(st, st->ndsource, st->dsources,
								 st->ndsource - 1, st->dsources[0].sets->ntids,
								 st->ingroupleft > 0);
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		return lion_emit_tuple(st, ps->storedkey, false, (Datum) 0, true,
							  count);
	}
}

/*
 * The same for a GROUP BY over TWO indexed columns (DESIGN.md §20).
 *
 * A nested loop over the two indexes' entries: the outer index - the one with
 * fewer entries, chosen at plan time - drives the scan exactly as a single
 * group column's does, and for each of its entries every key of the inner
 * index is tried.  A pair's count is the intersection of the two groups'
 * posting sets with the WHERE sources, and only a pair with at least one
 * visible row is a group at all, so only those are emitted.
 *
 * Pins and memory (DESIGN.md §9).  The outer group's set is located once and
 * held for the whole of its inner loop, in outercxt - with its pin until the
 * first of its pairs goes up as a row, and as a NOPIN copy after that
 * (lion_pause_run()), the pairs being carried by their inner sets; each
 * pair's inner set is located, counted and released inside pergroup, so at
 * most two group pins exist at a time however many distinct values either
 * column has.  The inner keys were read once per relation into innercxt
 * (lion_load_inner_keys()); when they did not fit its budget, innerkey is
 * NULL and the inner index's entry scan is walked once per outer group
 * instead, which holds one pin at a time as well.  The WHERE items are
 * collected once per relation when that pays (lion_group_count()), and the
 * pairs after that are counted against the copy - the inner set carrying the
 * interlock, as it does for the WHERE sets' copies once a row has gone up.
 *
 * That walk outlives many pairs, and so do the memory contexts
 * lion_entry_scan_begin_col() creates for its position and its batch, under
 * whatever context is current: it is begun in the query's context, never
 * inside pergroup, whose reset before every pair would delete them under the
 * walk (the 2026-09-27 review found exactly that: a leaf copied into freed
 * memory at the next pair, and the contexts deleted twice at the end).
 */
static TupleTableSlot *
lion_next_group2(LionCountScanState *st, bool *exhausted)
{
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		Datum		ikey = (Datum) 0;
		bool		ikeyisnull;
		bool		more;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		/* ---- the outer group ---- */
		if (!st->outeropen)
		{
			MemoryContextReset(st->outercxt);
			oldcxt = MemoryContextSwitchTo(st->outercxt);
			if (!lion_entry_scan_next(&st->escan, &st->outerkey, &st->groupset))
			{
				MemoryContextSwitchTo(oldcxt);
				*exhausted = true;
				return NULL;
			}
			MemoryContextSwitchTo(oldcxt);
			st->outerisnull = st->groupset.keyisnull;
			st->outeropen = true;
			st->inneridx = 0;
		}

		/* ---- the fallback: the inner index's walk, per outer group ---- */
		if (st->innerkey == NULL && !st->scanning2)
		{
			oldcxt = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);
			lion_entry_scan_begin_col(&st->escan2, st->groupidx2,
									 st->groupidxcol2);
			MemoryContextSwitchTo(oldcxt);
			st->scanning2 = true;
		}

		/*
		 * The row we returned last time may point into pergroup (the fallback
		 * path's inner key does), and the caller is done with it by now: a
		 * scan node's tuple is only guaranteed until its next call.
		 */
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		/* ---- the next inner group of it ---- */
		if (st->innerkey != NULL)
		{
			int			i = st->inneridx;

			if (i >= st->ninnerkey)
			{
				MemoryContextSwitchTo(oldcxt);
				lion_posting_set_release(&st->groupset);
				st->outeropen = false;
				continue;
			}
			st->inneridx++;

			ikey = st->innerkey[i];
			ikeyisnull = st->innerisnull[i];
			if (ikeyisnull)
				(void) lion_posting_set_lookup_null_col(st->groupidx2,
													   st->groupidxcol2,
													   &st->groupset2);
			else
				(void) lion_posting_set_lookup_col(st->groupidx2,
												  st->groupidxcol2, ikey,
												  InvalidOid, &st->groupset2);
		}
		else
		{
			Assert(st->scanning2);
			if (!lion_entry_scan_next(&st->escan2, &ikey, &st->groupset2))
			{
				MemoryContextSwitchTo(oldcxt);
				lion_entry_scan_end(&st->escan2);
				st->scanning2 = false;
				lion_posting_set_release(&st->groupset);
				st->outeropen = false;
				continue;
			}
			ikeyisnull = st->groupset2.keyisnull;
		}

		/*
		 * The WHERE items are collected once, when that pays, and the pair is
		 * counted against the copy; another inner key, or another outer entry
		 * in hand, is another count to come.
		 */
		more = (st->innerkey != NULL) ? (st->inneridx < st->ninnerkey) :
			(lion_entry_scan_batch_left(&st->escan2) > 0);
		more = more || lion_entry_scan_batch_left(&st->escan) > 0;
		count = lion_group_count(st, st->nsource, st->sources, st->nitem,
								 Min(st->groupset.ntids, st->groupset2.ntids),
								 more);
		lion_posting_set_release(&st->groupset2);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if at least one of its rows is visible. */
		if (count == 0)
			continue;

		return lion_emit_tuple(st, st->outerkey, st->outerisnull,
							  ikey, ikeyisnull, count);
	}
}

/*
 * One test of a count(DISTINCT k) walk (DESIGN.md §26): the rows of
 * (sources) when the target list needs them counted, and otherwise 1 or 0 for
 * whether there is at least one visible row at all - which is where the walk
 * saves its work, because an existence test stops at the first container that
 * shows one (lion_exists_sources_cached()).  Runs in pergroup, like a group's
 * count.
 */
static int64
lion_distinct_test(LionCountScanState *st, int nsource,
				   LionCountSource *sources, bool count)
{
	st->disttests++;
	return lion_node_count(st, nsource, sources, !count);
}

/*
 * count(DISTINCT k) without a GROUP BY (DESIGN.md §26): the one row.
 *
 * A scalar index puts every row under exactly one entry of k - its value's,
 * or the reserved NULL entry (§14, §15) - so the distinct non-NULL values of k
 * among the rows the WHERE selects are exactly the non-NULL entries whose set,
 * intersected with the WHERE sources, has a visible row.  So this is the
 * GROUP BY k walk of lion_next_group() with each group's count replaced by
 * an existence test and the groups summed into one row:
 *
 *	- the entries are k's index's, or - when the WHERE pins k itself to a
 *	  value or a list - that clause's own located sets, which are then not
 *	  intersected (st->ingroupitem, the §15 driver);
 *	- the NULL entry is never a distinct value.  It is skipped, except that
 *	  a total over every row (count(*)) counts it, and that a GROUP BY the
 *	  planner folded to one constant group asks it, at the end and only if
 *	  nothing else matched, whether the group exists at all;
 *	- when the target list wants rows as well (count(*), count(k), ...) each
 *	  test is a full count, summed: the entries are disjoint, so their counts
 *	  add up to the rows of their union.
 *
 * Pins (DESIGN.md §9): one entry's set at a time, released before the next
 * is located, exactly as in the GROUP BY walk; a list's sets belong to the
 * clause and were located once by lion_locate_where().
 */
TupleTableSlot *
lion_distinct_relation(LionCountScanState *st)
{
	LionCountSource *sources;
	MemoryContext oldcxt;
	int			nsource;
	int64		ndistinct = 0;
	int64		nonnull = 0;
	int64		total = 0;
	bool		found = false;
	bool		listdrive = (st->ingroupitem >= 0);
	Datum		key;

	if (st->ingroupitem >= 0 || st->sumallitem >= 0)
	{
		sources = st->dsources;
		nsource = st->ndsource;
	}
	else
	{
		sources = st->sources;
		nsource = st->nsource;
	}

	if (!listdrive)
	{
		lion_entry_scan_begin_range(&st->escan, st->groupidx, st->groupidxcol,
									st->hasrange ? &st->range : NULL);
		st->scanning = true;
	}

	for (;;)
	{
		LionPostingSet *ps;
		int64		n;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (listdrive)
		{
			LionCountSource *src = &st->sources[st->ingroupitem + 1];

			if (st->ingroupset >= src->nsets)
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}
			ps = &src->sets[st->ingroupset++];
			if (!ps->found)
			{
				MemoryContextSwitchTo(oldcxt);
				continue;		/* a listed value with no entry */
			}
			sources[0].nsets = 1;
			sources[0].sets = ps;
			sources[0].tree = NULL;
			sources[0].negated = false;
			sources[0].nomaterialize = false;
			sources[0].disjoint = false;
		}
		else
		{
			if (!lion_entry_scan_next(&st->escan, &key, &st->groupset))
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}
			ps = &st->groupset;
		}

		if (ps->keyisnull)
		{
			/*
			 * Not a value.  Its rows are part of the total, unless the WHERE
			 * says `k IS NOT NULL` (st->sumallitem), which excludes exactly
			 * them.
			 */
			if (st->distfull && st->sumallitem < 0)
				total += lion_distinct_test(st, nsource, sources, true);
		}
		else
		{
			n = lion_distinct_test(st, nsource, sources, st->distfull);
			if (n > 0)
			{
				ndistinct++;
				nonnull += n;
				total += n;
			}
		}

		if (!listdrive)
			lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
	}

	if (!listdrive)
	{
		lion_entry_scan_end(&st->escan);
		st->scanning = false;
	}

	found = (ndistinct > 0 || total > 0);

	/*
	 * A folded GROUP BY has a row only if some row matches, and a row whose k
	 * is NULL matches too.  Only asked when nothing else answered it, and
	 * never under a list or a range on k (DESIGN.md §28), which no NULL
	 * satisfies.
	 */
	if (st->singlegroup && !found && !listdrive && st->sumallitem < 0 &&
		!st->hasrange)
	{
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);
		if (lion_posting_set_lookup_null_col(st->groupidx, st->groupidxcol,
											 &st->groupset))
			found = (lion_distinct_test(st, nsource, sources, false) > 0);
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
	}

	st->done = true;
	if (st->singlegroup && !found)
		return NULL;

	st->distcount = ndistinct;
	st->distcolcount = nonnull;
	return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, total);
}

/*
 * count(DISTINCT k) per GROUP BY g (DESIGN.md §26): the (g, k) nested loop of
 * lion_next_group2(), g outer and k inner, with one row per GROUP rather than
 * per pair.
 *
 * For each entry of g, first the group itself: is there a visible row in
 * g ∩ WHERE at all?  (Counted, when the target list asks for count(*) or
 * count(g).)  A group without one is not emitted - a group exists only if one
 * of its rows is visible - and a group whose rows all have k NULL is emitted
 * with a distinct count of 0, which is what SQL says it is.  Then one test per
 * non-NULL key of k, of g ∩ WHERE ∩ k, and the row.
 *
 * Pins and memory are §20's: the outer set is located once and held for the
 * group's whole inner loop (in outercxt), each pair's inner set is located,
 * tested and released inside pergroup, so at most two group pins exist at a
 * time; the inner keys were read once per relation (lion_load_inner_keys()),
 * or, when they did not fit, k's entry scan is walked once per group.
 */
static TupleTableSlot *
lion_next_group_distinct(LionCountScanState *st, bool *exhausted)
{
	MemoryContext oldcxt;

	*exhausted = false;

	for (;;)
	{
		int64		count;
		int64		ndistinct = 0;
		int64		nonnull = 0;

		CHECK_FOR_INTERRUPTS();

		/* the previous group's key lived in outercxt; its row is consumed */
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		MemoryContextReset(st->outercxt);
		oldcxt = MemoryContextSwitchTo(st->outercxt);
		if (!lion_entry_scan_next(&st->escan, &st->outerkey, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			*exhausted = true;
			return NULL;
		}
		MemoryContextSwitchTo(oldcxt);
		st->outerisnull = st->groupset.keyisnull;
		st->outeropen = true;

		/* ---- the group: slot 0 and the WHERE items, not the inner slot ---- */
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);
		count = lion_distinct_test(st, st->nsource - 1, st->sources,
								   st->distgroupcount);
		MemoryContextSwitchTo(oldcxt);
		if (count == 0)
		{
			lion_posting_set_release(&st->groupset);
			st->outeropen = false;
			continue;
		}

		/* ---- every non-NULL key of k ---- */
		if (st->innerkey != NULL)
		{
			int			i;

			for (i = 0; i < st->ninnerkey; i++)
			{
				int64		n;

				CHECK_FOR_INTERRUPTS();
				if (st->innerisnull[i])
					continue;

				MemoryContextReset(st->pergroup);
				oldcxt = MemoryContextSwitchTo(st->pergroup);
				(void) lion_posting_set_lookup_col(st->groupidx2,
												   st->groupidxcol2,
												   st->innerkey[i], InvalidOid,
												   &st->groupset2);
				n = lion_distinct_test(st, st->nsource, st->sources,
									   st->distfull);
				lion_posting_set_release(&st->groupset2);
				MemoryContextSwitchTo(oldcxt);

				if (n > 0)
				{
					ndistinct++;
					nonnull += n;
				}
			}
		}
		else
		{
			lion_entry_scan_begin_col(&st->escan2, st->groupidx2,
									 st->groupidxcol2);
			st->scanning2 = true;
			for (;;)
			{
				Datum		ikey;
				int64		n;

				CHECK_FOR_INTERRUPTS();
				MemoryContextReset(st->pergroup);
				oldcxt = MemoryContextSwitchTo(st->pergroup);
				if (!lion_entry_scan_next(&st->escan2, &ikey, &st->groupset2))
				{
					MemoryContextSwitchTo(oldcxt);
					break;
				}
				n = 0;
				if (!st->groupset2.keyisnull)
					n = lion_distinct_test(st, st->nsource, st->sources,
										   st->distfull);
				lion_posting_set_release(&st->groupset2);
				MemoryContextSwitchTo(oldcxt);

				if (n > 0)
				{
					ndistinct++;
					nonnull += n;
				}
			}
			lion_entry_scan_end(&st->escan2);
			st->scanning2 = false;
		}

		lion_posting_set_release(&st->groupset);
		st->outeropen = false;

		st->distcount = ndistinct;
		st->distcolcount = nonnull;
		return lion_emit_tuple(st, st->outerkey, st->outerisnull,
							  (Datum) 0, true, count);
	}
}

/*
 * Whichever of them the plan asks for.
 */
TupleTableSlot *
lion_next_group_any(LionCountScanState *st, bool *exhausted)
{
	if (st->distattno != 0)
		return lion_next_group_distinct(st, exhausted);
	if (st->groupattno2 != 0)
		return lion_next_group2(st, exhausted);
	if (st->ingroupitem >= 0)
		return lion_next_group_inlist(st, exhausted);
	return lion_next_group(st, exhausted);
}
