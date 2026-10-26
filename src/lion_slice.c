/*-------------------------------------------------------------------------
 *
 * lion_slice.c
 *		The counts and the descent of a bit-sliced key column's slices
 *		(DESIGN.md §51, §52), which the LionCount node and the SQL function
 *		lion_index_slice_agg() both answer count, sum, avg, min and max of a
 *		sliced column under a filter from; the function itself; and what
 *		the planner reads of a column's slices.
 *
 * A sliced column keeps one posting set per bit of its values' encoding
 * (lion_slice_enc.h) - slice i holds the rows whose encoded value has bit i
 * set - and an existence slice E of the rows not NULL.  With F a set of rows,
 * all of them counted under the caller's snapshot:
 *
 *		count(x)	= |E ∧ F|
 *		sum(x)		= Σ 2^i |S_i ∧ F|  -  count(x) · 2^(nbits-1)
 *		max(x)		the greedy descent from the top bit: the candidates C
 *					start as E ∧ F, and bit i of the answer is set exactly when
 *					|C ∧ S_i| > 0, which then narrows C to C ∧ S_i
 *		min(x)		the same with the bits clear: bit i is clear when
 *					|C| - |C ∧ S_i| > 0, which narrows C to C ∧ ¬S_i
 *
 * (O'Neil and Quass 1997, "Improved query performance with variant indexes";
 * Rinfret, O'Neil and O'Neil 2001, "Bit-sliced index arithmetic").  Every
 * count is the count engine's (DESIGN.md §9): the slice is a set located with
 * its pin, which carries the visibility-map interlock, ANDed with a pinless
 * copy - F collected once (the function's filter, as lion_index_facets()
 * collects it; the node's E ∧ WHERE), or the descent's candidates - and every
 * TID on a page the map does not vouch for is decided in the heap under the
 * snapshot.  So each count is exact, and so is each answer made of them.
 *
 * The slices are counted a batch at a time against F's copy in one walk of
 * its container keys (lion_count_groups_copy_ext()), and at a key whose
 * every block is all-visible the slices' BITSETs and NARROWs of one width are
 * counted together by the plane kernel (DESIGN.md §50): one strided pass of
 * lion_planes_count_strided() over their payloads against the copy's image
 * at that width - the rest each by lion_container_and_cardinality().
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

PG_FUNCTION_INFO_V1(lion_index_slice_agg);
PG_FUNCTION_INFO_V1(lion_debug_slice_insert);

/* pg_lion.enable_slice_kernel (lion_am.c) */
bool		lion_enable_slice_kernel = true;

/* ---------------------------------------------------------------------
 * A run over one column
 * --------------------------------------------------------------------- */

void
lion_slice_agg_begin(LionSliceAgg *sa, Relation heap, Relation index,
					 AttrNumber attno, Snapshot snapshot, LionVisCache *cache,
					 bool rel_read_only)
{
	LionState  *state = lion_index_column_state(index, attno);

	memset(sa, 0, sizeof(LionSliceAgg));
	if (!state->sliced)
		elog(ERROR, "lion index \"%s\": key column %d has no slices",
			 RelationGetRelationName(index), attno);
	sa->heap = heap;
	sa->index = index;
	sa->attno = attno;
	sa->nbits = state->slicebits;
	sa->snapshot = snapshot;
	sa->cache = cache;
	sa->rel_read_only = rel_read_only;
	sa->kernel = lion_enable_slice_kernel;
	sa->walk.kernel = sa->kernel;
	sa->cxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion index slice aggregate",
									ALLOCSET_DEFAULT_SIZES);
	sa->stepcxt = AllocSetContextCreate(sa->cxt, "lion index slice count",
										ALLOCSET_DEFAULT_SIZES);
	sa->batch = Min(lion_count_groups_batch(index), LION_SLICE_NPOS(sa->nbits));
	sa->images = (PGAlignedBlock *)
		MemoryContextAlloc(sa->cxt, sizeof(PGAlignedBlock) * sa->batch);
}

void
lion_slice_agg_end(LionSliceAgg *sa)
{
	if (sa->cxt != NULL)
		MemoryContextDelete(sa->cxt);
	sa->cxt = NULL;
	sa->stepcxt = NULL;
	sa->candcxt = NULL;
	sa->images = NULL;
}

/* A slice of the column, located with its pin; false when it has no entry. */
static bool
lion_slice_locate(LionSliceAgg *sa, int pos, LionPostingSet *ps)
{
	sa->located++;
	if (lion_posting_set_lookup_slice_col(sa->index, sa->attno, pos, ps))
		return true;
	lion_posting_set_release(ps);
	sa->absent++;
	return false;
}

/*
 * The rows of a located set that the snapshot sees, ANDed with `with` - a
 * collected, pinless copy (the filter's, or the descent's candidates) - or,
 * when `with` is NULL, with the filter as it stands: its copy, its sources,
 * or nothing.  The set carries the §9 interlock with its pin.
 */
static int64
lion_slice_count_set(LionSliceAgg *sa, LionPostingSet *set,
					 const LionSliceFilter *f, const LionPostingSet *with)
{
	LionCountSource *src;
	int			n = 1;
	int			i;

	CHECK_FOR_INTERRUPTS();
	src = (LionCountSource *) palloc0(sizeof(LionCountSource) *
									  (2 + Max(f->nsrc, 0)));
	src[0].nsets = 1;
	src[0].sets = set;
	if (with != NULL || f->copy != NULL)
	{
		src[1].nsets = 1;
		src[1].sets = (LionPostingSet *) (with != NULL ? with : f->copy);
		n = 2;
	}
	else
	{
		for (i = 0; i < f->nsrc; i++)
			src[n++] = f->srcs[i];
	}
	sa->counts++;
	return lion_count_sources_cached(sa->heap, sa->snapshot, n, src,
									 &sa->stats, sa->cache, sa->rel_read_only);
}

/*
 * counts[i] = |S_i ∧ F| for every bit slice i of the column, and
 * counts[nbits] = |E ∧ F|, the rows not NULL - or, without `bits`, E's alone
 * and the bit slices' 0: each slice located with its pin and, against a
 * collected F, counted a batch at a time in one walk of F's container keys
 * (lion_count_groups_copy_ext(); DESIGN.md §10, "The groups of a walk,
 * counted together"), as lion_index_facets() counts its keys, by the plane
 * kernel at the keys it can (DESIGN.md §52).  Without a copy, each slice is
 * counted on its own beside F's sources.  A slice with no entry has no row.
 */
void
lion_slice_counts(LionSliceAgg *sa, const LionSliceFilter *f, bool bits,
				  int64 *counts)
{
	int			npos = LION_SLICE_NPOS(sa->nbits);
	int			first = bits ? 0 : sa->nbits;
	LionPostingSet *sets;
	int		   *which;
	int64	   *bcounts;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(sa->cxt);
	sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) * sa->batch);
	which = (int *) palloc(sizeof(int) * sa->batch);
	bcounts = (int64 *) palloc(sizeof(int64) * sa->batch);
	MemoryContextSwitchTo(oldcxt);

	memset(counts, 0, sizeof(int64) * npos);
	for (; first < npos; first += sa->batch)
	{
		int			last = Min(first + sa->batch, npos);
		int			nfound = 0;
		int			i;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(sa->stepcxt);
		oldcxt = MemoryContextSwitchTo(sa->stepcxt);
		for (i = first; i < last; i++)
		{
			if (lion_slice_locate(sa, lion_slice_index_pos(i, sa->nbits),
								  &sets[nfound]))
				which[nfound++] = i;
		}

		if (nfound > 0 && f->copy != NULL)
		{
			lion_count_groups_copy_ext(sa->heap, sa->snapshot, nfound, sets,
									   f->copy, bcounts, &sa->stats,
									   sa->cache, sa->rel_read_only,
									   sa->images, &sa->walk);
			sa->counts += nfound;
			for (i = 0; i < nfound; i++)
				counts[which[i]] = bcounts[i];
		}
		else
		{
			for (i = 0; i < nfound; i++)
				counts[which[i]] = lion_slice_count_set(sa, &sets[i], f, NULL);
		}
		for (i = 0; i < nfound; i++)
			lion_posting_set_release(&sets[i]);
		MemoryContextSwitchTo(oldcxt);
	}
	MemoryContextReset(sa->stepcxt);
	pfree(sets);
	pfree(which);
	pfree(bcounts);
}

/* ---------------------------------------------------------------------
 * The descent (min and max)
 * --------------------------------------------------------------------- */

/*
 * The candidates of a descent: on a primary a collected copy of them, which
 * every step narrows by collecting it again with the slice that decided the
 * step (lion_sources_collect(), pinless, as the filter's own copy is) - the
 * first of them the filter's own copy where that holds E ∧ F already, which
 * the descent does not own; on a hot standby, where nothing is collected,
 * the slices that decided the steps so far, each positive or negated, which
 * every count intersects again beside E and the filter's sources.
 */
typedef struct LionSliceCands
{
	bool		copied;
	bool		owned;			/* copy is the descent's, to release */
	LionPostingSet copy;		/* copied: the candidates */
	int			ncons;			/* !copied: the steps' slices ... */
	int			conpos[LION_SLICE_MAX_BITS];
	bool		conneg[LION_SLICE_MAX_BITS];	/* ... and how each counts */
} LionSliceCands;

/*
 * Collect sources[] into a new copy of the candidates, in place of the old
 * one.  The collection holds no pin and checks nothing against the map or the
 * heap: it is only ever counted ANDed with a slice located with its pin.
 */
static void
lion_slice_collect(LionSliceAgg *sa, LionSliceCands *c, int nsrc,
				   LionCountSource *srcs)
{
	LionPostingSet fresh;
	bool		spilled;
	bool		ok;
	MemoryContext oldcxt = MemoryContextSwitchTo(sa->candcxt);

	/*
	 * In the descent's context, which outlives the step's: the copies it
	 * replaces stay there until the descent is over, a geometric sum of what
	 * the first one takes, as each holds fewer rows than the one before.
	 */
	ok = lion_sources_collect(sa->heap, sa->snapshot, nsrc, srcs,
							  get_hash_memory_limit(), true, NULL, &fresh,
							  &spilled, &sa->stats);
	MemoryContextSwitchTo(oldcxt);
	if (!ok)
		elog(ERROR, "lion index \"%s\": the candidates of a slice descent could not be collected",
			 RelationGetRelationName(sa->index));
	if (c->copied && c->owned)
		lion_posting_set_release(&c->copy);
	c->copy = fresh;
	c->copied = true;
	c->owned = true;
	sa->collects++;
}

/*
 * The candidates of a descent at its start: E ∧ F, the rows not NULL that the
 * filter keeps - the filter's copy itself where it is that already, and
 * otherwise collected, E located again with its pin for it.
 */
static void
lion_slice_cands_begin(LionSliceAgg *sa, const LionSliceFilter *f,
					   LionSliceCands *c)
{
	LionPostingSet e;
	LionCountSource *srcs;
	int			nsrc = 1;
	int			i;

	memset(c, 0, sizeof(LionSliceCands));
	if (RecoveryInProgress())
		return;					/* counted from E and the filter's sources */

	if (f->copy != NULL && f->inexists)
	{
		c->copy = *f->copy;
		c->copied = true;
		c->owned = false;
		return;
	}

	if (!lion_slice_locate(sa, LION_SLICE_EXISTS, &e))
		elog(ERROR, "lion index \"%s\": the existence slice of key column %d has gone",
			 RelationGetRelationName(sa->index), sa->attno);
	srcs = (LionCountSource *) palloc0(sizeof(LionCountSource) *
									   (2 + Max(f->nsrc, 0)));
	srcs[0].nsets = 1;
	srcs[0].sets = &e;
	if (f->copy != NULL)
	{
		srcs[1].nsets = 1;
		srcs[1].sets = (LionPostingSet *) f->copy;
		nsrc = 2;
	}
	else
	{
		for (i = 0; i < f->nsrc; i++)
			srcs[nsrc++] = f->srcs[i];
	}
	lion_slice_collect(sa, c, nsrc, srcs);
	lion_posting_set_release(&e);
	pfree(srcs);
}

/*
 * |C ∧ S| for the slice S at pos, which is located here with its pin: 0 when
 * it has no entry.  On a primary C is the candidates' copy; on a standby the
 * count intersects E, the filter's sources and the steps' slices, all located
 * again for it.  When narrow is set and the slice splits the candidates,
 * they are narrowed by it - to C ∧ S, or C ∧ ¬S with neg - in the same breath,
 * while the slice is still located.
 */
static int64
lion_slice_step(LionSliceAgg *sa, const LionSliceFilter *f,
				LionSliceCands *c, int pos, bool narrow, bool neg,
				int64 *visible)
{
	LionPostingSet s;
	int64		count = 0;

	sa->steps++;
	if (!lion_slice_locate(sa, pos, &s))
		return 0;

	if (!RecoveryInProgress())
		count = lion_slice_count_set(sa, &s, f, &c->copy);
	else
	{
		/*
		 * A standby: E, the filter's sources, every step's slice and this
		 * one, intersected - E and the slices located with their pins.
		 */
		LionPostingSet *cons = (LionPostingSet *)
			palloc(sizeof(LionPostingSet) * (c->ncons + 1));
		LionCountSource *srcs = (LionCountSource *)
			palloc0(sizeof(LionCountSource) * (c->ncons + 2 + Max(f->nsrc, 0)));
		bool	   *own = (bool *) palloc0(sizeof(bool) *
										   (c->ncons + 2 + Max(f->nsrc, 0)));
		int			nsrc = 0;
		int			ncon = 0;
		int			i;

		srcs[nsrc].nsets = 1;
		srcs[nsrc++].sets = &s;
		if (lion_slice_locate(sa, LION_SLICE_EXISTS, &cons[ncon]))
		{
			own[nsrc] = true;
			srcs[nsrc].nsets = 1;
			srcs[nsrc++].sets = &cons[ncon++];
		}
		else
			goto standby_none;
		for (i = 0; i < f->nsrc; i++)
			srcs[nsrc++] = f->srcs[i];
		for (i = 0; i < c->ncons; i++)
		{
			LionPostingSet *cs = (LionPostingSet *)
				palloc(sizeof(LionPostingSet));

			if (!lion_slice_locate(sa, c->conpos[i], cs))
			{
				if (c->conneg[i])
					continue;	/* nothing to take away */
				goto standby_none;
			}
			own[nsrc] = true;
			srcs[nsrc].nsets = 1;
			srcs[nsrc].sets = cs;
			srcs[nsrc++].negated = c->conneg[i];
		}
		sa->counts++;
		count = lion_count_sources_cached(sa->heap, sa->snapshot, nsrc,
										  srcs, &sa->stats, sa->cache,
										  sa->rel_read_only);
standby_none:
		for (i = 1; i < nsrc; i++)
		{
			if (own[i])
				lion_posting_set_release(srcs[i].sets);
		}
	}

	/* the candidates narrowed, when the slice splits what they show */
	if (narrow && count > 0 && count < *visible)
	{
		if (!RecoveryInProgress())
		{
			LionCountSource srcs[2];

			memset(srcs, 0, sizeof(srcs));
			srcs[0].nsets = 1;
			srcs[0].sets = &c->copy;
			srcs[1].nsets = 1;
			srcs[1].sets = &s;
			srcs[1].negated = neg;
			lion_slice_collect(sa, c, 2, srcs);
		}
		else
		{
			c->conpos[c->ncons] = pos;
			c->conneg[c->ncons++] = neg;
		}
	}
	lion_posting_set_release(&s);
	return count;
}

/*
 * The largest (max) or smallest encoding among the visible rows of E ∧ F,
 * which hold `visible` rows (> 0): the greedy descent from the top bit, one
 * counted AND a bit.  The candidates C always show exactly the visible rows
 * of E ∧ F whose encodings agree with the answer's bits so far; C is
 * narrowed only when a bit splits them, as a bit that every candidate (or
 * none) has leaves the rows C shows as they were.
 *
 *	max: bit i is set when |C ∧ S_i| > 0, and C becomes C ∧ S_i;
 *	min: bit i is clear when |C ∧ S_i| < |C|, and C becomes C ∧ ¬S_i.
 *
 * A TID C holds that the snapshot does not see is decided in the heap by
 * every count that meets it, the same way each time: a row's visibility under
 * one snapshot does not change, and nothing removes a TID the snapshot sees
 * from a slice, so the visible rows C shows after a step are exactly the
 * visible rows of C before it, in or out of S_i.
 */
uint64
lion_slice_extreme(LionSliceAgg *sa, const LionSliceFilter *f, bool max,
				   int64 visible)
{
	LionSliceCands cands;
	uint64		enc = 0;
	int			bit;
	MemoryContext oldcxt;

	Assert(visible > 0);
	sa->candcxt = AllocSetContextCreate(sa->cxt,
										"lion index slice candidates",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(sa->candcxt);
	lion_slice_cands_begin(sa, f, &cands);
	MemoryContextSwitchTo(oldcxt);
	for (bit = sa->nbits - 1; bit >= 0; bit--)
	{
		int64		c;

		MemoryContextReset(sa->stepcxt);
		oldcxt = MemoryContextSwitchTo(sa->stepcxt);
		c = lion_slice_step(sa, f, &cands, bit, true, !max, &visible);
		MemoryContextSwitchTo(oldcxt);

		if (max)
		{
			if (c > 0)
			{
				enc |= UINT64CONST(1) << bit;
				visible = c;
			}
		}
		else
		{
			if (c == visible)
				enc |= UINT64CONST(1) << bit;	/* every candidate has it */
			else
				visible -= c;
		}
	}
	MemoryContextReset(sa->stepcxt);
	if (cands.copied && cands.owned)
		lion_posting_set_release(&cands.copy);
	MemoryContextDelete(sa->candcxt);
	sa->candcxt = NULL;
	return enc;
}

/* ---------------------------------------------------------------------
 * The results
 * --------------------------------------------------------------------- */

#ifdef HAVE_INT128
/* v as a numeric, exactly: in parts of 10^18 past int8's range */
Datum
lion_slice_int128_numeric(int128 v)
{
	const int64 base = INT64CONST(1000000000000000000);
	Datum		hi;

	if (v >= (int128) PG_INT64_MIN && v <= (int128) PG_INT64_MAX)
		return NumericGetDatum(int64_to_numeric((int64) v));
	hi = lion_slice_int128_numeric(v / base);
	return DirectFunctionCall2(numeric_add,
							   DirectFunctionCall2(numeric_mul, hi,
												   NumericGetDatum(int64_to_numeric(base))),
							   NumericGetDatum(int64_to_numeric((int64) (v % base))));
}

/*
 * The sum of the values of the rows counted, from the slices' counts:
 * Σ 2^i counts[i] less count · 2^(nbits-1), the encoding's offset
 * (lion_slice_enc.h), counts[nbits] being the count.  Exact: 64 slices of
 * 2^40 rows weigh to less than 2^105.
 */
int128
lion_slice_counts_sum(const int64 *counts, int nbits)
{
	int128		encsum = 0;
	int			i;

	for (i = 0; i < nbits; i++)
		encsum += (int128) counts[i] << i;
	return lion_slice_value_sum(encsum, counts[nbits], nbits);
}
#endif

/*
 * sum(x) of the rows counted, as the SQL function returns it: a numeric
 * whatever the type, exactly - in 128 bits where the compiler has them,
 * numeric arithmetic otherwise.
 */
static Datum
lion_slice_sum_numeric(const int64 *counts, int nbits)
{
#ifdef HAVE_INT128
	return lion_slice_int128_numeric(lion_slice_counts_sum(counts, nbits));
#else
	Datum		sum = NumericGetDatum(int64_to_numeric(0));
	Datum		two = NumericGetDatum(int64_to_numeric(2));
	Datum		pow = NumericGetDatum(int64_to_numeric(1));
	Datum		offset = pow;
	int			i;

	for (i = 0; i < nbits; i++)
	{
		if (counts[i] > 0)
			sum = DirectFunctionCall2(numeric_add, sum,
									  DirectFunctionCall2(numeric_mul, pow,
														  NumericGetDatum(int64_to_numeric(counts[i]))));
		if (i == nbits - 1)
			offset = pow;
		pow = DirectFunctionCall2(numeric_mul, pow, two);
	}
	return DirectFunctionCall2(numeric_sub, sum,
							   DirectFunctionCall2(numeric_mul, offset,
												   NumericGetDatum(int64_to_numeric(counts[nbits]))));
#endif
}

/*
 * The value an encoding stands for, as a Datum of the sliced type typid -
 * an int2, an int4 or a date, an int8, a time or a timestamp, each a
 * pass-by-value integer of its width (lion_slice_type_nbits()).
 */
Datum
lion_slice_value_datum(uint64 enc, int nbits, Oid typid)
{
	int64		v = lion_slice_decode(enc, nbits);

	switch (lion_slice_type_nbits(typid))
	{
		case 16:
			return Int16GetDatum((int16) v);
		case 32:
			return Int32GetDatum((int32) v);
		case 64:
			return Int64GetDatum(v);
		default:
			elog(ERROR, "lion index: type %u is not sliced", typid);
	}
	return (Datum) 0;			/* keep the compiler quiet */
}

/* The value an encoding stands for, as text in the column type's output. */
static Datum
lion_slice_value_text(LionState *state, int nbits, uint64 enc)
{
	Oid			outfunc;
	bool		isvarlena;

	getTypeOutputInfo(state->typid, &outfunc, &isvarlena);
	return CStringGetTextDatum(OidOutputFunctionCall(outfunc,
													 lion_slice_value_datum(enc, nbits,
																			state->typid)));
}

/*
 * EXECUTE on what the query the function stands for calls -
 * `SELECT count(x), sum(x), min(x), max(x) FROM t WHERE <filter>` - as the
 * executor would check it: count("any"), and sum, min and max of the key
 * type; sum only where the type has one (an integer: a date, a time or a
 * timestamp has none, and the function answers NULL for it).
 */
static void
lion_slice_check_execute(Oid typid)
{
	Oid			sumfn = InvalidOid;
	Oid			minfn;
	Oid			maxfn;

	switch (typid)
	{
		case INT2OID:
			sumfn = F_SUM_INT2;
			minfn = F_MIN_INT2;
			maxfn = F_MAX_INT2;
			break;
		case INT4OID:
			sumfn = F_SUM_INT4;
			minfn = F_MIN_INT4;
			maxfn = F_MAX_INT4;
			break;
		case INT8OID:
			sumfn = F_SUM_INT8;
			minfn = F_MIN_INT8;
			maxfn = F_MAX_INT8;
			break;
		case DATEOID:
			minfn = F_MIN_DATE;
			maxfn = F_MAX_DATE;
			break;
		case TIMEOID:
			minfn = F_MIN_TIME;
			maxfn = F_MAX_TIME;
			break;
		case TIMESTAMPOID:
			minfn = F_MIN_TIMESTAMP;
			maxfn = F_MAX_TIMESTAMP;
			break;
		case TIMESTAMPTZOID:
			minfn = F_MIN_TIMESTAMPTZ;
			maxfn = F_MAX_TIMESTAMPTZ;
			break;
		default:
			elog(ERROR, "lion index: type %u is not sliced", typid);
	}
	lion_check_aggregate_execute(F_COUNT_ANY);
	if (OidIsValid(sumfn))
		lion_check_aggregate_execute(sumfn);
	lion_check_aggregate_execute(minfn);
	lion_check_aggregate_execute(maxfn);
}

/* ---------------------------------------------------------------------
 * What the planner reads of a column's slices
 * --------------------------------------------------------------------- */

/*
 * The rows and items each slice of key column attno records in its entry's
 * header, read off the directory leaves its slices lie on - the last run of
 * the column's entries, a leaf or two (DESIGN.md §51, "The entry and its
 * order") - from where a descent to its slice 0 lands, without locating a
 * set: what lion_summary_shape() is to the summaries.  At most a few
 * leaves are read; past them `complete` is false.
 */
#define LION_SLICE_SHAPE_LEAVES	4

void
lion_slice_shape(Relation index, AttrNumber attno, LionSliceShape *shape)
{
	LionState  *col = lion_index_column_state(index, attno);
	LionSearchKey sk;
	uint8		posbyte;
	Buffer		buf;
	OffsetNumber off;
	int			nleaves = 1;
	LionRightWalk walk;

	memset(shape, 0, sizeof(LionSliceShape));
	if (!col->sliced)
		return;
	shape->nbits = col->slicebits;
	lion_rightwalk_init(&walk);

	lion_search_key_slice(col, &sk, 0, &posbyte);
	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

		for (; off <= maxoff; off++)
		{
			LionEntryTuple *e = lion_page_entry(page, off);
			int			pos;
			int			i;

			if (e->attno != col->attno || !LionEntryIsSlice(e))
			{
				/* past the column's slices, or not yet at them */
				if (e->attno > col->attno ||
					(e->attno == col->attno &&
					 lion_entry_kind(e) > LION_KIND_SLICE))
					shape->complete = true;
				if (shape->complete)
					break;
				continue;
			}
			pos = lion_entry_slice_pos(e);
			if (pos != LION_SLICE_EXISTS && pos >= col->slicebits)
				continue;		/* verify's to report */
			i = lion_slice_pos_index(pos, col->slicebits);
			shape->rows[i] = (double) lion_entry_ntids(e);
			shape->items[i] = (double) lion_entry_ncontainers(e);
			shape->nslices++;
			if (pos == LION_SLICE_EXISTS)
				shape->complete = true;	/* E sorts last */
		}
		if (shape->complete || LionPageIsRightmost(page))
		{
			shape->complete = true;
			break;
		}
		if (nleaves >= LION_SLICE_SHAPE_LEAVES)
			break;
		buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE, &walk);
		off = lion_page_first_data(BufferGetPage(buf));
		nleaves++;
	}
	UnlockReleaseBuffer(buf);
}

/* ---------------------------------------------------------------------
 * The SQL function
 * --------------------------------------------------------------------- */

/*
 * lion_index_slice_agg(idx regclass, filter text DEFAULT NULL, attno int2
 * DEFAULT 1, OUT count bigint, OUT sum numeric, OUT min text, OUT max text):
 * count(x), sum(x), min(x) and max(x) of key column attno's rows the
 * snapshot sees and the filter keeps, from the column's slices.  sum is NULL
 * for a date, time or timestamp column, and with count the only answer when
 * no row is counted.
 */
Datum
lion_index_slice_agg(PG_FUNCTION_ARGS)
{
	Oid			idxoid;
	AttrNumber	attno = PG_ARGISNULL(2) ? 1 : (AttrNumber) PG_GETARG_INT16(2);
	Snapshot	snapshot;
	LionSliceAgg sa;
	LionSliceFilter f;
	LionTextFilter tf;
	LionCountStats tfstats;
	LionVisCache *cache;
	LionCountCall call;
	LionState  *state;
	Relation	index;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, true, true, true};
	bool		any = true;
	int64	   *counts;
	int64		count = 0;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	idxoid = PG_GETARG_OID(0);
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	/* as lion_index_facets() refuses it, before anything else */
	if (attno < 1)
	{
		char	   *idxname = get_rel_name(idxoid);

		if (idxname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation with OID %u does not exist", idxoid)));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lion index \"%s\" has no key column %d",
						idxname, attno)));
	}

	/*
	 * The reader contract (DESIGN.md §9, "Direct readers"): the privileges
	 * the query reads with, row-level security, snapshot eligibility and the
	 * predicate locks, as for lion_index_facets(); EXECUTE on the aggregates
	 * the query calls, below, once the column's type is known.
	 */
	lion_count_open_indexes_ext(snapshot, 1, &idxoid, NULL, attno, false,
								true, &call);
	index = call.index[0];
	state = lion_index_column_state(index, attno);
	if (!state->sliced)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("key column %d of lion index \"%s\" is not bit-sliced",
						attno, RelationGetRelationName(index)),
				 lion_slice_type_nbits(state->typid) == 0 ?
				 errdetail("Its type %s cannot be sliced; integer, date, time and timestamp columns can.",
						   format_type_be(state->typid)) :
				 errhint("Build the index WITH (slices = on), or REINDEX it after ALTER INDEX ... SET (slices = on).")));
	lion_slice_check_execute(state->typid);
	cache = lion_vis_cache_create(CurrentMemoryContext);
	lion_slice_agg_begin(&sa, call.heap, index, attno, snapshot, cache,
						 false);

	memset(&tf, 0, sizeof(tf));
	memset(&tfstats, 0, sizeof(tfstats));
	memset(&f, 0, sizeof(f));
	if (!PG_ARGISNULL(1))
	{
		any = lion_text_filter_begin(&tf, "slice filter", call.heap,
									 snapshot,
									 text_to_cstring(PG_GETARG_TEXT_PP(1)),
									 &tfstats);
		if (tf.hascopy)
			f.copy = &tf.copy;
		else if (tf.hasfilter)
		{
			f.nsrc = 1;
			f.srcs = &tf.src;
		}
	}

	counts = (int64 *) palloc0(sizeof(int64) * LION_SLICE_NPOS(sa.nbits));
	if (any)
	{
		lion_slice_counts(&sa, &f, true, counts);
		count = counts[sa.nbits];
	}
	values[0] = Int64GetDatum(count);
	if (count > 0)
	{
		if (state->typid == INT2OID || state->typid == INT4OID ||
			state->typid == INT8OID)
		{
			values[1] = lion_slice_sum_numeric(counts, sa.nbits);
			nulls[1] = false;
		}
		values[2] = lion_slice_value_text(state, sa.nbits,
										  lion_slice_extreme(&sa, &f, false,
															 count));
		values[3] = lion_slice_value_text(state, sa.nbits,
										  lion_slice_extreme(&sa, &f, true,
															 count));
		nulls[2] = nulls[3] = false;
	}

	elog(DEBUG1, "lion_index_slice_agg(%s): %d slices, %lld lookups of a slice with no entry, %lld counts, %lld candidate sets collected, %lld heap blocks counted from the map, %lld TIDs rechecked, %lld slice containers counted, %lld by the plane kernel",
		 RelationGetRelationName(index), LION_SLICE_NPOS(sa.nbits),
		 (long long) sa.absent, (long long) sa.counts,
		 (long long) sa.collects,
		 (long long) (sa.stats.blocks_skipped_via_vm +
					  tfstats.blocks_skipped_via_vm),
		 (long long) (sa.stats.tids_rechecked + tfstats.tids_rechecked),
		 (long long) sa.walk.containers, (long long) sa.walk.kernelplanes);

	/* the index-usage statistics, as lion_index_facets() reports them */
	pgstat_count_index_scan(index);
	pgstat_count_index_tuples(index, count);
	lion_text_filter_report(&tf, index, count);

	lion_text_filter_end(&tf);
	lion_slice_agg_end(&sa);
	lion_vis_cache_destroy(cache);
	lion_count_sql_close(&call);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * lion_debug_slice_insert(idx regclass, attno int2, pos int2, t tid): put the
 * row t into the slice at pos (a bit, or 64 for the existence slice) of
 * sliced key column attno, whatever its value - a slice made wrong on
 * purpose, for the tests of lion_index_verify() (test/sql/slices.sql), which
 * create the function from the library as they create
 * lion_debug_key_skew().  Superuser only; the index is written as an insert
 * writes it, under RowExclusiveLock.
 */
Datum
lion_debug_slice_insert(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	AttrNumber	attno = PG_GETARG_INT16(1);
	int			pos = PG_GETARG_INT16(2);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(3);
	Relation	heap;
	Relation	index;
	LionIndexState *ix;
	LionState  *state;
	uint64		code;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("lion_debug_slice_insert() is for superusers")));

	heap = table_open(IndexGetRelation(relid, false), RowExclusiveLock);
	index = index_open(relid, RowExclusiveLock);
	if (index->rd_rel->relam != lion_get_am_oid())
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("index \"%s\" is not a lion index",
						RelationGetRelationName(index))));
	ix = lion_get_index_state(index);
	if (attno < 1 || attno > ix->ncolumns || !ix->cols[attno - 1].sliced)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key column %d of lion index \"%s\" is not bit-sliced",
						attno, RelationGetRelationName(index))));
	state = &ix->cols[attno - 1];
	if (pos != LION_SLICE_EXISTS && (pos < 0 || pos >= state->slicebits))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key column %d of lion index \"%s\" has no slice %d",
						attno, RelationGetRelationName(index), pos)));
	lion_check_key_offset(tid);
	code = lion_tid_to_code(tid);
	lion_slice_insert_one(index, heap, state, pos, lion_code_ckey(code),
						  lion_code_lo(code));
	index_close(index, RowExclusiveLock);
	table_close(heap, RowExclusiveLock);
	PG_RETURN_VOID();
}
