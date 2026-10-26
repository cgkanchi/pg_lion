/*-------------------------------------------------------------------------
 *
 * lion_slice.c
 *		lion_index_slice_agg(): count, sum, min and max of a bit-sliced key
 *		column's rows under an optional filter, from its SLICE entries alone
 *		(DESIGN.md §50, "The SQL function").
 *
 * A sliced column keeps one posting set per bit of its values' encoding
 * (lion_slice_enc.h) - slice i holds the rows whose encoded value has bit i
 * set - and an existence slice E of the rows not NULL.  With F the rows the
 * filter selects, all of them counted under the caller's snapshot:
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
 * copy - the filter collected once (lion_text_filter_begin(), as
 * lion_index_facets() collects it), or the descent's candidates - and every
 * TID on a page the map does not vouch for is decided in the heap under the
 * snapshot.  So each count is exact, and so is each answer made of them.
 *
 * THE SEAM for the bit-plane kernel (src/lion_planes.[ch], in progress):
 * lion_slice_counts() counts the slices one set at a time - a batch of them
 * through lion_count_groups_copy(), which reads the filter's container at
 * each key once for all of them, but ANDs and counts each slice's container
 * on its own.  The kernel will take a container key's k slice containers as
 * k planes and count the AND of each with the filter's in one strided pass.
 *
 * Nothing here is reached by the planner yet: the function is called by name
 * and goes through the reader contract of every such function
 * (lion_count_open_indexes_ext(), lion_reader_vet()).  LionCount answering
 * sum, avg, min, max and ranges from slices is the next piece of work.
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

/* One run of the function. */
typedef struct LionSliceRun
{
	Relation	heap;
	Relation	index;
	AttrNumber	attno;
	LionState  *state;
	int			nbits;
	Snapshot	snapshot;
	LionTextFilter tf;
	LionVisCache *cache;
	LionCountStats stats;
	int			batch;			/* slices counted together at most */
	PGAlignedBlock *images;		/* ... and their cursors' page images */
	MemoryContext stepcxt;		/* reset around every count */
	MemoryContext candcxt;		/* a descent's candidate sets */

	/* what DEBUG1 reports */
	int64		counts;			/* counted ANDs */
	int64		collects;		/* candidate sets collected */
	int64		absent;			/* slices with no entry */
} LionSliceRun;

/* ---------------------------------------------------------------------
 * Counting
 * --------------------------------------------------------------------- */

/*
 * The rows of a located set that the snapshot sees, ANDed with `with` - a
 * collected, pinless copy (the filter's, or the descent's candidates) - or,
 * when `with` is NULL, with the filter as it stands: its copy, its sets on a
 * standby, or nothing when there is no filter.  The set carries the §9
 * interlock with its pin.
 */
static int64
lion_slice_count_set(LionSliceRun *run, LionPostingSet *set,
					 const LionPostingSet *with)
{
	LionCountSource src[2];
	int			n = 1;

	CHECK_FOR_INTERRUPTS();
	memset(src, 0, sizeof(src));
	src[0].nsets = 1;
	src[0].sets = set;
	if (with != NULL)
	{
		src[1].nsets = 1;
		src[1].sets = (LionPostingSet *) with;
		n = 2;
	}
	else if (run->tf.hascopy)
	{
		src[1].nsets = 1;
		src[1].sets = &run->tf.copy;
		n = 2;
	}
	else if (run->tf.hasfilter)
	{
		src[1] = run->tf.src;
		n = 2;
	}
	run->counts++;
	return lion_count_sources_cached(run->heap, run->snapshot, n, src,
									 &run->stats, run->cache, false);
}

/*
 * counts[i] = |S_i ∧ F| for every bit slice i of the column, and
 * counts[nbits] = |E ∧ F|, the rows not NULL: each slice located with its
 * pin and, against a collected filter, counted a batch at a time in one walk
 * of the filter's container keys (lion_count_groups_copy(); DESIGN.md §10,
 * "The groups of a walk, counted together"), as lion_index_facets() counts
 * its keys.  A slice with no entry has no row.
 *
 * THE SEAM (see the file header): a slice at a time, a container of it at a
 * time.  The strided plane kernel replaces the inner AND-and-count of the
 * batch with one pass over the k slices' containers at a key.
 */
static void
lion_slice_counts(LionSliceRun *run, int64 *counts)
{
	int			npos = LION_SLICE_NPOS(run->nbits);
	LionPostingSet *sets;
	int		   *which;
	int64	   *bcounts;
	int			first;

	sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) * run->batch);
	which = (int *) palloc(sizeof(int) * run->batch);
	bcounts = (int64 *) palloc(sizeof(int64) * run->batch);

	for (first = 0; first < npos; first += run->batch)
	{
		int			last = Min(first + run->batch, npos);
		MemoryContext oldcxt;
		int			nfound = 0;
		int			i;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(run->stepcxt);
		oldcxt = MemoryContextSwitchTo(run->stepcxt);
		for (i = first; i < last; i++)
		{
			counts[i] = 0;
			if (lion_posting_set_lookup_slice_col(run->index, run->attno,
												  lion_slice_index_pos(i,
																	   run->nbits),
												  &sets[nfound]))
				which[nfound++] = i;
			else
			{
				lion_posting_set_release(&sets[nfound]);
				run->absent++;
			}
		}

		if (nfound > 0 && run->tf.hascopy)
		{
			lion_count_groups_copy(run->heap, run->snapshot, nfound, sets,
								   &run->tf.copy, bcounts, &run->stats,
								   run->cache, false, run->images);
			run->counts += nfound;
			for (i = 0; i < nfound; i++)
				counts[which[i]] = bcounts[i];
		}
		else
		{
			for (i = 0; i < nfound; i++)
				counts[which[i]] = lion_slice_count_set(run, &sets[i], NULL);
		}
		for (i = 0; i < nfound; i++)
			lion_posting_set_release(&sets[i]);
		MemoryContextSwitchTo(oldcxt);
	}

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
 * step (lion_sources_collect(), pinless, as the filter's own copy is); on a
 * hot standby, where nothing is collected (lion_text_filter_begin()), the
 * slices that decided the steps so far, each positive or negated, which every
 * count intersects again beside E and the filter's sets.
 */
typedef struct LionSliceCands
{
	bool		copied;
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
lion_slice_collect(LionSliceRun *run, LionSliceCands *c, int nsrc,
				   LionCountSource *srcs)
{
	LionPostingSet fresh;
	bool		spilled;
	bool		ok;
	MemoryContext oldcxt = MemoryContextSwitchTo(run->candcxt);

	/*
	 * In the descent's context, which outlives the step's: the copies it
	 * replaces stay there until the descent is over, a geometric sum of what
	 * the first one takes, as each holds fewer rows than the one before.
	 */
	ok = lion_sources_collect(run->heap, run->snapshot, nsrc, srcs,
							  get_hash_memory_limit(), true, NULL, &fresh,
							  &spilled, &run->stats);
	MemoryContextSwitchTo(oldcxt);
	if (!ok)
		elog(ERROR, "lion index \"%s\": the candidates of a slice descent could not be collected",
			 RelationGetRelationName(run->index));
	if (c->copied)
		lion_posting_set_release(&c->copy);
	c->copy = fresh;
	c->copied = true;
	run->collects++;
}

/*
 * The candidates of a descent at its start: E ∧ F, the rows not NULL that the
 * filter keeps.  E is located again, with its pin, for the collection.
 */
static void
lion_slice_cands_begin(LionSliceRun *run, LionSliceCands *c)
{
	LionPostingSet e;
	LionCountSource srcs[2];
	int			nsrc = 1;

	memset(c, 0, sizeof(LionSliceCands));
	if (RecoveryInProgress())
		return;					/* counted from E and the filter's sets */

	if (!lion_posting_set_lookup_slice_col(run->index, run->attno,
										   LION_SLICE_EXISTS, &e))
		elog(ERROR, "lion index \"%s\": the existence slice of key column %d has gone",
			 RelationGetRelationName(run->index), run->attno);
	memset(srcs, 0, sizeof(srcs));
	srcs[0].nsets = 1;
	srcs[0].sets = &e;
	if (run->tf.hascopy)
	{
		srcs[1].nsets = 1;
		srcs[1].sets = &run->tf.copy;
		nsrc = 2;
	}
	else if (run->tf.hasfilter)
		srcs[nsrc++] = run->tf.src;
	lion_slice_collect(run, c, nsrc, srcs);
	lion_posting_set_release(&e);
}

/*
 * |C ∧ S| for the slice S at pos, which is located here with its pin: 0 when
 * it has no entry.  On a primary C is the candidates' copy; on a standby the
 * count intersects E, the filter's sets and the steps' slices, all located
 * again for it.  When narrow is set and the slice splits the candidates,
 * they are narrowed by it - to C ∧ S, or C ∧ ¬S with neg - in the same breath,
 * while the slice is still located.
 */
static int64
lion_slice_step(LionSliceRun *run, LionSliceCands *c, int pos, bool narrow,
				bool neg, int64 *visible)
{
	LionPostingSet s;
	int64		count = 0;

	if (!lion_posting_set_lookup_slice_col(run->index, run->attno, pos, &s))
	{
		lion_posting_set_release(&s);
		run->absent++;
		return 0;
	}

	if (!RecoveryInProgress())
		count = lion_slice_count_set(run, &s, &c->copy);
	else
	{
		/*
		 * A standby: E, the filter's sets, every step's slice and this one,
		 * intersected - E and the slices located with their pins.
		 */
		LionPostingSet *cons = (LionPostingSet *)
			palloc(sizeof(LionPostingSet) * (c->ncons + 1));
		LionCountSource *srcs = (LionCountSource *)
			palloc0(sizeof(LionCountSource) * (c->ncons + 3));
		int			nsrc = 0;
		int			ncon = 0;
		int			i;

		srcs[nsrc].nsets = 1;
		srcs[nsrc++].sets = &s;
		if (lion_posting_set_lookup_slice_col(run->index, run->attno,
											  LION_SLICE_EXISTS, &cons[ncon]))
		{
			srcs[nsrc].nsets = 1;
			srcs[nsrc++].sets = &cons[ncon++];
		}
		else
			goto standby_none;
		if (run->tf.hasfilter)
			srcs[nsrc++] = run->tf.src;
		for (i = 0; i < c->ncons; i++)
		{
			LionPostingSet *cs = (LionPostingSet *)
				palloc(sizeof(LionPostingSet));

			if (!lion_posting_set_lookup_slice_col(run->index, run->attno,
												   c->conpos[i], cs))
			{
				lion_posting_set_release(cs);
				if (c->conneg[i])
					continue;	/* nothing to take away */
				goto standby_none;
			}
			srcs[nsrc].nsets = 1;
			srcs[nsrc].sets = cs;
			srcs[nsrc++].negated = c->conneg[i];
		}
		run->counts++;
		count = lion_count_sources_cached(run->heap, run->snapshot, nsrc,
										  srcs, &run->stats, run->cache,
										  false);
standby_none:
		for (i = 1; i < nsrc; i++)
		{
			if (srcs[i].sets != run->tf.src.sets && srcs[i].sets != NULL)
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
			lion_slice_collect(run, c, 2, srcs);
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
static uint64
lion_slice_extreme(LionSliceRun *run, bool max, int64 visible)
{
	LionSliceCands cands;
	uint64		enc = 0;
	int			bit;

	run->candcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "lion index slice candidates",
										 ALLOCSET_DEFAULT_SIZES);
	lion_slice_cands_begin(run, &cands);
	for (bit = run->nbits - 1; bit >= 0; bit--)
	{
		int64		c;
		MemoryContext oldcxt;

		MemoryContextReset(run->stepcxt);
		oldcxt = MemoryContextSwitchTo(run->stepcxt);
		c = lion_slice_step(run, &cands, bit, true, !max, &visible);
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
	if (cands.copied)
		lion_posting_set_release(&cands.copy);
	MemoryContextDelete(run->candcxt);
	run->candcxt = NULL;
	return enc;
}

/* ---------------------------------------------------------------------
 * The results
 * --------------------------------------------------------------------- */

#ifdef HAVE_INT128
/* v as a numeric, exactly: in parts of 10^18 past int8's range */
static Datum
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
#endif

/*
 * sum(x) of the rows counted, from the slices' counts: Σ 2^i counts[i] less
 * count · 2^(nbits-1), the encoding's offset (lion_slice_enc.h).  In 128 bits
 * where the compiler has them, numeric arithmetic otherwise; exact either
 * way, and a numeric whatever the type - core's sum(int8) is one, and its
 * sum(int2) and sum(int4) an int8 that a numeric holds.
 */
static Datum
lion_slice_sum(const int64 *counts, int nbits, int64 count)
{
#ifdef HAVE_INT128
	int128		encsum = 0;
	int			i;

	for (i = 0; i < nbits; i++)
		encsum += (int128) counts[i] << i;
	return lion_slice_int128_numeric(lion_slice_value_sum(encsum, count,
														  nbits));
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
												   NumericGetDatum(int64_to_numeric(count))));
#endif
}

/* The value an encoding stands for, as text in the column type's output. */
static Datum
lion_slice_value_text(LionSliceRun *run, uint64 enc)
{
	int64		v = lion_slice_decode(enc, run->nbits);
	Datum		d;
	Oid			outfunc;
	bool		isvarlena;

	switch (run->state->typlen)
	{
		case 2:
			d = Int16GetDatum((int16) v);
			break;
		case 4:
			d = Int32GetDatum((int32) v);
			break;
		default:
			d = Int64GetDatum(v);
			break;
	}
	getTypeOutputInfo(run->state->typid, &outfunc, &isvarlena);
	return CStringGetTextDatum(OidOutputFunctionCall(outfunc, d));
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
	LionSliceRun run;
	LionCountCall call;
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

	memset(&run, 0, sizeof(run));
	run.snapshot = GetActiveSnapshot();
	if (run.snapshot == NULL)
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
	lion_count_open_indexes_ext(run.snapshot, 1, &idxoid, NULL, attno, false,
								true, &call);
	run.heap = call.heap;
	run.index = call.index[0];
	run.attno = attno;
	run.state = lion_index_column_state(run.index, attno);
	if (!run.state->sliced)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("key column %d of lion index \"%s\" is not bit-sliced",
						attno, RelationGetRelationName(run.index)),
				 lion_slice_type_nbits(run.state->typid) == 0 ?
				 errdetail("Its type %s cannot be sliced; integer, date, time and timestamp columns can.",
						   format_type_be(run.state->typid)) :
				 errhint("Build the index WITH (slices = on), or REINDEX it after ALTER INDEX ... SET (slices = on).")));
	lion_slice_check_execute(run.state->typid);
	run.nbits = run.state->slicebits;
	run.cache = lion_vis_cache_create(CurrentMemoryContext);
	run.stepcxt = AllocSetContextCreate(CurrentMemoryContext,
										"lion index slice count",
										ALLOCSET_DEFAULT_SIZES);
	run.batch = Min(lion_count_groups_batch(run.index),
					LION_SLICE_NPOS(run.nbits));
	run.images = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock) * run.batch);

	if (!PG_ARGISNULL(1))
		any = lion_text_filter_begin(&run.tf, "slice filter", run.heap,
									 run.snapshot,
									 text_to_cstring(PG_GETARG_TEXT_PP(1)),
									 &run.stats);

	counts = (int64 *) palloc0(sizeof(int64) * LION_SLICE_NPOS(run.nbits));
	if (any)
	{
		lion_slice_counts(&run, counts);
		count = counts[run.nbits];
	}
	values[0] = Int64GetDatum(count);
	if (count > 0)
	{
		if (run.state->typid == INT2OID || run.state->typid == INT4OID ||
			run.state->typid == INT8OID)
		{
			values[1] = lion_slice_sum(counts, run.nbits, count);
			nulls[1] = false;
		}
		values[2] = lion_slice_value_text(&run,
										  lion_slice_extreme(&run, false, count));
		values[3] = lion_slice_value_text(&run,
										  lion_slice_extreme(&run, true, count));
		nulls[2] = nulls[3] = false;
	}

	elog(DEBUG1, "lion_index_slice_agg(%s): %d slices, %lld absent, %lld counts, %lld candidate sets collected, %lld heap blocks counted from the map, %lld TIDs rechecked",
		 RelationGetRelationName(run.index), LION_SLICE_NPOS(run.nbits),
		 (long long) run.absent, (long long) run.counts,
		 (long long) run.collects,
		 (long long) run.stats.blocks_skipped_via_vm,
		 (long long) run.stats.tids_rechecked);

	/* the index-usage statistics, as lion_index_facets() reports them */
	pgstat_count_index_scan(run.index);
	pgstat_count_index_tuples(run.index, count);
	lion_text_filter_report(&run.tf, run.index, count);

	lion_text_filter_end(&run.tf);
	lion_vis_cache_destroy(run.cache);
	MemoryContextDelete(run.stepcxt);
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
