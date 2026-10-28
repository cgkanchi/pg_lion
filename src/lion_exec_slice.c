/*-------------------------------------------------------------------------
 *
 * lion_exec_slice.c
 *		The aggregates of bit-sliced columns from their slices (DESIGN.md
 *		§52): count(x), sum(x), avg(x), min(x) and max(x) of a column a lion
 *		index slices (§51), under the node's WHERE - of its one row, or of
 *		each group of a GROUP BY.
 *
 * For each column, its existence slice E - the rows not NULL - is located
 * with its pin and collected with the WHERE's sources, as they stand located
 * for the node's own count, into one pinless copy C0 = E ∧ WHERE
 * (lion_sources_collect()): the sources of every clause, an OR, a multi-key
 * query, a list, a range taken as a source, the negated clauses that
 * subtract - and a range that bounds the sum over its entries, collected for
 * it.  A GROUP BY makes C0 once a relation and, for each group its walk
 * counts, the group's copy C = G ∧ C0 of the group's sets (one, or a pair's
 * two), located by the walk with their pins.  Then, through the count
 * engine's slice interface (lion_slice.c), with C the rows aggregated:
 *
 *		count(x)	= |E ∧ C|
 *		sum(x)		= Σ 2^i |S_i ∧ C| - count(x) · 2^(nbits-1)
 *		avg(x)		= sum(x) / count(x), as core's avg of the type
 *		min, max	the descent from the top bit, beginning from C
 *
 * every slice counted against C a batch at a time in one walk of C's
 * container keys - by the plane kernel where the slices are bitmaps of one
 * width and the key's blocks all-visible - and every step of a descent a
 * counted AND of a slice with the candidates.  A group is a few dozen counted
 * ANDs this way, the PAIRS of DESIGN.md §52.
 *
 * WHY IT IS EXACT (DESIGN.md §9, §51).  C0 and C are copies, made after the
 * snapshot was taken, pinless, and only ever counted ANDed with a slice
 * located with its pin: the argument of the WHERE collected once (§10).  A
 * dead TID a copy still lists is gone from the slice's container, or the
 * slice's page was read before VACUUM's ambulkdelete passed it, so its heap
 * page is not all-visible and the recheck decides; a copy lacks no row the
 * snapshot sees, which was in every index - E, the group's, the WHERE's -
 * before its transaction committed.  A row the snapshot sees is in E and in
 * exactly the bit slices of its value, its insert having gone through every
 * step before it committed.  So every count is the visible rows', and so is
 * every answer made of them.  The counts of the WHERE and of each group beside
 * them are the node's own, as always.
 *
 * On a hot standby nothing is collected (§10), and the planner offers none
 * of this there; a plan that runs there all the same counts every slice
 * beside E, the group's sets and the WHERE's located sources, which is exact
 * and slow.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

#include "access/genam.h"
#include "access/xlog.h"
#include "pgstat.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

/* The aggregates' results, from their columns' counts and extremes. */
static void
lion_slice_results(LionCountScanState *st)
{
	LionSliceState *ss = st->slice;
	MemoryContext oldcxt = MemoryContextSwitchTo(ss->cxt);
	int			i;

	for (i = 0; i < ss->nagg; i++)
	{
		LionSAgg   *a = &ss->agg[i];
		LionSCol   *c = &ss->col[a->col];

		a->result = (Datum) 0;
		a->resnull = true;
		if (a->kind == LION_SAGG_COUNT)
		{
			a->result = Int64GetDatum(c->count);
			a->resnull = false;
			continue;
		}
		if (c->count <= 0)
			continue;			/* NULL over no row, as core's */
		switch (a->kind)
		{
#ifdef HAVE_INT128
			case LION_SAGG_SUM:

				/*
				 * sum(int2) and sum(int4) add up in an int8 that wraps; the
				 * sum modulo 2^64 is what the wrapping adds make, in any
				 * order (DESIGN.md §37, "The results, as core computes
				 * them").
				 */
				a->result = Int64GetDatum((int64) (uint64) c->sum);
				break;
			case LION_SAGG_SUM8:
				a->result = lion_slice_int128_numeric(c->sum);
				break;
			case LION_SAGG_AVG:
			case LION_SAGG_AVG8:

				/*
				 * int8_avg() and numeric_poly_avg(): the sum over the count,
				 * both numerics - the sum an int8 that wraps for int2 and
				 * int4, exact for int8.
				 */
				a->result =
					DirectFunctionCall2(numeric_div,
										(a->kind == LION_SAGG_AVG) ?
										NumericGetDatum(int64_to_numeric((int64) (uint64) c->sum)) :
										lion_slice_int128_numeric(c->sum),
										NumericGetDatum(int64_to_numeric(c->count)));
				break;
#endif
			case LION_SAGG_MIN:
				a->result = lion_slice_value_datum(c->minenc, c->nbits,
												   c->typid);
				break;
			case LION_SAGG_MAX:
				a->result = lion_slice_value_datum(c->maxenc, c->nbits,
												   c->typid);
				break;
			default:
				elog(ERROR, "LionCount: slice aggregate kind %d", a->kind);
		}
		a->resnull = false;
	}
	MemoryContextSwitchTo(oldcxt);
}

/*
 * No row: every column's count 0 and every other answer NULL - a WHERE that
 * selects nothing (st->wheremissing), which locates no set to count.
 */
void
lion_slice_none(LionCountScanState *st)
{
	LionSliceState *ss = st->slice;
	int			c;

	MemoryContextReset(ss->cxt);
	for (c = 0; c < ss->ncol; c++)
		ss->col[c].count = 0;
	lion_slice_results(st);
}

/*
 * E of column col's index idx, located into *e with its pin, and the
 * sources of E ∧ WHERE into srcs (st->nitem + 2 of them at most): E, the
 * WHERE's sources as they stand located - but an IN list on the grouping
 * column that drives the groups (lion_locate_where()), whose values ARE the
 * groups, and is no source of a group's rows - and a range that bounds the
 * sum over its entries, collected into *range (*hasrange).  The number of
 * sources, or 0 when E has no entry: the column has no row not NULL.
 */
static int
lion_slice_where_sources(LionCountScanState *st, LionSCol *col, Relation idx,
						 LionPostingSet *e, LionPostingSet *range,
						 bool *hasrange, LionCountSource *srcs)
{
	int			n = 0;
	int			k;

	*hasrange = false;
	if (!lion_posting_set_lookup_slice_col(idx, col->idxcol, LION_SLICE_EXISTS,
										   e))
	{
		lion_posting_set_release(e);
		return 0;
	}
	memset(srcs, 0, sizeof(LionCountSource) * (st->nitem + 2));
	srcs[n].nsets = 1;
	srcs[n++].sets = e;
	for (k = 0; k < st->nitem; k++)
	{
		if (k == st->ingroupitem)
			continue;
		if (st->sources[k + 1].rangewalk != NULL ||
			st->sources[k + 1].compl != NULL)
			elog(ERROR, "LionCount: a slice aggregate beside a WHERE it cannot collect");
		srcs[n++] = st->sources[k + 1];
	}

	/*
	 * A range bounding the sum over its entries (DESIGN.md §28): its rows as
	 * one more source, collected as a range taken as a source is (§32) -
	 * pinless, beside E's pin - and in windows past the memory.  A range
	 * that bounds a GROUP BY's walk bounds its groups, which are each
	 * within it: no source.
	 */
	if (lion_plan_flag(st, LION_FLAG_RANGE) &&
		lion_plan_flag(st, LION_FLAG_SUMALL))
	{
		Size		limit = get_hash_memory_limit();
		Size		held = 0;
		bool		spilled;
		int64		nsets;
		int64		nsummaries;

		if (RecoveryInProgress())
			elog(ERROR, "LionCount: slice aggregates over a summed range during recovery");
		if (!lion_range_collect(st->groupidx, st->groupidxcol, &st->range,
								(st->rangesrc_held < limit) ?
								limit - st->rangesrc_held : 0,
								true, range, &held, &spilled, &nsets,
								&nsummaries))
			elog(ERROR, "LionCount: the range of a slice aggregate could not be collected");
		*hasrange = true;
		srcs[n].nsets = 1;
		srcs[n++].sets = range;
	}
	return n;
}

/* The sources collected into *out, which may spill. */
static void
lion_slice_collect_into(LionCountScanState *st, LionSliceAgg *sa, int n,
						LionCountSource *srcs, LionPostingSet *out)
{
	Size		limit = get_hash_memory_limit();
	bool		spilled;

	if (!lion_sources_collect(st->heap, st->css.ss.ps.state->es_snapshot, n,
							  srcs,
							  (st->rangesrc_held < limit) ?
							  limit - st->rangesrc_held : 0,
							  true, st->filter, out, &spilled, &sa->stats))
		elog(ERROR, "LionCount: the WHERE of a slice aggregate could not be collected");
	st->slice->copies++;
}

/*
 * The answers of column col over the rows of f, which hold no row the column
 * has NULL in: |E ∧ f|, every bit slice's count for a sum, the descents.
 */
static void
lion_slice_answer(LionSliceAgg *sa, LionSCol *col, LionSliceFilter *f)
{
	int64	   *counts;

	counts = (int64 *) palloc0(sizeof(int64) * LION_SLICE_NPOS(col->nbits));
	lion_slice_counts(sa, f, (col->want & LION_SWANT_SUM) != 0, counts);
	col->count = counts[col->nbits];
#ifdef HAVE_INT128
	if ((col->want & LION_SWANT_SUM) != 0)
		col->sum = lion_slice_counts_sum(counts, col->nbits);
#endif
	if (col->count > 0 && (col->want & LION_SWANT_MIN) != 0)
		col->minenc = lion_slice_extreme(sa, f, false, col->count);
	if (col->count > 0 && (col->want & LION_SWANT_MAX) != 0)
		col->maxenc = lion_slice_extreme(sa, f, true, col->count);
	pfree(counts);
}

/*
 * A GROUP BY's E ∧ WHERE of column col, C0, collected once a relation into
 * the relation's memory (col->c0; none, col->hasc0 false, where the column
 * has no row not NULL) - the copy every group's answers start from, a group
 * at a time (lion_slice_column()) or a batch (lion_slice_batch()).
 */
static void
lion_slice_c0(LionCountScanState *st, LionSCol *col, Relation idx,
			  LionSliceAgg *sa)
{
	LionSliceState *ss = st->slice;
	LionCountSource *srcs;
	LionPostingSet e;
	LionPostingSet range;
	bool		hasrange = false;
	MemoryContext oldcxt;
	int			n;

	if (col->c0done)
		return;
	srcs = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * (st->nitem + 2));
	sa->located++;
	n = lion_slice_where_sources(st, col, idx, &e, &range, &hasrange, srcs);
	col->c0done = true;
	if (n == 0)
	{
		sa->absent++;
		col->hasc0 = false;
	}
	else
	{
		oldcxt = MemoryContextSwitchTo(ss->relcxt);
		lion_slice_collect_into(st, sa, n, srcs, &col->c0);
		MemoryContextSwitchTo(oldcxt);
		col->hasc0 = true;
		lion_posting_set_release(&e);
		if (hasrange)
			lion_posting_set_release(&range);
	}
	pfree(srcs);
}

/*
 * One column's answers, the WHERE located as the node located it for its own
 * count: of the one row (ngset 0), or of the group whose sets gsets[] are -
 * located by the GROUP BY's walk with their pins, one for a column's group,
 * two for a pair (DESIGN.md §20).  See the file's header.
 */
static void
lion_slice_column(LionCountScanState *st, LionSCol *col, int ngset,
				  LionPostingSet *const *gsets)
{
	EState	   *estate = st->css.ss.ps.state;
	LionSliceState *ss = st->slice;
	Snapshot	snapshot = estate->es_snapshot;
	Relation	idx;
	LionState  *state;
	LionSliceAgg sa;
	LionSliceFilter f;
	LionPostingSet e;
	LionPostingSet range;
	LionPostingSet copy;
	LionCountSource *srcs;
	bool		hasrange = false;
	bool		hascopy = false;
	bool		hase = false;
	int			n;
	int			g;

	/* a partition's is open for the node's life (lion_open_parts()) */
	idx = (col->idx != NULL) ? col->idx :
		index_open(col->idxoid, AccessShareLock);
	state = lion_index_column_state(idx, col->idxcol);
	if (!state->sliced || lion_slice_type_nbits(state->typid) != state->slicebits)
		elog(ERROR, "LionCount: key column %d of lion index \"%s\" is not sliced",
			 col->idxcol, RelationGetRelationName(idx));
	col->typid = state->typid;
	col->nbits = state->slicebits;
	col->count = 0;
#ifdef HAVE_INT128
	col->sum = 0;
#endif

	lion_slice_agg_begin(&sa, st->heap, idx, col->idxcol, snapshot,
						 st->viscache, st->rel_read_only);
	memset(&f, 0, sizeof(f));
	srcs = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * (st->nitem + 2 + ngset));

	if (RecoveryInProgress())
	{
		/*
		 * A standby, which collects nothing: every count reads E, the group's
		 * sets and the WHERE's sources themselves, E with its pin.
		 */
		sa.located++;
		n = lion_slice_where_sources(st, col, idx, &e, &range, &hasrange,
									 srcs + ngset);
		if (n == 0)
		{
			sa.absent++;
			goto done;
		}
		hase = true;
		for (g = 0; g < ngset; g++)
		{
			srcs[g].nsets = 1;
			srcs[g].sets = gsets[g];
		}
		f.nsrc = ngset + n;
		f.srcs = srcs;
	}
	else if (ngset == 0)
	{
		/* one row: E ∧ WHERE, collected now */
		sa.located++;
		n = lion_slice_where_sources(st, col, idx, &e, &range, &hasrange,
									 srcs);
		if (n == 0)
		{
			sa.absent++;
			goto done;
		}
		lion_slice_collect_into(st, &sa, n, srcs, &copy);
		hascopy = true;
		lion_posting_set_release(&e);
		if (hasrange)
			lion_posting_set_release(&range);
		hasrange = false;
		f.copy = &copy;
		f.inexists = true;
	}
	else
	{
		/*
		 * A group: E ∧ WHERE once a relation, in the relation's memory, and
		 * the group's own copy of it.
		 */
		lion_slice_c0(st, col, idx, &sa);
		if (!col->hasc0)
			goto done;
		/* the slots C0's sources took, each a plain set again */
		memset(srcs, 0, sizeof(LionCountSource) * (st->nitem + 2 + ngset));
		for (g = 0; g < ngset; g++)
		{
			srcs[g].nsets = 1;
			srcs[g].sets = gsets[g];
		}
		srcs[ngset].nsets = 1;
		srcs[ngset].sets = &col->c0;
		lion_slice_collect_into(st, &sa, ngset + 1, srcs, &copy);
		hascopy = true;
		f.copy = &copy;
		f.inexists = true;
	}

	lion_slice_answer(&sa, col, &f);

	if (hascopy)
		lion_posting_set_release(&copy);
	if (hase)
		lion_posting_set_release(&e);
	if (hasrange)
		lion_posting_set_release(&range);

done:
	ss->located += sa.located;
	ss->absent += sa.absent;
	ss->counts += sa.counts;
	ss->containers += sa.walk.containers;
	ss->kernelkeys += sa.walk.kernelkeys;
	ss->kernelplanes += sa.walk.kernelplanes;
	ss->steps += sa.steps;
	ss->collects += sa.collects;
	ss->tidsrechecked += sa.stats.tids_rechecked;
	ss->blocksrechecked += sa.stats.blocks_rechecked;
	lion_slice_agg_end(&sa);
	pfree(srcs);

	/* a read of the index's slices (pg_stat_user_indexes) */
	pgstat_count_index_scan(idx);
	pgstat_count_index_tuples(idx, col->count);
	if (col->idx == NULL)
		index_close(idx, AccessShareLock);
}

/* Every column's answers, and so every aggregate's: see the callers. */
static void
lion_slice_columns(LionCountScanState *st, int ngset,
				   LionPostingSet *const *gsets)
{
	LionSliceState *ss = st->slice;
	MemoryContext runcxt;
	MemoryContext oldcxt;
	int			c;

	runcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "LionCount slice aggregates run",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(runcxt);
	for (c = 0; c < ss->ncol; c++)
	{
		CHECK_FOR_INTERRUPTS();
		lion_slice_column(st, &ss->col[c], ngset, gsets);
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(runcxt);
	lion_slice_results(st);
}

/*
 * The one row's answers, for the WHERE located now (lion_locate_where()) -
 * before the node hands up its row, while the WHERE's sets still hold their
 * pins.
 */
void
lion_slice_run(LionCountScanState *st)
{
	LionSliceState *ss = st->slice;

	Assert(ss != NULL && st->located);
	MemoryContextReset(ss->cxt);
	if (st->wheremissing)
	{
		lion_slice_none(st);
		return;
	}
	ss->runs++;
	lion_slice_columns(st, 0, NULL);
}

/*
 * THE ONE ROW OF A PARTITIONED TABLE (DESIGN.md §52.13).
 * The node counts each live leaf partition in its turn (§16), its WHERE
 * located in it, so the partitions' answers are added up here as their
 * turns come - nothing like a partial aggregate goes up: every column's rows
 * with a value counted, their 128-bit sums added, the least of their least
 * encodings and the greatest of their greatest kept - and the one row is
 * made of the totals once the last partition has had its turn, as the one
 * row of a table is made of its own (lion_slice_results()).  An encoding
 * orders as the values do (§51.1), and every partition's index slices the
 * column as one type at one width, which the planner chose them for and
 * which is checked again here.  A partition whose WHERE selects nothing, or
 * whose column has no row not NULL, adds nothing: a table of such
 * partitions, or of none, answers 0 and NULLs, as core's aggregates do over
 * no row.
 */
void
lion_slice_parts_begin(LionCountScanState *st)
{
	LionSliceState *ss = st->slice;
	int			c;

	Assert(ss != NULL && st->npart > 0);
	MemoryContextReset(ss->cxt);
	for (c = 0; c < ss->ncol; c++)
	{
		LionSCol   *col = &ss->col[c];

		col->tcount = 0;
#ifdef HAVE_INT128
		col->tsum = 0;
#endif
		col->tminenc = 0;
		col->tmaxenc = 0;
		col->ttypid = InvalidOid;
		col->tnbits = 0;
	}
	ss->parts = true;
	ss->runs++;
}

/*
 * The answers of the partition whose turn it is, its WHERE located now
 * (lion_locate_where()) and its sets still pinned, added to the totals.
 */
void
lion_slice_part(LionCountScanState *st)
{
	LionSliceState *ss = st->slice;
	MemoryContext runcxt;
	MemoryContext oldcxt;
	int			c;

	Assert(ss != NULL && ss->parts && st->located);
	if (st->wheremissing)
		return;
	runcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "LionCount slice aggregates run",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(runcxt);
	for (c = 0; c < ss->ncol; c++)
	{
		LionSCol   *col = &ss->col[c];

		CHECK_FOR_INTERRUPTS();
		if (col->idx == NULL)
			elog(ERROR, "LionCount: a partition without the index of a sliced column");
		lion_slice_column(st, col, 0, NULL);
		if (col->tnbits != 0 &&
			(col->typid != col->ttypid || col->nbits != col->tnbits))
			elog(ERROR, "LionCount: the partitions slice column %d as different types",
				 (int) col->attno);
		col->ttypid = col->typid;
		col->tnbits = col->nbits;
		if (col->count <= 0)
			continue;
#ifdef HAVE_INT128
		col->tsum += col->sum;
#endif
		if (col->tcount == 0 || col->minenc < col->tminenc)
			col->tminenc = col->minenc;
		if (col->tcount == 0 || col->maxenc > col->tmaxenc)
			col->tmaxenc = col->maxenc;
		col->tcount += col->count;
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(runcxt);
}

/* ... and the one row's answers, from the totals of every partition. */
void
lion_slice_parts_end(LionCountScanState *st)
{
	LionSliceState *ss = st->slice;
	int			c;

	Assert(ss != NULL && ss->parts);
	for (c = 0; c < ss->ncol; c++)
	{
		LionSCol   *col = &ss->col[c];

		col->count = col->tcount;
#ifdef HAVE_INT128
		col->sum = col->tsum;
#endif
		col->minenc = col->tminenc;
		col->maxenc = col->tmaxenc;
		if (col->tnbits != 0)
		{
			col->typid = col->ttypid;
			col->nbits = col->tnbits;
		}
	}
	ss->parts = false;
	MemoryContextReset(ss->cxt);
	lion_slice_results(st);
}

/*
 * A group's answers (DESIGN.md §52, "GROUP BY"): of the rows of gsets[] - the
 * group's set, located by the walk with its pin, or a pair's two (§20) - that
 * the WHERE keeps.  Called once the group's count has found it a row, and
 * before its sets are released; the answers are the next row's, until the
 * next call.
 */
void
lion_slice_group(LionCountScanState *st, int ngset,
				 LionPostingSet *const *gsets)
{
	LionSliceState *ss = st->slice;

	Assert(ss != NULL && st->located && ngset > 0);
	MemoryContextReset(ss->cxt);
	ss->runs++;
	ss->groups++;
	lion_slice_columns(st, ngset, gsets);
}

/*
 * One column's answers for a batch of n groups, decoded (DESIGN.md §52, "The
 * decode route"): the column's slices - every bit's and E - located and made
 * pinless, and one walk of the groups' sets - located by the GROUP BY's walk
 * with their pins - and the collected WHERE (`where`, NULL for none), which
 * reads every row's value off the slices (lion_count_groups_slices()).
 * out's arrays are n each.
 */
static void
lion_slice_column_batch(LionCountScanState *st, LionSCol *col, int n,
						LionPostingSet *gsets, PGAlignedBlock *images,
						const LionPostingSet *where, LionSliceGroupsOut *out)
{
	EState	   *estate = st->css.ss.ps.state;
	LionSliceState *ss = st->slice;
	Relation	idx;
	LionState  *state;
	LionSliceAgg sa;
	LionPostingSet *slices;
	LionDecodeStats dstats;
	int			i;
	int			g;

	idx = (col->idx != NULL) ? col->idx :
		index_open(col->idxoid, AccessShareLock);
	state = lion_index_column_state(idx, col->idxcol);
	if (!state->sliced || lion_slice_type_nbits(state->typid) != state->slicebits)
		elog(ERROR, "LionCount: key column %d of lion index \"%s\" is not sliced",
			 col->idxcol, RelationGetRelationName(idx));
	col->typid = state->typid;
	col->nbits = state->slicebits;

	lion_slice_agg_begin(&sa, st->heap, idx, col->idxcol,
						 estate->es_snapshot, st->viscache,
						 st->rel_read_only);
	memset(&dstats, 0, sizeof(dstats));
	for (g = 0; g < n; g++)
		out->count[g] = 0;

	/*
	 * Every bit's slice and E, pinless: they label the rows of the groups'
	 * pinned sets.
	 */
	slices = (LionPostingSet *)
		palloc0(sizeof(LionPostingSet) * (col->nbits + 1));
	for (i = 0; i <= col->nbits; i++)
	{
		sa.located++;
		if (lion_posting_set_lookup_slice_col(idx, col->idxcol,
											  lion_slice_index_pos(i, col->nbits),
											  &slices[i]))
			lion_posting_set_unpin(&slices[i]);
		else
		{
			lion_posting_set_release(&slices[i]);
			memset(&slices[i], 0, sizeof(LionPostingSet));
			sa.absent++;
		}
	}
	lion_count_groups_slices(st->heap, estate->es_snapshot, n, gsets, images,
							 where, col->nbits, slices, out, &sa.stats,
							 &dstats, st->viscache, st->rel_read_only);
	for (i = 0; i <= col->nbits; i++)
	{
		if (slices[i].found)
			lion_posting_set_release(&slices[i]);
	}
	pfree(slices);

	ss->located += sa.located;
	ss->absent += sa.absent;
	ss->tidsrechecked += sa.stats.tids_rechecked;
	ss->blocksrechecked += sa.stats.blocks_rechecked;
	ss->decodekeys += dstats.keys;
	ss->decoderows += dstats.rows;
	ss->decoderechecked += dstats.rechecked;
	lion_slice_agg_end(&sa);

	pgstat_count_index_scan(idx);
	if (col->idx == NULL)
		index_close(idx, AccessShareLock);
}

/*
 * The answers of a batch of n groups (DESIGN.md §52, "The decode route"):
 * the groups whose sets gsets[] are, located by the GROUP BY's walk with
 * their pins, counted already - gcounts[] - or, with count, counted by the
 * first column's walk into gcounts[], under the WHERE collected as `where`
 * (NULL with no WHERE): each column's answers for all of them from one walk
 * (lion_slice_column_batch()), and every group with a row
 * given its row's answers, copied into cxt as lion_slice_keep() copies them
 * (values[g], isnull[g]; a group with no row is given none).  Called before
 * the sets are released.
 */
void
lion_slice_batch(LionCountScanState *st, int n, LionPostingSet *gsets,
				 int64 *gcounts, bool count, PGAlignedBlock *images,
				 const LionPostingSet *where, MemoryContext cxt,
				 Datum **values, bool **isnull)
{
	LionSliceState *ss = st->slice;
	LionSliceGroupsOut *outs;
	MemoryContext runcxt;
	MemoryContext oldcxt;
	int			c;
	int			g;

	Assert(ss != NULL && st->located && n > 0);
	runcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "LionCount slice aggregates batch",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(runcxt);
	outs = (LionSliceGroupsOut *) palloc0(sizeof(LionSliceGroupsOut) * ss->ncol);
	for (c = 0; c < ss->ncol; c++)
	{
		LionSCol   *col = &ss->col[c];
		LionSliceGroupsOut *out = &outs[c];

		CHECK_FOR_INTERRUPTS();
		out->count = (int64 *) palloc0(sizeof(int64) * n);
		/* the groups' own counts too, from the first column's walk */
		if (count && c == 0)
			out->allcount = gcounts;
#ifdef HAVE_INT128
		if ((col->want & LION_SWANT_SUM) != 0)
			out->sumenc = (int128 *) palloc0(sizeof(int128) * n);
#endif
		if ((col->want & LION_SWANT_MIN) != 0)
			out->minenc = (uint64 *) palloc0(sizeof(uint64) * n);
		if ((col->want & LION_SWANT_MAX) != 0)
			out->maxenc = (uint64 *) palloc0(sizeof(uint64) * n);
		lion_slice_column_batch(st, col, n, gsets, images, where, out);
	}
	MemoryContextSwitchTo(oldcxt);

	/* each group's answers, as the one row's are made (lion_slice_results()) */
	for (g = 0; g < n; g++)
	{
		values[g] = NULL;
		isnull[g] = NULL;
		if (gcounts[g] == 0)
			continue;
		MemoryContextReset(ss->cxt);
		for (c = 0; c < ss->ncol; c++)
		{
			LionSCol   *col = &ss->col[c];
			LionSliceGroupsOut *out = &outs[c];

			col->count = out->count[g];
#ifdef HAVE_INT128
			col->sum = 0;
			if (out->sumenc != NULL)
				col->sum = out->sumenc[g] -
					(int128) col->count * ((int128) 1 << (col->nbits - 1));
#endif
			if (out->minenc != NULL)
				col->minenc = out->minenc[g];
			if (out->maxenc != NULL)
				col->maxenc = out->maxenc[g];
		}
		lion_slice_results(st);
		lion_slice_keep(st, cxt, &values[g], &isnull[g]);
		ss->groups++;
	}
	ss->runs++;
	ss->batches++;
	MemoryContextDelete(runcxt);
}

/*
 * The answers of the group just made, copied into cxt - *values and *isnull,
 * one an aggregate - for a batch of groups whose rows go up later
 * (lion_group_batch_fill()); and put back as the answers before its row goes
 * up.
 */
void
lion_slice_keep(LionCountScanState *st, MemoryContext cxt, Datum **values,
				bool **isnull)
{
	LionSliceState *ss = st->slice;
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);
	int			i;

	*values = (Datum *) palloc(sizeof(Datum) * Max(ss->nagg, 1));
	*isnull = (bool *) palloc(sizeof(bool) * Max(ss->nagg, 1));
	for (i = 0; i < ss->nagg; i++)
	{
		LionSAgg   *a = &ss->agg[i];
		bool		varlena = (a->kind == LION_SAGG_SUM8 ||
							   a->kind == LION_SAGG_AVG ||
							   a->kind == LION_SAGG_AVG8);

		(*isnull)[i] = a->resnull;
		(*values)[i] = a->resnull ? (Datum) 0 :
			datumCopy(a->result, !varlena, varlena ? -1 : (int) sizeof(Datum));
	}
	MemoryContextSwitchTo(oldcxt);
}

void
lion_slice_put(LionCountScanState *st, const Datum *values, const bool *isnull)
{
	LionSliceState *ss = st->slice;
	int			i;

	for (i = 0; i < ss->nagg; i++)
	{
		ss->agg[i].result = values[i];
		ss->agg[i].resnull = isnull[i];
	}
}

/*
 * The copies of E ∧ WHERE a GROUP BY made for its relation, released with
 * the WHERE they were made of (lion_release_where()).
 */
void
lion_slice_release(LionCountScanState *st)
{
	LionSliceState *ss = st->slice;
	int			c;

	if (ss == NULL)
		return;
	for (c = 0; c < ss->ncol; c++)
	{
		LionSCol   *col = &ss->col[c];

		if (col->hasc0)
			lion_posting_set_release(&col->c0);
		col->hasc0 = false;
		col->c0done = false;
	}
	if (ss->relcxt != NULL)
		MemoryContextReset(ss->relcxt);
}
