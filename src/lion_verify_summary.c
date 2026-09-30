/*-------------------------------------------------------------------------
 *
 * lion_verify_summary.c
 *		lion_index_verify(): the summary posting sets (DESIGN.md §32).
 *
 * Part of the SQL-callable helpers of the lion index; lion_funcs.h
 * describes them and declares what their files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_funcs.h"

/* ---------------------------------------------------------------------
 * Summary posting sets (DESIGN.md §32)
 *
 * Every summary must hold exactly the rows of its bucket - the union of the
 * posting sets of the column's keys above the previous summary's key and at or
 * below its own - and the open bucket's key must be at or above every key it
 * holds.  Checked a bucket at a time, in key order: the bucket's summary is
 * looked up as it is at that moment (lion_verify_summary_after()), the codes
 * of the bucket's keys are collected and sorted, and the summary's codes are
 * streamed past them in code order (lion_verify_bucket_compare()).  The keys'
 * codes are held in an array up to a quarter of maintenance_work_mem and past
 * it in a tuplesort of as much, which spills, and the array goes (as the
 * build's, lion_build.c); the summary's are never held at all: one bucket can
 * be most of the table - a column of few keys with summaries = on, or the
 * single bucket every row of a column whose keys arrive in descending order
 * goes into - and both used to be collected whole.
 *
 * Nothing else the check holds grows with the column either.  A bucket's
 * bounds are kept only while a candidate (below) is in it: the record of a
 * bucket without one goes as the walk leaves it, and every record but the
 * current one once the candidates are settled, and the candidates and the
 * records of their buckets, bounds included, are settled once they fill a
 * quarter of maintenance_work_mem.  A key's set and the summary compared with
 * its bucket are read in memory contexts of their own, emptied for the next:
 * a located set copies its key and an INLINE payload into the caller's
 * context, and the walk used to leave them all there, with every bucket's
 * bounds beside them, until the column was done (2026-09-28 review).
 *
 * Beside writers (DESIGN.md §7) the two cannot be read at one instant: an
 * insert puts a row under its key's entry and then into its summary, with
 * nothing held in between, and the walk may read the key after the first step
 * and the summary before the second.  So every difference is a CANDIDATE, as
 * for the structure: the statements writing the index are waited for
 * (lion_verify_wait_for_writers()) and the candidates are looked at again, in
 * one ordered pass over the summaries of their buckets and one over the
 * column's keys (lion_verify_settle()) - the row a summary lacks is looked for
 * in the summary its bucket has now, and the row a summary holds and its
 * bucket's keys lacked is looked for among the bucket's keys again.  What is
 * still missing is damage, unless the row is DEAD: an insert that failed or
 * crashed between its two steps leaves its row in one of the two sets and not
 * the other, and that row is one no snapshot can see, and the next VACUUM
 * removes it from both.
 *
 * The OPEN bucket is read when the walk reaches it: its key and its summary as
 * they are then, and every key the walk finds above that key belongs to a row
 * that ARRIVED while the check ran - an append - and is no candidate.  So does
 * every key of a column that has no summary at all above the last one the
 * walk read (one built on an empty table with summaries = on, or one a crash
 * left between the two records of a close): a row that went through both of
 * its steps before that lookup is in a summary above its key, so a row whose
 * key is above every summary there was is one whose insert had not, and it
 * may be in any bucket the inserts running beside the walk open and close
 * after it - it used to be a candidate looked for in the first of them only,
 * and a load into such a column reported the rows of the second as missing.
 * Only the largest key that arrived is kept, and it has to be inside a bucket
 * once the writers are done (lion_verify_check_above()).
 *
 * The walk used to take the open bucket's key from a copy of its leaf made at
 * its start, so every row appended while it ran was a candidate, and one that
 * cost a walk of the whole column to settle; and a cap on the candidates,
 * meant for a summary of garbage, was applied before any was settled - so a
 * column under steady appends was reported corrupt (2026-09-28 review).  Now
 * nothing caps the candidates but memory: once they fill their quarter of
 * maintenance_work_mem (above), they are settled there and then and the walk
 * goes on, and the first one that is still a candidate after settling is what
 * is reported.
 * --------------------------------------------------------------------- */

/* One difference the walk found, to be looked at again. */
typedef struct LionVerifySumCand
{
	uint64		code;			/* the row */
	int64		bucket;			/* index into the buckets below */
	bool		insummary;		/* the summary has it and the keys do not */
	bool		settled;		/* found where it belongs on the second look */
} LionVerifySumCand;

/*
 * A bucket as the walk saw it: its bounds, for the second look.  Only the
 * buckets candidates are in, and the one being walked, have one
 * (lion_verify_add_bucket()); the bounds are copies of their own.
 */
typedef struct LionVerifyBucket
{
	char	   *lo;				/* the previous summary's key, or NULL */
	Size		lolen;
	char	   *hi;				/* its own key; NULL: open above */
	Size		hilen;
} LionVerifyBucket;

/*
 * The codes of one bucket's keys, in bounded memory (see above), as the build
 * collects a bucket's (lion_build.c).  Each is stored shifted left by a bit
 * whose value is LION_VERIFY_ARRIVED for a row of a key above the open
 * bucket's key, so that one sort puts both kinds in code order.
 */
#define LION_VERIFY_ARRIVED		((uint64) 1)

typedef struct LionVerifyCodes
{
	uint64	   *codes;
	int64		n;
	int64		cap;
	int64		max;			/* past this the codes go to `sort` */
	int64		pos;			/* reading codes[] back */
	Tuplesortstate *sort;
} LionVerifyCodes;

typedef struct LionVerifySumState
{
	LionVerifyState *vs;
	LionState  *col;
	MemoryContext cxt;			/* what lasts for the column */
	MemoryContext bucketcxt;	/* the summary of the bucket being compared */
	MemoryContext keycxt;		/* the set of the key being read */
	MemoryContext settlecxt;	/* what a settle reads */
	LionVerifyCodes keys;		/* the codes of the bucket's keys */
	LionVerifySumCand *cands;
	int64		ncands;
	int64		maxcands;
	int64		candcap;		/* the most candidates `budget` holds */
	Size		budget;			/* for the candidates and their buckets */
	LionVerifyBucket *buckets;	/* the candidates' and the current one */
	int64		nbuckets;
	int64		maxbuckets;
	Size		bucketspace;	/* the records in buckets[], bounds included */
	/* the largest key the walk found above the open bucket's */
	char	   *above;
	Size		abovelen;
	/* the merge of a bucket's keys with its summary */
	bool		havekey;
	uint64		key;			/* the keys' next code, shifted and tagged */
	bool		haveprev;
	uint64		prev;			/* the keys' last code read */
	bool		hasresume;
	uint64		resume;			/* the summary's codes up to it are merged */
	uint32		ckey;
	bool		overflow;		/* budget full: settle, then go on */
	/* for the DEBUG1 line */
	int64		nsummaries;
	int64		nkeys;
	int64		nabove;
	int64		nsettled;
} LionVerifySumState;

/*
 * The candidates' budget is never less than this many of them take; a quarter
 * of maintenance_work_mem is more unless it is set near its least.  It used
 * to be a cap past which the column was reported corrupt, applied before a
 * single candidate was settled.
 */
#define LION_VERIFY_MAX_SUM_CANDS	10000

static int
lion_verify_code_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void
lion_verify_codes_put(LionVerifySumState *ss, uint64 code, bool arrived)
{
	LionVerifyCodes *kc = &ss->keys;
	uint64		tagged = (code << 1) | (arrived ? LION_VERIFY_ARRIVED : 0);

	if (kc->sort != NULL)
	{
		tuplesort_putdatum(kc->sort, Int64GetDatum((int64) tagged), false);
		return;
	}

	/* the first code after a bucket the tuplesort took (see below) */
	if (kc->codes == NULL)
	{
		kc->cap = 1024;
		kc->codes = (uint64 *) MemoryContextAlloc(ss->cxt,
												  sizeof(uint64) * kc->cap);
	}

	if (kc->n >= kc->cap)
	{
		if (kc->n >= kc->max)
		{
			MemoryContext old = MemoryContextSwitchTo(ss->cxt);
			int64		i;

			kc->sort = tuplesort_begin_datum(INT8OID, Int8LessOperator,
											 InvalidOid, false,
											 maintenance_work_mem / 4, NULL,
											 TUPLESORT_NONE);
			MemoryContextSwitchTo(old);
			for (i = 0; i < kc->n; i++)
				tuplesort_putdatum(kc->sort, Int64GetDatum((int64) kc->codes[i]),
								   false);
			kc->n = 0;
			tuplesort_putdatum(kc->sort, Int64GetDatum((int64) tagged), false);

			/*
			 * The array's quarter is the tuplesort's now: the array goes, and
			 * the bucket after this one starts a small one again.  It used to
			 * stay, full, beside the sort.
			 */
			pfree(kc->codes);
			kc->codes = NULL;
			kc->cap = 0;
			return;
		}
		kc->cap = Min(kc->cap * 2, kc->max);
		kc->codes = (uint64 *) repalloc_huge(kc->codes,
											 sizeof(uint64) * kc->cap);
	}
	kc->codes[kc->n++] = tagged;
}

/* Every code of a located posting set of the bucket's keys. */
typedef struct LionVerifyPutArg
{
	LionVerifySumState *ss;
	uint32		ckey;
	bool		arrived;
} LionVerifyPutArg;

static bool
lion_verify_put_code_cb(uint16 lo, void *arg)
{
	LionVerifyPutArg *pa = (LionVerifyPutArg *) arg;

	lion_verify_codes_put(pa->ss, lion_make_code(pa->ckey, lo), pa->arrived);
	return true;
}

static bool
lion_verify_put_container_cb(const LionContainer *c, void *arg)
{
	LionVerifyPutArg *pa = (LionVerifyPutArg *) arg;

	pa->ckey = c->ckey;
	lion_container_iterate(c, lion_verify_put_code_cb, pa);
	return true;
}

static void
lion_verify_put_set(LionVerifySumState *ss, LionPostingSet *ps, bool arrived)
{
	LionVerifyPutArg pa;

	if (!ps->found)
		return;
	pa.ss = ss;
	pa.ckey = 0;
	pa.arrived = arrived;
	(void) lion_sets_iterate(1, ps, NULL, lion_verify_put_container_cb, &pa);
}

/* The keys' codes are all in: sort them, for reading back. */
static void
lion_verify_codes_finish(LionVerifyCodes *kc)
{
	if (kc->sort != NULL)
		tuplesort_performsort(kc->sort);
	else if (kc->n > 1)
		qsort(kc->codes, (size_t) kc->n, sizeof(uint64), lion_verify_code_cmp);
	kc->pos = 0;
}

/* The keys' codes, sorted, read back one at a time. */
static bool
lion_verify_codes_next(LionVerifyCodes *kc, uint64 *tagged)
{
	if (kc->sort != NULL)
	{
		Datum		val;
		bool		isnull;

		if (!tuplesort_getdatum(kc->sort, true, false, &val, &isnull, NULL))
			return false;
		*tagged = (uint64) DatumGetInt64(val);
		return true;
	}
	if (kc->pos >= kc->n)
		return false;
	*tagged = kc->codes[kc->pos++];
	return true;
}

static void
lion_verify_codes_reset(LionVerifyCodes *kc)
{
	if (kc->sort != NULL)
	{
		tuplesort_end(kc->sort);
		kc->sort = NULL;
	}
	kc->n = 0;
	kc->pos = 0;
}

/*
 * A difference in the bucket being walked.  The candidates and the records of
 * their buckets are settled once they fill the budget; the array never grows
 * past what the budget holds of candidates alone, where it used to double
 * past it.
 */
static void
lion_verify_sum_cand(LionVerifySumState *ss, uint64 code, bool insummary)
{
	LionVerifySumCand *c;

	Assert(!ss->overflow && ss->ncands < ss->candcap);
	if (ss->ncands >= ss->maxcands)
	{
		ss->maxcands = Min(ss->maxcands * 2, ss->candcap);
		ss->cands = (LionVerifySumCand *)
			repalloc_huge(ss->cands, sizeof(LionVerifySumCand) * ss->maxcands);
	}
	c = &ss->cands[ss->ncands++];
	c->code = code;
	c->bucket = ss->nbuckets - 1;
	c->insummary = insummary;
	c->settled = false;
	if (ss->ncands >= ss->candcap ||
		(Size) ss->ncands * sizeof(LionVerifySumCand) + ss->bucketspace >=
		ss->budget)
		ss->overflow = true;
}

/* A copy of raw, len bytes, in the current memory context. */
static char *
lion_verify_keycopy(const char *raw, Size len)
{
	char	   *k = (char *) palloc(Max(len, 1));

	memcpy(k, raw, len);
	return k;
}

static char *
lion_verify_keydup(const LionEntryTuple *e, Size *lenp)
{
	*lenp = e->keylen;
	return lion_verify_keycopy(LionEntryGetKey(e), e->keylen);
}

/* What a bucket's record takes of the budget: itself and its bounds. */
static Size
lion_verify_bucket_space(const LionVerifyBucket *b)
{
	return sizeof(LionVerifyBucket) +
		(b->lo != NULL ? GetMemoryChunkSpace(b->lo) : 0) +
		(b->hi != NULL ? GetMemoryChunkSpace(b->hi) : 0);
}

static void
lion_verify_free_bucket(LionVerifySumState *ss, LionVerifyBucket *b)
{
	ss->bucketspace -= lion_verify_bucket_space(b);
	if (b->lo != NULL)
		pfree(b->lo);
	if (b->hi != NULL)
		pfree(b->hi);
	b->lo = b->hi = NULL;
}

/*
 * The record of the next bucket: (lo, hi], hi NULL for one open above, with
 * copies of both.  The record of the bucket the walk leaves goes first unless
 * a candidate is in it, so that buckets[] holds the buckets of the candidates
 * waiting and the current one, and no more: every bucket of the column used
 * to be kept, bounds and all, until the column was done.
 */
static void
lion_verify_add_bucket(LionVerifySumState *ss, const char *lo, Size lolen,
					   const char *hi, Size hilen)
{
	LionVerifyBucket *b;
	MemoryContext old;

	if (ss->nbuckets > 0 &&
		(ss->ncands == 0 ||
		 ss->cands[ss->ncands - 1].bucket != ss->nbuckets - 1))
		lion_verify_free_bucket(ss, &ss->buckets[--ss->nbuckets]);

	/*
	 * Every record kept has a candidate, and was charged to the budget when
	 * the last of them came, which left it short of full: so they are fewer
	 * than the budget holds of records alone, and the array never needs to
	 * grow past that and the current one.
	 */
	Assert(ss->nbuckets <= ss->ncands &&
		   (Size) ss->nbuckets * sizeof(LionVerifyBucket) < ss->budget);
	if (ss->nbuckets >= ss->maxbuckets)
	{
		ss->maxbuckets = Min(ss->maxbuckets * 2,
							 (int64) (ss->budget / sizeof(LionVerifyBucket)) + 1);
		ss->buckets = (LionVerifyBucket *)
			repalloc_huge(ss->buckets,
						  sizeof(LionVerifyBucket) * ss->maxbuckets);
	}
	Assert(ss->nbuckets < ss->maxbuckets);

	old = MemoryContextSwitchTo(ss->cxt);
	b = &ss->buckets[ss->nbuckets++];
	b->lo = (lo != NULL) ? lion_verify_keycopy(lo, lolen) : NULL;
	b->lolen = (lo != NULL) ? lolen : 0;
	b->hi = (hi != NULL) ? lion_verify_keycopy(hi, hilen) : NULL;
	b->hilen = (hi != NULL) ? hilen : 0;
	MemoryContextSwitchTo(old);
	ss->bucketspace += lion_verify_bucket_space(b);
}

/*
 * Every candidate is settled: no record but the current bucket's is wanted,
 * and that one is the first from now on.
 */
static void
lion_verify_forget_buckets(LionVerifySumState *ss)
{
	int64		i;

	Assert(ss->ncands == 0);
	if (ss->nbuckets <= 1)
		return;
	for (i = 0; i < ss->nbuckets - 1; i++)
		lion_verify_free_bucket(ss, &ss->buckets[i]);
	ss->buckets[0] = ss->buckets[ss->nbuckets - 1];
	ss->nbuckets = 1;
}

/* proc 4 of the column on a stored key and a raw one */
static inline int32
lion_verify_keycmp(LionState *col, Datum key, const char *raw)
{
	return DatumGetInt32(FunctionCall2Coll(&col->cmpproc, col->collation, key,
										   lion_fetch_key(col, raw)));
}

/* What the heap says of the row a code names (lion_verify_row_state()). */
typedef enum LionVerifyRow
{
	LION_VERIFY_ROW_DEAD,		/* dead to every transaction there is */
	LION_VERIFY_ROW_INSERTING,	/* the transaction that inserted it runs on */
	LION_VERIFY_ROW_INSERTED	/* its insert committed */
} LionVerifyRow;

/*
 * The heap row at code: DEAD - an unused or dead line pointer, or a tuple
 * HeapTupleSatisfiesVacuum() calls dead against the oldest non-removable xid,
 * which is what an insert that failed or crashed between its two steps leaves
 * - INSERTING while the transaction that inserted it is in progress, and
 * INSERTED otherwise: its insert went through both of its steps before it
 * committed.  A redirected line pointer is a HOT chain with a live member,
 * whose root's insert committed: pruning redirects only a dead root.  A row
 * the check's own transaction inserted is INSERTING, or INSERTED if it has
 * deleted it too; either is sound, as nothing of that transaction's is
 * between the two steps while the check runs.
 */
static LionVerifyRow
lion_verify_row_state(LionVerifyState *vs, uint64 code)
{
	ItemPointerData tid;
	BlockNumber blk;
	OffsetNumber off;
	Buffer		buf;
	Page		page;
	ItemId		lp;
	LionVerifyRow row;

	lion_code_to_tid(code, &tid);
	blk = ItemPointerGetBlockNumber(&tid);
	off = ItemPointerGetOffsetNumber(&tid);
	if (blk >= RelationGetNumberOfBlocks(vs->heap))
		return LION_VERIFY_ROW_DEAD;

	buf = ReadBuffer(vs->heap, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (PageIsNew(page) || off < FirstOffsetNumber ||
		off > PageGetMaxOffsetNumber(page))
		row = LION_VERIFY_ROW_DEAD;
	else
	{
		lp = PageGetItemId(page, off);
		if (!ItemIdIsUsed(lp) || ItemIdIsDead(lp))
			row = LION_VERIFY_ROW_DEAD;
		else if (ItemIdIsRedirected(lp))
			row = LION_VERIFY_ROW_INSERTED;
		else
		{
			HeapTupleData tup;

			tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			tup.t_len = ItemIdGetLength(lp);
			tup.t_tableOid = RelationGetRelid(vs->heap);
			ItemPointerSet(&tup.t_self, blk, off);
			switch (HeapTupleSatisfiesVacuum(&tup,
											 GetOldestNonRemovableTransactionId(vs->heap),
											 buf))
			{
				case HEAPTUPLE_DEAD:
					row = LION_VERIFY_ROW_DEAD;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:
					row = LION_VERIFY_ROW_INSERTING;
					break;
				default:
					/* LIVE, RECENTLY_DEAD, DELETE_IN_PROGRESS */
					row = LION_VERIFY_ROW_INSERTED;
					break;
			}
		}
	}
	UnlockReleaseBuffer(buf);
	return row;
}

/*
 * The first summary of the column whose key is above lo - its first summary
 * when lo is NULL - as it is now, which is the summary of the bucket whose
 * lower bound is lo: middle buckets never split, and VACUUM, the one thing
 * that deletes a summary, waits for the check.  A descent to (SUMMARY, lo)
 * lands on the summary keyed lo, and the walk steps past it.  Returns false
 * when the column has no summary there; otherwise *ps is its set, located as
 * a count's is (the caller releases it), *keyp a copy of its key and
 * *islastp whether it is the open bucket's.  The copy, and what locating the
 * set copies, are in the current memory context.
 */
static bool
lion_verify_summary_after(LionVerifySumState *ss, const char *lo,
						  LionPostingSet *ps, char **keyp, Size *keylenp,
						  bool *islastp)
{
	LionVerifyState *vs = ss->vs;
	LionState  *col = lion_column(vs->ix, ss->col->attno);
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	Datum		lokey = (Datum) 0;
	LionRightWalk walk;

	if (lo != NULL)
	{
		lokey = lion_fetch_key(col, lo);
		lion_search_key_init(col, &sk, LION_KIND_SUMMARY, lokey, 0);
	}
	else
	{
		/* no key: below every summary of the column, above its values */
		lion_search_key_init(col, &sk, LION_KIND_SUMMARY, (Datum) 0, 0);
		sk.cmpproc = NULL;
	}
	buf = lion_dir_search_first(vs->index, NULL, vs->ix, &sk,
								BUFFER_LOCK_SHARE, false, &off);
	lion_rightwalk_init(&walk);
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		LionEntryTuple *e;

		if (off > PageGetMaxOffsetNumber(page))
		{
			if (LionPageIsRightmost(page))
				break;
			buf = lion_dir_step_right(vs->index, buf, BUFFER_LOCK_SHARE,
									  &walk);
			off = lion_page_first_data(BufferGetPage(buf));
			continue;
		}
		e = lion_page_entry(page, off);
		if (LionEntryIsPivot(e) || e->attno != col->attno ||
			!LionEntryIsSummary(e))
			break;
		if (lo != NULL && !LionEntryIsSumLast(e) &&
			DatumGetInt32(FunctionCall2Coll(&col->cmpproc, col->collation,
											lion_fetch_key(col, LionEntryGetKey(e)),
											lokey)) <= 0)
		{
			off = OffsetNumberNext(off);
			continue;
		}
		*keyp = lion_verify_keydup(e, keylenp);
		*islastp = LionEntryIsSumLast(e);
		lion_posting_set_at(vs->index, col, buf, off, ps);
		UnlockReleaseBuffer(buf);
		return true;
	}
	UnlockReleaseBuffer(buf);
	return false;
}

/*
 * The key of the column's open bucket now, in a copy in the current memory
 * context, or false when it has none (a column that never had a row, or a
 * crash between the two records of a close).
 */
static bool
lion_verify_open_key(LionVerifySumState *ss, char **keyp, Size *keylenp)
{
	LionVerifyState *vs = ss->vs;
	LionState  *col = lion_column(vs->ix, ss->col->attno);
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	Page		page;
	bool		found = false;

	lion_search_key_init(col, &sk, LION_KIND_SUMLAST, (Datum) 0, 0);
	buf = lion_dir_search_first(vs->index, NULL, vs->ix, &sk,
								BUFFER_LOCK_SHARE, false, &off);
	page = BufferGetPage(buf);
	if (off <= PageGetMaxOffsetNumber(page))
	{
		LionEntryTuple *e = lion_page_entry(page, off);

		if (!LionEntryIsPivot(e) && e->attno == col->attno &&
			LionEntryIsSumLast(e))
		{
			*keyp = lion_verify_keydup(e, keylenp);
			found = true;
		}
	}
	UnlockReleaseBuffer(buf);
	return found;
}

/*
 * Is a key inside a bucket now: at or below the key of a summary of its
 * column, the open one's included?  The lookup an insert makes
 * (lion_summary_insert()).  If it is and coverp is given, *coverp is a copy,
 * in the current memory context, of that summary's key - every key up to
 * which is inside a bucket too, and stays so while VACUUM waits: a closed
 * summary's key never changes and the open one's only rises.
 */
static bool
lion_verify_key_bucketed(LionVerifySumState *ss, const char *keyraw,
						 char **coverp, Size *coverlenp)
{
	LionVerifyState *vs = ss->vs;
	LionState  *col = lion_column(vs->ix, ss->col->attno);
	Datum		key = lion_fetch_key(col, keyraw);
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	Page		page;
	bool		ok = false;

	lion_search_key_init(col, &sk, LION_KIND_SUMMARY, key, 0);
	buf = lion_dir_search_first(vs->index, NULL, vs->ix, &sk,
								BUFFER_LOCK_SHARE, false, &off);
	page = BufferGetPage(buf);
	if (off <= PageGetMaxOffsetNumber(page))
	{
		LionEntryTuple *e = lion_page_entry(page, off);

		/* lion_dir_search_first() stepped over any summary below the key */
		if (!LionEntryIsPivot(e) && e->attno == col->attno &&
			LionEntryIsSummary(e))
			ok = !LionEntryIsSumLast(e) ||
				lion_verify_keycmp(col, key, LionEntryGetKey(e)) <= 0;
		if (ok && coverp != NULL)
			*coverp = lion_verify_keydup(e, coverlenp);
	}
	UnlockReleaseBuffer(buf);
	return ok;
}

/* The candidates of one bucket, cands[from .. to), for the callbacks below. */
typedef struct LionVerifySettleArg
{
	LionVerifySumState *ss;
	int64		from;
	int64		to;
	int64		cur;
	uint32		ckey;
} LionVerifySettleArg;

/*
 * The next bucket's candidates at or after cands[from] with at least one of
 * the kind `insummary` among them.  They are in the order the walk found
 * them: by bucket, and in code order within one.
 */
static bool
lion_verify_next_run(LionVerifySumState *ss, int64 from, bool insummary,
					 int64 *runfrom, int64 *runto)
{
	while (from < ss->ncands)
	{
		int64		b = ss->cands[from].bucket;
		int64		to = from;
		bool		any = false;

		while (to < ss->ncands && ss->cands[to].bucket == b)
		{
			if (ss->cands[to].insummary == insummary)
				any = true;
			to++;
		}
		if (any)
		{
			*runfrom = from;
			*runto = to;
			return true;
		}
		from = to;
	}
	return false;
}

/* A code of a bucket's summary as it is now: in step with the candidates. */
static bool
lion_verify_settle_sum_code_cb(uint16 lo, void *arg)
{
	LionVerifySettleArg *sa = (LionVerifySettleArg *) arg;
	LionVerifySumCand *cands = sa->ss->cands;
	uint64		code = lion_make_code(sa->ckey, lo);

	while (sa->cur < sa->to && cands[sa->cur].code < code)
		sa->cur++;
	if (sa->cur >= sa->to)
		return false;
	if (cands[sa->cur].code == code && !cands[sa->cur].insummary)
		cands[sa->cur].settled = true;
	return true;
}

static bool
lion_verify_settle_sum_container_cb(const LionContainer *c, void *arg)
{
	LionVerifySettleArg *sa = (LionVerifySettleArg *) arg;

	if (sa->cur >= sa->to)
		return false;
	if (lion_make_code(c->ckey, LION_LO_MASK) < sa->ss->cands[sa->cur].code)
		return true;			/* below every candidate left */
	sa->ckey = c->ckey;
	lion_container_iterate(c, lion_verify_settle_sum_code_cb, sa);
	return sa->cur < sa->to;
}

/* A code of a key of the bucket as it is now: is it a candidate? */
static bool
lion_verify_settle_key_code_cb(uint16 lo, void *arg)
{
	LionVerifySettleArg *sa = (LionVerifySettleArg *) arg;
	LionVerifySumCand *cands = sa->ss->cands;
	uint64		code = lion_make_code(sa->ckey, lo);
	int64		l = sa->from;
	int64		h = sa->to;

	while (l < h)
	{
		int64		mid = l + (h - l) / 2;

		if (cands[mid].code < code)
			l = mid + 1;
		else
			h = mid;
	}
	if (l < sa->to && cands[l].code == code && cands[l].insummary)
		cands[l].settled = true;
	return true;
}

static bool
lion_verify_settle_key_container_cb(const LionContainer *c, void *arg)
{
	LionVerifySettleArg *sa = (LionVerifySettleArg *) arg;

	sa->ckey = c->ckey;
	lion_container_iterate(c, lion_verify_settle_key_code_cb, sa);
	return true;
}

/*
 * Settle the candidates waiting: wait for the statements writing the index,
 * then look at every one again - in ONE pass over the summaries of their
 * buckets, each read once, and ONE walk of the column's keys for the rows a
 * summary held that no key of its bucket did - and report the first that is
 * still a difference and not a dead row.  A candidate used to cost a walk of
 * the whole column of its own.
 */
static void
lion_verify_settle(LionVerifySumState *ss)
{
	LionVerifyState *vs = ss->vs;
	LionState  *col = ss->col;
	MemoryContext old;
	int64		from;
	int64		to;
	int64		i;

	if (ss->ncands == 0)
		return;

	if (vs->concurrent)
	{
		lion_verify_wait_for_writers(vs);
		vs->nwaits++;
	}

	/*
	 * The rows a summary lacked: in the summary of their bucket now?  Each
	 * summary is located and read in ss->settlecxt, emptied for the next.
	 */
	old = MemoryContextSwitchTo(ss->settlecxt);
	from = 0;
	while (lion_verify_next_run(ss, from, false, &from, &to))
	{
		LionPostingSet ps;
		char	   *key;
		Size		keylen;
		bool		islast;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(ss->settlecxt);
		if (lion_verify_summary_after(ss, ss->buckets[ss->cands[from].bucket].lo,
									  &ps, &key, &keylen, &islast))
		{
			LionVerifySettleArg sa;

			sa.ss = ss;
			sa.from = from;
			sa.to = to;
			sa.cur = from;
			sa.ckey = 0;
			if (ps.found)
				(void) lion_sets_iterate(1, &ps, NULL,
										 lion_verify_settle_sum_container_cb,
										 &sa);
			lion_posting_set_release(&ps);
		}
		from = to;
	}
	MemoryContextSwitchTo(old);

	/*
	 * The rows its keys lacked: under a key of their bucket now?  The walk
	 * lasts for the settle; each key's set is read in ss->settlecxt.
	 */
	if (lion_verify_next_run(ss, 0, true, &from, &to))
	{
		LionEntryScan es;
		LionPostingSet ps;
		Datum		dummy;
		bool		more = true;

		old = MemoryContextSwitchTo(ss->cxt);
		lion_entry_scan_begin_col(&es, vs->index, col->attno);
		MemoryContextSwitchTo(ss->settlecxt);
		for (;;)
		{
			LionVerifyBucket *b = &ss->buckets[ss->cands[from].bucket];

			CHECK_FOR_INTERRUPTS();
			MemoryContextReset(ss->settlecxt);
			if (!more || !lion_entry_scan_next(&es, &dummy, &ps))
				break;
			if (ps.keyisnull || !ps.hasstoredkey)
			{
				lion_posting_set_release(&ps);
				continue;
			}

			/* past the bucket: on to the next one with such candidates */
			while (b->hi != NULL &&
				   lion_verify_keycmp(col, ps.storedkey, b->hi) > 0)
			{
				if (!lion_verify_next_run(ss, to, true, &from, &to))
				{
					more = false;
					break;
				}
				b = &ss->buckets[ss->cands[from].bucket];
			}

			if (more &&
				(b->lo == NULL ||
				 lion_verify_keycmp(col, ps.storedkey, b->lo) > 0))
			{
				LionVerifySettleArg sa;

				sa.ss = ss;
				sa.from = from;
				sa.to = to;
				sa.cur = from;
				sa.ckey = 0;
				(void) lion_sets_iterate(1, &ps, NULL,
										 lion_verify_settle_key_container_cb,
										 &sa);
			}
			lion_posting_set_release(&ps);
		}
		MemoryContextSwitchTo(old);
		lion_entry_scan_end(&es);
	}

	for (i = 0; i < ss->ncands; i++)
	{
		LionVerifySumCand *c = &ss->cands[i];

		CHECK_FOR_INTERRUPTS();
		if (c->settled ||
			lion_verify_row_state(vs, c->code) == LION_VERIFY_ROW_DEAD)
			continue;
		if (c->insummary)
			lion_corrupt_reindex("lion index \"%s\": a summary of key column %d holds row (%u,%u), which no key of its bucket holds",
								 RelationGetRelationName(vs->index), col->attno,
								 (unsigned) (c->code >> LION_OFFSET_BITS),
								 (unsigned) (c->code & ((1 << LION_OFFSET_BITS) - 1)));
		else
			lion_corrupt_reindex("lion index \"%s\": row (%u,%u) of key column %d is not in the summary of its key's bucket",
								 RelationGetRelationName(vs->index),
								 (unsigned) (c->code >> LION_OFFSET_BITS),
								 (unsigned) (c->code & ((1 << LION_OFFSET_BITS) - 1)),
								 col->attno);
	}

	ss->nsettled += ss->ncands;
	ss->ncands = 0;
	ss->overflow = false;
	lion_verify_forget_buckets(ss);
	MemoryContextReset(ss->settlecxt);
}

/* The next of the bucket's keys' codes; no row is under two keys. */
static void
lion_verify_merge_advance(LionVerifySumState *ss)
{
	ss->havekey = lion_verify_codes_next(&ss->keys, &ss->key);
	if (ss->havekey)
	{
		uint64		code = ss->key >> 1;

		if (ss->haveprev && code == ss->prev)
			lion_corrupt_reindex("lion index \"%s\": row (%u,%u) is under two keys of key column %d",
								 RelationGetRelationName(ss->vs->index),
								 (unsigned) (code >> LION_OFFSET_BITS),
								 (unsigned) (code & ((1 << LION_OFFSET_BITS) - 1)),
								 ss->col->attno);
		ss->prev = code;
		ss->haveprev = true;
	}
}

/*
 * The keys' codes below `upto` - all of them, unbounded - are not in the
 * summary: a candidate each, but for a row that arrived.  Stops early when
 * the candidates fill their budget, for the caller to settle them.
 */
static void
lion_verify_merge_keys_below(LionVerifySumState *ss, uint64 upto,
							 bool bounded)
{
	while (!ss->overflow && ss->havekey &&
		   (!bounded || (ss->key >> 1) < upto))
	{
		if ((ss->key & LION_VERIFY_ARRIVED) == 0)
			lion_verify_sum_cand(ss, ss->key >> 1, false);
		lion_verify_merge_advance(ss);
	}
}

/* A code of the summary, merged with the keys' codes. */
static bool
lion_verify_merge_code_cb(uint16 lo, void *arg)
{
	LionVerifySumState *ss = (LionVerifySumState *) arg;
	uint64		code = lion_make_code(ss->ckey, lo);

	if (ss->hasresume && code <= ss->resume)
		return true;			/* merged before the candidates were settled */
	lion_verify_merge_keys_below(ss, code, true);
	if (ss->overflow)
		return false;
	if (ss->havekey && (ss->key >> 1) == code)
		lion_verify_merge_advance(ss);
	else
		lion_verify_sum_cand(ss, code, true);
	ss->resume = code;
	ss->hasresume = true;
	return !ss->overflow;
}

static bool
lion_verify_merge_container_cb(const LionContainer *c, void *arg)
{
	LionVerifySumState *ss = (LionVerifySumState *) arg;

	if (ss->hasresume && lion_make_code(c->ckey, LION_LO_MASK) <= ss->resume)
		return true;
	ss->ckey = c->ckey;
	lion_container_iterate(c, lion_verify_merge_code_cb, ss);
	return !ss->overflow;
}

/*
 * Compare the codes of the bucket's keys, collected in ss->keys, with those of
 * its summary `ps` (NULL: it has none), and note every difference.  The
 * summary's codes stream past the sorted keys' in code order.  When the
 * candidates fill their budget the stream stops, they are settled, and it
 * starts again past the last code merged.
 */
static void
lion_verify_bucket_compare(LionVerifySumState *ss, LionPostingSet *ps)
{
	LionVerifyCodes *kc = &ss->keys;

	lion_verify_codes_finish(kc);
	ss->haveprev = false;
	ss->hasresume = false;
	lion_verify_merge_advance(ss);

	if (ps != NULL && ps->found)
	{
		for (;;)
		{
			(void) lion_sets_iterate(1, ps, NULL,
									 lion_verify_merge_container_cb, ss);
			if (!ss->overflow)
				break;
			lion_verify_settle(ss);
		}
	}
	for (;;)
	{
		lion_verify_merge_keys_below(ss, 0, false);
		if (!ss->overflow)
			break;
		lion_verify_settle(ss);
	}

	lion_verify_codes_reset(kc);
}

/* Is a row of a key's set one whose insert committed?  (below) */
typedef struct LionVerifyInsertedArg
{
	LionVerifyState *vs;
	uint32		ckey;
	bool		found;
	uint64		code;			/* the first such row */
} LionVerifyInsertedArg;

static bool
lion_verify_inserted_code_cb(uint16 lo, void *arg)
{
	LionVerifyInsertedArg *ia = (LionVerifyInsertedArg *) arg;
	uint64		code = lion_make_code(ia->ckey, lo);

	if (lion_verify_row_state(ia->vs, code) != LION_VERIFY_ROW_INSERTED)
		return true;
	ia->found = true;
	ia->code = code;
	return false;
}

static bool
lion_verify_inserted_container_cb(const LionContainer *c, void *arg)
{
	LionVerifyInsertedArg *ia = (LionVerifyInsertedArg *) arg;

	CHECK_FOR_INTERRUPTS();
	ia->ckey = c->ckey;
	lion_container_iterate(c, lion_verify_inserted_code_cb, ia);
	return !ia->found;
}

/*
 * One key of the column, with its set ps, for lion_verify_check_above():
 * false once it is past the largest key the walk found above the open
 * bucket's.  Every key at or below *boundp - a copy in ss->cxt, or NULL - is
 * inside a bucket; it is raised to the key of every bucket found on the way,
 * so that the keys are looked up about once a bucket.
 */
static bool
lion_verify_above_key(LionVerifySumState *ss, LionPostingSet *ps,
					  char **boundp)
{
	LionVerifyState *vs = ss->vs;
	LionState  *col = ss->col;
	char	   *raw;
	char	   *cover = NULL;
	Size		coverlen = 0;

	if (ps->keyisnull || !ps->hasstoredkey)
		return true;
	if (*boundp != NULL && lion_verify_keycmp(col, ps->storedkey, *boundp) <= 0)
		return true;
	if (lion_verify_keycmp(col, ps->storedkey, ss->above) > 0)
		return false;

	raw = (char *) palloc(lion_key_datum_size(col, ps->storedkey));
	lion_store_key(col, ps->storedkey, raw);

	/*
	 * A key inside a bucket now is inside one for good, which is all that is
	 * asked of it here.  One that is not has its rows looked at, in code
	 * order, up to the first whose insert committed.
	 */
	if (!lion_verify_key_bucketed(ss, raw, &cover, &coverlen))
	{
		LionVerifyInsertedArg ia;

		ia.vs = vs;
		ia.ckey = 0;
		ia.found = false;
		ia.code = 0;
		if (ps->found)
			(void) lion_sets_iterate(1, ps, NULL,
									 lion_verify_inserted_container_cb, &ia);
		if (!ia.found)
			return true;		/* dead rows, or inserts still running */

		/*
		 * That insert went through both of its steps before it committed, and
		 * so before its row was looked at just now: the key is inside a
		 * bucket from then on, or the row is in no summary and never will be.
		 */
		if (!lion_verify_key_bucketed(ss, raw, &cover, &coverlen))
			lion_corrupt_reindex("lion index \"%s\": row (%u,%u) of key column %d is in no summary: its key is above the key of every summary of the column",
								 RelationGetRelationName(vs->index),
								 (unsigned) (ia.code >> LION_OFFSET_BITS),
								 (unsigned) (ia.code & ((1 << LION_OFFSET_BITS) - 1)),
								 col->attno);
	}

	if (*boundp != NULL)
		pfree(*boundp);
	*boundp = (char *) MemoryContextAlloc(ss->cxt, Max(coverlen, 1));
	memcpy(*boundp, cover, coverlen);
	return true;
}

/*
 * The keys the walk found above the open bucket's key - or above the last
 * summary it read, where the column had none after it - belong to rows that
 * arrived while it ran, and the largest of them has to be inside a bucket
 * once their inserts are done: an insert raises the open bucket's key, or
 * closes it and opens the next, or opens the column's first, right after
 * putting its row under its key.  If it is not, the keys up to it that are
 * not inside a bucket are looked at (lion_verify_above_key()): each row of
 * theirs has to be DEAD - an insert that failed or crashed between its two
 * steps - or belong to a transaction still running, which may be between the
 * two right now: one that began after the wait, as the next statement of a
 * steady load does.  A row whose insert COMMITTED went through both steps
 * before it did, so its key is inside a bucket by the time the row is seen
 * committed, or the row is in no summary and never will be.
 *
 * This used to wait a second time and report any row not dead after it -
 * which was every insert that happened to be between its steps then, below
 * a largest key whose own insert had failed - and to compare the keys with
 * the open bucket's key as it was before the walk of them began.
 */
static void
lion_verify_check_above(LionVerifySumState *ss)
{
	LionVerifyState *vs = ss->vs;
	LionEntryScan es;
	LionPostingSet ps;
	Datum		dummy;
	char	   *bound = NULL;
	Size		boundlen = 0;
	MemoryContext old;

	if (ss->above == NULL ||
		lion_verify_key_bucketed(ss, ss->above, NULL, NULL))
		return;
	if (vs->concurrent)
	{
		lion_verify_wait_for_writers(vs);
		vs->nwaits++;
		if (lion_verify_key_bucketed(ss, ss->above, NULL, NULL))
			return;
	}

	/* every key at or below the open bucket's key now is inside a bucket */
	old = MemoryContextSwitchTo(ss->cxt);
	(void) lion_verify_open_key(ss, &bound, &boundlen);
	lion_entry_scan_begin_col(&es, vs->index, ss->col->attno);
	MemoryContextSwitchTo(ss->keycxt);
	for (;;)
	{
		bool		more;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(ss->keycxt);
		if (!lion_entry_scan_next(&es, &dummy, &ps))
			break;
		more = lion_verify_above_key(ss, &ps, &bound);
		lion_posting_set_release(&ps);
		if (!more)
			break;
	}
	MemoryContextSwitchTo(old);
	lion_entry_scan_end(&es);
	if (bound != NULL)
		pfree(bound);
}

/*
 * One summarized key column: its buckets in key order beside one walk of its
 * keys, and every difference looked at again once the writers that could
 * explain it are done.
 */
static void
lion_verify_column_summaries(LionVerifyState *vs, LionState *col)
{
	LionVerifySumState ss;
	LionEntryScan vals;
	LionPostingSet vps;
	bool		havevps = false;
	bool		valsdone = false;
	char	   *prevkey = NULL;	/* the last bucket's key, in ss.cxt */
	Size		prevlen = 0;
	char	   *startkey = NULL;
	Size		startlen = 0;
	bool		hasstart;
	Datum		dummy;
	MemoryContext old;

	memset(&ss, 0, sizeof(ss));
	ss.vs = vs;
	ss.col = col;
	ss.cxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index verify summaries",
								   ALLOCSET_DEFAULT_SIZES);
	ss.bucketcxt = AllocSetContextCreate(ss.cxt,
										 "lion index verify summary",
										 ALLOCSET_DEFAULT_SIZES);
	ss.keycxt = AllocSetContextCreate(ss.cxt,
									  "lion index verify key",
									  ALLOCSET_DEFAULT_SIZES);
	ss.settlecxt = AllocSetContextCreate(ss.cxt,
										 "lion index verify settle",
										 ALLOCSET_DEFAULT_SIZES);
	ss.keys.cap = 1024;
	ss.keys.codes = (uint64 *) MemoryContextAlloc(ss.cxt,
												  sizeof(uint64) * ss.keys.cap);
	ss.keys.max = Max((int64) maintenance_work_mem * 1024L / 4 /
					  (int64) sizeof(uint64), (int64) 8192);

	/* the candidates and the records of their buckets: see above */
	ss.budget = Max((Size) maintenance_work_mem * 1024 / 4,
					(Size) LION_VERIFY_MAX_SUM_CANDS * sizeof(LionVerifySumCand));
	ss.candcap = (int64) (ss.budget / sizeof(LionVerifySumCand));
	ss.maxcands = 64;
	ss.cands = (LionVerifySumCand *)
		MemoryContextAlloc(ss.cxt, sizeof(LionVerifySumCand) * ss.maxcands);
	ss.maxbuckets = 64;
	ss.buckets = (LionVerifyBucket *)
		MemoryContextAlloc(ss.cxt, sizeof(LionVerifyBucket) * ss.maxbuckets);

	/*
	 * The open bucket's key as the check begins: a summary at or above it
	 * was the open bucket then, closed since, and is taken as the open one -
	 * which bounds the walk however fast appends close buckets behind it.
	 */
	old = MemoryContextSwitchTo(ss.cxt);
	hasstart = lion_verify_open_key(&ss, &startkey, &startlen);
	lion_entry_scan_begin_col(&vals, vs->index, col->attno);
	MemoryContextSwitchTo(old);

	for (;;)
	{
		LionPostingSet sps;
		char	   *key = NULL;
		Size		keylen = 0;
		bool		found;
		bool		isopen = true;

		CHECK_FOR_INTERRUPTS();

		/* the bucket's summary, located in ss.bucketcxt: emptied for the next */
		MemoryContextReset(ss.bucketcxt);
		old = MemoryContextSwitchTo(ss.bucketcxt);
		found = lion_verify_summary_after(&ss, prevkey, &sps, &key, &keylen,
										  &isopen);
		if (found)
		{
			ss.nsummaries++;

			/*
			 * A closed summary after prevkey is above it by the lookup; the
			 * open one sorts by its kind alone, and its key has to be above
			 * every closed one's too, or the day it closes it is out of order.
			 */
			if (prevkey != NULL &&
				lion_verify_keycmp(col, lion_fetch_key(col, key), prevkey) <= 0)
				lion_corrupt_reindex("lion index \"%s\": a summary of key column %d is out of order",
									 RelationGetRelationName(vs->index),
									 col->attno);
			if (!isopen && hasstart &&
				lion_verify_keycmp(col, lion_fetch_key(col, key), startkey) >= 0)
				isopen = true;
		}
		else
			isopen = true;		/* keys past the last summary: none holds them */
		lion_verify_add_bucket(&ss, prevkey, prevlen, isopen ? NULL : key,
							   isopen ? 0 : keylen);

		/*
		 * The keys of the bucket, each key's set read in ss.keycxt, emptied
		 * for the next: every value up to its key, or - the open bucket -
		 * every one left, those above its key as it was read just now being
		 * rows that arrived since.  Where the column has no summary after
		 * prevkey, every key left is one that arrived since (see above).
		 */
		MemoryContextSwitchTo(ss.keycxt);
		for (;;)
		{
			bool		arrived = false;

			if (!havevps)
			{
				if (valsdone)
					break;
				MemoryContextReset(ss.keycxt);
				if (!lion_entry_scan_next(&vals, &dummy, &vps))
				{
					valsdone = true;
					break;
				}
				havevps = true;
			}
			if (vps.keyisnull || !vps.hasstoredkey)
			{
				lion_posting_set_release(&vps);
				havevps = false;
				continue;
			}
			if (key == NULL || lion_verify_keycmp(col, vps.storedkey, key) > 0)
			{
				if (!isopen)
					break;		/* the next bucket's */
				arrived = true;
				if (ss.above != NULL)
					pfree(ss.above);
				ss.abovelen = lion_key_datum_size(vals.state, vps.storedkey);
				ss.above = (char *) MemoryContextAlloc(ss.cxt, ss.abovelen);
				lion_store_key(vals.state, vps.storedkey, ss.above);
				ss.nabove++;
			}
			lion_verify_put_set(&ss, &vps, arrived);
			ss.nkeys++;
			lion_posting_set_release(&vps);
			havevps = false;
		}

		MemoryContextSwitchTo(ss.bucketcxt);
		lion_verify_bucket_compare(&ss, found ? &sps : NULL);
		if (found)
			lion_posting_set_release(&sps);
		MemoryContextSwitchTo(old);

		if (isopen)
			break;

		/* the next bucket's lower bound, kept past ss.bucketcxt's reset */
		if (prevkey != NULL)
			pfree(prevkey);
		prevkey = (char *) MemoryContextAlloc(ss.cxt, Max(keylen, 1));
		memcpy(prevkey, key, keylen);
		prevlen = keylen;
	}
	if (havevps)
		lion_posting_set_release(&vps);
	lion_entry_scan_end(&vals);

	lion_verify_settle(&ss);
	lion_verify_check_above(&ss);

	elog(DEBUG1, "lion index \"%s\": key column %d has " INT64_FORMAT " summaries over " INT64_FORMAT " keys, " INT64_FORMAT " of them arrived during the check; " INT64_FORMAT " differences settled",
		 RelationGetRelationName(vs->index), col->attno, ss.nsummaries,
		 ss.nkeys, ss.nabove, ss.nsettled);

	MemoryContextDelete(ss.cxt);
}

/* Every summarized key column's summaries (DESIGN.md §32). */
void
lion_verify_summaries(LionVerifyState *vs)
{
	int			c;

	for (c = 0; c < vs->ix->ncolumns; c++)
	{
		if (vs->ix->cols[c].summarized)
			lion_verify_column_summaries(vs, &vs->ix->cols[c]);
	}
}
