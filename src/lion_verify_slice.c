/*-------------------------------------------------------------------------
 *
 * lion_verify_slice.c
 *		lion_index_verify(): the bit slices (DESIGN.md §50).
 *
 * Part of the SQL-callable helpers of the lion index; lion_funcs.h
 * describes them and declares what their files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_funcs.h"

#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/tuplesort.h"

/* ---------------------------------------------------------------------
 * Bit slices (DESIGN.md §50, "Verify")
 *
 * Every slice of a sliced column must hold exactly the rows its position
 * names: bit slice i the rows under a key whose encoding has bit i set, the
 * existence slice the rows under any key of the column - the union of its
 * VALUE entries.  Checked a container key at a time: the codes of every
 * VALUE entry of the column, each with its key's encoding, go through a
 * tuplesort by code (a quarter of maintenance_work_mem, spilling past it);
 * the slices are read as streams in container-key order beside them; and at
 * each key the slices' containers, made bitset images, are compared word by
 * word with the images the keys' codes make.  So the check holds a sort of
 * the column's rows and a page or two of images, whatever the column's size.
 *
 * Beside writers the two cannot be read at one instant: an insert puts the
 * row under its key, then into each of its slices, with nothing held in
 * between (lion_slice_insert()), and the walk reads the keys first and the
 * slices after.  So every difference is a CANDIDATE, as the summaries' are
 * (lion_verify_summary.c): the statements writing the index are waited for
 * (lion_verify_wait_for_writers()) and each candidate row is looked at again
 * - every slice it is in now, which spells its encoding, and the key that
 * encoding decodes to, under which it must be filed - and is damage only
 * when that still disagrees and the row is not DEAD: an insert that failed
 * or crashed between its steps leaves its row in some of the sets and not
 * the others, and that row no snapshot sees, and the next VACUUM takes out
 * of all of them.  VACUUM itself waits for the check on a primary.  On a
 * standby, where replay may hold an insert between its steps for as long as
 * the primary does, a candidate whose row's inserting transaction is still in
 * progress is settled as such, as the summaries' are.
 *
 * Under heapallindexed each row the heap scan sees is looked up in every
 * slice of the column too (lion_verify_row_slices()): in bit slice i exactly
 * when its value's encoding has bit i set, and in the existence slice.
 * --------------------------------------------------------------------- */

/* The candidates kept before they are settled: this many, at least. */
#define LION_VERIFY_MAX_SLICE_CANDS		10000

/* One difference the walk found, to be looked at again. */
typedef struct LionSliceCand
{
	uint64		code;			/* the row */
	uint64		enc;			/* its key's encoding, when haskey */
	bool		haskey;			/* the walk found it under a key */
} LionSliceCand;

typedef struct LionSliceCheck
{
	LionVerifyState *vs;
	LionState  *col;
	int			nbits;
	int			npos;			/* nbits + the existence slice */

	/* the keys' codes, each with its key's encoding, sorted by code */
	Tuplesortstate *sort;
	TupleDesc	desc;
	TupleTableSlot *slot;		/* what is put into the sort */
	TupleTableSlot *rslot;		/* ... and what is read back */
	bool		havenext;		/* the sort's next pair, read ahead */
	uint64		nextcode;
	uint64		nextenc;
	MemoryContext keycxt;		/* one key's set, emptied for the next */

	/* the slices, located and read as streams */
	LionPostingSet sets[LION_SLICE_MAX_POS];
	bool		found[LION_SLICE_MAX_POS];
	LionSetStream *streams[LION_SLICE_MAX_POS];
	const LionContainer *cur[LION_SLICE_MAX_POS];

	/* one container key: the slices' images, and what the keys make */
	uint64	   *img;			/* npos * LION_BITSET_WORDS */
	uint64	   *exp;			/* ... likewise */
	uint64	   *encs;			/* LION_CONTAINER_RANGE: each lo's encoding */

	LionSliceCand *cands;
	int64		ncands;
	int64		maxcands;

	int64		nrows;			/* the keys' codes read */
	int64		nckeys;			/* container keys compared */
	int64		nsettled;		/* candidates settled */
} LionSliceCheck;

static inline uint64
lion_verify_slice_encode(LionState *col, Datum key)
{
	int64		v;

	switch (col->slicebits)
	{
		case 16:
			v = DatumGetInt16(key);
			break;
		case 32:
			v = DatumGetInt32(key);
			break;
		default:
			v = DatumGetInt64(key);
			break;
	}
	return lion_slice_encode(v, col->slicebits);
}

static Datum
lion_verify_slice_datum(LionState *col, uint64 enc)
{
	int64		v = lion_slice_decode(enc, col->slicebits);

	switch (col->slicebits)
	{
		case 16:
			return Int16GetDatum((int16) v);
		case 32:
			return Int32GetDatum((int32) v);
		default:
			return Int64GetDatum(v);
	}
}

/* ---------------------------------------------------------------------
 * The keys' codes
 * --------------------------------------------------------------------- */

typedef struct LionSlicePutArg
{
	LionSliceCheck *sc;
	uint32		ckey;
	uint64		enc;
} LionSlicePutArg;

static bool
lion_verify_slice_put_code_cb(uint16 lo, void *arg)
{
	LionSlicePutArg *pa = (LionSlicePutArg *) arg;
	LionSliceCheck *sc = pa->sc;

	ExecClearTuple(sc->slot);
	sc->slot->tts_values[0] = Int64GetDatum((int64) lion_make_code(pa->ckey, lo));
	sc->slot->tts_values[1] = Int64GetDatum((int64) pa->enc);
	sc->slot->tts_isnull[0] = sc->slot->tts_isnull[1] = false;
	ExecStoreVirtualTuple(sc->slot);
	tuplesort_puttupleslot(sc->sort, sc->slot);
	if ((++sc->nrows & 0xFFFF) == 0)
		CHECK_FOR_INTERRUPTS();
	return true;
}

static bool
lion_verify_slice_put_container_cb(const LionContainer *c, void *arg)
{
	LionSlicePutArg *pa = (LionSlicePutArg *) arg;

	pa->ckey = c->ckey;
	lion_container_iterate(c, lion_verify_slice_put_code_cb, pa);
	return true;
}

/*
 * Every code of the column's VALUE entries, with its key's encoding, into the
 * sort: a walk of the column's entries in key order, each key's set read in a
 * context of its own.  The walk ends at the column's first summary or slice,
 * as every walk of keys does; the NULL entry's rows are in no slice.
 */
static void
lion_verify_slice_keys(LionSliceCheck *sc)
{
	LionEntryScan es;
	MemoryContext old;

	lion_entry_scan_begin_col(&es, sc->vs->index, sc->col->attno);
	for (;;)
	{
		Datum		key;
		LionPostingSet ps;
		LionSlicePutArg pa;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(sc->keycxt);
		old = MemoryContextSwitchTo(sc->keycxt);
		if (!lion_entry_scan_next(&es, &key, &ps))
		{
			MemoryContextSwitchTo(old);
			break;
		}
		if (!ps.keyisnull && ps.hasstoredkey)
		{
			pa.sc = sc;
			pa.ckey = 0;
			pa.enc = lion_verify_slice_encode(sc->col, ps.storedkey);
			(void) lion_sets_iterate(1, &ps, NULL,
									 lion_verify_slice_put_container_cb, &pa);
		}
		lion_posting_set_release(&ps);
		MemoryContextSwitchTo(old);
	}
	lion_entry_scan_end(&es);
	tuplesort_performsort(sc->sort);
}

static void
lion_verify_slice_read(LionSliceCheck *sc)
{
	bool		isnull;

	sc->havenext = tuplesort_gettupleslot(sc->sort, true, false, sc->rslot,
										  NULL);
	if (sc->havenext)
	{
		sc->nextcode = (uint64) DatumGetInt64(slot_getattr(sc->rslot, 1,
														   &isnull));
		sc->nextenc = (uint64) DatumGetInt64(slot_getattr(sc->rslot, 2,
														  &isnull));
	}
}

/* ---------------------------------------------------------------------
 * The second look
 * --------------------------------------------------------------------- */

/* Report a candidate that is still one, in the existing style. */
pg_noreturn static void
lion_verify_slice_report(LionSliceCheck *sc, const LionSliceCand *cand,
						 uint64 bits, bool inexists)
{
	LionVerifyState *vs = sc->vs;
	ItemPointerData tid;
	int			bit;

	lion_code_to_tid(cand->code, &tid);
	if (cand->haskey && !inexists)
		lion_corrupt("lion index \"%s\": heap tuple (%u,%u) is under a key of key column %u but not in its existence slice",
					RelationGetRelationName(vs->index),
					ItemPointerGetBlockNumber(&tid),
					ItemPointerGetOffsetNumber(&tid), sc->col->attno);
	if (!cand->haskey && inexists)
		lion_corrupt("lion index \"%s\": heap tuple (%u,%u) is in the existence slice of key column %u, but not under the key its bit slices spell",
					RelationGetRelationName(vs->index),
					ItemPointerGetBlockNumber(&tid),
					ItemPointerGetOffsetNumber(&tid), sc->col->attno);
	if (!cand->haskey)
		lion_corrupt("lion index \"%s\": heap tuple (%u,%u) is in a bit slice of key column %u, but neither in its existence slice nor under a key of it",
					RelationGetRelationName(vs->index),
					ItemPointerGetBlockNumber(&tid),
					ItemPointerGetOffsetNumber(&tid), sc->col->attno);

	/*
	 * Under a key, with bits that are not its encoding's: the first bit that
	 * differs.  Its number is part of the row's value, so it goes into the
	 * message only for a superuser, as a key value does
	 * (lion_verify_one_key()).
	 */
	bit = pg_rightmost_one_pos64(bits ^ cand->enc);
	if (!vs->showvalues)
		lion_corrupt("lion index \"%s\": heap tuple (%u,%u) is %s a bit slice of key column %u that its key %s",
					RelationGetRelationName(vs->index),
					ItemPointerGetBlockNumber(&tid),
					ItemPointerGetOffsetNumber(&tid),
					(bits & (UINT64CONST(1) << bit)) ? "in" : "missing from",
					sc->col->attno,
					(bits & (UINT64CONST(1) << bit)) ? "does not put it in" :
					"puts it in");
	lion_corrupt("lion index \"%s\": heap tuple (%u,%u) is %s the slice of bit %d of key column %u, but its key's encoding has that bit %s",
				RelationGetRelationName(vs->index),
				ItemPointerGetBlockNumber(&tid),
				ItemPointerGetOffsetNumber(&tid),
				(bits & (UINT64CONST(1) << bit)) ? "in" : "not in",
				bit, sc->col->attno,
				(bits & (UINT64CONST(1) << bit)) ? "clear" : "set");
}

/*
 * Look at every candidate again, once the writers that may explain them are
 * done (see the section header): the slices the row is in now spell an
 * encoding, which must be its key's - the key the walk found it under, or,
 * for a row the walk found under none, the key that encoding decodes to,
 * under which it must be filed now - with the row in the existence slice.
 * A DEAD row is never a difference.
 */
static void
lion_verify_slice_settle(LionSliceCheck *sc)
{
	LionVerifyState *vs = sc->vs;
	int64		i;

	if (sc->ncands == 0)
		return;
	if (vs->concurrent)
	{
		lion_verify_wait_for_writers(vs);
		vs->nwaits++;
	}

	for (i = 0; i < sc->ncands; i++)
	{
		LionSliceCand *cand = &sc->cands[i];
		uint64		bits = 0;
		bool		inexists;
		bool		ok;
		int			bit;

		CHECK_FOR_INTERRUPTS();

		/* a standby: replay may hold the insert between its steps */
		if (!vs->concurrent &&
			lion_verify_row_state(vs, cand->code) == LION_VERIFY_ROW_INSERTING)
		{
			sc->nsettled++;
			continue;
		}

		for (bit = 0; bit < sc->nbits; bit++)
			if (lion_verify_in_slice(vs, sc->col, bit, cand->code))
				bits |= UINT64CONST(1) << bit;
		inexists = lion_verify_in_slice(vs, sc->col, LION_SLICE_EXISTS,
										cand->code);
		if (cand->haskey)
			ok = inexists && bits == cand->enc;
		else
			ok = inexists &&
				lion_verify_under_key(vs, sc->col,
									  lion_verify_slice_datum(sc->col, bits),
									  cand->code);
		if (ok ||
			lion_verify_row_state(vs, cand->code) == LION_VERIFY_ROW_DEAD)
		{
			sc->nsettled++;
			continue;
		}
		lion_verify_slice_report(sc, cand, bits, inexists);
	}
	sc->ncands = 0;
}

static void
lion_verify_slice_cand(LionSliceCheck *sc, uint64 code, bool haskey,
					   uint64 enc)
{
	if (sc->ncands >= sc->maxcands)
		lion_verify_slice_settle(sc);
	sc->cands[sc->ncands].code = code;
	sc->cands[sc->ncands].haskey = haskey;
	sc->cands[sc->ncands].enc = enc;
	sc->ncands++;
}

/* ---------------------------------------------------------------------
 * The walk
 * --------------------------------------------------------------------- */

/*
 * One container key: the images the keys' codes at it make - a bit for each
 * code in the existence image and in the image of every bit its encoding has
 * set - against the slices' containers there, word by word.  A bit in one
 * and not the other is a candidate, with the encoding the walk found its row
 * under when it found one.
 */
static void
lion_verify_slice_ckey(LionSliceCheck *sc, uint32 ckey)
{
	int			p;
	int			w;

	memset(sc->img, 0, sizeof(uint64) * LION_BITSET_WORDS * sc->npos);
	memset(sc->exp, 0, sizeof(uint64) * LION_BITSET_WORDS * sc->npos);

	while (sc->havenext && lion_code_ckey(sc->nextcode) == ckey)
	{
		uint16		lo = lion_code_lo(sc->nextcode);
		uint64		enc = sc->nextenc;
		uint64		word = UINT64CONST(1) << (lo & 63);

		sc->encs[lo] = enc;
		sc->exp[(size_t) sc->nbits * LION_BITSET_WORDS + lo / 64] |= word;
		while (enc != 0)
		{
			int			bit = pg_rightmost_one_pos64(enc);

			sc->exp[(size_t) bit * LION_BITSET_WORDS + lo / 64] |= word;
			enc &= enc - 1;
		}
		lion_verify_slice_read(sc);
	}

	for (p = 0; p < sc->npos; p++)
	{
		if (sc->cur[p] != NULL && sc->cur[p]->ckey == ckey)
		{
			lion_container_or_into_bitset(sc->cur[p],
										  sc->img + (size_t) p * LION_BITSET_WORDS);
			sc->cur[p] = lion_stream_next(sc->streams[p]);
		}
	}

	/* a row that differs in several slices is one candidate */
	for (w = 0; w < LION_BITSET_WORDS; w++)
	{
		const uint64 *keyed = sc->exp + (size_t) sc->nbits * LION_BITSET_WORDS;
		uint64		diff = 0;

		for (p = 0; p < sc->npos; p++)
			diff |= sc->img[(size_t) p * LION_BITSET_WORDS + w] ^
				sc->exp[(size_t) p * LION_BITSET_WORDS + w];
		while (diff != 0)
		{
			int			b = pg_rightmost_one_pos64(diff);
			uint16		lo = (uint16) (w * 64 + b);
			bool		haskey = (keyed[w] >> b) & 1;

			lion_verify_slice_cand(sc, lion_make_code(ckey, lo), haskey,
								   haskey ? sc->encs[lo] : 0);
			diff &= diff - 1;
		}
	}
	sc->nckeys++;
}

static void
lion_verify_column_slices(LionVerifyState *vs, LionState *col)
{
	LionSliceCheck sc;
	MemoryContext cxt;
	MemoryContext old;
	int			workmem = Max(maintenance_work_mem / 4, 64);
	AttrNumber	sortcol = 1;
	Oid			sortop = Int8LessOperator;
	Oid			sortcoll = InvalidOid;
	bool		nullsfirst = false;
	int			p;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index verify slices",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);

	memset(&sc, 0, sizeof(sc));
	sc.vs = vs;
	sc.col = col;
	sc.nbits = col->slicebits;
	sc.npos = LION_SLICE_NPOS(sc.nbits);
	sc.keycxt = AllocSetContextCreate(cxt, "lion index verify slice key",
									  ALLOCSET_DEFAULT_SIZES);
	sc.maxcands = Max((int64) ((Size) maintenance_work_mem * 1024 / 4 /
							   sizeof(LionSliceCand)),
					  LION_VERIFY_MAX_SLICE_CANDS);
	sc.maxcands = Min(sc.maxcands,
					  (int64) (MaxAllocHugeSize / sizeof(LionSliceCand)));
	sc.cands = (LionSliceCand *)
		MemoryContextAllocHuge(cxt, sizeof(LionSliceCand) * sc.maxcands);
	sc.img = (uint64 *) palloc(sizeof(uint64) * LION_BITSET_WORDS * sc.npos);
	sc.exp = (uint64 *) palloc(sizeof(uint64) * LION_BITSET_WORDS * sc.npos);
	sc.encs = (uint64 *) palloc(sizeof(uint64) * LION_CONTAINER_RANGE);

	sc.desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(sc.desc, (AttrNumber) 1, "code", INT8OID, -1, 0);
	TupleDescInitEntry(sc.desc, (AttrNumber) 2, "enc", INT8OID, -1, 0);
	sc.slot = MakeSingleTupleTableSlot(sc.desc, &TTSOpsVirtual);
	sc.rslot = MakeSingleTupleTableSlot(sc.desc, &TTSOpsMinimalTuple);
	sc.sort = tuplesort_begin_heap(sc.desc, 1, &sortcol, &sortop, &sortcoll,
								   &nullsfirst, workmem, NULL, TUPLESORT_NONE);

	/* the keys first: a row in a slice went under its key before (§50) */
	lion_verify_slice_keys(&sc);
	lion_verify_slice_read(&sc);

	/* then the slices, each a stream of its containers */
	for (p = 0; p < sc.npos; p++)
	{
		int			pos = lion_slice_index_pos(p, sc.nbits);

		sc.found[p] = lion_posting_set_lookup_slice_col(vs->index, col->attno,
														pos, &sc.sets[p]);
		if (sc.found[p])
		{
			sc.streams[p] = lion_stream_begin(1, &sc.sets[p], NULL, false);
			sc.cur[p] = lion_stream_next(sc.streams[p]);
		}
		else
			lion_posting_set_release(&sc.sets[p]);
	}

	for (;;)
	{
		uint64		next = PG_UINT64_MAX;

		CHECK_FOR_INTERRUPTS();
		if (sc.havenext)
			next = lion_code_ckey(sc.nextcode);
		for (p = 0; p < sc.npos; p++)
			if (sc.cur[p] != NULL && sc.cur[p]->ckey < next)
				next = sc.cur[p]->ckey;
		if (next == PG_UINT64_MAX)
			break;
		lion_verify_slice_ckey(&sc, (uint32) next);
	}

	for (p = 0; p < sc.npos; p++)
	{
		if (!sc.found[p])
			continue;
		lion_stream_end(sc.streams[p]);
		lion_posting_set_release(&sc.sets[p]);
	}
	tuplesort_end(sc.sort);
	ExecDropSingleTupleTableSlot(sc.slot);
	ExecDropSingleTupleTableSlot(sc.rslot);

	lion_verify_slice_settle(&sc);

	elog(DEBUG1, "lion index \"%s\": key column %d, %d slices against %lld rows under its keys at %lld container keys, %lld candidates settled",
		 RelationGetRelationName(vs->index), col->attno, sc.npos,
		 (long long) sc.nrows, (long long) sc.nckeys,
		 (long long) sc.nsettled);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/*
 * The slices of every sliced key column, once the structure is known to be
 * sound: they are read the way the counts read them.
 */
void
lion_verify_slices(LionVerifyState *vs)
{
	int			c;

	for (c = 0; c < vs->ix->ncolumns; c++)
	{
		if (vs->ix->cols[c].sliced)
			lion_verify_column_slices(vs, &vs->ix->cols[c]);
	}
}

/*
 * heapallindexed: a row the heap scan sees, with key `key` in sliced column
 * state, is in exactly the bit slices its value's encoding has set, and in
 * the existence slice.  The row's insert committed before the scan's
 * snapshot was taken, so it went through every step: nothing is a candidate.
 */
void
lion_verify_row_slices(LionVerifyState *vs, LionState *state, ItemPointer tid,
					   Datum key, uint64 code)
{
	uint64		enc = lion_verify_slice_encode(state, key);
	int			bit;

	for (bit = 0; bit < state->slicebits; bit++)
	{
		bool		want = (enc >> bit) & 1;

		if (lion_verify_in_slice(vs, state, bit, code) == want)
			continue;
		if (!vs->showvalues)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("heap tuple (%u,%u) from table \"%s\" is %s a bit slice of key column %u in \"%s\" that its value %s",
							ItemPointerGetBlockNumber(tid),
							ItemPointerGetOffsetNumber(tid),
							RelationGetRelationName(vs->heap),
							want ? "missing from" : "in", state->attno,
							RelationGetRelationName(vs->index),
							want ? "puts it in" : "does not put it in")));
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" is %s the slice of bit %d of key column %u in \"%s\"",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(vs->heap),
						want ? "missing from" : "in", bit, state->attno,
						RelationGetRelationName(vs->index)),
				 errdetail("Its value's encoding is %016llX, which has bit %d %s.",
						   (unsigned long long) enc, bit,
						   want ? "set" : "clear")));
	}
	if (!lion_verify_in_slice(vs, state, LION_SLICE_EXISTS, code))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" is missing from the existence slice of key column %u in \"%s\"",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(vs->heap), state->attno,
						RelationGetRelationName(vs->index)),
				 errdetail("Key column %u of the tuple is not NULL.",
						   state->attno)));
}
