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

#include <math.h>

#include "lion_customscan.h"

#include "access/visibilitymap.h"
#include "utils/numeric.h"

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
 * the row filter - those queries - and the `IS NOT NULL` and `<>` clauses
 * tested on each row the snapshot sees.  It is the ordinary plan's work, which the
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

		if (st->item[k].orno >= 0 || !LION_CLAUSE_IS_NEGATED(cl->kind))
			continue;
		c = &scan.clauses[scan.nclauses++];
		c->attno = cl->idx->rd_index->indkey.values[cl->idxcol - 1];
		c->notnull = (cl->kind == LION_CLAUSE_NOTNULL);
		if (!c->notnull)
		{
			/* `col <> c` (DESIGN.md §35): its own operator, column on the left */
			c->collation = cl->idx->rd_indcollation[cl->idxcol - 1];
			c->value = cl->val;
			fmgr_info(get_opcode(cl->opno), &c->flinfo);
		}
	}

	/* every row it counts is a heap row, which a gather takes too (§40) */
	return lion_count_heap_gather(st->heap, estate->es_snapshot, &scan,
								  lion_vis_cache_get_gather(st->viscache),
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

		/*
		 * Small entries are one count, large ones one count each - except
		 * under a gather (DESIGN.md §40), which reads a window's store pages
		 * at every count of a container there: the leaf's entries are one
		 * union, so that each window's pages are read once a leaf and not
		 * once an entry.  A union too wide for the pins it may hold is still
		 * taken in batches (lion_run_batches()).
		 */
		per = (st->store != NULL ||
			   items <= (double) nsets * LION_SUM_UNION_MAX_ITEMS) ?
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
 *	- the range is unordered, or empty, or has a hole (`<>`, DESIGN.md §35):
 *	  there is no run to take apart, or no row to count;
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

	if (!range->ordered || range->empty || range->nholes > 0)
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

	/*
	 * A gather (DESIGN.md §40) is handed every row the count counts, and the
	 * complement counts rows it then takes away again: the inside, then.
	 */
	if (eval == LION_RANGE_EVAL_COMPLEMENT && st->store != NULL)
		eval = LION_RANGE_EVAL_INSIDE;

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

	/*
	 * The complement subtracts the rows of the entries below and above the
	 * range from those of every entry, which a gather (DESIGN.md §40) cannot
	 * take back once it has been handed them: it walks the inside.  The
	 * range that covers every entry is one count of every row, as before.
	 */
	if (eval == LION_RANGE_EVAL_COMPLEMENT && st->store != NULL)
		eval = LION_RANGE_EVAL_INSIDE;

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
 * A candidate of the top k (DESIGN.md §36): an entry's own count, which is
 * at least the group's, and its key.
 */
typedef struct LionTopkCand
{
	uint64		ntids;
	Datum		key;
	bool		isnull;
} LionTopkCand;

/* larger counts first */
static int
lion_topk_cand_cmp(const void *a, const void *b)
{
	uint64		x = ((const LionTopkCand *) a)->ntids;
	uint64		y = ((const LionTopkCand *) b)->ntids;

	return (x > y) ? -1 : (x < y) ? 1 : 0;
}

/*
 * Keep the `keep` largest of the n candidates, the rest let go of: the
 * largest count among those is *maxout's, if it is larger.
 */
static int
lion_topk_trim(LionTopkCand *cand, int n, int keep, bool byval,
			   uint64 *maxout)
{
	int			i;

	qsort(cand, n, sizeof(LionTopkCand), lion_topk_cand_cmp);
	if (n <= keep)
		return n;
	*maxout = Max(*maxout, cand[keep].ntids);
	for (i = keep; i < n; i++)
	{
		if (!byval && !cand[i].isnull)
			pfree(DatumGetPointer(cand[i].key));
	}
	return keep;
}

/* Is the entry's key one of the n stored keys excl? */
static bool
lion_topk_excluded(LionState *state, const LionEntryTuple *entry,
				   const Datum *excl, int n)
{
	Datum		key;
	int			i;

	if (n == 0)
		return false;
	key = lion_entry_key(state, entry);
	for (i = 0; i < n; i++)
	{
		if (datumIsEqual(key, excl[i], state->typbyval, state->typlen))
			return true;
	}
	return false;
}

/* The smallest of a min-heap of counts, n of them, sifted after a change. */
static void
lion_topk_sift(int64 *heap, int n, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1;
		int			m = i;
		int64		t;

		if (l < n && heap[l] < heap[m])
			m = l;
		if (l + 1 < n && heap[l + 1] < heap[m])
			m = l + 1;
		if (m == i)
			return;
		t = heap[i];
		heap[i] = heap[m];
		heap[m] = t;
		i = m;
	}
}

static void
lion_topk_push(int64 *heap, int *n, int64 cap, int64 v)
{
	int			i;

	if (*n >= cap)
	{
		if (v <= heap[0])
			return;
		heap[0] = v;
		lion_topk_sift(heap, *n, 0);
		return;
	}
	i = (*n)++;
	heap[i] = v;
	while (i > 0 && heap[(i - 1) / 2] > heap[i])
	{
		int64		t = heap[i];

		heap[i] = heap[(i - 1) / 2];
		heap[(i - 1) / 2] = t;
		i = (i - 1) / 2;
	}
}

/*
 * THE TOP k OF A GROUP BY ORDERED BY ITS COUNT (DESIGN.md §36): the groups
 * that can be among the first k, counted, into topkkey, topknull and
 * topkcount - or topkwhole set when the candidates could not be shown to be
 * enough, and the walk then counts every group as it would without a k.
 *
 * An entry's ntids is the number of TIDs its posting set holds, and a group
 * this snapshot sees has each of its visible rows there: a row is in the
 * index before its transaction commits, VACUUM takes out only what no
 * snapshot sees, and a TID is one row at most.  So ntids BOUNDS the group's
 * count, whatever the WHERE says - it can only take rows away.
 *
 * One walk of the driving column's entries (the same walk, under the same
 * range, as the groups would be counted in) reads nothing but their headers
 * and keeps the topkcand largest; maxout is the largest count it left out.
 * The candidates are then counted exactly, largest bound first, until the
 * k-th largest count so far is at least the next bound - more than it, when
 * every group tied with the k-th has to come out: no group not yet counted
 * can then have a larger count, or, strictly, an equal one.  The groups
 * counted go up, and the Sort and Limit above put them in order and cut them;
 * a group not counted cannot be among the first k.  Running out of
 * candidates short of that, with a count left out that might still be large
 * enough, is the case the walk of every group is for.
 */
static void
lion_topk_run(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	LionEntryScan es;
	LionTopkCand *cand;
	MemoryContext oldcxt;
	int64	   *heap;
	int			nheap = 0;
	int			cap = st->topkcand;
	int			n = 0;
	int			i;
	uint64		maxout = 0;
	uint64		floor = 0;
	bool		trimmed = false;
	bool		byval;
	int16		typlen;
	bool		enough = false;
	Datum	   *excl;
	int			nexcl = 0;
	bool		exclnull = false;
	int			k;

	st->topkran = true;
	if (st->topkcxt == NULL)
		st->topkcxt = AllocSetContextCreate(estate->es_query_cxt,
											"LionCount top k",
											ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(st->topkcxt);
	oldcxt = MemoryContextSwitchTo(st->topkcxt);

	/*
	 * The entries the WHERE takes out whole: `g <> c` subtracts c's entry and
	 * the NULL one, `g IS NOT NULL` the NULL one (DESIGN.md §35), so a group
	 * of either has no row - and c is often the largest entry there is, whose
	 * set counting would read for nothing.  Its entry is the one whose stored
	 * key the lookup found: one entry to an equality class, so the same bytes.
	 */
	excl = (Datum *) palloc(sizeof(Datum) * Max(2 * st->nitem, 1));
	for (k = 0; k < st->nitem; k++)
	{
		LionSourceItem *it = &st->item[k];
		LionCountSource *src = &st->sources[k + 1];
		LionClauseState *cl;
		int			j;

		if (it->orno >= 0 || it->rangesrc || !src->negated)
			continue;
		cl = &st->clause[it->clauseno];
		if ((cl->kind != LION_CLAUSE_NE && cl->kind != LION_CLAUSE_NOTNULL) ||
			cl->idx == NULL ||
			RelationGetRelid(cl->idx) != RelationGetRelid(st->groupidx) ||
			cl->idxcol != st->groupidxcol)
			continue;
		for (j = 0; j < src->nsets; j++)
		{
			if (!src->sets[j].found)
				continue;
			if (src->sets[j].keyisnull)
				exclnull = true;
			else if (src->sets[j].hasstoredkey)
				excl[nexcl++] = src->sets[j].storedkey;
		}
	}

	/* ---- the entries' own counts: the topkcand largest, and maxout ---- */
	cand = (LionTopkCand *) palloc(sizeof(LionTopkCand) * 2 * cap);
	lion_entry_scan_begin_range(&es, st->groupidx, st->groupidxcol,
								st->hasrange ? &st->range : NULL);
	byval = es.state->typbyval;
	typlen = es.state->typlen;
	for (;;)
	{
		LionEntryTuple *entry;
		Size		itemlen;

		entry = lion_entry_scan_next_copy(&es, &itemlen);
		if (entry == NULL)
			break;
		st->topkwalked++;

		if (LionEntryIsNullKey(entry) ? exclnull :
			lion_topk_excluded(es.state, entry, excl, nexcl))
			continue;

		/* smaller than every one kept, once there are enough */
		if (trimmed && entry->ntids <= floor)
		{
			maxout = Max(maxout, entry->ntids);
			continue;
		}
		cand[n].ntids = entry->ntids;
		cand[n].isnull = LionEntryIsNullKey(entry);
		cand[n].key = cand[n].isnull ? (Datum) 0 :
			datumCopy(lion_entry_key(es.state, entry), byval, typlen);
		if (++n == 2 * cap)
		{
			n = lion_topk_trim(cand, n, cap, byval, &maxout);
			floor = cand[n - 1].ntids;
			trimmed = true;
		}
	}
	lion_entry_scan_end(&es);
	n = lion_topk_trim(cand, n, cap, byval, &maxout);

	/* ---- counted, largest bound first, until the rest cannot matter ---- */
	st->topkkey = (Datum *) palloc(sizeof(Datum) * Max(n, 1));
	st->topknull = (bool *) palloc(sizeof(bool) * Max(n, 1));
	st->topkcount = (int64 *) palloc(sizeof(int64) * Max(n, 1));
	heap = (int64 *) palloc(sizeof(int64) * Min((int64) Max(n, 1),
												 st->topkn));
	st->topkout = 0;
	st->topkpos = 0;
	MemoryContextSwitchTo(oldcxt);

	for (i = 0; i < n && !enough; i++)
	{
		uint64		next = (i + 1 < n) ? cand[i + 1].ntids : maxout;
		int64		count;
		bool		found;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		/*
		 * The entry is looked up again by its key: VACUUM may have deleted it
		 * since the walk read it, which it does only to an entry holding no
		 * row this snapshot sees.
		 */
		if (cand[i].isnull)
			found = lion_posting_set_lookup_null_col(st->groupidx,
													 st->groupidxcol,
													 &st->groupset);
		else
			found = lion_posting_set_lookup_col(st->groupidx,
												st->groupidxcol,
												cand[i].key, InvalidOid,
												&st->groupset);
		count = found ?
			lion_group_count(st, st->nsource, st->sources, st->nitem,
							 st->groupset.ntids, i + 1 < n) : 0;
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
		st->topkcounted++;

		if (count > 0)
		{
			st->topkkey[st->topkout] = cand[i].key;
			st->topknull[st->topkout] = cand[i].isnull;
			st->topkcount[st->topkout] = count;
			st->topkout++;
			lion_topk_push(heap, &nheap, st->topkn, count);
		}

		/*
		 * Enough: no entry is left with a row, or k groups are counted and
		 * none still to come can pass the k-th.
		 */
		if (next == 0)
			enough = true;
		else if (nheap >= st->topkn &&
				 (st->topkstrict ? (uint64) heap[0] > next :
				  (uint64) heap[0] >= next))
			enough = true;
	}
	if (n == 0)
		enough = true;

	if (!enough)
	{
		st->topkwhole = true;
		st->topkwholes++;
		MemoryContextReset(st->topkcxt);
		st->topkkey = NULL;
		st->topknull = NULL;
		st->topkcount = NULL;
		st->topkout = 0;
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
		 * The top k by count (DESIGN.md §36): the groups that can be among
		 * them, counted before the first row goes up - unless that could not
		 * be shown, when the walk below counts every group.
		 */
		if (st->topkn > 0 && !st->topkwhole)
		{
			if (!st->topkran)
				lion_topk_run(st);
			if (!st->topkwhole)
			{
				int			i;

				if (st->topkpos >= st->topkout)
				{
					*exhausted = true;
					return NULL;
				}
				i = st->topkpos++;
				return lion_emit_tuple(st, st->topkkey[i], st->topknull[i],
									   (Datum) 0, true, st->topkcount[i]);
			}
		}

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
 * THE DECODED WALK (DESIGN.md §34): a GROUP BY of three or more columns, each
 * a column of a scalar lion index, counted by lion_count_groups_decode() a
 * pass at a time.  The columns are read a chunk of values at a time off their
 * entry walks, and the chunks advance like an odometer (LionDecodeRun); each
 * pass's tally goes up as partial rows, one a combination with rows, which
 * the Finalize Agg above adds up.
 *
 * Pins (DESIGN.md §9).  Nothing is pinned between two rows: the chunks'
 * sets are kept unpinned (an INLINE one gives its leaf's pin up as it is
 * read, a CHAIN one never held one), the entry walks let go of their leaves
 * once a chunk is read, and column 0's sets are located again for each pass
 * with their pins - the interlock of its counts - and released when it ends.
 */

/*
 * The next chunk of column c's values off its entry walk - its first, begun
 * again, with restart.  False when the walk has none left: the column is past
 * its last chunk.
 */
static bool
lion_decode_fill(LionCountScanState *st, int c, bool restart)
{
	LionDecodeRun *dr = st->decode;
	MemoryContext oldcxt;
	int			cap = dr->cap[c];
	int			n = 0;

	if (restart || !dr->scanning[c])
	{
		if (dr->scanning[c])
			lion_entry_scan_end(&dr->escan[c]);
		/* the walk's own contexts outlive the chunks (lion_next_group2()) */
		oldcxt = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);
		lion_entry_scan_begin_col(&dr->escan[c], dr->idx[c], dr->idxcol[c]);
		MemoryContextSwitchTo(oldcxt);
		dr->scanning[c] = true;
		dr->scandone[c] = false;
	}
	else if (dr->scandone[c])
		return false;

	MemoryContextReset(dr->chunkcxt[c]);
	oldcxt = MemoryContextSwitchTo(dr->chunkcxt[c]);
	dr->keys[c] = (Datum *) palloc(sizeof(Datum) * cap);
	dr->isnull[c] = (bool *) palloc(sizeof(bool) * cap);
	dr->sets[c] = (LionPostingSet *) palloc(sizeof(LionPostingSet) * cap);
	while (n < cap &&
		   lion_entry_scan_next(&dr->escan[c], &dr->keys[c][n], &dr->sets[c][n]))
	{
		dr->isnull[c][n] = dr->sets[c][n].keyisnull;
		lion_posting_set_unpin(&dr->sets[c][n]);
		n++;
	}
	MemoryContextSwitchTo(oldcxt);
	if (n < cap)
		dr->scandone[c] = true;
	lion_entry_scan_pause(&dr->escan[c]);
	dr->nvals[c] = n;
	return n > 0;
}

/*
 * The next combination of chunks, the last column's changing fastest: false
 * once column 0 is past its last chunk.
 */
static bool
lion_decode_advance(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	int			c;

	for (c = dr->ncol - 1; c >= 0; c--)
	{
		if (lion_decode_fill(st, c, false))
		{
			int			d;

			for (d = c + 1; d < dr->ncol; d++)
			{
				if (!lion_decode_fill(st, d, true))
					return false;
			}
			return true;
		}
	}
	return false;
}

/*
 * The chunk sizes: column 0's by the pins and the memory of one count's
 * cursors, as a batch of groups is (lion_count_groups_batch()), since its
 * sets hold pins; every other column's by the memory of a cursor each - a
 * page image for a CHAIN set - shared among them; and all of them so that
 * the combinations of a pass fit a code.
 */
static void
lion_decode_caps(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	Size		per = lion_count_cursor_bytes() + sizeof(PGAlignedBlock) +
		sizeof(LionPostingSet) + 2 * sizeof(Datum);
	Size		budget = Max((Size) work_mem * 1024 / per, (Size) 64);
	double		bits;
	int			c;

	dr->cap[0] = lion_count_groups_batch(dr->idx[0]);
	for (c = 1; c < dr->ncol; c++)
		dr->cap[c] = (int) Max(budget / (Size) (dr->ncol - 1), (Size) 16);
	for (;;)
	{
		int			widest = 0;

		bits = 0;
		for (c = 0; c < dr->ncol; c++)
		{
			bits += log2((double) dr->cap[c]);
			if (dr->cap[c] > dr->cap[widest])
				widest = c;
		}
		if (bits <= 60.0 || dr->cap[widest] <= 1)
			break;
		dr->cap[widest] = Max(dr->cap[widest] / 2, 1);
	}
	for (c = 0; c < dr->ncol; c++)
		dr->images[c] = (PGAlignedBlock *)
			MemoryContextAlloc(st->css.ss.ps.state->es_query_cxt,
							   sizeof(PGAlignedBlock) * dr->cap[c]);
}

/* One pass: every combination of the chunks the columns stand at, tallied. */
static void
lion_decode_pass(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	EState	   *estate = st->css.ss.ps.state;
	LionDecodeCol cols[LION_MAX_GROUPCOLS];
	bool	   *relocated;
	MemoryContext oldcxt;
	int			c;
	int			v;

	MemoryContextReset(dr->passcxt);
	oldcxt = MemoryContextSwitchTo(dr->passcxt);

	/* column 0's sets carry the interlock: located again, with their pins */
	relocated = (bool *) palloc0(sizeof(bool) * Max(dr->nvals[0], 1));
	cols[0].nsets = dr->nvals[0];
	cols[0].images = dr->images[0];
	cols[0].sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) *
											  Max(dr->nvals[0], 1));
	for (v = 0; v < dr->nvals[0]; v++)
	{
		LionPostingSet *ps = &dr->sets[0][v];

		if (ps->found && ps->nopin)
		{
			/*
			 * The entry as it is now, under a pin of its own (the listed
			 * value of lion_next_group_inlist()): one gone meanwhile held no
			 * row anyone sees, since VACUUM deletes only an empty entry (§18).
			 */
			if (ps->keyisnull)
				(void) lion_posting_set_lookup_null_col(dr->idx[0],
														dr->idxcol[0],
														&cols[0].sets[v]);
			else
				(void) lion_posting_set_lookup_col(dr->idx[0], dr->idxcol[0],
												  ps->storedkey, InvalidOid,
												  &cols[0].sets[v]);
			relocated[v] = true;
		}
		else
			cols[0].sets[v] = *ps;
	}
	for (c = 1; c < dr->ncol; c++)
	{
		cols[c].nsets = dr->nvals[c];
		cols[c].sets = dr->sets[c];
		cols[c].images = dr->images[c];
	}

	if (!lion_decode_tally_begin(dr->tally, dr->ncol, dr->nvals))
		elog(ERROR, "LionCount: the combinations of a decoded pass do not fit a code");
	lion_count_groups_decode(st->heap, estate->es_snapshot, dr->ncol, cols,
							 st->wcollected ? &st->wherecoll : NULL,
							 dr->tally, &st->stats, &dr->stats,
							 st->viscache, st->rel_read_only);

	for (v = 0; v < dr->nvals[0]; v++)
	{
		if (relocated[v])
			lion_posting_set_release(&cols[0].sets[v]);
	}
	MemoryContextSwitchTo(oldcxt);
	dr->passes++;
}

/*
 * The run begins: the WHERE collected, every column's first chunk read.
 * False when there is nothing to count at all.
 */
static bool
lion_decode_start(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	int			c;
	int			k;

	if (RecoveryInProgress())
		elog(ERROR, "LionCount: a decoded GROUP BY during recovery");
	for (k = 1; k <= st->nitem; k++)
	{
		if (st->sources[k].rangewalk != NULL)
			elog(ERROR, "LionCount: a decoded GROUP BY beside a range it walks");
	}

	/*
	 * The WHERE, as one copy, read before any column is (DESIGN.md §10, "The
	 * WHERE sets, collected once"), and pinless.  A WHERE whose sources hold
	 * no entry is no copy at all: nothing is counted.
	 */
	if (st->nitem > 0)
	{
		if (!st->wtried)
			lion_where_collect(st, st->sources, st->nitem);
		if (!st->wcollected)
			return false;
	}

	if (dr->cap[0] == 0)
		lion_decode_caps(st);
	for (c = 0; c < dr->ncol; c++)
	{
		if (!lion_decode_fill(st, c, true))
			return false;
	}
	return true;
}

/*
 * The next partial row of the decoded walk: a combination's key and count,
 * for the relation the node has open.  Returns NULL and sets *exhausted once
 * every pass has gone up.
 */
static TupleTableSlot *
lion_next_group_decode(LionCountScanState *st, bool *exhausted)
{
	LionDecodeRun *dr = st->decode;

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
			int			vals[LION_MAX_GROUPCOLS];
			int			c;
			TupleTableSlot *slot;

			if (lion_decode_tally_next(dr->tally, &code, &count))
			{
				lion_decode_code_split(code, dr->ncol, dr->nvals, vals);
				for (c = 0; c < dr->ncol; c++)
				{
					dr->rowkeys[c] = dr->keys[c][vals[c]];
					dr->rownull[c] = dr->isnull[c][vals[c]];
				}
				dr->rowsup++;
				slot = lion_emit_keys(st, dr->ncol, dr->rowkeys, dr->rownull,
									  count);
				if (slot == NULL)
					continue;
				return slot;
			}
			dr->emitting = false;
			lion_decode_tally_end(dr->tally);
			if (!lion_decode_advance(st))
			{
				dr->finished = true;
				continue;
			}
		}
		else if (!dr->started)
		{
			dr->started = true;
			if (!lion_decode_start(st))
			{
				dr->finished = true;
				continue;
			}
		}

		lion_decode_pass(st);
		dr->emitting = true;
	}
}

/*
 * The decoded walk ends, or begins again for a rescan: its entry walks and its
 * tally go, and the next call starts from the first chunks.
 */
void
lion_decode_reset(LionCountScanState *st)
{
	LionDecodeRun *dr = st->decode;
	int			c;

	if (dr == NULL)
		return;
	for (c = 0; c < dr->ncol; c++)
	{
		if (dr->scanning[c])
			lion_entry_scan_end(&dr->escan[c]);
		dr->scanning[c] = false;
		dr->scandone[c] = false;
		dr->nvals[c] = 0;
		if (dr->chunkcxt[c] != NULL)
			MemoryContextReset(dr->chunkcxt[c]);
	}
	if (dr->tally != NULL)
		lion_decode_tally_end(dr->tally);
	if (dr->passcxt != NULL)
		MemoryContextReset(dr->passcxt);
	dr->started = false;
	dr->finished = false;
	dr->emitting = false;
}

/*
 * THE AGGREGATES OVER LION COLUMNS' ENTRIES (DESIGN.md §37).
 *
 * Every row of a table with no WHERE is in exactly one entry of a scalar lion
 * column - the NULL one for a NULL - and holds its key's value when the keys
 * are the rows' own values (the value rule of §10, which the planner asked).
 * So sum(f(x)) is the sum over x's entries of f(key) times the entry's rows,
 * avg the same over their total, and min or max the first f(key) in the
 * aggregate's order among the entries with a row: one walk of each column,
 * however many aggregates are taken over it.
 *
 * An entry's rows are its visible TIDs.  lion_heap_all_visible() says when
 * its ntids is that number (lion_wagg_run()); otherwise each entry is counted
 * as a group of a walk is.
 */

/*
 * Is every page of heap all-visible, and how many are there?  An index TID on
 * an all-visible page is a row every snapshot sees - core's own promise, the
 * one an index-only scan rests on (a page with a dead item is never marked,
 * and a mark is taken away by the first change to the page, before the change
 * reaches any index).
 */
static bool
lion_heap_all_visible(Relation heap, BlockNumber *nblocks)
{
	BlockNumber allvisible;

	*nblocks = RelationGetNumberOfBlocks(heap);
	visibilitymap_count(heap, &allvisible, NULL);
	return allvisible == *nblocks;
}

/*
 * How many bulk deletes index has finished (DESIGN.md §37): the meta page's
 * count, which the end of every ambulkdelete call moves, read under the meta
 * page's share lock - whose acquisition orders this read after the looks at
 * the map that came before it.
 */
static uint32
lion_wagg_bulkdeletes(Relation index)
{
	LionMetaNdistinct nd;

	(void) lion_read_meta_ndistinct(index, &nd);
	return nd.bulkdeletes;
}

#ifdef HAVE_INT128
/* v as a numeric, exactly: in three parts of 10^18 past int8's range */
static Datum
lion_int128_numeric(int128 v)
{
	const int64 base = INT64CONST(1000000000000000000);
	Datum		hi;

	if (v >= (int128) PG_INT64_MIN && v <= (int128) PG_INT64_MAX)
		return NumericGetDatum(int64_to_numeric((int64) v));
	hi = lion_int128_numeric(v / base);
	return DirectFunctionCall2(numeric_add,
							   DirectFunctionCall2(numeric_mul, hi,
												   NumericGetDatum(int64_to_numeric(base))),
							   NumericGetDatum(int64_to_numeric((int64) (v % base))));
}
#endif

/* The aggregates of column c take an entry of key key (isnull) and rows. */
static void
lion_wagg_add(LionCountScanState *st, int c, Datum key, bool isnull,
			  int64 rows)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	bool		slotset = false;
	int			i;

	if (rows <= 0)
		return;
	ResetExprContext(econtext);
	for (i = 0; i < st->nwagg; i++)
	{
		LionWAgg   *a = &st->wagg[i];
		Datum		v;
		bool		vnull;

		if (a->col != c)
			continue;
		if (a->argiskey)
		{
			v = key;
			vnull = isnull;
		}
		else
		{
			/* the key in its column of the scan tuple, the rest NULL */
			if (!slotset)
			{
				int			j;

				ExecClearTuple(slot);
				for (j = 0; j < slot->tts_tupleDescriptor->natts; j++)
				{
					slot->tts_values[j] = (Datum) 0;
					slot->tts_isnull[j] = true;
				}
				slot->tts_values[st->wcol[c].slotcol] = key;
				slot->tts_isnull[st->wcol[c].slotcol] = isnull;
				ExecStoreVirtualTuple(slot);
				econtext->ecxt_scantuple = slot;
				slotset = true;
			}
			v = ExecEvalExprSwitchContext(a->arg, econtext, &vnull);
		}
		if (vnull)
			continue;

		switch (a->kind)
		{
#ifdef HAVE_INT128
			case LION_WAGG_SUM:
			case LION_WAGG_SUM8:
			case LION_WAGG_AVG:
			case LION_WAGG_AVG8:
				{
					int64		iv = (a->argwidth == 2) ? DatumGetInt16(v) :
						(a->argwidth == 4) ? DatumGetInt32(v) : DatumGetInt64(v);

					a->sum += (int128) iv * (int128) rows;
					a->n += rows;
				}
				break;
#endif
			case LION_WAGG_EXTREME:
				if (!a->hasext ||
					DatumGetBool(FunctionCall2Coll(&a->cmp, a->collation, v,
												   a->ext)))
				{
					MemoryContext oldcxt = MemoryContextSwitchTo(st->wcxt);

					if (a->hasext && !a->typbyval)
						pfree(DatumGetPointer(a->ext));
					a->ext = datumCopy(v, a->typbyval, a->typlen);
					a->hasext = true;
					MemoryContextSwitchTo(oldcxt);
				}
				break;
			default:
				elog(ERROR, "LionCount: aggregate kind %d over keys", a->kind);
		}
	}
}

/*
 * The entries the walk from the entries' own counts has read and not yet
 * given to their aggregates, because an aggregate over their column takes an
 * EXPRESSION of the key: it is evaluated on a key only once a look at the map
 * has vouched for the entry's count (lion_wagg_run()), or it could meet the
 * key of a row the snapshot does not see and fail where the sequential scan
 * does not - `sum(100 / k)` on an inserted k = 0 that rolled back.  At most
 * work_mem of them wait.
 */
typedef struct LionWPending
{
	int			col;
	bool		isnull;
	Datum		key;			/* in LionWBatch.cxt */
	int64		rows;
} LionWPending;

typedef struct LionWBatch
{
	MemoryContext cxt;			/* the keys and ent; NULL until the first */
	LionWPending *ent;
	int			n;
	int			max;
	BlockNumber nblocks;		/* the heap's, at the first look */
} LionWBatch;

#define LION_WAGG_MAXPENDING	((int) (MaxAllocSize / sizeof(LionWPending)))

/*
 * Are the counts of every header read so far the snapshot's rows
 * (lion_wagg_run())?  Every heap page all-visible, as many pages as at the
 * first look, and no walked index's count of bulk deletes moved.
 */
static bool
lion_wagg_unchanged(LionCountScanState *st, BlockNumber before)
{
	BlockNumber after;
	int			c;

	if (!lion_heap_all_visible(st->heap, &after) || after != before)
		return false;
	for (c = 0; c < st->nwcol; c++)
	{
		if (lion_wagg_bulkdeletes(st->wcol[c].idx) != st->wcol[c].bulkdeletes)
			return false;
	}
	return true;
}

/* The waiting entries given to their aggregates, once a look has said so. */
static void
lion_wagg_flush(LionCountScanState *st, LionWBatch *b)
{
	int			i;

	for (i = 0; i < b->n; i++)
	{
		LionWPending *p = &b->ent[i];

		CHECK_FOR_INTERRUPTS();
		lion_wagg_add(st, p->col, p->key, p->isnull, p->rows);
	}
	if (b->cxt != NULL)
		MemoryContextReset(b->cxt);
	b->ent = NULL;
	b->n = 0;
	b->max = 0;
}

/*
 * An entry of column c, read by the walk from the entries' counts, to wait
 * for a look; when the waiting ones fill work_mem, the look is taken now and
 * they are given their aggregates.  False when it found the counts no longer
 * the snapshot's.
 */
static bool
lion_wagg_defer(LionCountScanState *st, LionWBatch *b, int c,
				LionState *state, Datum key, bool isnull, int64 rows)
{
	MemoryContext oldcxt;
	LionWPending *p;

	if (b->cxt == NULL)
		b->cxt = AllocSetContextCreate(st->wcxt,
									   "LionCount keys awaiting a look",
									   ALLOCSET_DEFAULT_SIZES);
	if (b->n > 0 &&
		(b->n == LION_WAGG_MAXPENDING ||
		 MemoryContextMemAllocated(b->cxt, false) >= (Size) work_mem * 1024))
	{
		if (!lion_wagg_unchanged(st, b->nblocks))
			return false;
		lion_wagg_flush(st, b);
	}

	oldcxt = MemoryContextSwitchTo(b->cxt);
	if (b->n == b->max)
	{
		b->max = Min(Max(b->max * 2, 64), LION_WAGG_MAXPENDING);
		b->ent = (b->ent == NULL) ?
			(LionWPending *) palloc(sizeof(LionWPending) * b->max) :
			(LionWPending *) repalloc(b->ent, sizeof(LionWPending) * b->max);
	}
	p = &b->ent[b->n++];
	p->col = c;
	p->isnull = isnull;
	p->key = isnull ? (Datum) 0 :
		datumCopy(key, state->typbyval, state->typlen);
	p->rows = rows;
	MemoryContextSwitchTo(oldcxt);
	return true;
}

/*
 * One walk of column c's entries, every one of them the NULL one included:
 * with batch, each entry's rows its ntids - waiting in batch for a look, when
 * an aggregate over the column takes an expression of the key; without, each
 * entry counted.  False when a look the walk took found the counts no longer
 * the snapshot's, and it stopped there.
 */
static bool
lion_wagg_walk(LionCountScanState *st, int c, LionWBatch *batch)
{
	LionWCol   *wc = &st->wcol[c];
	LionEntryScan es;
	MemoryContext oldcxt;
	bool		defer = false;
	bool		ok = true;
	int			i;

	for (i = 0; batch != NULL && i < st->nwagg; i++)
	{
		if (st->wagg[i].col == c && !st->wagg[i].argiskey)
			defer = true;
	}

	lion_entry_scan_begin_col(&es, wc->idx, wc->idxcol);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);
		if (batch != NULL)
		{
			LionEntryTuple *entry;
			Size		itemlen;
			Datum		key;
			bool		isnull;

			entry = lion_entry_scan_next_copy(&es, &itemlen);
			if (entry == NULL)
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}
			isnull = LionEntryIsNullKey(entry);
			key = isnull ? (Datum) 0 : lion_entry_key(es.state, entry);
			if (!defer)
				lion_wagg_add(st, c, key, isnull, (int64) entry->ntids);
			else if (entry->ntids > 0 &&
					 !lion_wagg_defer(st, batch, c, es.state, key, isnull,
									  (int64) entry->ntids))
			{
				MemoryContextSwitchTo(oldcxt);
				ok = false;
				break;
			}
		}
		else
		{
			LionPostingSet ps;
			LionCountSource src;
			Datum		key;
			bool		isnull;
			int64		rows;

			if (!lion_entry_scan_next(&es, &key, &ps))
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}
			memset(&src, 0, sizeof(src));
			src.nsets = 1;
			src.sets = &ps;
			isnull = ps.keyisnull;
			rows = lion_node_count(st, 1, &src, false);
			lion_posting_set_release(&ps);
			lion_wagg_add(st, c, key, isnull, rows);
		}
		st->wentries++;
		MemoryContextSwitchTo(oldcxt);
	}
	lion_entry_scan_end(&es);
	return ok;
}

/* Every aggregate over keys, begun again. */
static void
lion_wagg_reset(LionCountScanState *st)
{
	int			i;

	if (st->wcxt != NULL)
		MemoryContextReset(st->wcxt);
	for (i = 0; i < st->nwagg; i++)
	{
		LionWAgg   *a = &st->wagg[i];

#ifdef HAVE_INT128
		a->sum = 0;
#endif
		a->n = 0;
		a->hasext = false;
		a->ext = (Datum) 0;
		a->result = (Datum) 0;
		a->resnull = true;
	}
}

/*
 * The result of one aggregate of §37 - and of an aggregate over a gathered
 * column (DESIGN.md §40, lion_exec_store.c), which keeps the same state -
 * from its sum, its count of values and its extreme value: into a->result
 * and a->resnull, allocated in the current memory context.
 */
void
lion_wagg_finish(LionWAgg *a)
{
	a->resnull = true;
	switch (a->kind)
	{
#ifdef HAVE_INT128
		case LION_WAGG_SUM:

			/*
			 * sum(int2) and sum(int4) add up in an int8 that wraps; the
			 * sum modulo 2^64 is what the wrapping adds make, in any order.
			 */
			if (a->n > 0)
			{
				a->result = Int64GetDatum((int64) (uint64) a->sum);
				a->resnull = false;
			}
			break;
		case LION_WAGG_SUM8:
			if (a->n > 0)
			{
				a->result = lion_int128_numeric(a->sum);
				a->resnull = false;
			}
			break;
		case LION_WAGG_AVG:
		case LION_WAGG_AVG8:

			/*
			 * int8_avg() and numeric_poly_avg(): the sum over the count,
			 * both numerics - the sum an int8 that wraps for int2 and int4,
			 * exact for int8.
			 */
			if (a->n > 0)
			{
				Datum		sum = (a->kind == LION_WAGG_AVG) ?
					NumericGetDatum(int64_to_numeric((int64) (uint64) a->sum)) :
					lion_int128_numeric(a->sum);

				a->result = DirectFunctionCall2(numeric_div, sum,
												NumericGetDatum(int64_to_numeric(a->n)));
				a->resnull = false;
			}
			break;
#endif
		case LION_WAGG_EXTREME:
			a->result = a->ext;
			a->resnull = !a->hasext;
			break;
	}
}

/*
 * Compute every aggregate over keys (DESIGN.md §37) into its result.
 *
 * The walk reads every row of the table, so it takes the predicate lock a
 * sequential scan takes, on the whole heap, before it reads anything.  The
 * walk from the entries' counts visits no heap page, so it takes no page lock
 * either, and the relation lock lion_open_relation() takes on the node's own
 * index sees an insert but not a DELETE, which reaches no index: without it,
 * two SERIALIZABLE transactions could each read a sum, each delete a row the
 * other's sum counted, and both commit.
 *
 * The entries' own counts are the rows when every heap page is all-visible
 * before the first header is read and after the last, the heap has as many
 * pages both times, and no index walked has finished a bulk delete in
 * between.  At the first look every TID in the index is a row every snapshot
 * sees.  A change made after it - an insert, an update, a delete, by a
 * transaction this snapshot cannot see, since one it sees had made its change
 * before the snapshot and so before the first look - takes the mark off its
 * page before its TID reaches an index.  If it committed, the page cannot be
 * marked again while this snapshot's xmin holds VACUUM back, and a new page is
 * not marked at all.  If it did not - an aborted insert or update, a
 * subtransaction rolled back, a speculative insert killed - its tuple is dead
 * to every snapshot whatever the horizon, and VACUUM may prune it, take its
 * TID out of the index and mark the page again, all after a header that
 * counted the TID was read: the second look alone cannot see that.  But the
 * page is marked only once the TID's line pointer is unused, after a bulk
 * delete of this index that had the TID among its dead ones has returned, and
 * that call ended after the header was read (the TID was still in it) and
 * counted itself on the meta page as it ended (lion_meta_count_bulkdelete()).
 * So the meta page's count, read before the first look and after the second,
 * moved.  When it did not, nothing changed in between, and every ntids read
 * counted exactly the rows of the first look: this snapshot's.
 *
 * Nothing there is particular to the last header: a look that finds the same
 * says it of every header read before it.  Until one has, a header may count
 * the TID of a row the snapshot does not see, and the expression of an
 * aggregate's argument must not meet that row's key, which the sequential scan
 * never evaluates it on: it is evaluated only on entries a look has vouched
 * for (LionWBatch), and when work_mem of them wait, that look is taken there
 * and then.  During recovery the standby's snapshot holds nothing back on the
 * primary, and every entry is counted.
 */
void
lion_wagg_run(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	BlockNumber before;
	bool		fast;
	int			c;
	int			i;

	if (st->wcxt == NULL)
		st->wcxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount aggregates over keys",
										 ALLOCSET_DEFAULT_SIZES);
	for (c = 0; c < st->nwcol; c++)
	{
		if (st->wcol[c].idx == NULL)
			st->wcol[c].idx = index_open(st->wcol[c].idxoid, AccessShareLock);
	}
	PredicateLockRelation(st->heap, estate->es_snapshot);

	lion_wagg_reset(st);
	fast = !RecoveryInProgress();
	if (fast)
	{
		for (c = 0; c < st->nwcol; c++)
			st->wcol[c].bulkdeletes = lion_wagg_bulkdeletes(st->wcol[c].idx);
		fast = lion_heap_all_visible(st->heap, &before);
	}
	if (fast)
	{
		LionWBatch	batch;

		memset(&batch, 0, sizeof(batch));
		batch.nblocks = before;

		/*
		 * Test hook: the map has been looked at and no header read yet, so a
		 * row inserted here, by a transaction that rolls back or commits, is
		 * in the headers the walks read (the isolation spec
		 * wagg_unseen_key).  Compiles to nothing without
		 * --enable-injection-points.
		 */
		LION_INJECTION_POINT("lion-wagg-looked");
		for (c = 0; fast && c < st->nwcol; c++)
			fast = lion_wagg_walk(st, c, &batch);

		/*
		 * Test hook: every header has been read and the map not looked at
		 * since the last, so a VACUUM here may take out TIDs the walks
		 * counted and mark their pages all-visible again (the isolation spec
		 * wagg_aborted_insert).  Compiles to nothing without
		 * --enable-injection-points.
		 */
		LION_INJECTION_POINT("lion-wagg-walked");
		fast = fast && lion_wagg_unchanged(st, before);
		if (fast)
			lion_wagg_flush(st, &batch);
		if (batch.cxt != NULL)
			MemoryContextDelete(batch.cxt);
		st->wfast += st->nwcol;
		if (!fast)
			lion_wagg_reset(st);
	}
	if (!fast)
	{
		for (c = 0; c < st->nwcol; c++)
			(void) lion_wagg_walk(st, c, NULL);
		st->wslow += st->nwcol;
	}

	for (i = 0; i < st->nwagg; i++)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(st->wcxt);

		lion_wagg_finish(&st->wagg[i]);
		MemoryContextSwitchTo(oldcxt);
	}

	for (c = 0; c < st->nwcol; c++)
	{
		index_close(st->wcol[c].idx, AccessShareLock);
		st->wcol[c].idx = NULL;
	}
}

/*
 * Whichever of them the plan asks for.
 */
TupleTableSlot *
lion_next_group_any(LionCountScanState *st, bool *exhausted)
{
	if (st->decode != NULL)
		return lion_next_group_decode(st, exhausted);
	if (st->distattno != 0)
		return lion_next_group_distinct(st, exhausted);
	if (st->groupattno2 != 0)
		return lion_next_group2(st, exhausted);
	if (st->ingroupitem >= 0)
		return lion_next_group_inlist(st, exhausted);
	return lion_next_group(st, exhausted);
}
